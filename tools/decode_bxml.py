import zlib, struct, sys, os

def decode_bxml(path):
    with open(path, 'rb') as f:
        raw = f.read()
    if raw[:4] != b'BXML':
        return None, 'not BXML'
    # Header: try several layouts. Common BXML header seen in-engine:
    #   0x00: "BXML"
    #   0x04: u32 version/flags
    #   0x08: u32 uncompressed_size (BE?)
    #   0x0C: u16 chunk_count?
    #   Then per-chunk header (u32 size, u32 something, u32 uncompressed_size, u32 reserved)
    # For these small files (1 chunk), the zlib stream starts shortly after
    # Magic. Let's find the zlib signature (0x78 0x9C).
    zlib_off = raw.find(b'\x78\x9C')
    if zlib_off < 0:
        return None, 'no zlib signature 78 9C found'
    try:
        data = zlib.decompress(raw[zlib_off:])
        return data, f'OK zlib@{zlib_off}: {len(data)} bytes decompressed'
    except Exception as e:
        return None, f'zlib@{zlib_off} decompress failed: {e}'

if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\VM\Desktop\mx\assets\Engine.bxml'
    data, msg = decode_bxml(path)
    print(f'=== {path} ===')
    print(msg)
    if data is not None:
        try:
            print(data.decode('utf-8'))
        except UnicodeDecodeError:
            print('(bytes)')
            sys.stdout.buffer.write(data)
            print()