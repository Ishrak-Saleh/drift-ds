"""C for 16-bit Thumb instructions. Mixed into the function writer; uses helpers from emit_base."""

from .bits import c_hex
from .emit_base import CONDITION_C, indent

THUMB_SHIFT_HELPERS = {2: "lsl_c", 3: "lsr_c", 4: "asr_c", 7: "ror_c"}
REGISTER_OFFSET_ACCESS = {
    0: "wr32({ad}, R[{rd}]);", 1: "wr16({ad}, R[{rd}]);", 2: "wr8({ad}, R[{rd}]);",
    3: "R[{rd}] = (uint32_t)(int32_t)(int8_t)rd8({ad});",
    4: "R[{rd}] = ldr32({ad});", 5: "R[{rd}] = rd16({ad});", 6: "R[{rd}] = rd8({ad});",
    7: "R[{rd}] = (uint32_t)(int32_t)(int16_t)rd16({ad});",
}


class ThumbEmitter:
    def thumb_ins(self, addr, step):
        return self.with_patch(addr, self._thumb_plain(addr, step))

    def _thumb_plain(self, addr, step):
        word = self.img.half(addr)
        if step.kind == "undef":
            return [f"cpu_undefined(c, {c_hex(addr)}, {c_hex(word)});"]
        # Thumb instruction formats are told apart by value ranges; the first bound that is
        # above the word picks the handler.
        for bound, handler in self._THUMB_FORMATS:
            if word < bound:
                return handler(self, addr, word, step)
        return self._thumb_long_branch(addr, word, step)

    def _thumb_shift_imm(self, addr, word, step):
        op, amount, rs, rd = (word >> 11) & 3, (word >> 6) & 31, (word >> 3) & 7, word & 7
        if op == 0:  # lsl
            if amount == 0:
                return [f"R[{rd}] = nz(c, R[{rs}]);"]
            return ["{", f"    uint32_t v = R[{rs}];", f"    c->c = (v >> {32 - amount}) & 1;",
                    f"    R[{rd}] = nz(c, v << {amount});", "}"]
        if op == 1:  # lsr; an amount of 0 means 32
            if amount == 0:
                return ["{", f"    c->c = R[{rs}] >> 31;", f"    R[{rd}] = nz(c, 0);", "}"]
            return ["{", f"    uint32_t v = R[{rs}];", f"    c->c = (v >> {amount - 1}) & 1;",
                    f"    R[{rd}] = nz(c, v >> {amount});", "}"]
        shift = 32 if amount == 0 else amount  # asr
        return ["{", f"    uint32_t v = R[{rs}];",
                f"    c->c = ((uint32_t)((int32_t)v >> {shift - 1})) & 1;",
                f"    R[{rd}] = nz(c, (uint32_t)((int32_t)v >> {min(shift, 31)}));", "}"]

    def _thumb_add_sub(self, addr, word, step):
        immediate, subtract = (word >> 10) & 1, (word >> 9) & 1
        rn, rs, rd = (word >> 6) & 7, (word >> 3) & 7, word & 7
        operand = c_hex(rn) if immediate else f"R[{rn}]"
        return [f"R[{rd}] = {'sub_f' if subtract else 'add_f'}(c, R[{rs}], {operand});"]

    def _thumb_imm8(self, addr, word, step):
        op, rd, imm = (word >> 11) & 3, (word >> 8) & 7, word & 0xFF
        return [[f"R[{rd}] = nz(c, {c_hex(imm)});", f"sub_f(c, R[{rd}], {c_hex(imm)});",
                 f"R[{rd}] = add_f(c, R[{rd}], {c_hex(imm)});",
                 f"R[{rd}] = sub_f(c, R[{rd}], {c_hex(imm)});"][op]]

    def _thumb_alu(self, addr, word, step):
        op, rs, rd = (word >> 6) & 15, (word >> 3) & 7, word & 7
        d, s = f"R[{rd}]", f"R[{rs}]"
        if op in THUMB_SHIFT_HELPERS:
            helper = THUMB_SHIFT_HELPERS[op]
            return ["{", "    uint32_t cy = c->c;", f"    {d} = nz(c, {helper}({d}, {s} & 0xFF, &cy));",
                    "    c->c = cy;", "}"]
        return [{0: f"{d} = nz(c, {d} & {s});", 1: f"{d} = nz(c, {d} ^ {s});",
                 5: f"{d} = adc_f(c, {d}, {s});", 6: f"{d} = sbc_f(c, {d}, {s});",
                 8: f"nz(c, {d} & {s});", 9: f"{d} = sub_f(c, 0, {s});",
                 10: f"sub_f(c, {d}, {s});", 11: f"add_f(c, {d}, {s});",
                 12: f"{d} = nz(c, {d} | {s});", 13: f"{d} = nz(c, {d} * {s});",
                 14: f"{d} = nz(c, {d} & ~{s});", 15: f"{d} = nz(c, ~{s});"}[op]]

    def _thumb_hi_registers(self, addr, word, step):
        pc = addr + 4
        op = (word >> 8) & 3
        rs = ((word >> 3) & 7) | ((word >> 3) & 8)
        rd = (word & 7) | ((word >> 4) & 8)
        source = self.reg(rs, pc)
        if op == 0:  # add
            if rd == 15:
                return self.emit_computed_jump(f"{c_hex(pc)} + {source}", False)
            return [f"R[{rd}] = R[{rd}] + {source};"]
        if op == 1:  # cmp
            return [f"sub_f(c, {self.reg(rd, pc)}, {source});"]
        if op == 2:  # mov
            if rd == 15:
                return self.emit_computed_jump(source, False)
            return [f"R[{rd}] = {source};"]
        if word & 0x80:  # blx
            return self.emit_computed_call(source, (addr + 2) | 1)
        if step.kind == "tail":
            return self.emit_tail(step)
        return self.emit_computed_jump(source, True)

    def _thumb_ldr_pc(self, addr, word, step):
        return [f"R[{(word >> 8) & 7}] = rd32({c_hex(step.literal)});"]

    def _thumb_reg_offset(self, addr, word, step):
        op, ro, rb, rd = (word >> 9) & 7, (word >> 6) & 7, (word >> 3) & 7, word & 7
        ad = f"R[{rb}] + R[{ro}]"
        return [REGISTER_OFFSET_ACCESS[op].format(ad=ad, rd=rd)]

    def _thumb_imm_offset(self, addr, word, step):
        is_byte, is_load = (word >> 12) & 1, (word >> 11) & 1
        offset, rb, rd = (word >> 6) & 31, (word >> 3) & 7, word & 7
        ad = f"R[{rb}] + {offset if is_byte else offset * 4}"
        if is_load:
            return [f"R[{rd}] = {'rd8' if is_byte else 'ldr32'}({ad});"]
        return [f"{'wr8' if is_byte else 'wr32'}({ad}, R[{rd}]);"]

    def _thumb_halfword_imm(self, addr, word, step):
        is_load, offset, rb, rd = (word >> 11) & 1, (word >> 6) & 31, (word >> 3) & 7, word & 7
        ad = f"R[{rb}] + {offset * 2}"
        return [f"R[{rd}] = rd16({ad});"] if is_load else [f"wr16({ad}, R[{rd}]);"]

    def _thumb_sp_relative(self, addr, word, step):
        is_load, rd, imm = (word >> 11) & 1, (word >> 8) & 7, (word & 0xFF) * 4
        return [f"R[{rd}] = ldr32(R[13] + {imm});"] if is_load else [f"wr32(R[13] + {imm}, R[{rd}]);"]

    def _thumb_address_add(self, addr, word, step):
        rd, imm = (word >> 8) & 7, (word & 0xFF) * 4
        if (word >> 11) & 1:  # add rd, sp, #imm
            return [f"R[{rd}] = R[13] + {imm};"]
        return [f"R[{rd}] = {c_hex(((addr + 4) & ~3) + imm)};"]  # add rd, pc, #imm

    def _thumb_misc(self, addr, word, step):
        if word & 0xFF00 == 0xB000:  # add/sub sp, #imm
            imm = (word & 0x7F) * 4
            return [f"R[13] {'-' if word & 0x80 else '+'}= {imm};"]
        if word & 0xF600 == 0xB400:  # push / pop
            is_load, with_extra = (word >> 11) & 1, (word >> 8) & 1
            regs = [r for r in range(8) if (word >> r) & 1]
            if with_extra:
                regs.append(15 if is_load else 14)
            count = len(regs)
            if is_load:
                lines = ["{", "    uint32_t ad = R[13];"]
                for k, r in enumerate(regs):
                    lines.append(f"    uint32_t npc = rd32(ad + {4 * k});" if r == 15
                                 else f"    R[{r}] = rd32(ad + {4 * k});")
                lines.append(f"    R[13] = ad + {4 * count};")
                if 15 in regs:
                    lines += indent(self.emit_computed_jump("npc", self.finder.v5))
                return lines + ["}"]
            lines = ["{", f"    uint32_t ad = R[13] - {4 * count};"]
            for k, r in enumerate(regs):
                lines.append(f"    wr32(ad + {4 * k}, R[{r}]);")
            return lines + ["    R[13] = ad;", "}"]
        return [f"cpu_undefined(c, {c_hex(addr)}, {c_hex(word)});"]

    def _thumb_block_transfer(self, addr, word, step):
        is_load, rb = (word >> 11) & 1, (word >> 8) & 7
        regs = [r for r in range(8) if (word >> r) & 1]
        count = len(regs)
        lines = ["{", f"    uint32_t ad = R[{rb}];"]
        if is_load:
            for k, r in enumerate(regs):
                lines.append(f"    R[{r}] = rd32(ad + {4 * k});")
            if rb not in regs:
                lines.append(f"    R[{rb}] = ad + {4 * count};")
        else:
            for k, r in enumerate(regs):
                value = ("ad" if k == 0 else f"(ad + {4 * count})") if r == rb else f"R[{r}]"
                lines.append(f"    wr32(ad + {4 * k}, {value});")
            lines.append(f"    R[{rb}] = ad + {4 * count};")
        return lines + ["}"]

    def _thumb_cond_branch(self, addr, word, step):
        cond = (word >> 8) & 15
        if cond == 0xF:
            return [f"bios_swi(c, {c_hex(word & 0xFF)});"]
        return [f"if ({CONDITION_C[cond]}) {{"] + indent(self.emit_branch(step, addr)) + ["}"]

    def _thumb_branch(self, addr, word, step):
        return self.emit_branch(step, addr)

    def _thumb_long_branch(self, addr, word, step):
        if step.kind == "call":
            return self.emit_call(step, (addr + 4) | 1)
        if step.kind == "branch":  # BL used as a long jump inside the function (see bl_acts_as_jump)
            return [f"R[14] = {c_hex((addr + 4) | 1)};"] + self.emit_branch(step, addr)
        return [f"cpu_undefined(c, {c_hex(addr)}, {c_hex(word)});"]

    # (exclusive upper bound of the instruction word, handler) in ascending order
    _THUMB_FORMATS = (
        (0x1800, _thumb_shift_imm),
        (0x2000, _thumb_add_sub),
        (0x4000, _thumb_imm8),
        (0x4400, _thumb_alu),
        (0x4800, _thumb_hi_registers),
        (0x5000, _thumb_ldr_pc),
        (0x6000, _thumb_reg_offset),
        (0x8000, _thumb_imm_offset),
        (0x9000, _thumb_halfword_imm),
        (0xA000, _thumb_sp_relative),
        (0xB000, _thumb_address_add),
        (0xC000, _thumb_misc),
        (0xD000, _thumb_block_transfer),
        (0xE000, _thumb_cond_branch),
        (0xE800, _thumb_branch),
    )
