"""Ties the finder and the C writers together and lays the generated sources out on disk."""

import os

from .bits import c_hex, func_symbol
from .emit_arm import ArmEmitter
from .emit_base import EmitBase
from .emit_thumb import ThumbEmitter
from .finder import FunctionFinder

FUNCS_PER_FILE = 400


class FunctionWriter(EmitBase, ArmEmitter, ThumbEmitter):
    """Writes one function as C."""


def save_if_changed(path, text):
    """Leave the file alone when its content is already right, so the build stays incremental."""
    if os.path.exists(path):
        with open(path) as fh:
            if fh.read() == text:
                return
    with open(path, "w") as fh:
        fh.write(text)


class Recompiler(FunctionFinder):
    # Folder (next to the generated overlays) that holds the header of the fixed ARM9 code.
    fixed_code_dir = "arm9"

    # -- signatures used by the overlay tables ------------------------------------------------

    def signature_short(self, key):
        """The first four bytes of the function as one number (0 when they are not in the image)."""
        addr = key[0]
        if not self.img.has(addr, 4):
            return 0
        return self.img.half(addr) | self.img.half(addr + 2) << 16

    def signature_long(self, key):
        """FNV-1a hash of up to 16 words at the function address.

        Two overlays that share an address range can hold functions with the same first four bytes.
        This hash tells which overlay is loaded. The C side (signature_long in runtime/cpu.c) must
        compute exactly the same value.
        """
        addr = key[0]
        digest = 2166136261
        for n in range(16):
            if not self.img.has(addr + 4 * n, 4):
                break
            word = self.img.half(addr + 4 * n) | self.img.half(addr + 4 * n + 2) << 16
            digest = ((digest ^ word) * 16777619) & 0xFFFFFFFF
        return digest or 1

    # -- output ---------------------------------------------------------------------------------

    def write(self, outdir, per_file=FUNCS_PER_FILE):
        os.makedirs(outdir, exist_ok=True)
        keys = sorted(self.funcs, key=lambda k: (k[0], k[1]))

        header = ['#pragma once\n#include "cpu.h"\n']
        if self.external:
            header.append(f'#include "../{self.fixed_code_dir}/funcs.h"\n')
        header += [f"void {func_symbol(*k, self.prefix)}(Cpu *c);\n" for k in keys]
        save_if_changed(os.path.join(outdir, "funcs.h"), "".join(header))

        files = []
        for first in range(0, len(keys), per_file):
            name = f"funcs_{first // per_file:03d}.c"
            files.append(name)
            parts = ['#include "funcs.h"\n#pragma clang diagnostic ignored "-Wunused-label"\n\n']
            for key in keys[first:first + per_file]:
                parts.append(FunctionWriter(self, self.funcs[key]).render() + "\n")
            save_if_changed(os.path.join(outdir, name), "".join(parts))

        entry_type = "FuncEntryOv" if self.signatures else "FuncEntry"
        table = [f'#include "funcs.h"\n\nconst {entry_type} g_funcs{self.prefix}[] = {{\n']
        for key in sorted(keys, key=lambda k: k[0] | int(k[1])):
            extra = ""
            if self.signatures:
                # Second field: a long hash, only for functions that exist in two overlays with the
                # same short signature. 0 means the short signature is enough
                # (see signature_long here and search_ov in runtime/cpu.c).
                short = self.signature_short(key)
                long_hash = self.signature_long(key) if (key[0] | int(key[1]), short) in self.ambiguous else 0
                extra = f", {c_hex(short)}, {c_hex(long_hash)}"
            table.append(f"    {{{c_hex(key[0] | int(key[1]))}, {func_symbol(*key, self.prefix)}{extra}}},\n")
        table.append(f"}};\nconst size_t g_nfuncs{self.prefix} = "
                     f"sizeof g_funcs{self.prefix} / sizeof g_funcs{self.prefix}[0];\n")
        save_if_changed(os.path.join(outdir, "table_data.c"), "".join(table))

        for stale in os.listdir(outdir):  # files left from an earlier run that had more of them
            if stale.startswith("funcs_") and stale.endswith(".c") and stale not in files:
                os.remove(os.path.join(outdir, stale))
        return files + ["table_data.c"]
