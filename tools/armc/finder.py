"""Finds the functions in a code image.

Starting from known entry points, each function is traced instruction by instruction. Then the
whole image is searched for functions nothing visibly calls: pointers stored in data, "push {lr}"
prologues, holes between functions, and code that follows a return. Every candidate must pass a
quick plausibility check before it is accepted.
"""

import struct

from .bits import rotate_right
from .decode import decode_arm, decode_thumb

# After a computed jump the code behind it is searched for the cases of the switch, but never
# further than this many bytes (a certain function entry ends the search earlier).
SWEEP_LIMIT = 0x1000
LARGEST_SWITCH = 1024
FAR_BL_SCAN = 128


class FuncInfo:
    def __init__(self, addr, thumb):
        self.addr = addr
        self.thumb = thumb
        self.steps = {}  # address -> Step
        self.calls = set()  # (address, thumb) of every function this one calls
        self.literals = set()  # addresses of constants loaded relative to pc
        self.computed_jumps = []  # addresses of jumps through a register or table
        self.jump_targets = set()  # places inside the function a computed jump may land on
        self.missing = set()  # addresses the control flow reaches but the image does not hold


class FunctionFinder:
    def __init__(self, img, entries, hooks=None, prefix="", armv4=False, disp=None, weak=None,
                 external=None, signatures=False):
        self.img = img
        # Name suffix of the generated symbols: "7" for the ARM7, "_o0" for overlay 0.
        self.prefix = prefix
        self.disp = disp or "dispatch" + prefix
        # Functions that live outside this image but are already translated (the fixed ARM9 code,
        # when an overlay is being translated). They are called by their plain name.
        self.external = external or set()
        self.signatures = signatures  # overlay tables carry the first bytes of every function
        self.ambiguous = set()  # (address, signature) pairs that exist in more than one overlay
        self.patches = {}  # address -> (C condition, C replacement for the instruction)
        self.outside = set()  # call targets beyond this image, resolved while the game runs
        self.v5 = not armv4  # on ARMv5 a load into pc picks ARM or Thumb from bit 0
        self.entries = set(entries)
        self.funcs = {}
        # (address, thumb) -> (runtime function, needs setjmp?[, "before" | "si"])
        self.hooks = hooks or {}
        self.entries |= set(self.hooks)
        # "Weak" entries come from missing.txt. They are translated, but they never end a sweep
        # and never turn a branch into a tail call, because they may sit inside another function.
        self.weak = set(weak or []) - self.entries

    # -- tracing one function --------------------------------------------------------------

    def _decode(self, thumb, addr):
        return decode_thumb(self.img, addr) if thumb else decode_arm(self.img, addr)

    def trace_function(self, key, strong):
        """Follow every path of the function that starts at key.

        strong holds the entries that are certain to be real functions (calls, boot code, hooks).
        A branch to one of them is a tail call.
        """
        addr, thumb = key
        fn = FuncInfo(addr, thumb)
        width = 2 if thumb else 4
        work = [addr]
        swept = set()
        while True:
            self._follow_paths(fn, work, strong, width)
            fresh = [site for site in fn.computed_jumps if site not in swept]
            if not fresh:
                break
            for site in fresh:
                swept.add(site)
                self._sweep_behind_jump(fn, site, work, strong, width)
        return fn

    def _follow_paths(self, fn, work, strong, width):
        thumb = fn.thumb
        while work:
            addr = work.pop()
            if addr in fn.steps:
                continue
            if not self.img.has(addr, width):
                fn.missing.add(addr)
                continue
            step = self._decode(thumb, addr)
            fn.steps[addr] = step
            if step.literal is not None:
                fn.literals.add(step.literal)
            kind = step.kind
            if kind == "branch":
                if (step.target, thumb) in strong and step.target != fn.addr:
                    step.is_tail = True
                    fn.calls.add((step.target, thumb))
                else:
                    work.append(step.target)
            elif kind == "call" and thumb and step.target_thumb and self.bl_acts_as_jump(step.target):
                step.kind = "branch"  # a far jump inside the function, not a call
                work.append(step.target)
            elif kind in ("call", "tail"):
                fn.calls.add((step.target, step.target_thumb))
            elif kind == "jumpind":
                fn.computed_jumps.append(addr)
            if step.falls_through():
                work.append(addr + step.width)

    def _sweep_behind_jump(self, fn, site, work, strong, width):
        """Collect the places a computed jump at `site` can reach."""
        img = self.img
        thumb = fn.thumb
        # An ARM "bx rN" is a jump through a register to another function (a bridge), never a
        # switch (those use add/ldr to pc). Sweeping behind it used to swallow the Thumb code next to it.
        if not thumb and img.word(site) & 0x0FFFFFF0 == 0x012FFF10:
            return
        # An ARM switch with a visible size ("cmp rX, #n" before "addls pc, pc, rX, lsl #2") has
        # exactly n + 1 cases. A blind sweep of 4 KB once ran into Thumb code, found false jumps
        # and kept growing: 0x0211127C ended up covering half of the ARM9.
        if not thumb:
            cases = self.arm_switch_targets(site)
            if cases:
                for target in cases:
                    if img.has(target, 4):
                        fn.jump_targets.add(target)
                        work.append(target)
                return
        # Blind sweep. It stops at the next certain function entry, skips the constant pools that
        # the Nintendo compiler leaves inside large functions, and stops at the next prologue.
        beyond = [entry for (entry, _) in strong if entry > site]
        stop = min(beyond + [site + SWEEP_LIMIT])
        pool_words = set()
        for lit in fn.literals:
            pool_words.update((lit, lit + 2))
        cursor = site + width
        while cursor < stop and img.has(cursor, 4):
            if cursor not in pool_words:
                if thumb and img.half(cursor) & 0xFF00 == 0xB500:  # push {.., lr}
                    stop = cursor
                    break
                if not thumb and img.word(cursor) & 0xFFFF4000 == 0xE92D4000:  # stmdb sp!, {.., lr}
                    stop = cursor
                    break
            cursor += width
        cursor = site + width
        while cursor < stop and img.has(cursor, width):
            if cursor not in pool_words:
                fn.jump_targets.add(cursor)
                work.append(cursor)
            cursor += width

    def bl_acts_as_jump(self, target):
        """Is this Thumb BL really a far jump inside the same function?

        A lone B reaches only +-2 KB, so the compiler uses BL to leave the middle sections of very
        large functions (overlay 0 jumps to its own epilogue like that). Treating it as a call made
        the piece pop the outer function's return address, and the game ended in "missing code".
        The tell: following the code from the target, the stack frame is undone (pop {pc}, or
        pop {rX} + bx rX) without a push first. A real function pushes, or ends with bx lr if it
        is a leaf.
        """
        img = self.img
        addr = target
        for _ in range(FAR_BL_SCAN):
            if not img.has(addr, 2):
                return False
            word = img.half(addr)
            if word & 0xFE00 == 0xB400 or word == 0x4770:  # push, bx lr
                return False
            if word & 0xFF00 == 0xBD00:  # pop {.., pc}
                return True
            if word & 0xFF87 == 0x4700 and addr > target:
                prev = img.half(addr - 2)
                if prev & 0xFF00 == 0xBC00 and (prev >> ((word >> 3) & 15)) & 1:
                    return True  # pop {rX} + bx rX
            if word & 0xFF87 == 0x4700 or word & 0xFF00 == 0xDF00:  # another bx, or swi: unknown
                return False
            addr += 4 if word & 0xF800 == 0xF000 else 2
        return False

    def arm_switch_targets(self, site):
        """Targets of an ARM switch written as "addls pc, pc, rX, lsl #2" (jump table right behind)
        or "ldrls pc, [pc, rX, lsl #2]" (table of addresses), when a "cmp rX, #n" is a few
        instructions earlier. Returns None when the pattern is not recognised."""
        img = self.img
        word = img.word(site)
        rx = word & 0xF
        is_add = word & 0xFFFFFFF0 == 0x908FF100
        is_ldr = word & 0xFFFFFFF0 == 0x979FF100
        if not (is_add or is_ldr):
            return None
        limit = None
        for back in range(1, 5):
            if not img.has(site - 4 * back, 4):
                break
            before = img.word(site - 4 * back)
            if before & 0xFFF0F000 == 0xE3500000 and (before >> 16) & 0xF == rx:  # cmp rX, #imm
                limit = rotate_right(before & 0xFF, ((before >> 8) & 0xF) * 2)
                break
            if (before >> 12) & 0xF == rx and before & 0x0C000000 == 0:
                break  # rX is changed before the compare, so the bound cannot be trusted
        if limit is None or limit > LARGEST_SWITCH:
            return None
        cases = [site + 4]  # rX above the limit falls to the next instruction
        if is_add:
            cases += [site + 8 + 4 * n for n in range(limit + 1)]
        else:
            for n in range(limit + 1):
                dest = img.word(site + 8 + 4 * n)
                if not dest & 1 and img.has(dest, 4):
                    cases.append(dest)
        return cases

    # -- searching for functions that nothing visibly calls -------------------------------

    def looks_like_function(self, addr, thumb):
        """Quick sanity check: does decoding from addr look like the start of a function?"""
        width = 2 if thumb else 4
        if not thumb and self.img.has(addr, 4) and (self.img.word(addr) >> 28) != 0xE:
            return False
        for _ in range(64):
            if not self.img.has(addr, width):
                return False
            step = self._decode(thumb, addr)
            if step.kind == "undef":
                return False
            if step.kind in ("ret", "jumpind", "tail", "branch") and not step.conditional:
                return True
            addr += step.width
        return True

    def scan_data_pointers(self):
        """Words anywhere in the image that point at plausible code (function pointers in tables)."""
        loaded = set()
        for fn in self.funcs.values():
            loaded |= fn.literals
        found = set()
        for _, blob in self.img.segments:
            for off in range(0, len(blob) - 3, 4):
                value = struct.unpack_from("<I", blob, off)[0]
                key = (value & ~1, True) if value & 1 else (value, False)
                if not value & 1 and value & 3:
                    continue
                if key in self.funcs or key[0] in loaded or not self.img.has(key[0], 2):
                    continue
                if self.looks_like_function(*key):
                    found.add(key)
        return found

    def scan_prologues(self):
        """Typical function starts (push {.., lr}) that are not known functions yet. Starts that
        sit inside a known Thumb function are included, because a sweep after a computed jump may
        have swallowed them. Addresses inside known ARM code are skipped."""
        inside_arm = set()
        for (_, thumb), fn in self.funcs.items():
            if thumb:
                continue
            for addr in fn.steps:
                inside_arm.add(addr)
                inside_arm.add(addr + 2)
        found = set()
        for start, blob in self.img.segments:
            for off in range(0, len(blob) - 3, 2):
                addr = start + off
                if addr in inside_arm:
                    continue
                half = blob[off] | blob[off + 1] << 8
                if half & 0xFF00 == 0xB500:  # Thumb push {.., lr}
                    if (addr, True) not in self.funcs and self.looks_like_function(addr, True):
                        found.add((addr, True))
                elif off % 4 == 0:
                    word = struct.unpack_from("<I", blob, off)[0]
                    if (word & 0xFFFF4000 == 0xE92D4000 and (addr, False) not in self.funcs
                            and self.looks_like_function(addr, False)):  # ARM stmdb sp!, {.., lr}
                        found.add((addr, False))
        return found

    def scan_gaps(self):
        """Holes between known code. When something decodes well after a hole starts, it is taken as
        a function (small ones without push lr that are reached through computed pointers)."""
        mode_at = {}  # address -> thumb flag of the function covering it
        for (_, thumb), fn in self.funcs.items():
            for addr, step in fn.steps.items():
                mode_at[addr] = thumb
                if step.width == 4:
                    mode_at[addr + 2] = thumb
            for lit in fn.literals:
                mode_at[lit] = thumb
                mode_at[lit + 2] = thumb
        found = set()
        for start, blob in self.img.segments:
            addr, end = start, start + len(blob)
            thumb = False
            while addr < end - 2:
                if addr in mode_at:
                    thumb = mode_at[addr]
                    addr += 2
                    continue
                # inside a hole: skip padding (zeros and nops)
                half = self.img.half(addr)
                if half in (0x0000, 0x46C0) or (not thumb and addr % 4):
                    addr += 2
                    continue
                key = (addr, thumb)
                if key not in self.funcs and self.looks_like_function(addr, thumb):
                    found.add(key)
                while addr < end - 2 and addr not in mode_at:  # move to the end of the hole
                    addr += 2
        return found

    def scan_after_returns(self):
        """After an instruction that ends a block (return, unconditional jump), anything that no jump
        of the function targets is the start of another function."""
        found = set()
        for (_, thumb), fn in list(self.funcs.items()):
            jump_dests = {s.target for s in fn.steps.values() if s.kind == "branch" and not s.is_tail}
            width = 2 if thumb else 4
            filler = (0x46C0, 0x0000) if thumb else (0xE1A00000, 0)
            for addr, step in fn.steps.items():
                if step.falls_through() or step.kind == "undef":
                    continue
                nxt = addr + step.width
                for _ in range(4):  # skip padding
                    if not self.img.has(nxt, width):
                        break
                    value = self.img.half(nxt) if thumb else self.img.word(nxt)
                    if value in filler:
                        nxt += width
                        continue
                    break
                if nxt in jump_dests or nxt in fn.literals or (nxt, thumb) in self.funcs:
                    continue
                if not self.img.has(nxt, width):
                    continue
                if self.looks_like_function(nxt, thumb):
                    found.add((nxt, thumb))
        return found

    # -- the whole search -------------------------------------------------------------------

    def discover(self, log=print, scan=True, prologues=False):
        # Pointers found in data may be false (they can point into the middle of a function).
        # They are translated, but only the strong entries turn a branch into a tail call.
        strong = set(self.entries)
        known = set(self.entries) | self.weak
        pending = list(known)
        scanned = not scan
        gap_rounds = 0
        while True:  # first pass: find every function
            while pending:
                key = pending.pop()
                if key in self.funcs:
                    continue
                if not self.img.has(key[0], 2):
                    self.outside.add(key)
                    continue
                fn = self.trace_function(key, strong)
                self.funcs[key] = fn
                for callee in fn.calls:
                    if key in strong:  # callees of a function found by searching are not certain
                        strong.add(callee)
                    if callee not in known:
                        known.add(callee)
                        pending.append(callee)
            if scanned:
                if prologues:  # once all else is done: loose function starts
                    prologues = False
                    gap_rounds = 3
                    extra = self.scan_prologues() - known
                    log(f"functions found by their start (push lr): {len(extra)}")
                    known |= extra
                    pending = list(extra)
                    continue
                if gap_rounds > 0:  # then fill the remaining holes, up to three times
                    gap_rounds -= 1
                    extra = (self.scan_gaps() | self.scan_after_returns()) - known
                    log(f"functions found in gaps and borders: {len(extra)}")
                    if extra:
                        known |= extra
                        pending = list(extra)
                        continue
                break
            scanned = True
            extra = self.scan_data_pointers() - known
            log(f"function pointers found in data: {len(extra)}")
            known |= extra
            pending = list(extra)
        # Second pass: with every function known, branches to another function become tail calls.
        for key in list(self.funcs):
            self.funcs[key] = self.trace_function(key, strong)
        total = sum(len(f.steps) for f in self.funcs.values())
        log(f"{len(self.funcs)} functions, {total} instructions")
