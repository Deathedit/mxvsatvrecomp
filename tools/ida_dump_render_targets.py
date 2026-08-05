import ida_auto
import ida_funcs
import ida_hexrays
import ida_kernwin
import ida_lines
import ida_pro
import idautils


def emit(text):
    ida_kernwin.msg(str(text) + "\n")


def dump_function(ea):
    emit("\n===== FUNCTION 0x%08X %s =====" %
         (ea, ida_funcs.get_func_name(ea)))
    try:
        emit("----- HEX-RAYS -----")
        emit(ida_hexrays.decompile(ea))
    except Exception as exc:
        emit("Hex-Rays failed: %s" % exc)
    emit("----- DISASSEMBLY -----")
    for head in idautils.FuncItems(ea):
        line = ida_lines.generate_disasm_line(head, 0)
        emit("%08X  %s" % (head, ida_lines.tag_remove(line or "")))


ida_auto.auto_wait()
needles = (
    "rendertarget", "render_target", "backbuffer", "back_buffer",
    "depthstencil", "depth_stencil", "surface", "present",
)
matches = []
all_d3d = []
for ea in idautils.Functions():
    name = ida_funcs.get_func_name(ea)
    if "d3d" in name.lower():
        all_d3d.append((ea, name))
    if any(needle in name.lower() for needle in needles):
        matches.append((ea, name))

emit("===== RENDER TARGET / SURFACE FUNCTION NAMES =====")
for ea, name in matches:
    emit("0x%08X %s" % (ea, name))

emit("\n===== ALL D3D FUNCTION NAMES =====")
for ea, name in all_d3d:
    emit("0x%08X %s" % (ea, name))

for ea, name in matches:
    lowered = name.lower()
    if ("setrendertarget" in lowered or "getrendertarget" in lowered or
            "getbackbuffer" in lowered or "createsurface" in lowered or
            "setdepthstencil" in lowered or "present" in lowered):
        dump_function(ea)

for ea in (0x8254BCE8, 0x8254BFD0, 0x8255CE98, 0x8255BD48):
    dump_function(ea)

ida_pro.qexit(0)
