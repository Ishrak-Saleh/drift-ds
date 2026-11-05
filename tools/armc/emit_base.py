"""Shared machinery for writing one function as C: the function frame, labels, and every kind of jump.

The C that comes out works on a Cpu structure `c` (registers R[0..15], flags, a cycle counter) and on
the emulated memory through runtime helpers (rd32, wr8, ...). Names of those helpers belong to the
runtime in runtime/cpu.h, so they must not change here.
"""

from .bits import c_hex, func_symbol

# Placed at the start of every function and on every backward jump, so that interrupts and the
# other processor get their turn.
TICK_LINE = "if (--c->cycles < 0) cpu_tick(c);"

# C expression for each ARM condition code (0xE is "always", 0xF is handled separately).
CONDITION_C = ["c->z", "!c->z", "c->c", "!c->c", "c->n", "!c->n", "c->v", "!c->v",
               "(c->c && !c->z)", "(!c->c || c->z)", "(c->n == c->v)", "(c->n != c->v)",
               "(!c->z && c->n == c->v)", "(c->z || c->n != c->v)", None, None]


def indent(lines, pad="    "):
    return [pad + line for line in lines]


class EmitBase:
    def __init__(self, finder, fn):
        self.finder = finder
        self.disp = finder.disp
        self.img = finder.img
        self.fn = fn
        self.thumb = fn.thumb

    # -- the function frame ---------------------------------------------------------------

    def render(self):
        fn = self.fn
        symbol = func_symbol(fn.addr, fn.thumb, self.finder.prefix)
        mode_addr = c_hex(fn.addr | int(fn.thumb))
        hook = self.finder.hooks.get((fn.addr, fn.thumb))
        # Hook kinds: none = the runtime replaces the function; "before" = the runtime runs first and
        # the original goes on; "si" = if the runtime returns 1, the function returns right away.
        hook_kind = hook[2] if hook and len(hook) > 2 else None
        if hook and not hook_kind:
            return (f"void {symbol}(Cpu *c) {{\n"
                    f"    c->func = {mode_addr};\n    c->elr = 0;\n    {hook[0]}(c);\n}}\n")
        # entry_lr: a jump from another piece of the same function hands its own return address over in c->elr.
        out = [f"void {symbol}(Cpu *c) {{",
               "    const uint32_t entry_lr = c->elr ? c->elr : R[14];",
               "    c->elr = 0;",
               "    (void)entry_lr;",
               "    uint32_t t_dyn; (void)t_dyn;",
               f"    c->func = {mode_addr};",
               "    " + TICK_LINE]
        if hook_kind == "before":
            out.append(f"    {hook[0]}(c);")
        elif hook_kind == "si":
            out.append(f"    if ({hook[0]}(c)) return;")
        addrs = sorted(fn.steps)
        if addrs and addrs[0] != fn.addr:
            out.append(f"    goto L_{fn.addr:08X};")
        if fn.addr in fn.missing:
            out.append(f"    cpu_missing(c, {c_hex(fn.addr)});")
        for idx, addr in enumerate(addrs):
            step = fn.steps[addr]
            out.append(f"L_{addr:08X}:;")
            body = self.thumb_ins(addr, step) if self.thumb else self.arm_ins(addr, step)
            out += indent(body)
            if step.falls_through():
                following = addr + step.width
                if idx + 1 >= len(addrs) or addrs[idx + 1] != following:
                    if following in fn.steps:
                        out.append(f"    goto L_{following:08X};")
                    else:
                        out.append(f"    cpu_missing(c, {c_hex(following)});")
        targets = sorted(fn.jump_targets)
        if targets:  # computed jumps inside the function (switch): a single shared table
            low_bit = 1 if self.thumb else 0
            out.append("L_dyn:")
            out.append("    switch (t_dyn) {")
            for addr in targets:
                out.append(f"    case {c_hex(addr | low_bit)}: goto L_{addr:08X};")
            out.append("    }")
            out.append(f"    {self.lr_hint_dynamic('t_dyn')}{self.disp}(c, t_dyn); return;")
        out.append("}")
        return "\n".join(out) + "\n"

    # -- return address hand-over ----------------------------------------------------------
    # c->elr is passed on only when the jump lands inside this function's own address range: that
    # is a jump into a split-off piece of the same function. A jump to another function (tail call,
    # function pointer) keeps r14.

    def lr_hint_static(self, target):
        if not self.fn.steps:
            return ""
        low, high = min(self.fn.steps), max(self.fn.steps) + 4
        return "c->elr = entry_lr; " if low <= target < high else ""

    def lr_hint_dynamic(self, var):
        if not self.fn.steps:
            return ""
        low, high = min(self.fn.steps), max(self.fn.steps) + 4
        return f"if (({var} & ~1u) - {c_hex(low)} < {c_hex(high - low)}) c->elr = entry_lr; "

    # -- jumps and calls -----------------------------------------------------------------

    def symbol_for(self, addr, thumb):
        """C function for a known target, or None when it has to be resolved while the game runs
        (inside another overlay, or fixed code that was not translated)."""
        if (addr, thumb) in self.finder.funcs:
            return func_symbol(addr, thumb, self.finder.prefix)
        if (addr, thumb) in self.finder.external:
            return func_symbol(addr, thumb)
        return None

    def emit_branch(self, step, addr):
        if not step.is_tail and step.target in self.fn.steps:
            if step.target <= addr:  # backward jump: a loop, give interrupts a chance
                return [TICK_LINE, f"goto L_{step.target:08X};"]
            return [f"goto L_{step.target:08X};"]
        name = self.symbol_for(step.target, self.thumb)
        hint = self.lr_hint_static(step.target)
        if name:
            return [f"{hint}{name}(c); return;"]
        return [f"{hint}{self.disp}(c, {c_hex(step.target | int(self.thumb))}); return;"]

    def emit_call(self, step, return_addr):
        name = self.symbol_for(step.target, step.target_thumb)
        restore_func = f"c->func = {c_hex(self.fn.addr | int(self.thumb))};"
        if not name:
            return [f"R[14] = {c_hex(return_addr)};",
                    f"{self.disp}(c, {c_hex(step.target | int(step.target_thumb))});",
                    restore_func]
        hook = self.finder.hooks.get((step.target, step.target_thumb))
        if hook and hook[1]:
            # OS_SaveContext: execution comes back here (longjmp) when that context is loaded again.
            return [f"R[14] = {c_hex(return_addr)};",
                    f"if (__builtin_setjmp(hle_ctx_buf(c, R[0])) == 0) {name}(c);",
                    restore_func]
        return [f"R[14] = {c_hex(return_addr)};", f"{name}(c);", restore_func]

    def emit_tail(self, step):
        name = self.symbol_for(step.target, step.target_thumb)
        hint = self.lr_hint_static(step.target)
        if not name:
            return [f"{hint}{self.disp}(c, {c_hex(step.target | int(step.target_thumb))}); return;"]
        return [f"{hint}{name}(c); return;"]

    def emit_computed_jump(self, expr, interwork):
        """Jump to an address computed at run time."""
        if interwork:
            key = "t"
        elif self.thumb:
            key = "(t | 1u)"
        else:
            key = "(t & ~3u)"
        lines = ["{", f"    uint32_t t = {expr};",
                 "    if (((t ^ entry_lr) & ~1u) == 0) return;"]
        if self.fn.jump_targets:  # the table of targets exists once, at L_dyn
            return lines + [f"    t_dyn = {key}; goto L_dyn;", "}"]
        return lines + [f"    {self.lr_hint_dynamic('t')}{self.disp}(c, {key}); return;", "}"]

    def emit_computed_call(self, expr, return_addr, interwork=True):
        key = "t" if interwork else ("(t | 1u)" if self.thumb else "(t & ~3u)")
        return ["{", f"    uint32_t t = {expr};", f"    R[14] = {c_hex(return_addr)};",
                f"    {self.disp}(c, {key});", "}"]

    def emit_pc_write(self, addr, step, expr, interwork):
        """An ordinary instruction that writes pc (mov pc, ldr pc, pop {pc}, ...)."""
        if step.kind == "callind":  # "mov lr, pc" before it already set the return address
            target = "t" if interwork else "(t & ~3u)"
            return ["{", f"    uint32_t t = {expr};", f"    {self.disp}(c, {target});", "}"]
        if step.kind == "tail":
            return [f"(void)({expr});"] + self.emit_tail(step)
        if step.kind == "call":  # "mov lr, pc" + "ldr pc, =function"
            name = self.symbol_for(step.target, step.target_thumb)
            call = f"{name}(c);" if name else f"{self.disp}(c, {c_hex(step.target | int(step.target_thumb))});"
            return [f"(void)({expr});", call]
        return self.emit_computed_jump(expr, interwork)

    def with_patch(self, addr, lines):
        patch = self.finder.patches.get(addr)
        if not patch:
            return lines
        condition, replacement = patch
        return [f"if ({condition}) {{", f"    {replacement}", "} else {"] + indent(lines) + ["}"]

    def reg(self, num, pc_value):
        return c_hex(pc_value) if num == 15 else f"R[{num}]"
