"""The container that wraps every heap in a .xenon.package.

A heap is not one compressed blob. Three layers sit between the heap extent the
.xenon.database gives you and the asset bytes:

  1. HEAP            u32 heap_len (= heap_size - 4)
                     one or more STREAMs, filling exactly to heap_size - 4
                     u32 trailer

  2. STREAM          u32 0x00010000
                     u32 stream_len   -- INCLUDES the 5-byte terminator below
                     stream_len bytes of chunk framing

  3. CHUNK framing   Xbox 360 XCompress. Per chunk:
                       0xFF, u16 BE uncompressed, u16 BE compressed
                     or, when the first byte is not 0xFF:
                       u16 BE compressed, with uncompressed implied to be
                       0x8000 (a full LZX frame)
                     terminated by five zero bytes.

  4. The chunk payloads concatenate into a raw LZX bitstream (16-bit LE words).

Verified on MXUI.xenon.package (1533 heaps): every heap parses as a stream
sequence landing exactly on heap_size - 4, and all 4388 chunk streams terminate
with exactly 5 bytes remaining.

WINDOW SIZE IS 17 BITS (128 KB), determined rather than assumed: at 15 and 16
the decoder's 0xDC window fill shows through in the output, and at 17
DefaultIcon decodes to a 'JHM\\0' magic followed by 'BXML' and a legible zlib
stream.

Two traps worth naming, because both produce output rather than an error:

  * The implicit-0x8000 chunk form. Reading only the 0xFF form makes the FIRST
    chunk of a large stream look like a terminator, so 335 of 1533 heaps
    "decompressed" to a short prefix with no error raised.
  * The 8-byte stream header. Treating the leading 12 bytes as one header and
    the rest as payload works for the 1198 single-stream heaps and silently
    truncates the other 335.

Usage:
    from xcompress import decompress_heap
    blocks = decompress_heap(package_bytes, heap_offset, heap_size)
"""

import struct

WINDOW_BITS = 17
STREAM_TAG = 0x00010000
CHUNK_MARKER = 0xFF
FRAME_SIZE = 0x8000
TERMINATOR_BYTES = 5


def split_streams(data, offset, size):
    """[(payload_offset, payload_len)] for each stream in the heap.

    Raises when the heap does not have this shape, rather than returning what
    it managed to parse -- a short read here looks exactly like a short asset.
    """
    if offset + 12 > len(data) or offset + size > len(data):
        raise ValueError('heap at %d (size %d) runs past EOF' % (offset, size))
    heap_len = struct.unpack_from('<I', data, offset)[0]
    if heap_len != size - 4:
        raise ValueError('heap at %d declares %d, database says %d'
                         % (offset, heap_len + 4, size))
    end = offset + size - 4
    i = offset + 4
    out = []
    while i < end:
        tag, stream_len = struct.unpack_from('<II', data, i)
        if tag != STREAM_TAG:
            raise ValueError('heap at %d: stream tag %#x at +%d'
                             % (offset, tag, i - offset))
        i += 8
        if i + stream_len > end:
            raise ValueError('heap at %d: stream of %d overruns at +%d'
                             % (offset, stream_len, i - offset))
        out.append((i, stream_len))
        i += stream_len
    if i != end:
        raise ValueError('heap at %d: streams end %d bytes off' % (offset, i - end))
    return out


def split_chunks(data, offset, length):
    """[(compressed_bytes, uncompressed_size)] for one stream."""
    end = offset + length
    i = offset
    out = []
    while i < end:
        if data[i] == CHUNK_MARKER:
            if i + 5 > end:
                raise ValueError('chunk header at +%d truncated' % i)
            unc, comp = struct.unpack_from('>HH', data, i + 1)
            i += 5
        else:
            if i + 2 > end:
                raise ValueError('chunk header at +%d truncated' % i)
            comp = struct.unpack_from('>H', data, i)[0]
            unc = FRAME_SIZE
            i += 2
        if comp == 0:
            # Zero-length chunk: the five-byte terminator. Anything other than
            # the terminator sitting here means the walk has desynchronised.
            left = end - i + 2
            if left != TERMINATOR_BYTES:
                raise ValueError('terminator at +%d leaves %d bytes, want %d'
                                 % (i - 2, left, TERMINATOR_BYTES))
            return out
        if i + comp > end:
            raise ValueError('chunk at +%d claims %d bytes, %d left'
                             % (i, comp, end - i))
        out.append((data[i:i + comp], unc))
        i += comp
    raise ValueError('stream at %d ended without a terminator' % offset)


def decompress_heap(data, offset, size, window_bits=WINDOW_BITS):
    """The heap's streams, decompressed, as a list of byte strings.

    Each stream gets its own LZX decoder: they are independent bitstreams, and
    a stream that begins mid-window would decode against the previous stream's
    history.
    """
    from lzx_decompress import LZXDecoder

    blocks = []
    for stream_off, stream_len in split_streams(data, offset, size):
        chunks = split_chunks(data, stream_off, stream_len)
        dec = LZXDecoder(window_bits)
        blocks.append(b''.join(dec.decompress(c, u) for c, u in chunks))
    return blocks


def decompress_asset(data, offset, size, window_bits=WINDOW_BITS):
    """decompress_heap flattened, for callers that want one buffer."""
    return b''.join(decompress_heap(data, offset, size, window_bits))
