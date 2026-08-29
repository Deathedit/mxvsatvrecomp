#!/usr/bin/env python3
"""
MX vs ATV Alive BXML decoder — full XML reconstruction.

Container: 36-byte file header + zlib raw deflate.
Decompressed payload:
  1. Strings section (null-terminated ASCII, sized by header field @12)
  2. Stream A: attribute triples [name_idx, value_idx, 0x00010000]
  3. Node records: node_count × 32 bytes (8 u32 LE fields each)

32-byte node record layout (verified on 4 reference files):
  field[0] = element_name_string_idx
  field[1] = text_content_string_idx (-1 = no text)
  field[2] = reserved (always 0)
  field[3] = has_text flag (0/1)
  field[4] = first_child_index (record index of child 0; unused when count is 0)
  field[5] = child_count
  field[6] = attr_start_index (into Stream A triples)
  field[7] = attr_count

Tree traversal: children are named EXPLICITLY -- node i's children are records
field[4] .. field[4]+field[5]-1. Nothing about the record ORDER has to be
assumed, and nothing should be: see build_tree() for the two orderings this
decoder wrongly assumed before.

Usage:
    python bxml_decoder.py <file.bxml>           # decode to XML
    python bxml_decoder.py <file.bxml> --raw     # dump internals
    python bxml_decoder.py <dir>                   # batch decode
"""
import zlib, struct, sys, os, collections
from pathlib import Path
from dataclasses import dataclass, field as dc_field
from typing import Optional, List

MARKER = 0x00010000
NO_TEXT = 0xFFFFFFFF

@dataclass
class BxmlNode:
    name: str = ""
    attrs: list = dc_field(default_factory=list)  # [(name, value)]
    text: Optional[str] = None
    children: List['BxmlNode'] = dc_field(default_factory=list)

    def to_xml(self, indent=0):
        # Iterative DFS to avoid recursion limit on deep trees (NAT_Farm = 5247 nodes, depth ~20).
        out = []
        # Stack frames: (node, indent, is_closing_tag)
        stack = [(self, indent, False)]
        while stack:
            node, ind, is_close = stack.pop()
            pad = "   " * ind
            attr_str = "".join(f' {k}="{v}"' for k, v in node.attrs)
            if is_close:
                out.append(f"{pad}</{node.name}>")
                continue
            if node.children:
                out.append(f"{pad}<{node.name}{attr_str}>")
                if node.text and node.text.strip():
                    out.append(f"{pad}   {node.text.strip()}")
                # Push children in reverse so they emit in original order, with closing tag at end.
                stack.append((node, ind, True))  # closing tag at same indent
                for child in reversed(node.children):
                    stack.append((child, ind + 1, False))
            elif node.text is not None and node.text.strip():
                out.append(f"{pad}<{node.name}{attr_str}>{node.text.strip()}</{node.name}>")
            else:
                out.append(f"{pad}<{node.name}{attr_str}/>")
        return out

    def walk(self):
        # Iterative pre-order generator.
        stack = [self]
        while stack:
            node = stack.pop()
            yield node
            for child in reversed(node.children):
                stack.append(child)

def read_bxml(path):
    with open(path, 'rb') as f:
        return read_bxml_bytes(f.read())


def read_bxml_bytes(raw):
    """Same as read_bxml but from a buffer, so a BXML block EMBEDDED in a larger
    file (a .xenon.package carries several at arbitrary offsets) can be decoded
    without being carved out to a temp file first.

    Uses decompressobj rather than zlib.decompress because an embedded block is
    followed by the rest of the package, and the one-shot call rejects trailing
    data.
    """
    if raw[:4] != b'BXML':
        raise ValueError(f'Not BXML: {raw[:4]}')
    string_count = struct.unpack('<I', raw[8:12])[0]
    strings_size = struct.unpack('<I', raw[12:16])[0]
    streaming_flag = struct.unpack('<I', raw[16:20])[0]
    aux_count = struct.unpack('<I', raw[20:24])[0]
    node_count = struct.unpack('<I', raw[24:28])[0]
    need = strings_size + streaming_flag + aux_count * 12 + node_count * 32

    # NOT EVERY BXML IS COMPRESSED. The word at +4 is version 0x03EA in the low
    # half and a COMPRESSED FLAG in the high half:
    #
    #   0x000103EA   zlib, preceded by a u32 length at 0x20, stream at 0x24
    #   0x000003EA   raw payload, starting at 0x20 with no length word
    #
    # This used to search for a 78 9C signature unconditionally. On an
    # uncompressed file that finds a byte pair somewhere in the DATA and
    # inflates garbage, which is why every `material` asset failed with
    # "unpack requires a buffer of 4 bytes" -- 10 of them in MX_tire alone.
    # The config .bxml files are all compressed, so nothing noticed for as long
    # as only those were decoded.
    #
    # CONFIRMED, not inferred: MX_Rim.material is 2395 bytes, its header sums to
    # a 2363-byte payload, and 2395 - 0x20 is exactly 2363.
    compressed = (struct.unpack('<I', raw[4:8])[0] >> 16) & 0xFFFF
    if compressed:
        # The stream is at 0x24 by the layout above. Falling back to a search
        # keeps any file whose header we have mis-read from becoming a hard
        # failure, since that was the previous behaviour for all of them.
        start = 0x24 if raw[0x24:0x26] == b'\x78\x9c' else raw.find(b'\x78\x9c')
        bin_d = zlib.decompressobj().decompress(raw[start:])
    else:
        bin_d = raw[0x20:0x20 + need]

    if len(bin_d) < need:
        raise ValueError('BXML payload short: %d bytes, header needs %d'
                         % (len(bin_d), need))
    return bin_d, string_count, strings_size, streaming_flag, aux_count, node_count

def parse_strings(bin_d, strings_size):
    strings = []
    i = 0
    while i < strings_size:
        end = bin_d.find(b'\x00', i)
        if end < 0 or end >= strings_size: break
        if end == i: i += 1; continue
        strings.append(bin_d[i:end].decode('ascii', errors='replace'))
        i = end + 1
    return strings

TYPE_STRIDES = {
    1: 0,    # string reference (no binary data)
    3: 4,    # i32
    4: 4,    # i32
    5: 4,    # i32
    6: 8,    # u64
    7: 16,   # vec4
    8: 64,   # m4x4
    9: 8,    # u64
    0xA: 12, # vec3
    0xB: 4,  # bool
    0xC: 16, # vec4
}

def read_typed_value(bin_data, offset, type_code):
    """Read a value from binary data at the given offset, interpreting by type code."""
    stride = TYPE_STRIDES.get(type_code, 4)
    if stride == 0:
        return None  # string reference, handled by caller
    raw = bin_data[offset:offset+stride]
    if type_code in (3, 4, 5, 0xB):  # int or bool
        val = struct.unpack('<i', raw[:4])[0]
        return str(val) if type_code != 0xB else ('true' if val else 'false')
    elif type_code in (6, 9):  # u64
        return str(struct.unpack('<Q', raw[:8])[0])
    elif type_code == 0xA:  # vec3 (3 floats)
        x, y, z = struct.unpack('<3f', raw[:12])
        return f'{x:.6f},{y:.6f},{z:.6f}'
    elif type_code in (7, 0xC):  # vec4 (4 floats)
        x, y, z, w = struct.unpack('<4f', raw[:16])
        return f'{x:.6f},{y:.6f},{z:.6f},{w:.6f}'
    elif type_code == 8:  # m4x4 (16 floats)
        vals = struct.unpack('<16f', raw[:64])
        return ','.join(f'{v:.6f}' for v in vals)
    return raw.hex()

def decode_bxml(path):
    """Decode a BXML file into a BxmlNode tree.
    Handles both config .bxml (string-only attrs) and .xenon.database (typed binary attrs).
    """
    with open(path, 'rb') as f:
        return decode_bxml_bytes(f.read())


def decode_bxml_bytes(raw):
    """decode_bxml over a buffer. See read_bxml_bytes for why this exists."""
    bin_d, str_count, str_size, stream_flag, aux_count, node_count = read_bxml_bytes(raw)
    strings = parse_strings(bin_d, str_size)

    # Post-strings layout: [binary_data | attr_descriptors | node_records]
    pos = str_size
    bin_data = bin_d[pos:pos + stream_flag]
    pos += stream_flag

    # Parse attr descriptors (aux_count × 12 bytes each)
    # Each: [name_str_idx(u32), value(u32), type_code(u32)]
    # type_code: high u16 = data type, low u16 = flag (0=string_ref, 1=binary_data_offset)
    all_attrs = []
    for _ in range(aux_count):
        name_idx = struct.unpack('<I', bin_d[pos:pos+4])[0]
        value    = struct.unpack('<I', bin_d[pos+4:pos+8])[0]
        type_full = struct.unpack('<I', bin_d[pos+8:pos+12])[0]
        type_code = (type_full >> 16) & 0xFFFF  # high u16
        flag      = type_full & 0xFFFF           # low u16

        n = strings[name_idx] if 0 <= name_idx < len(strings) else f'?{name_idx}'

        if flag == 0:
            # String reference — value is a string index
            # (For config .bxml: type_code=1, flag=0. type_full = 0x00010000 = MARKER)
            v = strings[value] if 0 <= value < len(strings) else str(value)
        else:
            # Binary data reference — value is an offset into bin_data
            v = read_typed_value(bin_data, value, type_code)

        all_attrs.append((n, v))
        pos += 12

    # Parse node records (node_count × 32 bytes)
    node_start = pos
    records = []
    for ni in range(node_count):
        rec = bin_d[node_start + ni*32 : node_start + (ni+1)*32]
        f = struct.unpack('<8I', rec)
        records.append(f)

    # Reconstruct the tree from the EXPLICIT first-child index in field[4].
    #
    # This decoder guessed the parent link twice before, and both guesses were
    # wrong in ways that produced plausible-looking XML:
    #
    #   1. Pre-order DFS ("the next child_count records are my children").
    #      Wrong from the very first file. The record dump makes it obvious:
    #        rec 0 Database f5=2, rec 1 Handles f5=322, rec 2 Packages f5=71
    #      `Packages` sits immediately after `Handles`, BEFORE Handles' 322
    #      children -- so a DFS walker adopts Packages into Handles and every
    #      level below compounds it. This is what turned the asset list into
    #      400-deep nesting with each <Asset> apparently containing the next.
    #
    #   2. Breadth-first allocation ("hand out record indices level by level").
    #      Fixes the case above and is still wrong. It agreed with the file for
    #      the FIRST package and diverged after: record 325 declares its
    #      children at 396..403, but record 326 declares 428, not 404. The
    #      grandchildren of 325 sit in between. BFS-by-allocation produced a
    #      Package whose direct children were bare <Heap>/<Block>/<Compress>
    #      with the <Asset> wrappers gone.
    #
    # Neither ordering has to be assumed, because field[4] names the first
    # child outright. Verified across all 130 .xenon.database files shipped
    # with the game: every record is reached exactly once, and no edge points
    # backwards or past the end.
    #
    # (field[4] was documented as "total_node_count (validation)". It is not.
    # It only looked constant because the four reference files used to derive
    # the layout were shallow enough that most nodes were leaves, and a leaf's
    # field[4] is never read.)
    def build_tree():
        if not records:
            return BxmlNode()
        nodes = []
        for f in records:
            name = strings[f[0]] if 0 <= f[0] < len(strings) else f'?{f[0]}'
            node = BxmlNode(name=name)
            if f[3] and f[1] != NO_TEXT and 0 <= f[1] < len(strings):
                node.text = strings[f[1]]
            node.attrs = all_attrs[f[6]:f[6] + f[7]]
            nodes.append(node)

        for i, f in enumerate(records):
            first, count = f[4], f[5]
            for c in range(first, min(first + count, len(nodes))):
                nodes[i].children.append(nodes[c])
        return nodes[0]

    return build_tree()

def main():
    if len(sys.argv) < 2:
        print(f'Usage: {sys.argv[0]} <file.bxml|dir> [--raw]')
        sys.exit(1)

    path = Path(sys.argv[1])

    if path.is_dir():
        files = sorted(list(path.rglob('*.bxml')) + list(path.rglob('*.xenon.database')))
        for f in files:
            try:
                root = decode_bxml(str(f))
                lines = root.to_xml()
                print(f'\n=== {f.name} ===')
                print('\n'.join(lines[:20]))
                if len(lines) > 20:
                    print(f'... ({len(lines)-20} more lines)')
            except Exception as e:
                print(f'{f.name}: ERROR {e}')
    elif path.is_file():
        root = decode_bxml(str(path))
        print('\n'.join(root.to_xml()))

if __name__ == '__main__':
    sys.setrecursionlimit(50000)
    main()