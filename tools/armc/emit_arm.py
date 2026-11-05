"""C for 32-bit ARM instructions. Mixed into the function writer; uses helpers from emit_base."""

from .bits import c_hex, rotate_right
from .decode import classify_arm
from .emit_base import CONDITION_C, indent

# Data-processing opcodes: C expression of the result without flags, and the runtime call that
# also sets the flags. {n} is the first operand, {m} the second one.
DP_VALUE = {
    0: "{n} & {m}", 1: "{n} ^ {m}", 2: "{n} - {m}", 3: "{m} - {n}",
    4: "{n} + {m}", 5: "{n} + {m} + c->c", 6: "{n} - {m} - (c->c ^ 1)",
    7: "{m} - {n} - (c->c ^ 1)", 8: "{n} & {m}", 9: "{n} ^ {m}",
    12: "{n} | {m}", 13: "{m}", 14: "{n} & ~{m}", 15: "~{m}",
}
DP_WITH_FLAGS = {
    2: "sub_f(c, {n}, {m})", 3: "sub_f(c, {m}, {n})", 4: "add_f(c, {n}, {m})",
    5: "adc_f(c, {n}, {m})", 6: "sbc_f(c, {n}, {m})", 7: "sbc_f(c, {m}, {n})",
    10: "sub_f(c, {n}, {m})", 11: "add_f(c, {n}, {m})",
}
LOGICAL_OPCODES = (0, 1, 8, 9, 12, 13, 14, 15)  # these set N and Z from the result and C from the shifter
SHIFT_HELPERS = ["lsl_c", "lsr_c", "asr_c", "ror_c"]
HALFWORD_LOADS = {1: "rd16(ad)", 2: "(uint32_t)(int32_t)(int8_t)rd8(ad)",
                  3: "(uint32_t)(int32_t)(int16_t)rd16(ad)"}
BLOCK_START = {(0, 1): "base", (1, 1): "base + 4"}  # (P, U) -> first address; the others depend on the count


def _bit(word, n):
    return (word >> n) & 1


def _reg_field(word, low):
    return (word >> low) & 15


class ArmEmitter:
    def arm_ins(self, addr, step):
        return self.with_patch(addr, self._arm_plain(addr, step))

    def _arm_plain(self, addr, step):
        word = self.img.word(addr)
        cond = word >> 28
        if cond == 0xF:
            if step.kind == "call":
                return self.emit_call(step, addr + 4)
            if step.kind == "undef":
                return [f"cpu_undefined(c, {c_hex(addr)}, {c_hex(word)});"]
            return ["/* pld */"]
        body = getattr(self, "_arm_" + classify_arm(word))(addr, word, step)
        if cond == 0xE:
            return body
        return [f"if ({CONDITION_C[cond]}) {{"] + indent(body) + ["}"]

    def shifted_operand(self, word, pc_value):
        """Operand 2 "register shifted by an immediate" -> (C expression, carry-out expression or None)."""
        rm, kind, amount = word & 15, (word >> 5) & 3, (word >> 7) & 31
        m = self.reg(rm, pc_value)
        if kind == 0:  # lsl
            if amount == 0:
                return (m, None)
            return (f"({m} << {amount})", f"(({m} >> {32 - amount}) & 1)")
        if kind == 1:  # lsr; an amount of 0 means 32
            if amount == 0:
                return ("0u", f"({m} >> 31)")
            return (f"({m} >> {amount})", f"(({m} >> {amount - 1}) & 1)")
        if kind == 2:  # asr; an amount of 0 means 32
            if amount == 0:
                return f"(uint32_t)((int32_t){m} >> 31)", f"({m} >> 31)"
            return f"(uint32_t)((int32_t){m} >> {amount})", f"(({m} >> {amount - 1}) & 1)"
        if amount == 0:  # rrx
            return f"((c->c << 31) | ({m} >> 1))", f"({m} & 1)"
        return f"ror32({m}, {amount})", f"(({m} >> {amount - 1}) & 1)"

    def _arm_dp(self, addr, word, step):
        opcode, sets_flags = (word >> 21) & 15, _bit(word, 20)
        rn, rd = _reg_field(word, 16), _reg_field(word, 12)
        shift_by_reg = not _bit(word, 25) and _bit(word, 4)
        pc_value = addr + 12 if shift_by_reg else addr + 8
        logical = opcode in LOGICAL_OPCODES
        lines = ["{"]
        carry = None
        if _bit(word, 25):  # rotated 8-bit immediate
            rot = ((word >> 8) & 15) * 2
            imm = rotate_right(word & 0xFF, rot)
            op2 = c_hex(imm)
            if rot:
                carry = str(imm >> 31)
        elif shift_by_reg:
            helper = SHIFT_HELPERS[(word >> 5) & 3]
            lines.append("    uint32_t cy = c->c;")
            lines.append(f"    uint32_t op2 = {helper}({self.reg(word & 15, pc_value)}, "
                         f"{self.reg((word >> 8) & 15, pc_value)} & 0xFF, &cy);")
            op2, carry = "op2", "cy"
        else:
            expr, carry_expr = self.shifted_operand(word, pc_value)
            if sets_flags and logical and carry_expr is not None:
                lines.append(f"    uint32_t cy = {carry_expr};")
                carry = "cy"
            lines.append(f"    uint32_t op2 = {expr};")
            op2 = "op2"
        n = self.reg(rn, pc_value)
        operands = {"n": n, "m": op2}
        if 8 <= opcode <= 11:  # TST TEQ CMP CMN: only the flags
            if logical:
                lines.append("    nz(c, " + DP_VALUE[opcode].format(**operands) + ");")
                if carry is not None:
                    lines.append(f"    c->c = {carry};")
            else:
                lines.append("    " + DP_WITH_FLAGS[opcode].format(**operands) + ";")
            return lines + ["}"]
        if sets_flags and logical:
            lines.append("    uint32_t res = nz(c, " + DP_VALUE[opcode].format(**operands) + ");")
            if carry is not None:
                lines.append(f"    c->c = {carry};")
        elif sets_flags:
            lines.append("    uint32_t res = " + DP_WITH_FLAGS[opcode].format(**operands) + ";")
        else:
            lines.append("    uint32_t res = " + DP_VALUE[opcode].format(**operands) + ";")
        if rd == 15:
            if sets_flags:
                lines.append("    cpu_restore_spsr(c);")
            lines += indent(self.emit_pc_write(addr, step, "res", False))
        else:
            lines.append(f"    R[{rd}] = res;")
        return lines + ["}"]

    def _arm_sdt(self, addr, word, step):
        reg_offset, pre, up = _bit(word, 25), _bit(word, 24), _bit(word, 23)
        is_byte, writeback_bit, is_load = _bit(word, 22), _bit(word, 21), _bit(word, 20)
        rn, rd = _reg_field(word, 16), _reg_field(word, 12)
        offset = self.shifted_operand(word, addr + 8)[0] if reg_offset else c_hex(word & 0xFFF)
        sign = "+" if up else "-"
        lines = ["{", f"    uint32_t base = {self.reg(rn, addr + 8)};", f"    uint32_t off = {offset};",
                 f"    uint32_t ad = {'base ' + sign + ' off' if pre else 'base'};"]
        writeback = (not pre) or writeback_bit
        if is_load:
            lines.append(f"    uint32_t v = {'rd8(ad)' if is_byte else 'ldr32(ad)'};")
            if writeback and rn != rd:
                lines.append(f"    R[{rn}] = base {sign} off;")
            if rd == 15:
                lines += indent(self.emit_pc_write(addr, step, "v", self.finder.v5))
            else:
                lines.append(f"    R[{rd}] = v;")
        else:
            lines.append(f"    {'wr8' if is_byte else 'wr32'}(ad, {self.reg(rd, addr + 12)});")
            if writeback:
                lines.append(f"    R[{rn}] = base {sign} off;")
        return lines + ["}"]

    def _arm_hw(self, addr, word, step):
        pre, up, immediate = _bit(word, 24), _bit(word, 23), _bit(word, 22)
        writeback_bit, is_load = _bit(word, 21), _bit(word, 20)
        rn, rd, kind = _reg_field(word, 16), _reg_field(word, 12), (word >> 5) & 3
        offset = c_hex(((word >> 4) & 0xF0) | (word & 0xF)) if immediate else self.reg(word & 15, addr + 8)
        sign = "+" if up else "-"
        lines = ["{", f"    uint32_t base = {self.reg(rn, addr + 8)};", f"    uint32_t off = {offset};",
                 f"    uint32_t ad = {'base ' + sign + ' off' if pre else 'base'};"]
        writeback = (not pre) or writeback_bit
        write_back_line = f"    R[{rn}] = base {sign} off;"
        if is_load:
            lines.append(f"    uint32_t v = {HALFWORD_LOADS[kind]};")
            if writeback and rn != rd:
                lines.append(write_back_line)
            lines.append(f"    R[{rd}] = v;")
        elif kind == 1:  # STRH
            lines.append(f"    wr16(ad, {self.reg(rd, addr + 12)});")
            if writeback:
                lines.append(write_back_line)
        elif kind == 2:  # LDRD
            lines.append("    uint32_t v0 = rd32(ad), v1 = rd32(ad + 4);")
            if writeback:
                lines.append(write_back_line)
            lines += [f"    R[{rd}] = v0;", f"    R[{rd + 1}] = v1;"]
        else:  # STRD
            lines += [f"    wr32(ad, R[{rd}]);", f"    wr32(ad + 4, R[{rd + 1}]);"]
            if writeback:
                lines.append(write_back_line)
        return lines + ["}"]

    def _arm_ldm(self, addr, word, step):
        pre, up, user_bit, writeback_bit, is_load = (_bit(word, 24), _bit(word, 23), _bit(word, 22),
                                                     _bit(word, 21), _bit(word, 20))
        rn = _reg_field(word, 16)
        regs = [r for r in range(16) if (word >> r) & 1]
        count = len(regs)
        if count == 0:
            return [f"cpu_undefined(c, {c_hex(addr)}, {c_hex(word)});"]
        start = BLOCK_START.get((pre, up)) or (f"base - {4 * count - 4}" if not pre else f"base - {4 * count}")
        new_base = f"base + {4 * count}" if up else f"base - {4 * count}"
        lines = ["{", f"    uint32_t base = R[{rn}];", f"    uint32_t ad = {start};"]
        user_regs = user_bit and not (is_load and 15 in regs)  # "^" without pc: transfer the user mode registers

        def slot(r):
            return f"*cpu_usr(c, {r})" if user_regs and r >= 8 else f"R[{r}]"

        if is_load:
            for k, r in enumerate(regs):
                where = "ad" if k == 0 else f"ad + {4 * k}"
                lines.append(f"    uint32_t npc = rd32({where});" if r == 15 else f"    {slot(r)} = rd32({where});")
            if writeback_bit and not (rn in regs and regs[-1] == rn and count > 1):
                lines.append(f"    R[{rn}] = {new_base};")
            if 15 in regs:
                if user_bit:
                    lines.append("    cpu_restore_spsr(c);")
                lines += indent(self.emit_pc_write(addr, step, "npc", self.finder.v5))
        else:
            for k, r in enumerate(regs):
                where = "ad" if k == 0 else f"ad + {4 * k}"
                if r == 15:
                    value = c_hex(addr + 12)
                elif r == rn:
                    value = "base" if k == 0 else f"({new_base})"
                else:
                    value = slot(r)
                lines.append(f"    wr32({where}, {value});")
            if writeback_bit:
                lines.append(f"    R[{rn}] = {new_base};")
        return lines + ["}"]

    def _arm_b(self, addr, word, step):
        if step.kind == "call":
            return self.emit_call(step, addr + 4)
        return self.emit_branch(step, addr)

    def _arm_bx(self, addr, word, step):
        rm = word & 15
        if step.kind == "tail":
            return self.emit_tail(step)
        if step.kind == "callind":  # "mov lr, pc" before it already set the return address
            return ["{", f"    uint32_t t = R[{rm}];", f"    {self.disp}(c, t);", "}"]
        return self.emit_computed_jump(f"R[{rm}]", True)

    def _arm_blxr(self, addr, word, step):
        return self.emit_computed_call(f"R[{word & 15}]", addr + 4)

    def _arm_clz(self, addr, word, step):
        return [f"R[{_reg_field(word, 12)}] = clz32(R[{word & 15}]);"]

    def _arm_qarith(self, addr, word, step):
        op = (word >> 21) & 3
        rn, rd, rm = _reg_field(word, 16), _reg_field(word, 12), word & 15
        expr = {0: f"qadd(c, R[{rm}], R[{rn}])", 1: f"qsub(c, R[{rm}], R[{rn}])",
                2: f"qadd(c, R[{rm}], qadd(c, R[{rn}], R[{rn}]))",
                3: f"qsub(c, R[{rm}], qadd(c, R[{rn}], R[{rn}]))"}[op]
        return [f"R[{rd}] = {expr};"]

    def _arm_smulxy(self, addr, word, step):
        op = (word >> 21) & 3
        rd, rn, rs, rm = _reg_field(word, 16), _reg_field(word, 12), _reg_field(word, 8), word & 15
        x, y = _bit(word, 5), _bit(word, 6)

        def half(r, top):
            return f"(int32_t)(int16_t)(R[{r}] >> 16)" if top else f"(int32_t)(int16_t)R[{r}]"

        if op == 0:  # SMLAxy (games never use the Q flag)
            return [f"R[{rd}] = (uint32_t)({half(rm, x)} * {half(rs, y)}) + R[{rn}];"]
        if op == 1:  # SMLAWy / SMULWy
            product = f"(uint32_t)(((int64_t)(int32_t)R[{rm}] * {half(rs, y)}) >> 16)"
            return [f"R[{rd}] = {product};"] if x else [f"R[{rd}] = {product} + R[{rn}];"]
        if op == 2:  # SMLALxy: RdHi = rd, RdLo = rn
            return ["{", f"    uint64_t acc = ((uint64_t)R[{rd}] << 32) | R[{rn}];",
                    f"    acc += (uint64_t)(int64_t)({half(rm, x)} * {half(rs, y)});",
                    f"    R[{rn}] = (uint32_t)acc; R[{rd}] = (uint32_t)(acc >> 32);", "}"]
        return [f"R[{rd}] = (uint32_t)({half(rm, x)} * {half(rs, y)});"]  # SMULxy

    def _arm_swp(self, addr, word, step):
        rn, rd, rm, is_byte = _reg_field(word, 16), _reg_field(word, 12), word & 15, _bit(word, 22)
        return ["{", f"    uint32_t ad = R[{rn}];",
                f"    uint32_t v = {'rd8(ad)' if is_byte else 'ldr32(ad)'};",
                f"    {'wr8' if is_byte else 'wr32'}(ad, R[{rm}]);", f"    R[{rd}] = v;", "}"]

    def _arm_mul(self, addr, word, step):
        accumulate, sets_flags = _bit(word, 21), _bit(word, 20)
        rd, rn, rs, rm = _reg_field(word, 16), _reg_field(word, 12), _reg_field(word, 8), word & 15
        expr = f"R[{rm}] * R[{rs}]" + (f" + R[{rn}]" if accumulate else "")
        return [f"R[{rd}] = nz(c, {expr});" if sets_flags else f"R[{rd}] = {expr};"]

    def _arm_mull(self, addr, word, step):
        signed, accumulate, sets_flags = _bit(word, 22), _bit(word, 21), _bit(word, 20)
        hi, lo, rs, rm = _reg_field(word, 16), _reg_field(word, 12), _reg_field(word, 8), word & 15
        lines = ["{"]
        if signed:
            lines.append(f"    uint64_t p = (uint64_t)((int64_t)(int32_t)R[{rm}] * (int64_t)(int32_t)R[{rs}]);")
        else:
            lines.append(f"    uint64_t p = (uint64_t)R[{rm}] * R[{rs}];")
        if accumulate:
            lines.append(f"    p += ((uint64_t)R[{hi}] << 32) | R[{lo}];")
        lines.append(f"    R[{lo}] = (uint32_t)p; R[{hi}] = (uint32_t)(p >> 32);")
        if sets_flags:
            lines.append("    c->n = (uint32_t)(p >> 63); c->z = p == 0;")
        return lines + ["}"]

    def _arm_mrs(self, addr, word, step):
        rd = _reg_field(word, 12)
        return [f"R[{rd}] = {'cpu_get_spsr(c)' if _bit(word, 22) else 'cpu_get_cpsr(c)'};"]

    def _arm_msr(self, addr, word, step):
        value = c_hex(rotate_right(word & 0xFF, ((word >> 8) & 15) * 2)) if _bit(word, 25) else f"R[{word & 15}]"
        return [f"cpu_msr(c, {_bit(word, 22)}, {_reg_field(word, 16)}, {value});"]

    def _arm_mcr(self, addr, word, step):
        if _reg_field(word, 8) != 15:
            return [f"/* coprocessor {_reg_field(word, 8)} */"]
        op1, crn, rd = (word >> 21) & 7, _reg_field(word, 16), _reg_field(word, 12)
        crm, op2 = word & 15, (word >> 5) & 7
        if _bit(word, 20):
            if rd == 15:
                return [f"(void)cp15_read(c, {crn}, {crm}, {op1}, {op2});"]
            return [f"R[{rd}] = cp15_read(c, {crn}, {crm}, {op1}, {op2});"]
        return [f"cp15_write(c, {crn}, {crm}, {op1}, {op2}, {self.reg(rd, addr + 12)});"]

    def _arm_cop(self, addr, word, step):
        return ["/* coprocessor */"]

    def _arm_swi(self, addr, word, step):
        return [f"bios_swi(c, {c_hex((word >> 16) & 0xFF)});"]

    def _arm_undef(self, addr, word, step):
        return [f"cpu_undefined(c, {c_hex(addr)}, {c_hex(word)});"]
