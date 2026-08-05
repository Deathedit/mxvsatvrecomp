import ida_auto
import ida_funcs
import ida_hexrays
import ida_kernwin
import ida_pro
import idautils


TARGET = 0x82564C50


def main():
    ida_auto.auto_wait()
    ida_kernwin.msg("=== PatchVertexShader 0x%X ===\n" % TARGET)
    func = ida_funcs.get_func(TARGET)
    if not func:
        ida_kernwin.msg("function not found\n")
        return
    try:
        cfunc = ida_hexrays.decompile(TARGET)
        ida_kernwin.msg(str(cfunc) + "\n")
        ida_kernwin.msg("--- lvars ---\n")
        for lvar in cfunc.lvars:
            ida_kernwin.msg("%s : %s\n" % (lvar.name, lvar.type()))
    except Exception as exc:
        ida_kernwin.msg("decompile failed: %s\n" % exc)
    ida_kernwin.msg("--- callers ---\n")
    for xref in idautils.XrefsTo(TARGET):
        caller = ida_funcs.get_func(xref.frm)
        if caller:
            ida_kernwin.msg("0x%X %s call@0x%X\n" % (
                caller.start_ea, ida_funcs.get_func_name(caller.start_ea),
                xref.frm))
            try:
                ida_kernwin.msg(str(ida_hexrays.decompile(caller.start_ea)) +
                                   "\n")
            except Exception as exc:
                ida_kernwin.msg("caller decompile failed: %s\n" % exc)


main()
ida_pro.qexit(0)
