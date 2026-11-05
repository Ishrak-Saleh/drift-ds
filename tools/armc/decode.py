"""First pass over machine code: what does one instruction do to the program counter?

Each decoded instruction becomes a Step. The kind tells the function finder how control moves on:
  seq      runs into the next instruction
  branch   jumps to a fixed place in the same function
  call     calls a fixed target
  callind  calls through a register or memory (the return address is set by "mov lr, pc" before it)
  ret      returns to the caller
  jumpind  jumps to a computed address (switch table, function pointer)
  tail     leaves for another function without coming back
  undef    not valid code; execution cannot continue here
"""

from .bits import MASK32, sign_extend

MOV_LR_PC = 0xE1A0E00F  # the instruction compilers put in front of a computed call


class Step:
    __slots__ = ("width", "kind", "conditional", "target", "target_thumb", "literal", "is_tail")

    def __init__(self, width):
        self.width = width
        self.kind = "seq"
        self.conditional = False
        self.target = None
        self.target_thumb = False
        self.literal = None  # address of a constant the instruction loads relative to pc
        self.is_tail = False  # a branch that is emitted as a tail call

    def falls_through(self):
        """Can execution continue with the next instruction?"""
        if self.kind in ("seq", "call", "callind"):
            return True
        if self.kind == "undef":
            return False
        return self.conditional


# ---------------------------------------------------------------------------
# ARM instruction classes
# ---------------------------------------------------------------------------

# (mask, value, class) rules for instructions whose bits 27-25 are 000, in priority order.
_DATA_FORM_RULES = (
    (0x0FFFFFF0, 0x012FFF10, "bx"),
    (0x0FFFFFF0, 0x012FFF30, "blxr"),
    (0x0FFF0FF0, 0x016F0F10, "clz"),
    (0x0F900FF0, 0x01000050, "qarith"),
    (0x0F900090, 0x01000080, "smulxy"),
    (0x0FB00FF0, 0x01000090, "swp"),
    (0x0FC000F0, 0x00000090, "mul"),
    (0x0F8000F0, 0x00800090, "mull"),
)
# Checked after the halfword transfer test.
_STATUS_RULES_000 = (
    (0x0FBF0FFF, 0x010F0000, "mrs"),
    (0x0FB0FFF0, 0x0120F000, "msr"),
    (0x01900000, 0x01000000, "undef"),
)
_STATUS_RULES_001 = (
    (0x0FB0F000, 0x0320F000, "msr"),
    (0x01900000, 0x01000000, "undef"),
)


def _first_match(word, rules, default):
    for mask, value, name in rules:
        if word & mask == value:
            return name
    return default


def classify_arm(word):
    form = (word >> 25) & 7
    if form == 0:
        name = _first_match(word, _DATA_FORM_RULES, None)
        if name:
            return name
        if word & 0x90 == 0x90 and word & 0x60:
            return "hw"
        return _first_match(word, _STATUS_RULES_000, "dp")
    if form == 1:
        return _first_match(word, _STATUS_RULES_001, "dp")
    if form == 2:
        return "sdt"
    if form == 3:
        return "undef" if word & 0x10 else "sdt"
    if form == 4:
        return "ldm"
    if form == 5:
        return "b"
    if form == 6:
        return "cop"
    if (word >> 24) & 15 == 0xF:
        return "swi"
    return "mcr" if word & 0x10 else "cop"


def _indirect_kind(after_mov_lr):
    return "callind" if after_mov_lr else "jumpind"


def decode_arm(img, addr):
    word = img.word(addr)
    cond = word >> 28
    step = Step(4)
    step.conditional = cond < 0xE
    if cond == 0xF:  # unconditional space: BLX with an immediate, PLD, and invalid codes
        if (word >> 25) & 7 == 5:
            step.kind = "call"
            half_bit = ((word >> 24) & 1) << 1
            step.target = (addr + 8 + (sign_extend(word, 24) << 2) + half_bit) & MASK32
            step.target_thumb = True
        elif word & 0xFD70F000 != 0xF550F000:  # everything except PLD
            step.kind = "undef"
        return step

    cls = classify_arm(word)
    after_mov_lr = img.has(addr - 4, 4) and img.word(addr - 4) == MOV_LR_PC

    if cls == "bx":
        rm = word & 15
        if rm == 15:
            step.kind, step.target, step.target_thumb = "tail", addr + 8, False
        elif rm == 14:
            step.kind = "ret"
        else:
            step.kind = _indirect_kind(after_mov_lr)
    elif cls == "blxr":
        step.kind = "callind"
    elif cls == "b":
        step.target = (addr + 8 + (sign_extend(word, 24) << 2)) & MASK32
        step.kind = "call" if (word >> 24) & 1 else "branch"
    elif cls == "ldm":
        loads_pc = (word >> 20) & 1 and (word >> 15) & 1
        if loads_pc:
            step.kind = "ret" if (word >> 16) & 15 == 13 else _indirect_kind(after_mov_lr)
    elif cls == "sdt":
        rn, rd, is_load = (word >> 16) & 15, (word >> 12) & 15, (word >> 20) & 1
        if is_load and rn == 15 and not (word >> 25) & 1:
            offset = word & 0xFFF
            step.literal = addr + 8 + (offset if (word >> 23) & 1 else -offset)
        if is_load and rd == 15:
            if step.literal is not None and img.has(step.literal, 4):
                value = img.word(step.literal)
                step.kind = "call" if after_mov_lr else "tail"
                step.target = value & ~1 if value & 1 else value & ~3
                step.target_thumb = bool(value & 1)
            elif rn == 13 and not (word >> 24) & 1:
                step.kind = "ret"  # ldr pc, [sp], #4
            else:
                step.kind = _indirect_kind(after_mov_lr)
    elif cls == "dp":
        opcode, rd = (word >> 21) & 15, (word >> 12) & 15
        writes_pc = rd == 15 and not 8 <= opcode <= 11  # TST, TEQ, CMP, CMN never write rd
        if writes_pc:
            is_mov_pc_lr = opcode == 13 and not (word >> 25) & 1 and word & 0xFFF == 14
            step.kind = "ret" if is_mov_pc_lr else _indirect_kind(after_mov_lr)
    elif cls == "undef":
        step.kind = "undef"
    return step


# ---------------------------------------------------------------------------
# Thumb
# ---------------------------------------------------------------------------

def _pops_register(img, addr, reg):
    """Is the instruction before addr a "pop {..}" that includes the given low register?"""
    if not img.has(addr - 2, 2):
        return False
    prev = img.half(addr - 2)
    return prev & 0xFF00 == 0xBC00 and (prev >> reg) & 1 == 1


def decode_thumb(img, addr):
    word = img.half(addr)
    step = Step(2)
    if word & 0xFF00 == 0x4700:  # BX / BLX with a register
        rm = (word >> 3) & 15
        if word & 0x80:
            step.kind = "callind"
        elif rm == 15:
            step.kind, step.target, step.target_thumb = "tail", (addr + 4) & ~3, False
        elif rm == 14:
            step.kind = "ret"
        elif rm < 8 and _pops_register(img, addr, rm):
            step.kind = "ret"  # "pop {rX}; bx rX" is how the Nintendo compiler returns in Thumb
        else:
            step.kind = "jumpind"
    elif word & 0xFC00 == 0x4400 and ((word >> 8) & 3) in (0, 2) and ((word & 7) | ((word >> 4) & 8)) == 15:
        # "add pc, rX" or "mov pc, rX"
        rm = (word >> 3) & 15
        step.kind = "ret" if (word >> 8) & 3 == 2 and rm == 14 else "jumpind"
    elif word & 0xFF00 == 0xBD00:  # pop {..., pc}
        step.kind = "ret"
    elif word & 0xF800 == 0x4800:  # ldr rX, [pc, #n]
        step.literal = ((addr + 4) & ~3) + (word & 0xFF) * 4
    elif word & 0xF000 == 0xD000:  # conditional branch (or swi, or an invalid code)
        cond = (word >> 8) & 15
        if cond == 0xE:
            step.kind = "undef"
        elif cond != 0xF:
            step.kind, step.conditional = "branch", True
            step.target = (addr + 4 + sign_extend(word, 8) * 2) & MASK32
    elif word & 0xF800 == 0xE000:  # unconditional branch
        step.kind = "branch"
        step.target = (addr + 4 + sign_extend(word, 11) * 2) & MASK32
    elif word & 0xF800 == 0xF000:  # first half of a BL / BLX pair
        if img.has(addr + 2, 2):
            second = img.half(addr + 2)
            high = sign_extend(word, 11) << 12
            if second & 0xF800 == 0xF800:
                step.width, step.kind, step.target_thumb = 4, "call", True
                step.target = (addr + 4 + high + ((second & 0x7FF) << 1)) & MASK32
            elif second & 0xF801 == 0xE800:
                step.width, step.kind, step.target_thumb = 4, "call", False
                step.target = (addr + 4 + high + ((second & 0x7FF) << 1)) & ~3 & MASK32
            else:
                step.kind = "undef"
        else:
            step.kind = "undef"
    elif word & 0xF000 == 0xF000 or word & 0xF800 == 0xE800 or word & 0xFF00 == 0xBE00:
        step.kind = "undef"
    return step
