"""Who emits vertex shader constant 255, if not the device shadow?

Measured at draw time: the VS constant file at `device + 0x780` holds 70 live
vec4 spanning indices 0..218, and c255 is all zero — while the compositor
vertex shader's only ALU instruction is `MAD export0 = r0 * c255 + c255`. The
file is populated, so the read is right; the guest simply never publishes 255
through `SetVertexShaderConstantF`.

`D3DDevice_DrawVertices` (0x825561B0) has two constant-ish emitters:
  sub_82564B00(device, dirty, 0x4000, device + 1920)  — the VS float shadow
  sub_825649A8(device, 0)                             — gated on a separate
                                                        dirty word, contents
                                                        unknown
The second is the candidate: a runtime-computed constant (viewport scale/bias
for a fullscreen quad is the obvious shape) written straight into the ring,
never into the shadow we snapshot.

Run headless against a COPY of the IDB:
  idat.exe -A -L<log> -S"tools/ida_dump_const_emitters.py" <copy>.i64
"""

import ida_auto
import ida_funcs
import ida_hexrays
import ida_kernwin
import ida_pro
import idautils
import idc

TARGETS = [
    (0x825649A8, "emitter gated on the second dirty word (from DrawVertices)"),
    (0x82564B00, "VS/PS float constant shadow flush"),
    (0x82564768, "context register flush (used for SQ_PROGRAM_CNTL)"),
]

# 255 as a constant index, and the LOAD_ALU_CONSTANT register address it would
# use: ALU constants start at register 0x4000, four dwords per slot, so slot 255
# is 0x4000 + 255*4 = 0x43FC. The PM4 log already shows LOAD_ALU_CONSTANT
# writes at 0x43F0 and 0x47F0 — the last four slots of each half — so this
# neighbourhood is where the answer should be.
SCAN = [(0x43FC, "ALU const register for VS slot 255"),
        (0x43F0, "ALU const register seen in the PM4 log"),
        (0x47F0, "ALU const register seen in the PM4 log (PS half)")]


def emit(text):
    ida_kernwin.msg(str(text) + "\n")


def decompile(ea, label):
    emit("")
    emit("===== 0x%08X %s =====" % (ea, label))
    func = ida_funcs.get_func(ea)
    if func is None:
        emit("no function object — raw disassembly")
        for off in range(0, 0x180, 4):
            emit("  0x%08X  %s" % (ea + off,
                                   idc.generate_disasm_line(ea + off, 0)))
        return
    try:
        emit(str(ida_hexrays.decompile(func.start_ea)))
    except Exception as exc:
        emit("decompile failed: %s" % exc)


def scan_for_value(value, label):
    emit("")
    emit("===== instructions with operand 0x%X (%d) — %s =====" %
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
    for ea, label in TARGETS:
        decompile(ea, label)
    for value, label in SCAN:
        scan_for_value(value, label)


main()
ida_pro.qexit(0)
