import struct
import sys
sys.path.insert(0, '.')

from lzx_decompress import LZXDecoder

with open('assets/default.xex', 'rb') as f:
    xex = f.read()

pe_off = struct.unpack_from('>I', xex, 0x08)[0]
sec_off = struct.unpack_from('>I', xex, 0x10)[0]
img_size = struct.unpack_from('>I', xex, sec_off + 4)[0]

compressed = xex[pe_off:]
print(f'Compressed size: {len(compressed)}')
print(f'Expected uncompressed: {img_size} ({img_size/1024/1024:.1f}MB)')

for wbits in [15, 16, 17, 18, 19, 20, 21]:
    try:
        dec = LZXDecoder(wbits)
        result = dec.decompress(compressed, img_size)
        # Verify by checking for PE32+ magic or code
        if result[:2] == b'MZ':
            print(f'window_bits={wbits}: SUCCESS - got MZ header, {len(result)} bytes')
            with open(f'assets/extracted_pe.bin', 'wb') as f:
                f.write(result)
            break
        else:
            # Check if result looks like valid PE/code data
            sample = result[:64]
            print(f'window_bits={wbits}: {len(result)} bytes, first bytes: {sample.hex()[:80]}')
    except Exception as e:
        print(f'window_bits={wbits}: error - {e}')
