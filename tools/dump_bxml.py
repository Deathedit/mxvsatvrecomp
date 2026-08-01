import zlib, struct, sys

def decode_bxml(path):
    with open(path, 'rb') as f:
        raw = f.read()
    if raw[:4] != b'BXML':
        return None
    zlib_off = raw.find(b'\x78\x9C')
    if zlib_off < 0:
        return None
    return zlib.decompress(raw[zlib_off:])

path = sys.argv[1]
data = decode_bxml(path)
if data is None:
    print('failed')
    sys.exit(1)

print(f'=== {path} decompressed: {len(data)} bytes ===')
# Hex+ASCII dump
for i in range(0, len(data), 16):
    line = data[i:i+16]
    hex_part = ' '.join(f'{b:02X}' for b in line)
    ascii_part = ''.join(chr(b) if 32 <= b < 127 else '.' for b in line)
    print(f'{i:04X}: {hex_part:<48} {ascii_part}')