import ida_auto
import ida_funcs
import ida_hexrays
import ida_kernwin
import ida_lines
import ida_pro
import idautils


def emit(text):
    ida_kernwin.msg(str(text) + "\n")


ida_auto.auto_wait()
for ea in (0x825506B0, 0x825506E8, 0x8254E748, 0x825508A8):
    func = ida_funcs.get_func(ea)
    emit("\n===== FUNCTION 0x%08X %s =====" %
         (ea, ida_funcs.get_func_name(ea)))
    if func is None:
        emit("not a function")
        continue
    try:
        cfunc = ida_hexrays.decompile(ea)
        emit("----- HEX-RAYS -----")
        emit(cfunc)
    except Exception as exc:
        emit("Hex-Rays failed: %s" % exc)
    emit("----- DISASSEMBLY -----")
    for head in idautils.FuncItems(ea):
        line = ida_lines.generate_disasm_line(head, 0)
        emit("%08X  %s" % (head, ida_lines.tag_remove(line or "")))

emit("\n===== PIXEL SHADER FUNCTIONS =====")
for ea in idautils.Functions():
    name = ida_funcs.get_func_name(ea)
    if "pixelshader" not in name.lower():
        continue
    emit("0x%08X %s" % (ea, name))
    if "createpixelshader" not in name.lower():
        continue
    try:
        emit("----- HEX-RAYS -----")
        emit(ida_hexrays.decompile(ea))
    except Exception as exc:
        emit("Hex-Rays failed: %s" % exc)
    emit("----- DISASSEMBLY -----")
    for head in idautils.FuncItems(ea):
        line = ida_lines.generate_disasm_line(head, 0)
        emit("%08X  %s" % (head, ida_lines.tag_remove(line or "")))

ida_pro.qexit(0)
