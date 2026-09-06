"""
Xenos (Xbox 360 GPU) shader microcode disassembler.

Two entry points over one decoder:

  blob mode   python tools/xenos_shader_disasm.py <file> [...]  - no IDA needed
              Disassembles microcode dwords. Accepts the renderer's own dump
              format ("=== GUEST MICROCODE (n dwords) ===" followed by hex),
              a bare hex dump, or a raw binary .bin.

  scan mode   run inside IDA (File > Script file...)
              Scans non-executable segments for embedded shader blobs, with
              structural validation, and disassembles what it finds.

  self-test   python tools/xenos_shader_disasm.py --selftest
              Decodes a known blob and checks the invariants.

  verify      python tools/xenos_shader_disasm.py --verify logs/hlsldump/*.txt
              Cross-checks this decoder against the renderer's C++ one over a
              corpus of real dumps. The C++ side is the oracle. Note that both
              are ours, so a mistake shared by the two agrees with itself --
              which is exactly what happened with mask-less scalar ops. Prefer
              --xenia when a Xenia dump is available.

  xenia       python tools/xenos_shader_disasm.py --xenia <dir> [dumps...]
              Diffs this decoder against XENIA's disassembly of the same
              microcode, paired by SHA-1 of the bytes rather than by name.
              <dir> is a Xenia shader dump (the *.ucode.bin.* / *.ucode.*
              pairs). Defaults to logs/hlsldump/*.txt when no dumps are given.
              An independent implementation, so it catches what --verify cannot.

  scan-file   python tools/xenos_shader_disasm.py --scan-file <binary>
              Scans a raw binary. Note that assets/default.xex is LZX
              compressed, so scanning it finds nothing real - it serves as a
              large false-positive stress test (4.4 candidates/MB of
              high-entropy data, none of them shaders). To scan the real
              decompressed image, use scan mode inside IDA.

  strip       run inside IDA with STRIP = True to remove annotations this
              script made. Annotation is off by default.

Where the shaders actually are: this title uploads microcode via PM4
IM_LOAD_IMMEDIATE at runtime, and the renderer dumps it to logs/hlsldump/ (see
hooks_d3d9.cpp). That is the reliable source, and blob mode reads it directly.
The 2481 packaged `shader` assets live in .xenon.package heaps that are
encrypted (tools/README.md), so neither scan mode nor anything else recovers
them without the content key. Scan mode is for microcode embedded in the
executable image itself, which is a much smaller population.

Every bit layout and opcode name below is transcribed from Xenia's
src/xenia/gpu/ucode.h and ucode.cc. Nothing here is inferred from staring at
hex. Where a struct is quoted, the field order is the C bitfield order, LSB
first, which is how the layouts unpack.

Why the rewrite: the previous version stepped 4 bytes per instruction and
classified with `(dword >> 30) == 3`. Xenos ALU and fetch instructions are 3
dwords (12 bytes); control flow is 48-bit, packed two per three dwords. The
4-byte step meant it was decoding PowerPC: `(dword >> 30) == 3` selects PPC
primary opcodes 48-51 (lfs/lfsu/lfd/lfdu), which is where its 105,623 "fetch"
hits came from. Scanning the .text image, with no requirement that a candidate
contain an exec or terminate, produced 586,594 "shader blocks" of which 522k
were 1-2 "ops". None of them were shaders.
"""

import glob
import hashlib
import os
import re
import struct
import sys

# ---------------------------------------------------------------------------
# Configuration (scan mode)
# ---------------------------------------------------------------------------

# Write comments into the IDB at confirmed shader starts. Off by default: the
# previous version issued 2.3M set_cmt calls across the PPC text segment. When
# on, every comment carries COMMENT_TAG so STRIP can remove exactly those and
# nothing else.
ANNOTATE = False

# Remove this script's annotations and do nothing else.
STRIP = False

COMMENT_TAG = "[XENOS]"

# The structural minimum: one CF triple (3 dwords) plus one instruction (3).
# Real shaders do reach this floor - ps_21686C60 is 9 dwords, of which 3 are
# trailing padding - so blob mode must not impose more than the encoding does.
MIN_BLOB_DWORDS = 6

# Scan mode is a different problem: it is looking for shaders in undifferentiated
# data, where a 6-dword window of anything has a real chance of decoding. The
# larger floor buys specificity at the cost of missing the very smallest
# shaders, which is the right trade when the alternative is a report full of
# noise. Blob mode does not use this.
SCAN_MIN_BLOB_DWORDS = 12

# Minimum fraction of an instruction span that control flow must reach for a
# scan candidate to be believed. The worst of the 160 real shaders in
# logs/hlsldump measures 0.50, so 0.33 leaves headroom for a sparser one while
# still rejecting the long, near-empty spans that dominate false positives.
SCAN_MIN_DENSITY = 0.33

# Xenos executes at most 6 instructions per exec: the sequence field is 12 bits
# at 2 bits per instruction. A count above this is a decode error, not a big
# shader. This is the single most effective validity check.
MAX_EXEC_COUNT = 6


# ---------------------------------------------------------------------------
# Opcode tables - ucode.cc kAluVectorOpcodeInfos / kAluScalarOpcodeInfos
# ---------------------------------------------------------------------------

# ucode.cc:83. The trailing opcode_30/opcode_31 are Xenia's own placeholders for
# undefined encodings; keeping them means a malformed blob prints its opcode
# number instead of raising.
ALU_VECTOR_NAMES = [
    "add", "mul", "max", "min", "seq", "sgt", "sge", "sne",
    "frc", "trunc", "floor", "mad", "cndeq", "cndge", "cndgt", "dp4",
    "dp3", "dp2add", "cube", "max4", "setp_eq_push", "setp_ne_push",
    "setp_gt_push", "setp_ge_push", "kill_eq", "kill_gt", "kill_ge", "kill_ne",
    "dst", "maxa", "opcode_30", "opcode_31",
]

# Operand count per vector opcode, derived from the src write masks in
# kAluVectorOpcodeInfos (an entry with three masks reads three sources).
ALU_VECTOR_SRC_COUNT = {
    "mad": 3, "cndeq": 3, "cndge": 3, "cndgt": 3, "dp2add": 3,
    "frc": 1, "trunc": 1, "floor": 1, "max4": 1,
}

# ucode.cc:16. Duplicated names (mulsc/addsc/subsc) are genuine: the low bit of
# the opcode selects which constant slot the operand comes from.
ALU_SCALAR_NAMES = [
    "adds", "adds_prev", "muls", "muls_prev", "muls_prev2", "maxs", "mins",
    "seqs", "sgts", "sges", "snes", "frcs", "truncs", "floors", "exp", "logc",
    "log", "rcpc", "rcpf", "rcp", "rsqc", "rsqf", "rsq", "maxas", "maxasf",
    "subs", "subs_prev", "setp_eq", "setp_ne", "setp_gt", "setp_ge",
    "setp_inv", "setp_pop", "setp_clr", "setp_rstr", "kills_eq", "kills_gt",
    "kills_ge", "kills_ne", "kills_one", "sqrt", "opcode_41", "mulsc", "mulsc",
    "addsc", "addsc", "subsc", "subsc", "sin", "cos", "retain_prev",
    "opcode_51", "opcode_52", "opcode_53", "opcode_54", "opcode_55",
    "opcode_56", "opcode_57", "opcode_58", "opcode_59", "opcode_60",
    "opcode_61", "opcode_62", "opcode_63",
]

# Source operand count, from the second field of kAluScalarOpcodeInfos. The
# two-source forms are the *sc family, which take a constant and a temp.
ALU_SCALAR_SRC_COUNT = {
    "mulsc": 2, "addsc": 2, "subsc": 2,
    "setp_clr": 0, "retain_prev": 0,
}

# The *sc family: a constant operand and a temp operand, addressed differently
# from every other scalar op.
ALU_SCALAR_CONST_REG_OPS = frozenset(["mulsc", "addsc", "subsc"])

# Scalar operands are AB = WX, not XY: after the component-relative swizzle, the
# left operand is .w and the right is .x. These opcodes consume both; the rest
# consume only .w. Taken from the scalar switch in src/gpu/shader_hlsl.cpp,
# which is the emitter this must not disagree with.
ALU_SCALAR_TWO_COMPONENT = frozenset(["adds", "muls", "subs", "maxs", "mins"])

# ucode.h:91 enum class ControlFlowOpcode.
CF_NAMES = [
    "nop", "exec", "exec_end", "cond_exec", "cond_exec_end", "cond_exec_pred",
    "cond_exec_pred_end", "loop_start", "loop_end", "cond_call", "return",
    "cond_jmp", "alloc", "cond_exec_pred_clean", "cond_exec_pred_clean_end",
    "mark_vs_fetch_done",
]

CF_NOP = 0
CF_EXEC = 1
CF_EXEC_END = 2
CF_COND_EXEC = 3
CF_COND_EXEC_END = 4
CF_COND_EXEC_PRED = 5
CF_COND_EXEC_PRED_END = 6
CF_LOOP_START = 7
CF_LOOP_END = 8
CF_COND_CALL = 9
CF_RETURN = 10
CF_COND_JMP = 11
CF_ALLOC = 12
CF_COND_EXEC_PRED_CLEAN = 13
CF_COND_EXEC_PRED_CLEAN_END = 14
CF_MARK_VS_FETCH_DONE = 15

# ucode.h:168 IsControlFlowOpcodeExec.
CF_EXEC_OPCODES = frozenset([
    CF_EXEC, CF_EXEC_END, CF_COND_EXEC, CF_COND_EXEC_END, CF_COND_EXEC_PRED,
    CF_COND_EXEC_PRED_END, CF_COND_EXEC_PRED_CLEAN,
    CF_COND_EXEC_PRED_CLEAN_END,
])

# ucode.h:179 DoesControlFlowOpcodeEndShader.
CF_END_OPCODES = frozenset([
    CF_EXEC_END, CF_COND_EXEC_END, CF_COND_EXEC_PRED_END,
    CF_COND_EXEC_PRED_CLEAN_END,
])

# The cond_exec family carries a bool constant index where plain exec has
# padding; it changes how the instruction prints, not how it is bounded.
CF_COND_EXEC_BOOL = frozenset([
    CF_COND_EXEC, CF_COND_EXEC_END, CF_COND_EXEC_PRED_CLEAN,
    CF_COND_EXEC_PRED_CLEAN_END,
])

# ucode.h:526 enum class FetchOpcode.
FETCH_NAMES = {
    0: "vfetch", 1: "tfetch", 16: "getBCF", 17: "getCompTexLOD",
    18: "getGradients", 19: "getWeights", 24: "setTexLOD",
    25: "setGradientH", 26: "setGradientV",
}

# xenos.h enum class VertexFormat. Sparse on purpose - the field is 6 bits and
# most of the 64 values are undefined.
VERTEX_FORMATS = {
    0: "undefined", 6: "8_8_8_8", 7: "2_10_10_10", 16: "10_11_11",
    17: "11_11_10", 25: "16_16", 26: "16_16_16_16", 31: "16_16_FLOAT",
    32: "16_16_16_16_FLOAT", 33: "32", 34: "32_32", 35: "32_32_32_32",
    36: "32_FLOAT", 37: "32_32_FLOAT", 38: "32_32_32_32_FLOAT",
    57: "32_32_32_FLOAT",
}

# ucode.h:150 - the per-component destination swizzle selectors.
DST_SWIZZLE_CHARS = "xyzw01_?"

# ucode.h AllocType. kVsInterpolators and kPsColors share value 2; which one it
# means depends on the shader type, which a blob does not state.
ALLOC_TYPES = {0: "none", 1: "position", 2: "interpolators/colors", 3: "memory"}

# The vertex position export destination. Matches kPositionExportRegister in
# src/gpu/shader_ucode.h.
POSITION_EXPORT_REGISTER = 62


# ---------------------------------------------------------------------------
# Bitfield helpers
# ---------------------------------------------------------------------------

def sign5(v):
    """Five-bit two's complement. Xenos texel offsets run -16..15."""
    return v - 32 if v & 16 else v


def bits(value, offset, width):
    """Unsigned field of `width` bits starting at `offset`, LSB numbered 0."""
    return (value >> offset) & ((1 << width) - 1)


def sbits(value, offset, width):
    """Signed (two's complement) field, for the layouts declared int32_t:n."""
    raw = bits(value, offset, width)
    sign = 1 << (width - 1)
    return raw - (1 << width) if raw & sign else raw


# ---------------------------------------------------------------------------
# Control flow - ucode.h:490 union ControlFlowInstruction
# ---------------------------------------------------------------------------

class ControlFlow(object):
    """One 48-bit control flow instruction, held as its two dwords.

    Field offsets follow the C bitfield order in ucode.h. Word 0 carries
    address/count/sequence for the exec family; the opcode always lives in the
    top 4 bits of word 1's low 16.
    """

    __slots__ = ("dword_0", "dword_1")

    def __init__(self, dword_0, dword_1):
        self.dword_0 = dword_0
        self.dword_1 = dword_1

    @property
    def opcode(self):
        # ucode.h:507 - unused_1 : 12 then opcode_value : 4, within word 1.
        return bits(self.dword_1, 12, 4)

    @property
    def opcode_name(self):
        return CF_NAMES[self.opcode]

    # -- exec family (ControlFlowExecInstruction, ucode.h:218) --------------
    @property
    def address(self):
        return bits(self.dword_0, 0, 12)

    @property
    def count(self):
        return bits(self.dword_0, 12, 3)

    @property
    def is_yield(self):
        return bool(bits(self.dword_0, 15, 1))

    @property
    def sequence(self):
        return bits(self.dword_0, 16, 12)

    def is_fetch(self, index):
        """Sequence bit [0] of instruction `index`: ALU (0) or fetch (1).

        This is what makes the ALU/fetch split evidence rather than a guess -
        the instruction words themselves do not say which they are.
        """
        return bool((self.sequence >> (2 * index)) & 1)

    def is_serialized(self, index):
        return bool((self.sequence >> (2 * index + 1)) & 1)

    @property
    def bool_address(self):
        # ControlFlowCondExecInstruction: vc_lo_ : 2 then bool_address_ : 8.
        return bits(self.dword_1, 2, 8)

    @property
    def condition(self):
        return bool(bits(self.dword_1, 10, 1))

    # -- alloc (ControlFlowAllocInstruction, ucode.h:469) -------------------
    @property
    def alloc_size(self):
        return bits(self.dword_0, 0, 3)

    @property
    def alloc_type(self):
        return bits(self.dword_1, 9, 2)

    @property
    def is_exec(self):
        return self.opcode in CF_EXEC_OPCODES

    @property
    def ends_shader(self):
        return self.opcode in CF_END_OPCODES

    def format(self):
        op = self.opcode
        name = self.opcode_name
        if self.is_exec:
            text = "%s addr=%d count=%d" % (name, self.address, self.count)
            if op in CF_COND_EXEC_BOOL:
                text += " bool=b%d cond=%d" % (self.bool_address,
                                               int(self.condition))
            if self.is_yield:
                text += " yield"
            seq = "".join("F" if self.is_fetch(i) else "A"
                          for i in range(self.count))
            if seq:
                text += " seq=" + seq
            return text
        if op == CF_ALLOC:
            return "alloc %s size=%d" % (
                ALLOC_TYPES.get(self.alloc_type, "?"), self.alloc_size)
        if op in (CF_COND_JMP, CF_COND_CALL):
            return "%s addr=%d" % (name, self.address)
        if op in (CF_LOOP_START, CF_LOOP_END):
            return "%s addr=%d" % (name, self.address)
        return name


def unpack_control_flow(dwords, index):
    """Two CF instructions from three dwords at `index`.

    Verbatim from ucode.h:515 UnpackControlFlowInstructions. The middle dword is
    split: its low 16 bits finish the first instruction, its high 16 begin the
    second. Getting this wrong is silent - you get plausible-looking opcodes
    that decode to nothing.
    """
    d0 = dwords[index]
    d1 = dwords[index + 1]
    d2 = dwords[index + 2]
    return (
        ControlFlow(d0, d1 & 0xFFFF),
        ControlFlow(((d1 >> 16) | (d2 << 16)) & 0xFFFFFFFF, d2 >> 16),
    )


# ---------------------------------------------------------------------------
# ALU - ucode.h:1849 struct AluInstruction
# ---------------------------------------------------------------------------

class AluInstruction(object):
    """One 3-dword ALU instruction issuing a vector op and a scalar op."""

    __slots__ = ("w0", "w1", "w2")

    def __init__(self, w0, w1, w2):
        self.w0 = w0
        self.w1 = w1
        self.w2 = w2

    # -- word 0 ------------------------------------------------------------
    vector_dest = property(lambda s: bits(s.w0, 0, 6))
    vector_dest_rel = property(lambda s: bool(bits(s.w0, 6, 1)))
    abs_constants = property(lambda s: bool(bits(s.w0, 7, 1)))
    scalar_dest = property(lambda s: bits(s.w0, 8, 6))
    scalar_dest_rel = property(lambda s: bool(bits(s.w0, 14, 1)))
    is_export = property(lambda s: bool(bits(s.w0, 15, 1)))
    vector_write_mask = property(lambda s: bits(s.w0, 16, 4))
    scalar_write_mask = property(lambda s: bits(s.w0, 20, 4))
    vector_clamp = property(lambda s: bool(bits(s.w0, 24, 1)))
    scalar_clamp = property(lambda s: bool(bits(s.w0, 25, 1)))
    scalar_opcode = property(lambda s: bits(s.w0, 26, 6))

    # Exports mask differently: both halves write vector_dest, and components
    # neither half claims can come from the constant 0/1 encoding instead.
    # Transcribed from ucode.h:1992-2003.
    const0_write_mask = property(
        lambda s: (0xF & ~(s.vector_write_mask | s.scalar_write_mask))
        if (s.is_export and s.scalar_dest_rel) else 0)
    const1_write_mask = property(
        lambda s: (s.vector_write_mask & s.scalar_write_mask)
        if s.is_export else 0)

    # -- word 1 ------------------------------------------------------------
    src3_swiz = property(lambda s: bits(s.w1, 0, 8))
    src2_swiz = property(lambda s: bits(s.w1, 8, 8))
    src1_swiz = property(lambda s: bits(s.w1, 16, 8))
    src3_negate = property(lambda s: bool(bits(s.w1, 24, 1)))
    src2_negate = property(lambda s: bool(bits(s.w1, 25, 1)))
    src1_negate = property(lambda s: bool(bits(s.w1, 26, 1)))
    pred_condition = property(lambda s: bool(bits(s.w1, 27, 1)))
    is_predicated = property(lambda s: bool(bits(s.w1, 28, 1)))
    const_address_register_relative = property(
        lambda s: bool(bits(s.w1, 29, 1)))
    const_1_rel_abs = property(lambda s: bool(bits(s.w1, 30, 1)))
    const_0_rel_abs = property(lambda s: bool(bits(s.w1, 31, 1)))

    # -- word 2 ------------------------------------------------------------
    src3_reg = property(lambda s: bits(s.w2, 0, 8))
    src2_reg = property(lambda s: bits(s.w2, 8, 8))
    src1_reg = property(lambda s: bits(s.w2, 16, 8))
    vector_opcode = property(lambda s: bits(s.w2, 24, 5))
    src3_sel = property(lambda s: bool(bits(s.w2, 29, 1)))
    src2_sel = property(lambda s: bool(bits(s.w2, 30, 1)))
    src1_sel = property(lambda s: bool(bits(s.w2, 31, 1)))

    def src_reg(self, i):
        return (self.src1_reg, self.src2_reg, self.src3_reg)[i - 1]

    def src_swizzle(self, i):
        return (self.src1_swiz, self.src2_swiz, self.src3_swiz)[i - 1]

    def src_negate(self, i):
        return (self.src1_negate, self.src2_negate, self.src3_negate)[i - 1]

    def src_is_temp(self, i):
        return (self.src1_sel, self.src2_sel, self.src3_sel)[i - 1]

    def scalar_const_reg_op_src_temp_reg(self):
        """Temp register index for the *sc family.

        The index is assembled from three unrelated fields - the low opcode bit,
        src3_sel and part of src3_swiz - rather than being a register field.
        Transcribed from AluInstruction::scalar_const_reg_op_src_temp_reg.
        """
        return ((self.scalar_opcode & 1) | (int(self.src3_sel) << 1)
                | (self.src3_swiz & 0x3C))

    def src_const_is_addressed(self, i):
        """Which of the two rel_abs bits governs source `i`.

        Not simply "bit i" - the mapping depends on how many earlier sources are
        temps, because temps consume no constant slot. Transcribed from
        AluInstruction::src_const_is_addressed.
        """
        if i == 1:
            return self.const_0_rel_abs
        if i == 2:
            return (self.const_0_rel_abs if self.src_is_temp(1)
                    else self.const_1_rel_abs)
        return (self.const_0_rel_abs
                if (self.src_is_temp(1) and self.src_is_temp(2))
                else self.const_1_rel_abs)

    @property
    def vector_opcode_name(self):
        return ALU_VECTOR_NAMES[self.vector_opcode]

    @property
    def scalar_opcode_name(self):
        return ALU_SCALAR_NAMES[self.scalar_opcode]


def write_mask_str(mask):
    """A 4-bit write mask as .xyzw, or the empty-mask marker.

    An empty mask does not mean the instruction is dead: maxa/maxas/maxasf carry
    one and still set the address register, and gating on it once dropped a0 and
    un-posed the rider. So it is printed as an explicit [no write] rather than
    as a bare dot that reads like a formatting slip.
    """
    if mask == 0b1111:
        return ""
    if mask == 0:
        return " [no write]"
    return "." + "".join(c for i, c in enumerate("xyzw") if mask & (1 << i))


def swizzle_str(swizzle, components=4):
    """Source swizzle, expanded.

    Xenos swizzles are component-relative: component c reads
    ((swizzle >> 2c) + c) & 3, so a swizzle of 0 is identity rather than xxxx.
    Transcribed from AluInstruction::GetSwizzledComponentIndex.
    """
    out = "".join("xyzw"[((swizzle >> (2 * c)) + c) & 3]
                  for c in range(components))
    return "" if out == "xyzw"[:components] else "." + out


def format_alu_source(alu, i, swizzled=True):
    """One source operand as text, with its register class and modifiers.

    `swizzled` is cleared by the scalar path, which appends the single component
    it actually consumes instead of a four-component swizzle.
    """
    reg = alu.src_reg(i)
    text = ""
    if alu.src_is_temp(i):
        # A relative temp index is aL-relative; the register file has no a0
        # addressing mode.
        if reg & 0x40:
            text = "r[aL+%d]" % (reg & 0x3F)
        else:
            text = "r%d" % (reg & 0x3F)
        if reg & 0x80:
            text = "|%s|" % text
    else:
        index = reg & 0xFF
        if alu.src_const_is_addressed(i):
            # AddressingMode: 1 = c[a0 + n], 0 = c[aL + n]. The accessor name
            # reads "address register relative", and the address register is a0.
            base = "a0" if alu.const_address_register_relative else "aL"
            text = "c[%s+%d]" % (base, index)
        else:
            text = "c%d" % index
        if alu.abs_constants:
            text = "|%s|" % text
    if swizzled:
        text += swizzle_str(alu.src_swizzle(i))
    if alu.src_negate(i):
        text = "-" + text
    return text


def swizzled_component(swizzle, component):
    """The single component a scalar operand reads, as a letter."""
    return "xyzw"[((swizzle >> (2 * component)) + component) & 3]


def format_scalar_sources(alu, sop):
    """Operand text for the scalar half of an ALU instruction."""
    if sop in ALU_SCALAR_CONST_REG_OPS:
        # A constant and a temp, each one component, and the temp's index is
        # assembled rather than stored. Component 3 selects the constant's,
        # component 0 the temp's.
        const_text = "c%d.%s" % (alu.src3_reg & 0xFF,
                                 swizzled_component(alu.src3_swiz, 3))
        temp_text = "r%d.%s" % (alu.scalar_const_reg_op_src_temp_reg(),
                                swizzled_component(alu.src3_swiz, 0))
        if alu.abs_constants:
            const_text = "|%s|" % const_text
            temp_text = "|%s|" % temp_text
        if alu.src_negate(3):
            const_text = "-" + const_text
            temp_text = "-" + temp_text
        return "%s, %s" % (const_text, temp_text)

    if ALU_SCALAR_SRC_COUNT.get(sop, 1) == 0:
        return ""

    # Everything else reads src3. Scalar operands are AB = WX, so show exactly
    # the component(s) consumed rather than a vec4 swizzle that overstates it.
    base = format_alu_source(alu, 3, swizzled=False)
    comp = swizzled_component(alu.src3_swiz, 3)
    if sop in ALU_SCALAR_TWO_COMPONENT:
        comp += swizzled_component(alu.src3_swiz, 0)
    return base + "." + comp


def format_alu(alu):
    """Both halves of an ALU instruction, as a list of text lines."""
    lines = []
    pred = ""
    if alu.is_predicated:
        pred = "(%sp0) " % ("" if alu.pred_condition else "!")

    vop = alu.vector_opcode_name
    vmask = alu.vector_write_mask
    # An export with an empty vector mask can still write through the constant
    # 0/1 encoding (`max oC0._000, r0, r0`), and maxa is issued for a0 alone.
    # Same rule as has_vector in shader_hlsl.cpp.
    if (vmask or alu.const0_write_mask or alu.const1_write_mask
            or vop == "maxa"
            or vop.startswith("kill") or vop.startswith("setp")):
        if alu.is_export:
            dest = "export%d" % alu.vector_dest
            if alu.vector_dest == POSITION_EXPORT_REGISTER:
                dest += "(position)"
        elif alu.vector_dest_rel:
            dest = "r[aL+%d]" % alu.vector_dest
        else:
            dest = "r%d" % alu.vector_dest
        n = ALU_VECTOR_SRC_COUNT.get(vop, 2)
        srcs = ", ".join(format_alu_source(alu, i) for i in range(1, n + 1))
        lines.append("%s%s%s %s%s%s" % (
            pred, vop, "_sat" if alu.vector_clamp else "",
            dest, write_mask_str(vmask), (", " + srcs) if srcs else ""))

    sop = alu.scalar_opcode_name
    smask = alu.scalar_write_mask
    # The scalar half prints unconditionally. An empty write mask does NOT mean
    # the slot is idle: ps is written by every scalar issue, and the ops that
    # exist purely for that side effect (a maxs feeding the next instruction's
    # muls_prev) carry no mask at all. Gating the print on the mask hid exactly
    # those -- which is why --verify agreeing with the C++ decoder proved
    # nothing, since both had the same blind spot. Xenia prints them with an
    # empty mask as `r0._`; so do we.
    if alu.is_export:
        dest = "export%d" % alu.scalar_dest
    elif alu.scalar_dest_rel:
        dest = "r[aL+%d]" % alu.scalar_dest
    else:
        dest = "r%d" % alu.scalar_dest
    srcs = format_scalar_sources(alu, sop)
    lines.append("%s%s%s %s%s%s" % (
        pred, sop, "_sat" if alu.scalar_clamp else "",
        dest, write_mask_str(smask), (", " + srcs) if srcs else ""))

    return lines


# ---------------------------------------------------------------------------
# Fetch - ucode.h:690 VertexFetchInstruction / :777 TextureFetchInstruction
# ---------------------------------------------------------------------------

class FetchInstruction(object):
    """One 3-dword fetch. Vertex and texture share word 0's low fields."""

    __slots__ = ("w0", "w1", "w2")

    def __init__(self, w0, w1, w2):
        self.w0 = w0
        self.w1 = w1
        self.w2 = w2

    opcode = property(lambda s: bits(s.w0, 0, 5))
    src_reg = property(lambda s: bits(s.w0, 5, 6))
    src_reg_am = property(lambda s: bool(bits(s.w0, 11, 1)))
    dst_reg = property(lambda s: bits(s.w0, 12, 6))
    dst_reg_am = property(lambda s: bool(bits(s.w0, 18, 1)))

    # -- vertex fetch ------------------------------------------------------
    vf_must_be_one = property(lambda s: bits(s.w0, 19, 1))
    vf_const_index = property(lambda s: bits(s.w0, 20, 5))
    vf_const_index_sel = property(lambda s: bits(s.w0, 25, 2))
    vf_prefetch_count = property(lambda s: bits(s.w0, 27, 3))
    vf_src_swiz = property(lambda s: bits(s.w0, 30, 2))

    vf_dst_swiz = property(lambda s: bits(s.w1, 0, 12))
    vf_format_comp_all = property(lambda s: bool(bits(s.w1, 12, 1)))
    vf_num_format_all = property(lambda s: bool(bits(s.w1, 13, 1)))
    vf_signed_rf_mode_all = property(lambda s: bits(s.w1, 14, 1))
    vf_is_index_rounded = property(lambda s: bool(bits(s.w1, 15, 1)))
    vf_format = property(lambda s: bits(s.w1, 16, 6))
    vf_exp_adjust = property(lambda s: sbits(s.w1, 24, 6))
    vf_is_mini_fetch = property(lambda s: bool(bits(s.w1, 30, 1)))
    vf_is_predicated = property(lambda s: bool(bits(s.w1, 31, 1)))

    vf_stride = property(lambda s: bits(s.w2, 0, 8))
    vf_offset = property(lambda s: sbits(s.w2, 8, 23))
    vf_pred_condition = property(lambda s: bool(bits(s.w2, 31, 1)))

    # ucode.h:719-720. Note the polarity: num_format_all == 0 means normalized,
    # so the flag reads inverted from its name.
    vf_is_signed = property(lambda s: bool(bits(s.w1, 12, 1)))
    vf_is_normalized = property(lambda s: not bits(s.w1, 13, 1))

    # -- texture fetch -----------------------------------------------------
    tf_fetch_valid_only = property(lambda s: bool(bits(s.w0, 19, 1)))
    tf_const_index = property(lambda s: bits(s.w0, 20, 5))
    tf_tx_coord_denorm = property(lambda s: bool(bits(s.w0, 25, 1)))
    tf_src_swiz = property(lambda s: bits(s.w0, 26, 6))

    tf_dst_swiz = property(lambda s: bits(s.w1, 0, 12))
    tf_use_comp_lod = property(lambda s: bool(bits(s.w1, 28, 1)))
    tf_use_reg_lod = property(lambda s: bool(bits(s.w1, 29, 1)))
    tf_is_predicated = property(lambda s: bool(bits(s.w1, 31, 1)))
    # Xenia ucode.h, TextureFetchInstruction word 2: use_reg_gradients(1) +
    # sample_location(1) + lod_bias(7) + unused(5) + dimension(2) +
    # offset_x/y/z(5+5+5) = 31, so pred_condition is the top bit.
    tf_pred_condition = property(lambda s: bool(bits(s.w2, 31, 1)))

    tf_dimension = property(lambda s: bits(s.w2, 14, 2))

    # TEXEL OFFSETS, decoded because a shader cannot be reimplemented without
    # them. bloom.shader::BlurHPS issues three tfetch2D from the SAME register
    # and the SAME fetch constant; read without offsets that is three identical
    # taps, which is not a blur. The taps differ only by these fields, and a
    # native shader written from the disassembly above would have silently been
    # a 3x-weighted copy of one texel.
    #
    # Word 2 packs use_reg_gradients(1) + sample_location(1) + lod_bias(7) +
    # unused(5) + dimension(2) before them, so x starts at bit 16. Five bits
    # each and SIGNED -- an unsigned read turns a -1 tap into +31.
    # HALF-TEXEL units, so the field is halved to give texels. The SDK is the
    # authority here and says so outright -- ucode.h:816,
    # `offset_x() { return data_.offset_x * 0.5f; }`. Reported raw, these read
    # as +/-3 for bloom's blur and the taps are really +/-1.5 texels, which is
    # both twice the distance and not expressible as an HLSL integer offset.
    tf_offset_x = property(lambda s: sign5(bits(s.w2, 16, 5)) * 0.5)
    tf_offset_y = property(lambda s: sign5(bits(s.w2, 21, 5)) * 0.5)
    tf_offset_z = property(lambda s: sign5(bits(s.w2, 26, 5)) * 0.5)

    @property
    def opcode_name(self):
        return FETCH_NAMES.get(self.opcode, "fetch_op_%d" % self.opcode)

    @property
    def is_vertex_fetch(self):
        return self.opcode == 0

    @property
    def is_texture_fetch(self):
        return self.opcode == 1


def fetch_dst_swizzle_str(swizzle, components=4):
    """Destination swizzle, 3 bits per component: xyzw01_ plus keep."""
    out = ""
    for c in range(components):
        out += DST_SWIZZLE_CHARS[(swizzle >> (3 * c)) & 7]
    return "." + out


def format_fetch(fetch):
    """One fetch instruction as text.

    The `(p0)` / `(!p0)` prefix is printed here for the same reason format_alu
    prints it. It was NOT, for a long time: tf_is_predicated and
    vf_is_predicated were decoded and then never used, so every fetch listed as
    unpredicated no matter what the bit said. That reads as a fact about the
    shader and is a fact about this tool -- it cost an analysis that concluded
    "ALU instructions inside a cond_exec_pred are predicated but fetches are
    not", which is false; all of them are. If a field is worth decoding it is
    worth printing.
    """
    pred = ""
    if fetch.is_vertex_fetch and fetch.vf_is_predicated:
        pred = "(%sp0) " % ("" if fetch.vf_pred_condition else "!")
    elif fetch.is_texture_fetch and fetch.tf_is_predicated:
        pred = "(%sp0) " % ("" if fetch.tf_pred_condition else "!")

    if fetch.is_vertex_fetch:
        fmt = VERTEX_FORMATS.get(fetch.vf_format, "fmt%d" % fetch.vf_format)
        text = "vfetch%s r%d%s, r%d.%s, vf%d" % (
            "_mini" if fetch.vf_is_mini_fetch else "_full",
            fetch.dst_reg, fetch_dst_swizzle_str(fetch.vf_dst_swiz),
            fetch.src_reg, "xyzw"[fetch.vf_src_swiz],
            fetch.vf_const_index)
        text += " [%s" % fmt
        # stride and offset are in dwords in the encoding; bytes is what the
        # vertex buffer is actually indexed in.
        text += " stride=%d" % (fetch.vf_stride * 4)
        text += " offset=%d" % (fetch.vf_offset * 4)
        if fetch.vf_exp_adjust:
            text += " exp=%d" % fetch.vf_exp_adjust
        if fetch.vf_is_signed:
            text += " signed"
        if fetch.vf_is_normalized:
            text += " norm"
        if fetch.vf_is_index_rounded:
            text += " rounded"
        text += "]"
        return pred + text

    if fetch.is_texture_fetch:
        dim = ("1D", "2D", "3D", "cube")[fetch.tf_dimension]
        text = "tfetch%s r%d%s, r%d, tf%d" % (
            dim, fetch.dst_reg, fetch_dst_swizzle_str(fetch.tf_dst_swiz),
            fetch.src_reg, fetch.tf_const_index)
        flags = []
        if fetch.tf_tx_coord_denorm:
            flags.append("unnormalized")
        if fetch.tf_use_reg_lod:
            flags.append("reg_lod")
        if not fetch.tf_use_comp_lod:
            flags.append("no_comp_lod")
        # Printed only when non-zero, and only the components the dimension
        # actually has: a 2D fetch's offset_z is not a field, it is whatever
        # those bits happen to hold.
        used = {"1D": 1, "2D": 2, "3D": 3, "cube": 3}[dim]
        off = [fetch.tf_offset_x, fetch.tf_offset_y, fetch.tf_offset_z][:used]
        if any(off):
            flags.append("offset=" + ",".join("%g" % v for v in off))
        if flags:
            text += " [%s]" % " ".join(flags)
        return pred + text

    return pred + "%s r%d, r%d, const%d" % (
        fetch.opcode_name, fetch.dst_reg, fetch.src_reg, fetch.tf_const_index)


# ---------------------------------------------------------------------------
# Shader decode and validation
# ---------------------------------------------------------------------------

class DecodeError(Exception):
    """A blob is not valid microcode. The message names which check failed."""


class Shader(object):
    """A decoded shader: its CF listing and every instruction in program order."""

    def __init__(self):
        self.cf = []            # (index, ControlFlow)
        self.instructions = []  # (address, is_fetch, object)
        self.cf_dword_count = 0
        self.dword_count = 0
        self.vertex_fetches = 0
        self.texture_fetches = 0
        self.exports = set()
        self.alu_count = 0

    @property
    def kind(self):
        """Vertex or pixel, inferred from which fetch kinds appear.

        A shader with neither is reported as unknown rather than guessed at:
        both kinds exist that fetch nothing.
        """
        if self.vertex_fetches and not self.texture_fetches:
            return "vertex"
        if self.texture_fetches and not self.vertex_fetches:
            return "pixel"
        if self.vertex_fetches and self.texture_fetches:
            return "vertex+texture"
        return "unknown"

    @property
    def exports_position(self):
        return POSITION_EXPORT_REGISTER in self.exports


def find_cf_end(dwords, limit):
    """Dword index where the control flow section ends.

    The blob carries no header saying so. CF runs until the first instruction
    that an exec jumps to, so the end is the lowest exec target, and the bound
    is re-read each iteration so it shrinks as execs are seen. Sound because a
    compiler never places an exec target ahead of the CF referencing it. Same
    approach as src/gpu/shader_ucode.cpp:414.

    Raises DecodeError if the CF stream is not well formed - which is the check
    the previous script was missing entirely.
    """
    max_cf = limit - (limit % 3)
    saw_exec = False
    saw_end = False
    i = 0
    while i + 2 < max_cf:
        for cf in unpack_control_flow(dwords, i):
            if cf.opcode > 15:
                raise DecodeError("CF opcode %d out of range" % cf.opcode)
            if cf.ends_shader:
                saw_end = True
            if not cf.is_exec:
                continue
            saw_exec = True
            if cf.count > MAX_EXEC_COUNT:
                # sequence is 12 bits at 2 bits per instruction, so 6 is the
                # hardware maximum. Anything larger is a misaligned decode.
                raise DecodeError("exec count %d exceeds %d"
                                  % (cf.count, MAX_EXEC_COUNT))
            target = cf.address * 3
            if target + cf.count * 3 > limit:
                raise DecodeError("exec target %d+%d outside blob"
                                  % (cf.address, cf.count))
            if target < max_cf:
                max_cf = target
        i += 3

    if not saw_exec:
        raise DecodeError("no exec instruction")
    if not saw_end:
        raise DecodeError("no end-of-shader control flow")
    if max_cf == 0:
        raise DecodeError("exec target at address 0")
    return max_cf


def decode_shader(dwords):
    """Decode a blob into a Shader, or raise DecodeError.

    Each exec block is visited once, in program order: branches are not followed
    and loops are not unrolled. That over-approximates for a shader with
    alternative paths, which is the right error for a listing.
    """
    if len(dwords) < MIN_BLOB_DWORDS:
        raise DecodeError("blob shorter than %d dwords" % MIN_BLOB_DWORDS)

    limit = len(dwords)
    cf_end = find_cf_end(dwords, limit)

    first = unpack_control_flow(dwords, 0)[0]
    if first.opcode == CF_NOP:
        # Real shaders do not open with padding. Rejecting this removes a large
        # class of zero-filled false positives in a scan.
        raise DecodeError("first control flow instruction is nop")

    shader = Shader()
    shader.dword_count = limit
    shader.cf_dword_count = cf_end

    i = 0
    index = 0
    while i + 2 < cf_end:
        for cf in unpack_control_flow(dwords, i):
            shader.cf.append((index, cf))
            index += 1
        i += 3

    for _, cf in shader.cf:
        if not cf.is_exec:
            continue
        for n in range(cf.count):
            at = (cf.address + n) * 3
            if at + 2 >= limit:
                raise DecodeError("instruction at %d outside blob" % at)
            w0, w1, w2 = dwords[at], dwords[at + 1], dwords[at + 2]
            if cf.is_fetch(n):
                fetch = FetchInstruction(w0, w1, w2)
                # The 5-bit field has 32 values and only 9 are defined. An
                # undefined one is a misaligned decode, not an exotic shader:
                # across the 160 real dumps, zero fetches land outside this set.
                if fetch.opcode not in FETCH_NAMES:
                    raise DecodeError("undefined fetch opcode %d"
                                      % fetch.opcode)
                shader.instructions.append((cf.address + n, True, fetch))
                if fetch.is_vertex_fetch:
                    shader.vertex_fetches += 1
                elif fetch.is_texture_fetch:
                    shader.texture_fetches += 1
            else:
                alu = AluInstruction(w0, w1, w2)
                shader.instructions.append((cf.address + n, False, alu))
                shader.alu_count += 1
                if alu.is_export:
                    if alu.vector_write_mask:
                        shader.exports.add(alu.vector_dest)
                    if alu.scalar_write_mask:
                        shader.exports.add(alu.scalar_dest)

    if not shader.instructions:
        raise DecodeError("no instructions reached by control flow")

    # A shader with no ALU work computes nothing. Measured across the 160 real
    # dumps in logs/hlsldump: every one has at least one, and the minimum is
    # exactly 1, so this rejects noise without excluding anything genuine.
    if shader.alu_count == 0:
        raise DecodeError("no ALU instructions")

    return shader


def disassemble(shader, address=None):
    """A Shader as printable lines."""
    out = []
    head = "; %d dwords, %s shader, %d ALU, %d vfetch, %d tfetch" % (
        shader.dword_count, shader.kind, shader.alu_count,
        shader.vertex_fetches, shader.texture_fetches)
    if address is not None:
        head = "; 0x%08X %s" % (address, head[2:])
    out.append(head)
    if shader.exports:
        out.append("; exports: %s%s" % (
            ", ".join("e%d" % e for e in sorted(shader.exports)),
            " (position)" if shader.exports_position else ""))
    out.append("; --- control flow (%d dwords) ---" % shader.cf_dword_count)
    for index, cf in shader.cf:
        if cf.opcode == CF_NOP and index > 0:
            continue
        out.append("  %3d  %s" % (index, cf.format()))
    out.append("; --- instructions ---")
    for at, is_fetch, insn in shader.instructions:
        if is_fetch:
            out.append("  %3d  %s" % (at, format_fetch(insn)))
        else:
            lines = format_alu(insn)
            out.append("  %3d  %s" % (at, lines[0]))
            for extra in lines[1:]:
                out.append("       %s" % extra)
    return out


# ---------------------------------------------------------------------------
# Blob mode - the primary path
# ---------------------------------------------------------------------------

# The renderer's dump header, written beside the emitted HLSL by
# src/hooks/hooks_d3d9.cpp.
GUEST_MICROCODE_RE = re.compile(
    r"===\s*GUEST MICROCODE\s*\((\d+)\s*dwords\)\s*===", re.IGNORECASE)

HEX_DWORD_RE = re.compile(r"\b[0-9a-fA-F]{8}\b")

# Identifies a file as one of the renderer's dumps even when it carries no
# microcode, so that case can be reported rather than guessed at.
RENDERER_DUMP_RE = re.compile(
    r"^;\s*guest\s+(vertex|pixel)\s+shader", re.IGNORECASE | re.MULTILINE)


class NoMicrocodeError(Exception):
    """The file is a recognised dump, but carries no microcode to decode."""


def load_dwords(path, big_endian=None):
    """Read microcode dwords from a dump file or a raw binary.

    Text input is preferred and detected first: the renderer's dump is text, and
    a .bin is only reached when the bytes are not decodable.
    """
    with open(path, "rb") as handle:
        raw = handle.read()

    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        try:
            text = raw.decode("latin-1")
        except UnicodeDecodeError:
            text = None

    if text is not None:
        match = GUEST_MICROCODE_RE.search(text)
        if match:
            # Stop at the next section header so the EMITTED HLSL and DXBC that
            # follow are not scraped in as microcode.
            region = text[match.end():]
            stop = region.find("===")
            if stop != -1:
                region = region[:stop]
            words = [int(w, 16) for w in HEX_DWORD_RE.findall(region)]
            declared = int(match.group(1))
            if len(words) > declared:
                words = words[:declared]
            elif len(words) < declared:
                sys.stderr.write(
                    "warning: header declares %d dwords, found %d\n"
                    % (declared, len(words)))
            return words

        # A renderer dump with no microcode section. The vsfetch_* variants are
        # like this - HLSL only. Scraping hex out of them yields shader-shaped
        # garbage, so say what is actually wrong instead.
        if RENDERER_DUMP_RE.search(text):
            raise NoMicrocodeError(
                "renderer dump with no GUEST MICROCODE section")

        # A plain hex dump with no header at all is still legitimate input.
        if HEX_DWORD_RE.search(text):
            words = [int(w, 16) for w in HEX_DWORD_RE.findall(text)]
            if words:
                return words

    # Raw binary. The XEX is big-endian and so is microcode captured from guest
    # memory; little-endian is offered because a host-side dump may be swapped.
    count = len(raw) // 4
    fmt = ">" if big_endian is not False else "<"
    return list(struct.unpack("%s%dI" % (fmt, count), raw[:count * 4]))


def run_blob_mode(paths, big_endian=None):
    failures = 0
    skipped = 0
    for path in paths:
        print("=" * 72)
        print(path)
        print("=" * 72)
        try:
            dwords = load_dwords(path, big_endian)
        except NoMicrocodeError as exc:
            # Not a decode failure - there is simply nothing here to decode.
            print("  skipped: %s" % exc)
            skipped += 1
            continue
        except (IOError, OSError) as exc:
            print("  cannot read: %s" % exc)
            failures += 1
            continue
        if not dwords:
            print("  no microcode dwords found")
            failures += 1
            continue
        try:
            shader = decode_shader(dwords)
        except DecodeError as exc:
            print("  not valid microcode: %s" % exc)
            # Byte-swapping is the common cause, so say so once rather than
            # leaving the reader to guess.
            swapped = [struct.unpack("<I", struct.pack(">I", w))[0]
                       for w in dwords]
            try:
                decode_shader(swapped)
            except DecodeError:
                pass
            else:
                print("  (decodes correctly byte-swapped -"
                      " re-run with --little-endian)")
            failures += 1
            continue
        for line in disassemble(shader):
            print(line)
        print("")

    if len(paths) > 1:
        print("%d file(s): %d decoded, %d skipped, %d failed"
              % (len(paths), len(paths) - skipped - failures, skipped,
                 failures))
    return 1 if failures else 0


# ---------------------------------------------------------------------------
# Scanning
# ---------------------------------------------------------------------------

def scan_dwords(dwords, base=0, window=1024, progress=None):
    """Slide over `dwords` and yield every offset that decodes as a shader.

    Shared by the IDB scan and the raw-file scan so both apply exactly the same
    validation. Advances past a match by its real length, so one shader is
    reported once rather than at every interior offset.

    Yields (dword_index, size_in_dwords, Shader).
    """
    total = len(dwords)
    i = 0
    while i + SCAN_MIN_BLOB_DWORDS <= total:
        if progress and (i & 0xFFFFF) == 0:
            progress(i, total)
        chunk = dwords[i:i + window]
        try:
            shader = decode_shader(chunk)
        except DecodeError:
            i += 1
            continue

        last = max(at for at, _, _ in shader.instructions)
        span = last + 1
        # Density: how much of the span control flow actually reaches. Real
        # shaders are dense - the worst of the 160 real dumps is 0.50 - while a
        # false positive is typically a couple of instructions scattered across
        # hundreds of slots of unrelated data. A scan-only heuristic, kept out
        # of decode_shader so blob mode stays faithful to what it is handed.
        if len(shader.instructions) / float(span) < SCAN_MIN_DENSITY:
            i += 1
            continue

        size = span * 3
        yield (base + i, size, shader)
        i += size
    if progress:
        progress(total, total)


def run_scan_file_mode(path, big_endian=True):
    """Scan a raw binary for embedded shader blobs.

    Also the honest large-scale false-positive test: point it at something that
    is definitely not microcode and the count should be at or near zero. A
    validator that cannot do that is not a validator.
    """
    with open(path, "rb") as handle:
        raw = handle.read()
    count = len(raw) // 4
    fmt = ">" if big_endian else "<"
    dwords = list(struct.unpack("%s%dI" % (fmt, count), raw[:count * 4]))

    print("Scanning %s (%d bytes, %d dwords, %s-endian)"
          % (path, len(raw), count, "big" if big_endian else "little"))

    def progress(at, total):
        sys.stderr.write("\r  %.1f%%" % (100.0 * at / max(total, 1)))
        sys.stderr.flush()

    found = list(scan_dwords(dwords, progress=progress))
    sys.stderr.write("\r          \r")

    for index, size, shader in found:
        # A fetch is the strongest single signal that a candidate is real, but
        # it cannot be required: 18 of the 160 real shaders fetch nothing at
        # all. So it grades confidence instead of gating.
        fetches = shader.vertex_fetches + shader.texture_fetches
        print("  0x%08X  %-14s %4d dwords  %d ALU %d vfetch %d tfetch  %s"
              % (index * 4, shader.kind, size, shader.alu_count,
                 shader.vertex_fetches, shader.texture_fetches,
                 "strong" if fetches else "weak"))

    strong = sum(1 for _, _, s in found
                 if s.vertex_fetches or s.texture_fetches)
    print("  %d candidate(s) in %d dwords (%.1f per MB) - %d strong, %d weak"
          % (len(found), count, len(found) / max(len(raw) / 1048576.0, 1e-9),
             strong, len(found) - strong))
    return found


# ---------------------------------------------------------------------------
# IDB scan mode
# ---------------------------------------------------------------------------

def run_scan_mode():
    """Scan non-executable segments for embedded shader blobs.

    Only reached inside IDA. Code segments are skipped outright: microcode does
    not live in PPC .text, and scanning it is what produced 586,594 phantom
    blocks.
    """
    import ida_bytes
    import ida_segment
    import idaapi

    if not idaapi.inf_is_be():
        print("[!] Database is little-endian. The XEX is big-endian; a "
              "mismatch turns valid microcode into garbage. Aborting.")
        return

    desktop = os.path.join(os.path.expanduser("~"), "Desktop")
    report_path = os.path.join(desktop, "xenos_shader_report.txt")

    found = []
    skipped_code = 0

    with open(report_path, "w", encoding="utf-8") as log:
        log.write("=== XENOS SHADER SCAN ===\n\n")

        for n in range(ida_segment.get_segm_qty()):
            seg = ida_segment.getnseg(n)
            if not seg:
                continue
            name = ida_segment.get_segm_name(seg)
            if seg.perm & ida_segment.SEGPERM_EXEC:
                skipped_code += 1
                log.write("skipping executable segment %s "
                          "(0x%08X-0x%08X)\n" % (name, seg.start_ea,
                                                 seg.end_ea))
                continue

            log.write("scanning %s (0x%08X-0x%08X)\n"
                      % (name, seg.start_ea, seg.end_ea))

            # Read the segment once. Shaders are small but the scan slides one
            # dword at a time, so re-reading a window per offset would make this
            # quadratic in IDA API calls.
            count = (seg.end_ea - seg.start_ea) // 4
            dwords = [ida_bytes.get_dword(seg.start_ea + i * 4)
                      for i in range(count)]

            for index, size, shader in scan_dwords(dwords):
                ea = seg.start_ea + index * 4
                found.append((ea, size, shader))

                if ANNOTATE:
                    ida_bytes.set_cmt(
                        ea, "%s %s shader, %d dwords"
                        % (COMMENT_TAG, shader.kind, size), 0)

                for line in disassemble(shader, ea):
                    log.write(line + "\n")
                log.write("\n")

        log.write("\n" + "=" * 72 + "\n")
        log.write("Segments skipped as executable: %d\n" % skipped_code)
        log.write("Shaders found: %d\n" % len(found))
        vertex = sum(1 for _, _, s in found if s.kind == "vertex")
        pixel = sum(1 for _, _, s in found if s.kind == "pixel")
        log.write("  vertex: %d  pixel: %d  other: %d\n"
                  % (vertex, pixel, len(found) - vertex - pixel))
        log.write("=" * 72 + "\n")

    print("[+] %d shaders found. Report: %s" % (len(found), report_path))
    if not found:
        print("[i] Zero is a real answer: this title uploads shaders via PM4 "
              "IM_LOAD_IMMEDIATE at runtime, so the XEX image may hold none.")
    if ANNOTATE:
        print("[i] Annotations written. Set STRIP = True to remove them.")


def run_strip_mode():
    """Remove only the comments this script wrote."""
    import ida_bytes
    import idautils

    removed = 0
    for ea in idautils.Heads():
        for repeatable in (0, 1):
            cmt = ida_bytes.get_cmt(ea, repeatable)
            if cmt and COMMENT_TAG in cmt:
                ida_bytes.set_cmt(ea, "", repeatable)
                removed += 1
    print("[+] Removed %d %s comments." % (removed, COMMENT_TAG))


# ---------------------------------------------------------------------------
# Verify mode - cross-check against the C++ decoder
# ---------------------------------------------------------------------------

# The header line each dump carries, written by the renderer's own translator.
DUMP_HEADER_RE = re.compile(
    r"sampler_count (\d+) max_const_index (\d+) input_mask 0x([0-9a-fA-F]+) "
    r"export_mask 0x([0-9a-fA-F]+) dropped_export_mask 0x([0-9a-fA-F]+) "
    r"writes_position (\d)")


def run_verify_mode(paths):
    """Check this decoder against the C++ one on real dumps.

    Every dump states, from the C++ translator, whether the shader exports a
    position, which export registers it writes, and how many distinct samplers
    it fetches. Those are three independent consequences of the bit layouts
    transcribed above, so agreement across a corpus is real evidence the
    transcription is right - and a disagreement means this file is wrong, not
    the renderer.
    """
    checked = 0
    skipped = 0
    failures = {"position": [], "exports": [], "samplers": [], "decode": []}

    for path in paths:
        try:
            with open(path, "rb") as handle:
                text = handle.read().decode("latin-1")
        except (IOError, OSError):
            continue
        header = DUMP_HEADER_RE.search(text)
        if not header:
            continue
        try:
            dwords = load_dwords(path)
        except NoMicrocodeError:
            skipped += 1
            continue
        except (IOError, OSError):
            continue
        try:
            shader = decode_shader(dwords)
        except DecodeError as exc:
            failures["decode"].append((path, str(exc), ""))
            continue

        checked += 1
        sampler_count = int(header.group(1))
        export_mask = int(header.group(4), 16)
        dropped_mask = int(header.group(5), 16)
        writes_position = bool(int(header.group(6)))

        if shader.exports_position != writes_position:
            failures["position"].append(
                (path, shader.exports_position, writes_position))

        # 62 is position and is accounted for separately; compare the low
        # interpolator/colour exports against emitted|dropped, because a dropped
        # export was still written by the guest.
        mine = 0
        for export in shader.exports:
            if export < 32:
                mine |= 1 << export
        theirs = export_mask | dropped_mask
        if mine != theirs:
            failures["exports"].append((path, hex(mine), hex(theirs)))

        samplers = set()
        for _, is_fetch, insn in shader.instructions:
            if is_fetch and insn.is_texture_fetch:
                samplers.add(insn.tf_const_index)
        if len(samplers) != sampler_count:
            failures["samplers"].append(
                (path, len(samplers), sampler_count))

    print("Verify against the C++ decoder")
    print("  %d dumps checked, %d without microcode" % (checked, skipped))
    if not checked:
        print("  nothing to check - pass the renderer's logs/hlsldump/*.txt")
        return 1

    total = 0
    for name in ("decode", "position", "exports", "samplers"):
        rows = failures[name]
        total += len(rows)
        if not rows:
            print("  ok    %s agrees on all %d" % (name, checked))
            continue
        print("  FAIL  %s: %d disagreement(s)" % (name, len(rows)))
        for row in rows[:8]:
            print("        %s  mine=%s theirs=%s" % row)

    return 1 if total else 0


# ---------------------------------------------------------------------------
# Xenia cross-check - the only reference that is not us
# ---------------------------------------------------------------------------

# `/*   31   */` is an instruction slot; `/*    3.1 */` is a control-flow slot.
XENIA_SLOT_RE = re.compile(r"^\s*/\*\s*(\d+)\s*\*/\s*(.*)$")
XENIA_COISSUE_RE = re.compile(r"^\s*\+\s*(.*)$")


def parse_xenia_listing(path):
    """Xenia's own disassembly, as {address: [mnemonic, ...]}.

    First entry is the vector or fetch half, any second entry the co-issued
    scalar. Mnemonics only: the operand notation differs between the two
    disassemblers in ways that are cosmetic, while a mnemonic disagreement never
    is.
    """
    slots = {}
    current = None
    # Xenia prints the serialize flag on a line of its own, with the actual
    # instruction on the next line and no slot marker of its own.
    continued = False
    with open(path, "r") as handle:
        for line in handle:
            match = XENIA_SLOT_RE.match(line)
            if match:
                current = int(match.group(1))
                body = match.group(2).strip()
                continued = body == "serialize"
                if body and not continued:
                    slots.setdefault(current, []).append(body.split())
                continue
            match = XENIA_COISSUE_RE.match(line)
            if match and current is not None:
                body = match.group(1).strip()
                if body:
                    slots.setdefault(current, []).append(body.split())
                continue
            if continued and current is not None and line.strip():
                slots.setdefault(current, []).append(line.split())
                continued = False
    return slots


def _normalise_mnemonic(mnemonic):
    """Drop the notation the two disassemblers spell differently.

    The saturate suffix rides on the operands here and on the mnemonic there,
    the predicate prefix is a separate token, and Xenia capitalises tfetchCube.
    None of the three is a decode disagreement.
    """
    mnemonic = mnemonic.lower()
    if mnemonic.endswith("_sat"):
        mnemonic = mnemonic[:-4]
    return mnemonic


def _mnemonics(token_lists):
    """Mnemonic per printed half, with notation and empty slots removed."""
    out = []
    for tokens in token_lists:
        # A predicate prefix, "(p0)" or "(!p0)", is its own token on both sides.
        tokens = [t for t in tokens if not t.startswith("(")]
        if tokens:
            out.append(_normalise_mnemonic(tokens[0]))
    return out


def run_xenia_mode(xenia_dir, paths):
    """Diff this decoder against Xenia's disassembly of the same microcode.

    --verify only ever compared this file to the renderer's C++ decoder, so a
    mistake both of them made agreed with itself. Xenia is an independent
    implementation, and its shader dump carries the raw microcode alongside the
    listing, so pairing is exact rather than by name: SHA-1 over the bytes.
    Xenia writes them little-endian, the renderer's dumps are big-endian hex.
    """
    index = {}
    for entry in sorted(glob.glob(os.path.join(xenia_dir, "*.ucode.bin.*"))):
        listing = entry.replace(".ucode.bin.", ".ucode.")
        if not os.path.exists(listing):
            continue
        # A dump directory can be written while this runs, so a path from the
        # glob is not a promise the file is still there.
        try:
            with open(entry, "rb") as handle:
                index[hashlib.sha1(handle.read()).hexdigest()] = listing
        except (IOError, OSError):
            continue

    print("Diff against Xenia's disassembly")
    print("  %d Xenia listings indexed from %s" % (len(index), xenia_dir))

    paired = unpaired = 0
    divergent = []
    for path in sorted(paths):
        try:
            dwords = load_dwords(path)
        except (IOError, OSError, NoMicrocodeError, struct.error):
            continue
        if not dwords:
            continue
        digest = hashlib.sha1(
            b"".join(struct.pack("<I", w & 0xFFFFFFFF) for w in dwords)
        ).hexdigest()
        listing = index.get(digest)
        if listing is None:
            unpaired += 1
            continue
        paired += 1

        try:
            shader = decode_shader(dwords)
        except DecodeError as exc:
            divergent.append((path, listing, ["decode failed: %s" % exc]))
            continue

        theirs = parse_xenia_listing(listing)
        notes = []
        for address, is_fetch, instruction in shader.instructions:
            printed = _mnemonics(theirs.get(address, []))
            if not printed:
                continue
            if is_fetch:
                # Strip a leading predicate prefix before taking the mnemonic.
                # format_fetch emits "(p0) tfetch2D ..." for a predicated fetch,
                # so .split()[0] used to yield "(p0)" and every predicated fetch
                # was reported as a divergence against Xenia -- which puts the
                # predicate in its own column. That was the ONLY FAIL over a
                # 108-shader corpus on 2026-08-26, and it was ours, not a real
                # disagreement: both sides read instruction 20 of ps_215F16A0
                # as tfetch2D. A permanent false FAIL is worse than none,
                # because the next real divergence gets waved off as the known
                # one.
                words = format_fetch(instruction).split()
                if words and words[0].startswith("(") and len(words) > 1:
                    words = words[1:]
                mine = [_normalise_mnemonic(words[0])]
                if mine != printed:
                    notes.append("  %4d  fetch mine=%-22s xenia=%s"
                                 % (address, mine[0], "+".join(printed)))
                continue

            # Xenia prints one line per half but omits a half it considers
            # idle, and the surviving line is not tagged, so position cannot say
            # which half it is. Classify each printed mnemonic against THIS
            # instruction's two decoded opcode names instead, and compare only
            # the halves Xenia actually printed. What is being checked is the
            # decode; whether either disassembler chooses to print an idle half
            # is presentation, and the two disagree about that harmlessly.
            vname = _normalise_mnemonic(instruction.vector_opcode_name)
            sname = _normalise_mnemonic(instruction.scalar_opcode_name)
            unclaimed = []
            for mnemonic in printed:
                if mnemonic == vname or mnemonic == sname:
                    continue
                unclaimed.append(mnemonic)
            for mnemonic in unclaimed:
                notes.append("  %4d  xenia=%-22s mine: vector=%s scalar=%s"
                             % (address, mnemonic, vname, sname))

            # Printing policy is presentation, with one exception worth a guard:
            # if Xenia shows both halves and we show fewer lines, we are hiding
            # a half it considers live. That is the exact shape of the mask-less
            # scalar bug this mode was written to catch, so it stays checked.
            shown = len(_mnemonics(line.split() for line in
                                   format_alu(instruction)))
            if len(printed) > shown:
                notes.append("  %4d  xenia prints %d half/halves, we print %d"
                             % (address, len(printed), shown))
        if notes:
            divergent.append((path, listing, notes))

    print("  %d paired, %d with no Xenia counterpart" % (paired, unpaired))
    # Nothing compared is not the same as nothing wrong. Reporting "ok" over an
    # empty set is the false all-clear this whole mode exists to prevent.
    if not paired:
        print("  FAIL  nothing to compare -- no dump paired with a Xenia "
              "listing. Check the directory is a Xenia shader dump and is "
              "not mid-write.")
        return 1
    if not divergent:
        print("  ok    every paired shader agrees, slot for slot")
        return 0

    print("  FAIL  %d shader(s) diverge" % len(divergent))
    for path, listing, notes in divergent:
        print("")
        print("  %s  vs  %s" % (os.path.basename(path),
                                os.path.basename(listing)))
        for note in notes[:12]:
            print(note)
        if len(notes) > 12:
            print("        ... %d more" % (len(notes) - 12))
    return 1


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

def build_test_blob():
    """A minimal valid shader, assembled field by field.

    Deliberately built from the layout constants rather than pasted as hex, so
    the test exercises the same field offsets the decoder reads. A hardcoded
    blob would pass even if both encoder and decoder were wrong in the same way,
    which is why this also asserts against independently known values below.
    """
    # CF 0: exec addr=3 count=2, sequence = fetch then ALU (bit0 set for
    # instruction 0 only).
    exec_w0 = 3 | (2 << 12) | (0b0001 << 16)
    exec_w1 = CF_EXEC << 12
    # CF 1: exec_end addr=5 count=1, one ALU.
    end_w0 = 5 | (1 << 12)
    end_w1 = CF_EXEC_END << 12

    # Repack two 48-bit CF instructions into three dwords - the inverse of
    # unpack_control_flow.
    d0 = exec_w0
    d1 = (exec_w1 & 0xFFFF) | ((end_w0 & 0xFFFF) << 16)
    d2 = (end_w0 >> 16) | (end_w1 << 16)

    dwords = [d0, d1, d2]
    dwords += [0, 0, 0] * 2  # padding up to instruction address 3

    # Instruction 3: vfetch_full, dst r1, src r0.x, const 0, fmt 32_32_32_FLOAT,
    # stride 28 bytes (7 dwords), offset 0.
    #
    # dst_swiz 0xA88 is xyz1 (3 bits per component: x, y, z, then selector 5 =
    # constant 1). Not an invented value - src/gpu/shader_ucode.h records
    # "fmt 57 -> 0xA88" from a census of 1851 real fetches in this game, so this
    # test is decoding a swizzle the hardware actually emits.
    vf_w0 = 0 | (0 << 5) | (1 << 12) | (1 << 19) | (0 << 20)
    vf_w1 = 0xA88 | (57 << 16)
    vf_w2 = 7  # stride in dwords
    dwords += [vf_w0, vf_w1, vf_w2]

    # Instruction 4: mul r2 = r1 * c0 (vector), no scalar write.
    alu_w0 = 2 | (0b1111 << 16)
    alu_w1 = 0
    alu_w2 = (0 << 0) | (0 << 8) | (1 << 16) | (1 << 24) | (1 << 31)
    dwords += [alu_w0, alu_w1, alu_w2]

    # Instruction 5: export to register 62 (position), vector mask xyzw.
    exp_w0 = POSITION_EXPORT_REGISTER | (1 << 15) | (0b1111 << 16)
    exp_w1 = 0
    exp_w2 = (2 << 16) | (0 << 24) | (1 << 31)  # add, src1 = r2
    dwords += [exp_w0, exp_w1, exp_w2]

    return dwords


def run_selftest():
    failures = []

    def check(name, condition, detail=""):
        if condition:
            print("  ok    %s" % name)
        else:
            print("  FAIL  %s %s" % (name, detail))
            failures.append(name)

    print("Self-test")

    # -- swizzle is component-relative, not absolute -----------------------
    check("swizzle 0 is identity", swizzle_str(0) == "")
    # xxxx means every component reads x: component c needs (s>>2c)+c == 0.
    xxxx = (0 & 3) | (((0 - 1) & 3) << 2) | (((0 - 2) & 3) << 4) \
        | (((0 - 3) & 3) << 6)
    check("swizzle xxxx round-trips", swizzle_str(xxxx) == ".xxxx",
          swizzle_str(xxxx))

    # -- CF pack/unpack round-trip ----------------------------------------
    blob = build_test_blob()
    a, b = unpack_control_flow(blob, 0)
    check("CF[0] is exec", a.opcode == CF_EXEC, a.opcode_name)
    check("CF[0] addr/count", a.address == 3 and a.count == 2,
          "addr=%d count=%d" % (a.address, a.count))
    check("CF[0] sequence splits fetch/ALU",
          a.is_fetch(0) and not a.is_fetch(1))
    check("CF[1] is exec_end and ends shader",
          b.opcode == CF_EXEC_END and b.ends_shader, b.opcode_name)

    # -- full decode -------------------------------------------------------
    try:
        shader = decode_shader(blob)
    except DecodeError as exc:
        check("decode succeeds", False, str(exc))
        shader = None

    if shader:
        check("one vertex fetch", shader.vertex_fetches == 1,
              str(shader.vertex_fetches))
        check("no texture fetch", shader.texture_fetches == 0)
        check("two ALU instructions", shader.alu_count == 2,
              str(shader.alu_count))
        check("classified as vertex", shader.kind == "vertex", shader.kind)
        check("exports position", shader.exports_position,
              str(sorted(shader.exports)))
        fetch = shader.instructions[0][2]
        check("vfetch stride is 28 bytes", fetch.vf_stride * 4 == 28,
              str(fetch.vf_stride * 4))
        check("vfetch format is 32_32_32_FLOAT",
              VERTEX_FORMATS.get(fetch.vf_format) == "32_32_32_FLOAT",
              str(fetch.vf_format))
        check("vfetch dst swizzle 0xA88 decodes to xyz1",
              fetch_dst_swizzle_str(fetch.vf_dst_swiz) == ".xyz1",
              fetch_dst_swizzle_str(fetch.vf_dst_swiz))
        check("vfetch is normalized", fetch.vf_is_normalized)
        print("")
        for line in disassemble(shader):
            print("    " + line)
        print("")

    # -- rejection: the checks that stop false positives --------------------
    def rejects(name, dwords):
        try:
            decode_shader(dwords)
        except DecodeError:
            check(name, True)
        else:
            check(name, False, "accepted")

    rejects("rejects all-zero blob", [0] * 64)
    rejects("rejects too-short blob", blob[:6])
    rejects("rejects 0xFFFFFFFF fill", [0xFFFFFFFF] * 64)

    # PowerPC text is the exact input the previous version misread. A handful of
    # real instructions from the game's entry region: if any of these decode as
    # a shader, the validation is not doing its job.
    ppc = [
        0x7C0802A6, 0x9421FFB0, 0x93E1004C, 0x7C7F1B78, 0x90010054,
        0x38600000, 0x4BFFFF71, 0x807F0008, 0x2F800000, 0x419E0010,
        0x80630004, 0x4800001C, 0x38210050, 0x80010054, 0x7C0803A6,
        0x83E1004C, 0x4E800020, 0x60000000, 0x7C0802A6, 0x9421FF80,
    ] * 4
    rejects("rejects PowerPC code", ppc)

    print("")
    if failures:
        print("%d check(s) failed: %s" % (len(failures), ", ".join(failures)))
        return 1
    print("All checks passed.")
    return 0


# ---------------------------------------------------------------------------
# Entry
# ---------------------------------------------------------------------------

def in_ida():
    try:
        import idaapi  # noqa: F401
    except ImportError:
        return False
    return True


def main():
    args = [a for a in sys.argv[1:]]

    if "--selftest" in args:
        return run_selftest()

    if "--verify" in args:
        args.remove("--verify")
        return run_verify_mode([a for a in args if not a.startswith("-")])

    if "--xenia" in args:
        at = args.index("--xenia")
        if at + 1 >= len(args):
            print("--xenia needs the directory holding Xenia's shader dump")
            return 1
        xenia_dir = args[at + 1]
        del args[at:at + 2]
        targets = [a for a in args if not a.startswith("-")]
        if not targets:
            targets = glob.glob(os.path.join("logs", "hlsldump", "*.txt"))
        return run_xenia_mode(xenia_dir, targets)

    if "--scan-file" in args:
        args.remove("--scan-file")
        little = "--little-endian" in args
        targets = [a for a in args if not a.startswith("-")]
        if not targets:
            print("--scan-file needs a path")
            return 1
        for target in targets:
            run_scan_file_mode(target, big_endian=not little)
        return 0

    big_endian = None
    if "--little-endian" in args:
        args.remove("--little-endian")
        big_endian = False
    if "--big-endian" in args:
        args.remove("--big-endian")
        big_endian = True

    paths = [a for a in args if not a.startswith("-")]
    if paths:
        return run_blob_mode(paths, big_endian)

    print(__doc__.strip())
    print("")
    print("No input given. Run --selftest, pass a dump file, or run this "
          "inside IDA to scan.")
    return 0


if __name__ == "__main__":
    if in_ida():
        if STRIP:
            run_strip_mode()
        else:
            run_scan_mode()
    else:
        sys.exit(main())
