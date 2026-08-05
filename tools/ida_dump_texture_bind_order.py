import ida_auto
import ida_funcs
import ida_hexrays
import ida_kernwin
import ida_name
import ida_pro
import idautils


def emit(text):
    ida_kernwin.msg(str(text) + "\n")


def find_named(suffix):
    matches = []
    suffix = suffix.lower()
    for ea in idautils.Functions():
        name = ida_funcs.get_func_name(ea)
        if name.lower().endswith(suffix):
            matches.append(ea)
    return matches


def dump_callers(target):
    name = ida_funcs.get_func_name(target)
    emit("\n===== TARGET 0x%08X %s =====" % (target, name))
    callers = {}
    for xref in idautils.XrefsTo(target):
        func = ida_funcs.get_func(xref.frm)
        if func is not None:
            callers[func.start_ea] = callers.get(func.start_ea, []) + [xref.frm]
    emit("direct caller count: %d" % len(callers))
    for caller, sites in sorted(callers.items()):
        emit("\n----- CALLER 0x%08X %s sites %s -----" %
             (caller, ida_funcs.get_func_name(caller),
              ", ".join("0x%08X" % site for site in sites)))
        try:
            emit(ida_hexrays.decompile(caller))
        except Exception as exc:
            emit("Hex-Rays failed: %s" % exc)


ida_auto.auto_wait()
targets = []
for ea in idautils.Functions():
    lowered = ida_funcs.get_func_name(ea).lower()
    if "settexture" in lowered or lowered.endswith("resolve") or \
            "d3ddevice_resolve" in lowered:
        targets.append(ea)

# This database may retain only automatic sub_ names. Validate the two entry
# points already established from the import table, rather than assuming an
# address denotes code, and use them only when semantic names were unavailable.
if not targets:
    for ea in (0x8254E748, 0x8255CE98):
        func = ida_funcs.get_func(ea)
        if func is not None and func.start_ea == ea:
            targets.append(ea)

emit("===== D3D9 TEXTURE BIND / RESOLVE CALL ORDER =====")
if not targets:
    emit("No named SetTexture/Resolve implementation functions found")
for target in sorted(set(targets)):
    dump_callers(target)

ida_pro.qexit(0)
