"""Dump the game's script binding table.

The front end turned out not to be C++ virtual dispatch at all. The functions
that lead to the load-request API are entries in a name -> function table at
0x8203F2E0, alternating `const char*` and code pointer:

    [132] "StartWorldLoad"   [133] sub_824CD280
    [190] "AdvanceNetwork.."  [191] sub_824CFEC8

and the surrounding names are a scripting API — ExecuteScript, LoadAssetDB,
Get/SetRegistry*, GetUIState, RegisterUIEvent, SendUIEvent, LoadUIAssetPackage,
PlayUISound. So the menu is script-driven, and nothing calls StartWorldLoad
because no script is running.

This dumps the table properly (string text, not just the label), so the API
surface is on record, and decompiles the script-execution binding so the next
question — what feeds it — has an address to start from.

Run headless against a COPY of the IDB:
  idat.exe -A -L<log> -S"tools/ida_dump_script_api.py" <copy>.i64
"""

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_kernwin
import ida_pro
import idautils
import idc

TABLE = 0x8203F2E0
MAX_PAIRS = 400

IMAGE_LO = 0x82000000
IMAGE_HI = 0x83200000


def emit(text):
    ida_kernwin.msg(str(text) + "\n")


def read_c_string(ea, limit=96):
    if ea < IMAGE_LO or ea >= IMAGE_HI:
        return None
    out = []
    for i in range(limit):
        b = ida_bytes.get_byte(ea + i)
        if b == 0:
            break
        if b < 0x20 or b > 0x7E:
            return None
        out.append(chr(b))
    return "".join(out) if out else None


def in_code(ea):
    return ida_funcs.get_func(ea) is not None


def dump_table():
    """Walk the pairs until the (string, function) shape stops holding."""
    emit("===== script binding table at 0x%08X =====" % TABLE)
    rows = []
    for i in range(MAX_PAIRS):
        name_ptr = ida_bytes.get_dword(TABLE + i * 8)
        func_ptr = ida_bytes.get_dword(TABLE + i * 8 + 4)
        name = read_c_string(name_ptr)
        if name is None or not in_code(func_ptr):
            emit("table ends after %d entries "
                 "(name_ptr=0x%08X func_ptr=0x%08X)" % (i, name_ptr, func_ptr))
            break
        rows.append((i, name, func_ptr))
        emit("  [%3d] %-40s 0x%08X %s" % (
            i, name, func_ptr, ida_funcs.get_func_name(func_ptr)))
    return rows


def decompile(ea, label):
    emit("")
    emit("===== 0x%08X %s =====" % (ea, label))
    func = ida_funcs.get_func(ea)
    if func is None:
        emit("no function object")
        return
    try:
        emit(str(ida_hexrays.decompile(func.start_ea)))
    except Exception as exc:
        emit("decompile failed: %s" % exc)


def who_references(ea, label):
    emit("")
    emit("===== references to 0x%08X — %s =====" % (ea, label))
    hits = 0
    for xref in idautils.XrefsTo(ea):
        func = ida_funcs.get_func(xref.frm)
        where = ida_funcs.get_func_name(func.start_ea) if func else "<no func>"
        emit("  0x%08X in %s  |  %s" % (
            xref.frm, where, idc.generate_disasm_line(xref.frm, 0)))
        hits += 1
    emit("--- %d reference(s) ---" % hits)


def main():
    ida_auto.auto_wait()
    ida_hexrays.init_hexrays_plugin()
    rows = dump_table()

    # Whoever registers this table owns the script environment.
    who_references(TABLE, "the binding table itself")

    # The bindings that matter for "why is there no menu": the one that runs a
    # script at all, and the one that would start the world load.
    wanted = ("executescript", "startworldload", "loaduiassetpackage",
              "loaduiassetdata", "senduievent", "registeruievent")
    for index, name, func_ptr in rows:
        if name.lower().replace(" ", "") in wanted:
            decompile(func_ptr, "binding [%d] \"%s\"" % (index, name))


main()
ida_pro.qexit(0)
