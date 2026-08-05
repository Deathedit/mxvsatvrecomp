"""Find who writes vertex shader constant 255 on the D3D9 device.

Measured: the compositor vertex shader reads c255 twice and gets (0,0,0,0)
both times, so its UV export is zero and the fullscreen triangle samples one
texel. The HLE constant file is read from `device + 0x780` (16 bytes per vec4),
so c255 is `device + 0x780 + 255*16` = `device + 0x1770` = device + 6000.

Two things to establish from the code, not from reasoning:
  1. that 0x780 + index*16 is really the constant file layout, by reading
     D3DDevice_SetVertexShaderConstantF's own arithmetic;
  2. which function writes slot 255, since the game plainly never sets it
     through the public setter (the file is zero there at draw time).

Run headless against a COPY of the IDB:
  idat.exe -A -L<log> -S"tools/ida_dump_vs_const255.py" <copy>.i64
"""

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_kernwin
import ida_pro
import idautils
import idc

# The public setter, plus the state-setting entry points that could plausibly
# publish a viewport- or target-derived constant behind the game's back.
DECOMPILE = [
    (0x82550320, "D3DDevice_SetVertexShaderConstantF"),
    (0x8254BF50, "D3DDevice_SetViewport"),
    (0x8255CE98, "D3DDevice_Resolve"),
    (0x8254C060, "D3DDevice_SetRenderTarget"),
]

# device + 0x1770 is c255 under the assumed layout. Scan every instruction in
# the image for this displacement so the answer does not depend on guessing
# which function is responsible.
C255_OFFSET = 0x1770
CONST_FILE_BASE = 0x780


def emit(text):
    ida_kernwin.msg(str(text) + "\n")


def decompile(ea, label):
    emit("")
    emit("===== 0x%08X %s =====" % (ea, label))
    func = ida_funcs.get_func(ea)
    if func is None:
        # Auto-analysis leaves some matched entry points undefined (AGENTS.md
        # records this for the two constant setters). The bytes are there; only
        # the function object is missing, so make one.
        emit("no function object — creating one at 0x%08X" % ea)
        ida_funcs.add_func(ea)
        func = ida_funcs.get_func(ea)
        if func is None:
            # Still readable as instructions, which is all that is needed to
            # see how a constant index becomes a device offset.
            emit("could not define a function; raw disassembly instead")
            for off in range(0, 0x120, 4):
                emit("  0x%08X  %s" % (ea + off,
                                       idc.generate_disasm_line(ea + off, 0)))
            return
    try:
        emit(str(ida_hexrays.decompile(func.start_ea)))
    except Exception as exc:
        emit("decompile failed: %s" % exc)


def scan_for_offset(value, label):
    """Every instruction anywhere in the image carrying `value` as an operand."""
    emit("")
    emit("===== instructions referencing 0x%X (%d) — %s =====" %
         (value, value, label))
    hits = 0
    for func_ea in idautils.Functions():
        func = ida_funcs.get_func(func_ea)
        if func is None:
            continue
        for head in idautils.Heads(func.start_ea, func.end_ea):
            for op in range(3):
                if idc.get_operand_value(head, op) == value:
                    emit("  0x%08X  in %s  |  %s" % (
                        head, ida_funcs.get_func_name(func_ea),
                        idc.generate_disasm_line(head, 0)))
                    hits += 1
                    break
    emit("--- %d hit(s) ---" % hits)


def main():
    ida_auto.auto_wait()
    ida_hexrays.init_hexrays_plugin()
    for ea, label in DECOMPILE:
        decompile(ea, label)
    scan_for_offset(C255_OFFSET, "device + c255 under the assumed layout")
    scan_for_offset(CONST_FILE_BASE, "device + constant file base")


main()
ida_pro.qexit(0)
