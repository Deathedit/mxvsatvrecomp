"""Find PowerPC vtable thunks that IDA never turned into functions.

Why this exists
---------------
A static recompiler emits code for the functions IDA knows about. Xbox 360
binaries are full of compiler-generated vtable thunks that IDA leaves as bare
`loc_` labels, because nothing ever *calls* them directly — they are reached
only through a vtable slot at runtime. The recompiler therefore skips them, and
the first time the guest dispatches through one the process dies with

    [FATAL] Call to invalid or unregistered function at guest address 0x...

They come in contiguous tables, one thunk per vtable slot, so finding them one
crash at a time is a long and avoidable grind. This finds the whole table.

What it matches
---------------
Two shapes, both standard MSVC/Xenon output:

  vtable tail-call (0x10 bytes)      adjustor thunk (0x8 bytes)
    lwz  rT, 0(r3)                     addi r3, r3, -N
    lwz  rU, SLOT(rT)                  b    target
    mtctr rU
    bctr

Usage
-----
In the IDA GUI:  File > Script file..., or  Alt+F7.
Headless:        idat64 -A -S"find_vtable_thunks.py" target.i64

Set CONFIG_TOML below (or the FIND_THUNKS_TOML environment variable) to an
existing recompiler config and already-listed addresses are reported but not
re-emitted, so the output can be pasted straight in.

Output goes to the IDA console and, if OUT_PATH is set, to that file.
"""

import os

import ida_bytes
import ida_funcs
import ida_kernwin
import ida_segment
import ida_ua
import idautils

# --- configuration ----------------------------------------------------------

# Existing recompiler config, to suppress addresses already listed.
#
# Left empty, an interactive IDA session ASKS for it with a file picker and the
# report prints the absolute path it settled on. Comparing against the wrong
# config is the one failure that looks exactly like success — it prints a tidy
# list of "missing" entries that are already there, or worse, stays quiet about
# entries that are not — so the path is chosen deliberately and always shown,
# never guessed from the working directory.
#
# Set FIND_THUNKS_TOML to skip the prompt (headless runs need this).
CONFIG_TOML = os.environ.get("FIND_THUNKS_TOML", "")

# Where to write the emitted TOML. Empty = console only.
OUT_PATH = os.environ.get("FIND_THUNKS_OUT", "")

# Report thunks that are already inside a defined function. They need no config
# entry; seeing them is how you confirm the scan found the whole table.
SHOW_ALREADY_DEFINED = False

# --- matching ---------------------------------------------------------------

# lwz rT, 0(r3) = 100000 rT rA=3 offset ; primary opcode 32.
_LWZ = 32
_ADDI = 14


def _insn(ea):
    """Decode one instruction, or None."""
    i = ida_ua.insn_t()
    return i if ida_ua.decode_insn(i, ea) > 0 else None


def _is_lwz(ea):
    """(dest_reg, base_reg, displacement) for a lwz, else None.

    Read from the raw word rather than trusting mnemonic text, so the script is
    not hostage to processor-module naming across IDA versions.
    """
    w = ida_bytes.get_dword(ea)
    if w is None:
        return None
    # Big-endian image; get_dword already respects the loader's endianness for
    # PPC, but the fields are easier to read from the byte order in the file.
    if (w >> 26) != _LWZ:
        return None
    rd = (w >> 21) & 0x1F
    ra = (w >> 16) & 0x1F
    disp = w & 0xFFFF
    if disp >= 0x8000:
        disp -= 0x10000
    return rd, ra, disp


def match_vtable_thunk(ea):
    """0x10 if `ea` starts a vtable tail-call thunk, else 0."""
    first = _is_lwz(ea)
    if not first:
        return 0
    rt, ra, disp = first
    # Load the vtable pointer from the object in r3, at offset 0.
    if ra != 3 or disp != 0:
        return 0
    second = _is_lwz(ea + 4)
    if not second:
        return 0
    ru, rbase, slot = second
    # Load a slot out of the vtable we just loaded.
    if rbase != rt or slot < 0:
        return 0
    # mtctr ru ; bctr
    if ida_bytes.get_dword(ea + 8) != (0x7C0903A6 | (ru << 21)):
        return 0
    if ida_bytes.get_dword(ea + 12) != 0x4E800420:
        return 0
    return 0x10


def match_adjustor_thunk(ea):
    """0x8 if `ea` starts an adjustor thunk (`addi r3,r3,-N; b target`)."""
    w = ida_bytes.get_dword(ea)
    if w is None or (w >> 26) != _ADDI:
        return 0
    rd = (w >> 21) & 0x1F
    ra = (w >> 16) & 0x1F
    if rd != 3 or ra != 3:
        return 0
    nxt = ida_bytes.get_dword(ea + 4)
    # Unconditional branch, not a call (LK clear), not absolute (AA clear).
    if nxt is None or (nxt >> 26) != 18 or (nxt & 3) != 0:
        return 0
    return 0x8


MATCHERS = (
    ("vtable", match_vtable_thunk),
    ("adjustor", match_adjustor_thunk),
)

# --- scan -------------------------------------------------------------------


def resolve_config():
    """Decide which config to compare against, and say so out loud.

    Returns (path, note) where note explains the choice for the report. An
    empty path means "compare against nothing" — a legitimate answer, but one
    the report has to state plainly, because every thunk then looks missing.
    """
    if CONFIG_TOML:
        path = os.path.abspath(CONFIG_TOML)
        if not os.path.exists(path):
            return "", "set to %s, but that file does not exist" % path
        return path, "from FIND_THUNKS_TOML"

    if not ida_kernwin.is_idaq():
        return "", "not set (headless; set FIND_THUNKS_TOML)"

    picked = ida_kernwin.ask_file(
        False, "*.toml", "Recompiler config to compare against (Cancel = none)"
    )
    if not picked:
        return "", "none chosen — every thunk below is reported as missing"
    path = os.path.abspath(picked)
    if not os.path.exists(path):
        return "", "chosen file does not exist: %s" % path
    return path, "chosen in the file picker"


def load_existing(path):
    """Addresses already present in a recompiler config."""
    known = set()
    if not path or not os.path.exists(path):
        return known
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line.startswith("0x") or "=" not in line:
                continue
            try:
                known.add(int(line.split("=", 1)[0].strip(), 16))
            except ValueError:
                pass
    return known


def scan(known):
    found = []  # (ea, size, kind, already_defined, already_listed)

    for seg_ea in idautils.Segments():
        seg = ida_segment.getseg(seg_ea)
        if not seg or seg.perm & ida_segment.SEGPERM_EXEC == 0:
            continue
        ea, end = seg.start_ea, seg.end_ea
        while ea < end:
            size = 0
            kind = ""
            for name, matcher in MATCHERS:
                size = matcher(ea)
                if size:
                    kind = name
                    break
            if not size:
                ea += 4
                continue
            defined = ida_funcs.get_func(ea) is not None
            found.append((ea, size, kind, defined, ea in known))
            ea += size
    return found


def report(found, config_path, config_note, known_count):
    missing = [f for f in found if not f[3] and not f[4]]
    listed = [f for f in found if f[4]]
    defined = [f for f in found if f[3] and not f[4]]

    print("")
    print("=== vtable / adjustor thunk scan ===")
    print("  config: %s" % (config_path or "<none>"))
    print("          (%s)" % config_note)
    print("          %d addresses parsed from it" % known_count)
    if config_path and known_count == 0:
        print("          ^^ WARNING: parsed nothing. Wrong file, or the")
        print("             entries are not `0x... = ...` lines?")
    print("")
    print("  %5d matched" % len(found))
    print("  %5d already a defined function (no entry needed)" % len(defined))
    print("  %5d already in the config above" % len(listed))
    print("  %5d MISSING — recompiler will not emit these" % len(missing))

    if SHOW_ALREADY_DEFINED and defined:
        print("")
        print("--- already defined ---")
        for ea, size, kind, _, _ in defined:
            print("  0x%08X  %s  size 0x%X" % (ea, kind, size))

    if not missing:
        print("")
        print("Nothing missing.")
        return ""

    # Contiguous runs are the interesting unit: a table found whole is a table
    # that will not come back one crash at a time.
    lines = []
    print("")
    print("--- MISSING, as TOML ---")
    run_start = None
    prev_end = None
    for ea, size, kind, _, _ in missing:
        if prev_end is not None and ea != prev_end:
            print("  # table 0x%08X..0x%08X" % (run_start, prev_end - 1))
            run_start = None
        if run_start is None:
            run_start = ea
        line = "0x%08X = { size = 0x%X }" % (ea, size)
        lines.append(line)
        print("  " + line)
        prev_end = ea + size
    if run_start is not None:
        print("  # table 0x%08X..0x%08X" % (run_start, prev_end - 1))

    return "\n".join(lines) + "\n"


def main():
    config_path, config_note = resolve_config()
    known = load_existing(config_path)
    found = scan(known)
    text = report(found, config_path, config_note, len(known))
    if OUT_PATH and text:
        with open(OUT_PATH, "w", encoding="utf-8") as f:
            f.write(text)
        print("")
        print("Wrote %s" % OUT_PATH)


if __name__ == "__main__":
    main()
