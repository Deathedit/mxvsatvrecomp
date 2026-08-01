"""Decode .xenon.package files using bxml_full_decoder.

Each heap in a package = [33-byte heap header] + [standard BXML block].

The .xenon.database sibling file declares the heap layout (offset+size per Heap node).
Without the database, we scan for BXML blocks directly.
"""
import struct, zlib, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bxml_full_decoder import decode_bxml

def attr_by_name(node, name):
    for k, v in node.attrs:
        if k == name:
            return v
    return None

def find_pkgs_in_db(pkg_db_path):
    """Returns a list of {'file': str, 'heaps': [(off, size)]} dicts parsed from .xenon.database."""
    root = decode_bxml(pkg_db_path)
    pkgs = []
    for node in root.walk():
        if node.name == 'Package':
            file_attr = attr_by_name(node, 'file') or attr_by_name(node, 'name')
            heaps = []
            # Walk ONLY this node's descendants for Heap elements
            for child in node.walk():
                if child.name == 'Heap':
                    off_s = attr_by_name(child, 'offset')
                    sz_s = attr_by_name(child, 'size')
                    block_off_s = attr_by_name(child, 'blockOffset')
                    heap_off_s = attr_by_name(child, 'heapOffset')
                    if off_s is not None and sz_s is not None:
                        try:
                            off = int(off_s)
                            sz = int(sz_s)
                            heaps.append({'offset': off, 'size': sz,
                                          'blockOffset': int(block_off_s) if block_off_s else 0,
                                          'heapOffset': int(heap_off_s) if heap_off_s else 0})
                        except ValueError:
                            pass
            pkgs.append({'file': file_attr or '(unnamed)', 'heaps': heaps})
    return pkgs

def decode_heap(heap_data, label, out_lines, verbose=False):
    out_lines.append(f"\n=== Heap '{label}' ({len(heap_data)} bytes) ===")
    if len(heap_data) < 33:
        out_lines.append("  too small")
        return False

    hdr = heap_data[:33]
    bxml_idx = heap_data.find(b'BXML')
    if bxml_idx < 0:
        out_lines.append("  NO BXML signature found")
        return False

    bxml_data = heap_data[bxml_idx:]

    import tempfile
    tmp = os.path.join(tempfile.gettempdir(), 'pkg_heap.bxml')
    with open(tmp, 'wb') as f:
        f.write(bxml_data)

    try:
        root = decode_bxml(tmp)
        xml_lines = root.to_xml()
        out_lines.append(f"  BXML@{bxml_idx}: decoded {len(xml_lines)} lines")
        # Output first 15 + summary
        for line in xml_lines[:15]:
            out_lines.append(f"    {line}")
        if len(xml_lines) > 15:
            out_lines.append(f"    ... ({len(xml_lines)-15} more lines)")
            # Count distinct element names
            counts = {}
            for node in root.walk():
                counts[node.name] = counts.get(node.name, 0) + 1
            top = sorted(counts.items(), key=lambda x: -x[1])[:15]
            out_lines.append(f"  Element-type histogram: " + ", ".join(f"{n}:{c}" for n, c in top))
        return True
    except Exception as e:
        out_lines.append(f"  DECODE FAILED at BXML@{bxml_idx}: {e}")
        return False

def direct_scan_heaps(pkg_path):
    """Scan for BXML-magic blocks. Returns list of (heap_start, bxml_idx, comp_size)."""
    with open(pkg_path, 'rb') as f: data = f.read()
    heaps = []
    pos = 0
    while True:
        idx = data.find(b'BXML', pos)
        if idx < 0: break
        heap_start = idx - 33
        if heap_start < 0:
            pos = idx + 4
            continue
        bxml_hdr = data[idx:idx+36]
        if len(bxml_hdr) < 36:
            break
        comp_size = struct.unpack('<I', bxml_hdr[32:36])[0]
        heaps.append({'heap_start': heap_start, 'bxml_idx': idx, 'comp_size': comp_size})
        pos = idx + 36 + comp_size
        if pos >= len(data):
            break
    return heaps

def main():
    if len(sys.argv) < 2:
        pkg_path = r'C:\Users\VM\Desktop\mx\assets\Database\NAT_Farm.xenon.package'
    else:
        pkg_path = sys.argv[1]
    stem = os.path.basename(pkg_path).replace('.xenon.package', '').replace('.package', '')
    pkg_db = os.path.join(os.path.dirname(pkg_path), stem + '.xenon.database')
    print(f"Package: {os.path.basename(pkg_path)} ({os.path.getsize(pkg_path)} bytes)")
    if os.path.exists(pkg_db):
        print(f"Database: {os.path.basename(pkg_db)}")

    # Get heap layout from database
    pkgs = []
    if os.path.exists(pkg_db):
        try:
            pkgs = find_pkgs_in_db(pkg_db)
            heap_count = sum(len(p['heaps']) for p in pkgs)
            print(f"DB declares {len(pkgs)} packages, {heap_count} heaps total")
        except Exception as e:
            print(f"DB layout extract failed: {e}")
            import traceback
            traceback.print_exc()

    # For ALL packages reference the SAME physical .package file (multi-Package DB), OR
    # this .package file matches exactly one Package entry. Try both.
    target_pkg = None
    if pkgs:
        # Prefer one whose 'file' attribute matches the package stem
        for p in pkgs:
            if p['file'].lower() == stem.lower():
                target_pkg = p
                break
        if target_pkg is None:
            target_pkg = pkgs[0]
            print(f"No exact pkg match; using first entry '{target_pkg['file']}'")

    out_lines = [f"# {os.path.basename(pkg_path)}"]
    out_lines.append(f"Size: {os.path.getsize(pkg_path)} bytes")

    with open(pkg_path, 'rb') as f: pkg = f.read()

    if target_pkg and target_pkg['heaps']:
        out_lines.append(f"\nPackage '{target_pkg['file']}' has {len(target_pkg['heaps'])} heaps from DB")
        for i, h in enumerate(target_pkg['heaps']):
            off, sz = h['offset'], h['size']
            if off + sz > len(pkg):
                sz = len(pkg) - off
            out_lines.append(f"\n--- Heap {i+1}/{len(target_pkg['heaps'])} (off={off}, size={sz}, blockOffset={h['blockOffset']}, heapOffset={h['heapOffset']}) ---")
            decode_heap(pkg[off:off+sz], f"Heap{i+1}", out_lines)
    else:
        # Direct-scan fallback
        heaps = direct_scan_heaps(pkg_path)
        out_lines.append(f"\nDirect-scan found {len(heaps)} BXML blocks")
        for i, h in enumerate(heaps):
            # Read comp_size bytes after BXML header end as heap end
            end = h['bxml_idx'] + 36 + h['comp_size']
            if end > len(pkg): end = len(pkg)
            heap_start = h['heap_start']
            decode_heap(pkg[heap_start:end], f"ScanHeap{i+1}@{heap_start}", out_lines)

    # Write manifest to file
    out_dir = r'C:\Users\VM\Desktop\mx\out'
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, stem + '_package_manifest.txt')
    with open(out_path, 'w', encoding='utf-8') as fp:
        fp.write('\n'.join(out_lines))
    print(f"\nWrote manifest ({len(out_lines)} lines) to {out_path}")

    # Print a summary of the last 40 lines on screen
    print('\n'.join(out_lines[:70]))

if __name__ == '__main__':
    main()