"""Name the front-end class from the vtables that hold its methods.

Without `force_load` the game never requests a scene: the LoadStateMachine
parks in state 2 and the guest reads exactly one registry setting in a whole
run. Tracing the load-request chain through the recompiled sources runs out of
static call sites four levels up — these functions are reached only through
virtual dispatch:

  sub_82534980  load-request API
    <- sub_82352AE0            resolves the "Location" key, requests the load
      <- sub_82367A50 / sub_8236B660 / sub_824FB1F0 / sub_824FC9A0 / sub_8236B470
        <- sub_824E0288 / sub_8236C548 / sub_824CD280 / sub_824CFEC8 / sub_824FB1E0
             ^ four of these five have zero direct callers

So the addresses below are function pointers sitting in vtables. Finding the
tables names the class whose absence is the whole problem, and finding who
writes a table's address names the constructor.

Run headless against a COPY of the IDB:
  idat.exe -A -L<log> -S"tools/ida_dump_frontend_vtables.py" <copy>.i64
"""

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_kernwin
import ida_pro
import idautils
import idc

# Level 3 first — the ones with no static callers are the interesting ones.
TARGETS = [
    (0x824E0288, "calls sub_82367A50"),
    (0x8236C548, "calls sub_8236B660"),
    (0x824CD280, "calls sub_824FB1F0"),
    (0x824CFEC8, "calls sub_824FC9A0"),
    (0x824FB1E0, "calls sub_8236B660"),
    (0x8236B470, "level 2, no direct callers at all"),
]

IMAGE_LO = 0x82000000
IMAGE_HI = 0x83200000
MAX_VTABLE_SCAN = 256  # slots to walk in either direction


def emit(text):
    ida_kernwin.msg(str(text) + "\n")


def looks_like_code_pointer(ea):
    """A dword at `ea` that points into the image's code range."""
    if ea < IMAGE_LO or ea + 4 > IMAGE_HI:
        return False
    value = ida_bytes.get_dword(ea)
    return IMAGE_LO <= value < IMAGE_HI


def vtable_bounds(slot_ea):
    """Walk out from a slot until the run of code pointers stops."""
    start = slot_ea
    for _ in range(MAX_VTABLE_SCAN):
        prev = start - 4
        if not looks_like_code_pointer(prev):
            break
        start = prev
    end = slot_ea + 4
    for _ in range(MAX_VTABLE_SCAN):
        if not looks_like_code_pointer(end):
            break
        end += 4
    return start, end


def name_of(ea):
    name = idc.get_name(ea)
    if name:
        return name
    func = ida_funcs.get_func(ea)
    if func:
        return ida_funcs.get_func_name(func.start_ea)
    return "0x%08X" % ea


def report_vtable(base, end, highlight):
    emit("  vtable 0x%08X..0x%08X (%d slots)  name=%s" % (
        base, end, (end - base) // 4, idc.get_name(base) or "<unnamed>"))
    for i, ea in enumerate(range(base, end, 4)):
        value = ida_bytes.get_dword(ea)
        mark = "  <== " if value == highlight else "      "
        emit("    [%3d] 0x%08X %s%s" % (i, value, mark, name_of(value)))
    # Whoever references the table's address installs it: the constructor.
    emit("  --- references to the vtable base ---")
    found = False
    for xref in idautils.XrefsTo(base):
        func = ida_funcs.get_func(xref.frm)
        where = ida_funcs.get_func_name(func.start_ea) if func else "<no func>"
        emit("    0x%08X in %s  |  %s" % (
            xref.frm, where, idc.generate_disasm_line(xref.frm, 0)))
        found = True
    if not found:
        emit("    none — the address is probably built with lis/addi, "
             "so search for its halves instead")
        emit("    lis half = 0x%04X, addi half = 0x%04X (signed %d)" % (
            (base >> 16) & 0xFFFF, base & 0xFFFF,
            (base & 0xFFFF) - (0x10000 if (base & 0x8000) else 0)))


def investigate(ea, note):
    emit("")
    emit("===== 0x%08X — %s =====" % (ea, note))
    func = ida_funcs.get_func(ea)
    emit("function: %s" % (ida_funcs.get_func_name(ea) if func else "<none>"))

    slots = [x for x in idautils.DataRefsTo(ea)]
    if not slots:
        emit("no data references — not in any vtable IDA has typed as data")
        # Fall back to a raw scan: the pointer may sit in a region IDA never
        # converted to dwords, in which case DataRefsTo reports nothing.
        emit("scanning the image for the raw dword instead...")
        hits = 0
        cur = IMAGE_LO
        while cur < IMAGE_HI and hits < 16:
            found_at = ida_bytes.bin_search(
                cur, IMAGE_HI, ea.to_bytes(4, "big"), None,
                ida_bytes.BIN_SEARCH_FORWARD, ida_bytes.BIN_SEARCH_CASE)
            if found_at in (None, idc.BADADDR):
                break
            if isinstance(found_at, tuple):
                found_at = found_at[0]
            if found_at == idc.BADADDR:
                break
            emit("  raw hit at 0x%08X" % found_at)
            base, end = vtable_bounds(found_at)
            report_vtable(base, end, ea)
            hits += 1
            cur = found_at + 4
        if not hits:
            emit("  no raw hit either — reached only through a computed "
                 "pointer, or not dispatched at all")
        return

    for slot in slots:
        emit("  referenced from data at 0x%08X" % slot)
        base, end = vtable_bounds(slot)
        report_vtable(base, end, ea)


def main():
    ida_auto.auto_wait()
    ida_hexrays.init_hexrays_plugin()
    for ea, note in TARGETS:
        investigate(ea, note)


main()
ida_pro.qexit(0)
