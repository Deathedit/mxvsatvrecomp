"""How does a script binding read its arguments?

`ExecuteScriptAsset` (binding [10], sub_824AF838) fires exactly twice at boot
and then never again, and the script VM's dispatcher (sub_82AA7638) goes silent
1.6 seconds in. Naming those two assets says what the script layer managed to
do before it stalled.

The binding is called by the VM with the VM context in r3, so the asset name is
not in a register — it is pulled off the VM's argument stack. This dumps the
binding bodies so the accessor and its offsets can be read rather than assumed,
and dumps the accessor itself once it is identified by name.

Run headless against a COPY of the IDB:
  idat.exe -A -L<log> -S"tools/ida_dump_execute_script.py" <copy>.i64
"""

import ida_auto
import ida_funcs
import ida_hexrays
import ida_kernwin
import ida_pro
import idautils
import idc

TARGETS = [
    (0x824AF838, "ExecuteScriptAsset — binding [10], the one that fires"),
    (0x824AF3C0, "LoadAssetDB — binding [6], for the argument-reading pattern"),
    (0x824CD280, "StartWorldLoad — binding [66], the endpoint that never fires"),
    (0x82AA7638, "the VM native-call dispatcher that calls them"),
]


def emit(text):
    ida_kernwin.msg(str(text) + "\n")


def decompile(ea, label):
    emit("")
    emit("===== 0x%08X %s =====" % (ea, label))
    func = ida_funcs.get_func(ea)
    if func is None:
        emit("no function object — raw disassembly")
        for off in range(0, 0x140, 4):
            emit("  0x%08X  %s" % (ea + off,
                                   idc.generate_disasm_line(ea + off, 0)))
        return
    emit("range 0x%08X..0x%08X (%d bytes)" % (
        func.start_ea, func.end_ea, func.end_ea - func.start_ea))
    try:
        emit(str(ida_hexrays.decompile(func.start_ea)))
    except Exception as exc:
        emit("decompile failed: %s" % exc)
        return
    # The callees are where the argument accessors live.
    emit("--- callees ---")
    seen = set()
    for head in idautils.Heads(func.start_ea, func.end_ea):
        for xref in idautils.XrefsFrom(head):
            target = xref.to
            callee = ida_funcs.get_func(target)
            if callee is None or callee.start_ea == func.start_ea:
                continue
            if callee.start_ea in seen:
                continue
            seen.add(callee.start_ea)
            emit("  0x%08X %s  (%d bytes)" % (
                callee.start_ea, ida_funcs.get_func_name(callee.start_ea),
                callee.end_ea - callee.start_ea))


def main():
    ida_auto.auto_wait()
    ida_hexrays.init_hexrays_plugin()
    for ea, label in TARGETS:
        decompile(ea, label)


main()
ida_pro.qexit(0)
