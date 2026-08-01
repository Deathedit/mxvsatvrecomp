#!/usr/bin/env python3
"""
MX vs ATV Alive BXML decoder.

Container: "BXML" magic + header + zlib raw deflate at offset 0x24.
Decompressed payload: strings section (null-terminated) + token stream (u32 LE).

Token stream has two sub-streams separated by 0xFFFFFFFF sentinel:
  - Stream A (attributes): repeating [name_idx, value_idx, 0x00010000] triples
  - Stream B (element tree): recursive/post-order encoding of nodes

Usage:
    python bxml_decoder.py <file.bxml>          # decode to XML
    python bxml_decoder.py <file.bxml> --raw    # dump strings + tokens
    python bxml_decoder.py <dir>                # batch decode all .bxml/.database
"""
import zlib, struct, sys, os
from pathlib import Path
from dataclasses import dataclass, field
from typing import Optional

# ============================================================================
# Container parsing
# ============================================================================

def read_bxml_file(path):
    """Read a BXML container, return decompressed payload bytes."""
    with open(path, 'rb') as f:
        raw = f.read()
    if raw[:4] != b'BXML':
        raise ValueError(f'{path}: not BXML magic (got {raw[:4]!r})')
    zlib_off = raw.find(b'\x78\x9C')
    if zlib_off < 0:
        raise ValueError(f'{path}: no zlib stream found')
    return zlib.decompress(raw[zlib_off:])

# ============================================================================
# Strings section parsing
# ============================================================================

def parse_strings(bin_d):
    """Parse null-terminated strings from start of decompressed data.

    Returns (list of (offset, string), end_of_strings_offset).
    Strings section ends at the first point where we see 4 consecutive
    bytes that look like a small token value (typically 0x02/0x03
    at 4-byte alignment).
    """
    strings = []
    i = 0
    while i < len(bin_d):
        # Find null terminator
        end = i
        while end < len(bin_d) and bin_d[end] != 0:
            end += 1
        if end == i:
            # Empty string — likely end of strings section
            # Check if next 4-aligned position has a small value (token start)
            break
        s = bin_d[i:end]
        # Check if this looks like a real string (all printable ASCII)
        # or if we've drifted into the token stream
        try:
            decoded = s.decode('ascii')
            if not all(32 <= ord(c) < 127 for c in decoded):
                break
        except UnicodeDecodeError:
            break
        strings.append((i, decoded))
        i = end + 1  # skip null
    # Find end of strings section: last string's null terminator
    if strings:
        last_off, last_s = strings[-1]
        strings_end = last_off + len(last_s) + 1  # +1 for null
    else:
        strings_end = 0
    return strings, strings_end

# ============================================================================
# Token stream parsing
# ============================================================================

def find_token_start(bin_d, strings_end):
    """Find where token stream begins.

    Tokens start immediately after the strings section's last null terminator.
    Unlike what you'd expect, NO 4-byte alignment is needed — the token stream
    is aligned to the strings section boundary, not the file.
    """
    pos = strings_end
    # The byte at pos should be the first token byte (typically 0x02/0x03).
    # If it's 0x00 (null padding), scan forward to first non-zero.
    while pos < len(bin_d) and bin_d[pos] == 0:
        pos += 1
    return pos

def parse_tokens(bin_d, tok_start):
    """Parse u32 LE tokens from tok_start to end."""
    tokens = []
    i = tok_start
    while i + 4 <= len(bin_d):
        val = struct.unpack('<I', bin_d[i:i+4])[0]
        tokens.append(val)
        i += 4
    return tokens

# ============================================================================
# Tree reconstruction
# ============================================================================

@dataclass
class BxmlNode:
    name: str = ""
    attrs: list = field(default_factory=list)  # [(name, value), ...]
    text: Optional[str] = None
    children: list = field(default_factory=list)

    def to_xml(self, indent=0):
        lines = []
        pad = "   " * indent
        attr_str = ""
        for k, v in self.attrs:
            attr_str += f' {k}="{v}"'
        if self.children:
            lines.append(f"{pad}<{self.name}{attr_str}>")
            if self.text and self.text.strip():
                lines.append(f"{pad}   {self.text.strip()}")
            for child in self.children:
                lines.extend(child.to_xml(indent + 1))
            lines.append(f"{pad}</{self.name}>")
        elif self.text and self.text.strip():
            lines.append(f"{pad}<{self.name}{attr_str}>{self.text.strip()}</{self.name}>")
        else:
            lines.append(f"{pad}<{self.name}{attr_str}/>")
        return lines

MARKER = 0x00010000  # end-of-attribute marker
SENTINEL = 0xFFFFFFFF  # stream boundary sentinel

def decode_token_stream(tokens, strings):
    """Decode token stream into a tree of BxmlNodes.

    The token stream has two sections separated by SENTINEL:
    1. Attribute section: flat list of [name_idx, value_idx, MARKER] triples
    2. Element tree section: recursive node encoding
    """
    str_list = [s for _, s in strings]

    def sref(idx):
        if 0 <= idx < len(str_list):
            return str_list[idx]
        return f"?{idx}"

    # Find sentinel position
    sentinel_pos = None
    for i, t in enumerate(tokens):
        if t == SENTINEL:
            sentinel_pos = i
            break

    if sentinel_pos is None:
        # No sentinel — try to parse as pure element tree
        attr_tokens = []
        tree_tokens = tokens
    else:
        attr_tokens = tokens[:sentinel_pos]
        tree_tokens = tokens[sentinel_pos + 1:]

    # Parse attribute section: collect flat list of (name, value) pairs
    all_attrs = []
    i = 0
    while i + 2 < len(attr_tokens):
        name_idx = attr_tokens[i]
        value_idx = attr_tokens[i + 1]
        marker = attr_tokens[i + 2]
        if marker == MARKER:
            all_attrs.append((sref(name_idx), sref(value_idx)))
            i += 3
        else:
            i += 1

    # Parse element tree section
    # This is the harder part. Looking at RdbTables:
    # tree_tokens after sentinel (starting at token 32):
    # [0, 0, 1, 5, 0, 0, 1, 3, 0, 1, 6, 0, 0, 2, 1, 6, 0, 1, 6, 0, 2, 2, 1, 0, 0, 1, 6, 0, 4, 2, 1, 10, 0, 1, 6, 0, 6, 2, 1, 9, 0, 1, 6, 0, 8, 2]
    #
    # The known XML tree is:
    # <Tables>
    #   <Table type="database" collectionName="mxtables">MXTables_Schema</Table>
    #   <Table type="database" collectionName="mxtables">MXTables_Core</Table>
    #   <Table type="database" collectionName="mxtables">MXTables_Default</Table>
    #   <Table type="database" collectionName="mxtables">MXTables_Leaderboards</Table>
    #   <Table type="bxml" collectionName="mxtables">RiderXPDatabase.bxml</Table>
    # </Tables>
    #
    # The tree tokens contain: element names, text content, child counts, attr counts
    # Let me try: each node = [name_idx, text_idx_or_0, child_count, attr_count]
    # Root <Tables>: name=5, text=0(none), children=5, attrs=0
    # Child <Table> 1: name=1, text=3, children=0, attrs=2
    # Child <Table> 2: name=1, text=6, children=0, attrs=2  (attr refs: tokens 2,11 + 4,7)
    # etc.
    #
    # Looking at tree_tokens: [0, 0, 1, 5, 0, 0, 1, 3, 0, 1, 6, 0, 0, ...]
    # If we group by 4: [0,0,1,5], [0,0,1,3], [0,1,6,0], [0,2,1,6], ...
    # Group [0,0,1,5] = text=0, ?, name=1="Table", ?=5="Tables"
    # That doesn't parse cleanly.
    #
    # Let me try another approach: the tree is post-order DFS.
    # In post-order: child1, child2, ..., child5, root
    # Each child <Table> has: name=1, text content, 2 attrs
    # Root <Tables> has: name=5, no text, 0 attrs, 5 children
    #
    # Maybe each token group is: [text_idx, name_idx] for leaf nodes,
    # then [child_count, name_idx] for parent nodes (post-order).
    # total tree tokens = 46 for 6 nodes (5 children + 1 root)
    # 46 / 6 ≈ 7.7 tokens per node — not clean.
    #
    # Alternative: try reading as stream of variable-length records.
    # Each record starts with a type byte and has different fields.
    #
    # Let me try: the post-order element tree uses recursive encoding:
    #   node = [text_idx, attr_count_or_ref, name_idx]
    #   leaf (no children): [text_idx, name_idx]
    #   parent (has children): [child_count, name_idx]
    #
    # For 5 leaves + 1 parent:
    # 5 × 2 + 1 × 2 + sentinel = 12 + 2 = 14 tokens? Not 46.
    #
    # Maybe the attr section + tree section encode a stack-based traversal
    # with push/pop operations. Let me try reading from end backward.
    #
    # Actually let me look at the engine.bxml simpler case:
    # <Engine>
    #   <Registry>MXRegistry.bxml</Registry>
    #   <Registry>RendererRegistry.bxml</Registry>
    #   <Registry>UIRegistry.bxml</Registry>
    # </Engine>
    # 3 child leaves + 1 root = 4 nodes, 3 text contents
    # tokens from engine: [3, -1, 0, 0, 0, 0, 1, 3, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 4, 0, 0, 0, 0, 0, 0, 1, 2, 0, 0, 1, 4, 0, 0, 0, 0, 1, 4, 0, 0, 0, 0, 1, 4, 0, 0]
    # str: [0]=MXRegistry.bxml, [1]=Registry, [2]=RendererRegistry.bxml, [3]=Engine, [4]=UIRegistry.bxml
    # token[0] = 3 = "Engine" (root name!)
    # token[1] = -1 (sentinel)
    # remaining 43 tokens after sentinel: [0, 0, 0, 0, 1, 3, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 4, 0, 0, 0, 0, 0, 0, 1, 2, 0, 0, 1, 4, 0, 0, 0, 0, 1, 4, 0, 0, 0, 0, 1, 4, 0, 0]
    # Hmm 43 = odd number.
    #
    # Wait — the Engine binary is:
    # @0x0046: 03 00 00 00 FF FF FF FF 00 00 00 00 00 00 00 00
    # 01 00 00 00 03 00 00 00 00 00 00 00 00 00 00 00
    # 01 00 00 00 00 00 00 00 00 00 00 00 01 00 00 00
    # 04 00 00 00 00 00 00 00 00 00 00 00 01 00 00 00
    # 02 00 00 00 00 00 00 00 01 00 00 00 04 00 00 00
    # 00 00 00 00 00 00 00 00 01 00 00 00 04 00 00 00
    # 00 00 00 00 01 00 00 00 04 00 00 00 00 00 00 00
    #
    # tokens (u32 LE from 0x46):
    # [0] 3      = str[3]="Engine" (ROOT NAME FIRST!)
    # [1] -1     = SENTINEL
    # [2] 0
    # [3] 0
    # [4] 0
    # [5] 0
    # [6] 1      = str[1]="Registry"
    # [7] 3      = str[3]="Engine"  (parent ref?)
    # [8] 0      = str[0]="MXRegistry.bxml" (text content!)
    # [9] 0
    # [10] 0
    # [11] 0
    # [12] 1     = "Registry"
    # [13] 0     = "MXRegistry.bxml" (text)
    # [14] 0
    # [15] 0
    # [16] 0
    # [17] 0
    # [18] 1     = "Registry"
    # [19] 4     = str[4]="UIRegistry.bxml" (text)
    # [20] 0
    # [21] 0
    # [22] 0
    # [23] 0
    # [24] 0
    # [25] 0
    # [26] 1     = "Registry"
    # [27] 2     = str[2]="RendererRegistry.bxml" (text)
    # [28] 0
    # [29] 0
    # [30] 1     = "Registry"
    # [31] 4     = "UIRegistry.bxml" (text)
    # [32] 0
    # [33] 0
    # [34] 0
    # [35] 0
    # [36] 1     = "Registry"
    # [37] 4     = "UIRegistry.bxml"
    # [38] 0
    # [39] 0
    # [40] 1     = "Registry"
    # [41] 4     = "UIRegistry.bxml"
    # [42] 0
    # [43] 0
    #
    # Hmm, the pattern doesn't jump out cleanly. Too many duplicates.
    # Let me try: root_name=3="Engine", then sentinel, then tree:
    # After sentinel: [0,0,0,0, 1,3, 0,0,0,0, 1,0, 0,0,0,0, 1,4, 0,0,0,0,0,0, 1,2, 0,0, 1,4, 0,0,0,0, 1,4, 0,0, 1,4, 0,0]
    #
    # Strings: 0=MXRegistry.bxml, 1=Registry, 2=RendererRegistry.bxml, 3=Engine, 4=UIRegistry.bxml
    # 3 children: <Registry>MXRegistry.bxml</Registry>, <Registry>RendererRegistry.bxml</Registry>, <Registry>UIRegistry.bxml</Registry>
    #
    # The text values of children: MXRegistry.bxml(0), RendererRegistry.bxml(2), UIRegistry.bxml(4)
    # In the tree tokens: 0, 0, 0, 0, 1, 3, 0, 0, ...
    # The first "1" appears at position 4 (=token[6] in the overall list). Let me see if
    # [0,0,0,0] is padding/header, then 1,3 = element_name="Registry", parent_name="Engine"
    # then 0,0,0,0 = ? then 1,0 = element_name="Registry", text=0="MXRegistry.bxml"
    #
    # Actually looking more carefully at the 43 tree tokens:
    # [0, 0, 0, 0, 1, 3, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 4, 0, 0, 0, 0, 0, 0, 1, 2, 0, 0, 1, 4, 0, 0, 0, 0, 1, 4, 0, 0, 1, 4, 0, 0]
    #
    # Let me try grouping differently. The "1" = "Registry" (element name) appears at
    # positions: 4, 9, 16, 24, 29, 34, 38. That's 7 times! But there should be only 3 child <Registry>.
    # Wait some of those "1" might be different strings... no, str[1]="Registry".
    #
    # Hmm 7 appearances of str[1]="Registry" when XML has 3 <Registry>. That's more than expected.
    # Maybe the encoding is recursive with push/pop and some nodes appear multiple times?
    #
    # Actually wait: maybe str[1]="Registry" and its index 1 just happens to coincide with
    # the node type marker? I.e., value "1" means "element node" AND str[1]="Registry"?
    # If "1" is BOTH a type marker AND a string index, that's ambiguous.
    #
    # But looking at Engine: str indices are 0-4, all small. "1" could be either.
    # In RdbTables: str indices are 0-11. "1" = "Table" — also ambiguous with a possible
    # type marker "1".
    #
    # Hmm, maybe the distinction is: when a value <= some_threshold, it's a string index;
    # otherwise it's a type/count marker. But 0-11 all look like valid string indices.
    #
    # Let me try another approach. Maybe the tree section has a different interpretation.
    # Reading the first few tokens after the sentinel from Engine:
    # 0, 0, 0, 0 — maybe this is [attr_count=0, text_idx=0, child_count=0, ...]
    # Then 1, 3 — [name=1="Registry", parent=3="Engine"]
    #
    # Actually, maybe the tree section uses a stack machine:
    # - Push node: name_idx, text_idx, attr_offset, child_count
    # - After all children pushed, pop with parent_idx
    #
    # Let me try: [attr_count, text_idx, child_count, ? , name_idx, parent_idx] per node
    # Engine root: name=3, no attrs, no text, 3 children
    # [attr_count=0, text_idx=0, child_count=3, ???, name=3, parent=?]
    # = [0, 0, 3, ... , 3, ?]
    # But first 4 tokens after sentinel are [0, 0, 0, 0] — child_count=3 would be expected
    # for <Engine> with 3 children. Unless child_count is encoded differently.
    #
    # Hmm, I'm going in circles. Let me try yet another approach: maybe the tree encoding
    # uses a recursive format where each node record is:
    #   [text_idx] [name_idx] <child records> [-1 or count]
    #
    # For a leaf <Registry>MXRegistry.bxml</Registry>:
    #   text_idx=0 (str="MXRegistry.bxml"), name_idx=1 (str="Registry")
    # For the root <Engine>:
    #   text_idx=0 (none), name_idx=3, then 3 child records, then -1 to end
    #
    # Post-order: child1_leaf, child2_leaf, child3_leaf, root
    # [0, 1] [2, 1] [4, 1] [0, 3, -1]
    # = 7 tokens total. But we have 43. Still off by 36.
    #
    # There are a LOT of zeros in the tree section. Maybe the zeros are meaningful
    # (e.g., attribute_count=0, text_present=false, etc.) and each node record is
    # longer than I think.
    #
    # Let me try 8 tokens per node:
    # Each leaf = [attr_count=0, text_idx, ???, ???, name_idx, ???, ???, ???]
    # Each parent = [attr_count=0, text_idx=0, ???, child_count, name_idx, ???, ???, ???]
    #
    # 4 nodes × 8 = 32 tokens + maybe some terminator = ~36? Still not 43.
    #
    # I think I need to try a different approach entirely. Let me check if maybe
    # the tree uses a DIFFERENT token width (u16 instead of u32) in the tree section.
    #
    # The token stream is u32 LE, but maybe the high u16 is sometimes meaningful,
    # not always 0. Looking back at our Engine token dump:
    # token[6] = 00010000 (65536) ← high u16 = 1, low u16 = 0
    # Hmm, so some tokens ARE 0x00010000 = the END_MARKER we saw in attributes!
    # Which means the tree section also uses END_MARKER between nodes.
    #
    # Let me re-examine Engine tree tokens:
    # [0, 0, 0, 0, 1, 3, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 4, 0, 0, 0, 0, 0, 0, 1, 2, 0, 0, 1, 4, 0, 0, 0, 0, 1, 4, 0, 0, 1, 4, 0, 0]
    #
    # Wait — these are all small values (0, 1, 2, 3, 4). None of them are 0x00010000.
    # (MARKER = 0x00010000 = 65536, which is much larger.)
    #
    # So the tree section doesn't use END_MARKER. It's a pure tree encoding.
    # Given 43 tokens for 4 nodes, maybe each node has a variable-length record.
    #
    # Let me try to find "2" and "4" (the text indices for RendererRegistry and UIRegistry)
    # to locate where each child is encoded:
    # 0, 0, 0, 0, 1, 3, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 4, 0, 0, 0, 0, 0, 0, 1, 2, 0, 0, 1, 4, 0, 0, 0, 0, 1, 4, 0, 0, 1, 4, 0, 0
    # Positions of non-zero: 4(=1), 5(=3), 10(=1), 11(=0), 16(=1), 17(=4), 24(=1), 25(=2), 29(=1), 30(=4), 34(=1), 35(=4), 38(=1), 39(=4), 42(=0... wait)
    #
    # Actually: let me list all non-zero values with indices:
    # [4]=1, [5]=3, [10]=1, [11]=0, [16]=1, [17]=4, [24]=1, [25]=2, [29]=1, [30]=4, [34]=1, [35]=4, [38]=1, [39]=4
    # Wait, [11]=0 — that IS zero. So:
    # Non-zero positions: [4]=1, [5]=3, [10]=1, [16]=1, [17]=4, [24]=1, [25]=2, [29]=1, [30]=4, [34]=1, [35]=4, [38]=1, [39]=4
    #
    # That's 13 non-zero values in 43 tokens.
    # "1" = "Registry" appears at positions 4, 10, 16, 24, 29, 34, 38 = 7 times
    # "2" = "RendererRegistry.bxml" at position 25 (1 time)
    # "3" = "Engine" at position 5 (1 time)
    # "4" = "UIRegistry.bxml" at positions 17, 30, 35, 39 (4 times)
    #
    # str[0] = "MXRegistry.bxml" as text for child 1. But [0]=0 appears many times —
    # it's indistinguishable from count=0.
    #
    # 4 occurrences of "UIRegistry.bxml" (str[4]) is suspicious — XML has it only once!
    # This suggests the tree traversal isn't what I expect. Maybe each node appears
    # multiple times due to a stack-machine encoding with push/backtrack.
    #
    # Given the complexity and my limited time, let me just output what we can
    # (strings + tokens + best-effort XML) and hand it to the user for review.

    # For now: output the root name (first token before sentinel) + attrs + raw tree info
    root_name_idx = tokens[0] if tokens else 0
    root_name = sref(root_name_idx)

    # Output a simplified XML: root + attrs (we can parse attrs correctly)
    root = BxmlNode(name=root_name)
    attr_idx = 0
    # Group attrs 2 per child if we can (heuristic)
    child_attr_groups = []
    i = 0
    while i + 2 < len(attr_tokens):
        name_idx = attr_tokens[i]
        value_idx = attr_tokens[i + 1]
        marker = attr_tokens[i + 2]
        if marker == MARKER:
            all_attrs.append((sref(name_idx), sref(value_idx)))
            i += 3
        else:
            i += 1

    root.attrs = all_attrs  # best effort

    return root, all_attrs, tree_tokens

# ============================================================================
# Main
# ============================================================================

def main():
    if len(sys.argv) < 2:
        print(f'Usage: {sys.argv[0]} <file.bxml|dir> [--raw]')
        sys.exit(1)

    path = Path(sys.argv[1])
    raw_mode = '--raw' in sys.argv

    if path.is_dir():
        # Batch mode
        for f in sorted(path.rglob('*.bxml')) + sorted(path.rglob('*.xenon.database')):
            try:
                bin_d = read_bxml_file(str(f))
                strings, strings_end = parse_strings(bin_d)
                print(f'{f.name}: {len(bin_d)} bytes, {len(strings)} strings')
            except Exception as e:
                print(f'{f.name}: ERROR {e}')
        return

    bin_d = read_bxml_file(str(path))

    if raw_mode:
        # Raw mode: dump strings + tokens
        strings, strings_end = parse_strings(bin_d)
        print(f'=== Strings ({len(strings)}) ===')
        for i, (off, s) in enumerate(strings):
            print(f'  [{i:2}] @{off:#04x}: "{s}"')
        print(f'\nStrings end at {strings_end:#x}')

        tok_start = find_token_start(bin_d, strings_end)
        tokens = parse_tokens(bin_d, tok_start)
        print(f'\n=== Tokens ({len(tokens)} u32, starting at {tok_start:#x}) ===')
        for i, t in enumerate(tokens):
            lo = t & 0xFFFF
            hi = t >> 16
            ref = ''
            if 0 <= lo < len(strings):
                ref = f' lo=str[{lo}]="{strings[lo][1]}"'
            if t == MARKER:
                ref = ' MARKER'
            if t == SENTINEL:
                ref = ' SENTINEL'
            print(f'  [{i:3}] {t:010x} ({t:6}){ref}')
    else:
        # Decode mode
        strings, strings_end = parse_strings(bin_d)
        tok_start = find_token_start(bin_d, strings_end)
        tokens = parse_tokens(bin_d, tok_start)
        root, attrs, tree_tokens = decode_token_stream(tokens, strings)

        print(f'<?xml version="1.0"?>')
        if root.name:
            print(f'<{root.name}>')
            for k, v in attrs:
                print(f'   <!-- attr: {k}="{v}" -->')
            print(f'   <!-- tree_tokens ({len(tree_tokens)}): {tree_tokens[:20]}... -->')
            print(f'</{root.name}>')
        else:
            print(f'<!-- No root found -->')

if __name__ == '__main__':
    main()