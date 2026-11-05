"""Turns the code extracted from the ROM into C.

    python tools/translate.py

Reads extracted/ (made by rom_reader.py) and writes generated/<dir>/*.c for the ARM9 main binary,
for every ARM9 overlay and for the ARM7 binary.
"""

import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from armc.code_image import CodeImage  # noqa: E402
from armc.recompiler import Recompiler, save_if_changed  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Output folders under generated/. The runtime sources and the build script use the same names.
ARM9_DIR = "arm9"
ARM7_DIR = "arm7"


def overlay_dir(n):
    return f"ov{n}"


# Addresses that differ between regions, selected by the game code in the ROM header. The game
# code is the same in both regions, but the overlays are linked at different addresses (and so
# is the data after the ARM9 code); tools/map_region.py builds the USA numbers from the decomp.
# ARM9 hooks below 0x02150000 and all ARM7 hooks sit at the same address in both regions.
REGION_TABLE = {
    "AMCP": {  # Europe
        "label": "EUR",
        "entry_lists": ["eur.txt"],
        "ov1_levels": 0x0219F3A4, "ov1_boss": 0x021A0E34,  # Missions menu (overlay 1)
        "ov0_wcm_phase": 0x021A57FC,  # Nintendo WFC (overlay 0)
    },
    "AMCE": {  # USA, revision 0 (SHA-1 691e00d9a5dd80b04f80cc7559503e8b06848785)
        "label": "USA",
        "entry_lists": ["usa.txt", "usa_extra.txt"],
        "ov1_levels": 0x0219F384, "ov1_boss": 0x021A0E14,
        "ov0_wcm_phase": 0x021A57C8,
    },
}

# OS thread functions of the Nintendo SDK that the runtime implements itself, per processor:
# address -> (runtime function, needs setjmp?[, when]). "before" runs the runtime first and then
# the original code.
ARM9_HOOKS = {
    (0x0200E6EC, False): ("hle_save_context", False),  # OS_SaveContext
    (0x0200E738, False): ("hle_load_context", False),  # OS_LoadContext
    (0x0200E688, False): ("hle_init_context", False, "before"),  # OS_InitContext
}
ARM7_HOOKS = {
    (0x037F96DC, False): ("hle_save_context", False),  # OS_SaveContext
    (0x037F9710, False): ("hle_load_context", False),  # OS_LoadContext
    (0x037F9678, False): ("hle_init_context", False, "before"),  # OS_InitContext
}


def pick_region(info):
    region = REGION_TABLE.get(info["gamecode"])
    if not region:
        sys.exit(f"ROM {info['gamecode']} is not supported (supported: {', '.join(sorted(REGION_TABLE))})")
    return region


def read_manual_entries(list_names, only_overlay=None):
    """Entry points that the automatic search cannot find, kept in tools/entry_points/*.txt, one per
    line: "ADDR" for ARM, "T:ADDR" for Thumb. A line may start with "oN:" to apply to overlay N only;
    the others apply to any image that contains the address."""
    entries = []
    for list_name in list_names:
        path = os.path.join(ROOT, "tools", "entry_points", list_name)
        if not os.path.exists(path):
            continue
        with open(path) as fh:
            for raw in fh:
                line = raw.strip()
                if not line or line.startswith("#"):
                    continue
                wanted = None
                if line[0] == "o" and ":" in line:
                    wanted, line = line[1:].split(":", 1)
                    wanted = int(wanted)
                if wanted is None or wanted == only_overlay:
                    entries.append((int(line[2:], 16), True) if line.startswith("T:") else (int(line, 16), False))
    return entries


def translate_image(label, img, entries, hooks, out_dir, write_out=True, **options):
    started = time.time()
    print(f"{label}:")
    patches = options.pop("patches", None)
    recompiler = Recompiler(img, entries, hooks, **options)
    if patches:
        recompiler.patches = patches
    recompiler.discover(log=lambda msg: print("  " + msg), prologues=True)
    thumb_count = sum(1 for key in recompiler.funcs if key[1])
    print(f"  ARM: {len(recompiler.funcs) - thumb_count} functions, Thumb: {thumb_count}")
    if write_out:
        files = recompiler.write(os.path.join(ROOT, "generated", out_dir))
        print(f"  {len(files)} C files in generated/{out_dir} ({time.time() - started:.0f} s)")
    return recompiler


def find_bridge_functions(data, base):
    """ARM bridge functions without "push lr" ("ldr ip, =target ... bx ip") that are only called
    through a pointer. No search finds the ones of the Missions (overlay 1). They only count when
    they sit right behind data or another jump, so that a bigger function is not cut in two."""
    found = set()
    for off in range(4, len(data) - 16, 4):
        if struct.unpack_from("<I", data, off)[0] & 0xFFFFF000 != 0xE59FC000:
            continue
        before = struct.unpack_from("<I", data, off - 4)[0]
        if before >> 28 == 0xE and before & 0x0FFFFFF0 != 0x012FFF10:
            continue
        window = struct.unpack_from("<3I", data, off + 4)
        if 0xE12FFF1C in window and all(w >> 28 == 0xE for w in window[:window.index(0xE12FFF1C)]):
            found.add((base + off, False))
    return found


def translate_overlays(extracted, info, fixed_funcs, region):
    """Translate each overlay on its own (symbols get an _oN suffix and the tables carry
    signatures, because overlays reuse addresses)."""
    overlays = list(info["overlays9"])
    blobs = {}
    for ov in overlays:
        with open(os.path.join(extracted, f"overlay9_{ov['id']}.bin"), "rb") as fh:
            blobs[ov["id"]] = fh.read()
    # Entry points that one overlay needs in another one; found in the first round, used in the second.
    cross_entries = {ov["id"]: set() for ov in overlays}
    for _round in range(2):
        results = {}
        for ov in overlays:
            n, base, data = ov["id"], ov["ram"], blobs[ov["id"]]
            manual = read_manual_entries(region["entry_lists"], n)
            img = CodeImage()
            img.add(base, data)
            entries = set(cross_entries[n])
            # table of static initialisers (function pointers)
            for slot in range(ov["sinit_start"], ov["sinit_end"], 4):
                if base <= slot < base + len(data) - 3:
                    value = struct.unpack_from("<I", data, slot - base)[0]
                    entries.add((value & ~1, True) if value & 1 else (value, False))
            entries |= find_bridge_functions(data, base)
            weak = {k for k in manual if base <= k[0] < base + len(data) and img.has(k[0], 2)}
            entries = {k for k in entries if img.has(k[0], 2)}
            results[n] = translate_image(f"overlay {n}", img, sorted(entries), None, overlay_dir(n),
                                         write_out=False, prefix=f"_o{n}", disp="dispatch",
                                         external=fixed_funcs, signatures=True, weak=weak)
        # calls from one overlay into another become entries of the target in the next round
        added = False
        for n, result in results.items():
            for key in result.outside:
                for ov in overlays:
                    if ov["id"] != n and ov["ram"] <= key[0] < ov["ram"] + len(blobs[ov["id"]]):
                        if key not in cross_entries[ov["id"]]:
                            cross_entries[ov["id"]].add(key)
                            added = True
        if not added:
            break

    # Overlays 0 and 1 share an address range, and a function can start with the same four bytes in
    # both. The runtime could not tell them apart by the short signature and would pick overlay 0
    # even while overlay 1 is loaded. Such functions get a long hash in their table entry (see
    # signature_long in armc/recompiler.py). The USA build has three of them, and one broke the
    # character select screen.
    seen = {}
    for result in results.values():
        for key in result.funcs:
            sig = (key[0] | int(key[1]), result.signature_short(key))
            seen[sig] = seen.get(sig, 0) + 1
    ambiguous = {sig for sig, times in seen.items() if times > 1}
    print(f"overlay functions with the same address and signature in two overlays: {len(ambiguous)}")
    for result in results.values():
        result.ambiguous = ambiguous
    written = []
    for n, result in results.items():
        if not result.funcs:
            continue
        result.write(os.path.join(ROOT, "generated", overlay_dir(n)))
        written.append(n)

    # Table that tells the runtime which overlays exist (cpu.c: search_ov).
    lines = ['#include "cpu.h"\n']
    for n in written:
        lines.append(f"extern const FuncEntryOv g_funcs_o{n}[];\nextern const size_t g_nfuncs_o{n};\n")
    lines.append("\nconst TableOv g_overlays[] = {\n")
    for n in written:
        lines.append(f"    {{g_funcs_o{n}, &g_nfuncs_o{n}}},\n")
    if not written:
        lines.append("    {0, 0},\n")
    lines.append(f"}};\nconst size_t g_noverlays = {len(written)};\n")
    save_if_changed(os.path.join(ROOT, "generated", ARM9_DIR, "overlays.c"), "".join(lines))
    print(f"overlays translated: {written}")


def translate_arm9(extracted, info, region):
    with open(os.path.join(extracted, "arm9.bin"), "rb") as fh:
        binary = fh.read()
    base = info["arm9_ram"]
    img = CodeImage()
    img.add(base, binary[:info["module_params"]["autoload_start"] - base])
    for block in info["autoloads"]:  # ITCM and DTCM, placed where they run
        img.add(block["ram"], binary[block["offset"]:block["offset"] + block["size"]])
    entries = [(info["arm9_entry"], False)]
    return translate_image("ARM9", img, entries, ARM9_HOOKS, ARM9_DIR, patches=None,
                           weak=read_manual_entries(region["entry_lists"]))


def translate_arm7(extracted, info):
    with open(os.path.join(extracted, "arm7.bin"), "rb") as fh:
        binary = fh.read()
    base = info["arm7_ram"]
    img = CodeImage()
    img.add(base, binary)
    # The ARM7 boot code copies pieces to its WRAM, to the VRAM that the ARM9 lends it and to
    # 0x027E0000. The table (list start, list end, data) is in the literal that the copy routine
    # reads (0x02380198 in the USA ROM).
    cursor, stop, source = struct.unpack_from("<3I", binary, 0x198)
    while cursor < stop:
        dest, size, _bss = struct.unpack_from("<3I", binary, cursor - base)
        img.add(dest, binary[source - base:source - base + size])
        print(f"  ARM7: {size} bytes of code/data at {dest:08X}")
        source += size
        cursor += 12
    entries = [(info["arm7_entry"], False)]
    translate_image("ARM7", img, entries, ARM7_HOOKS, ARM7_DIR, prefix="7", armv4=True,
                    weak=read_manual_entries(["arm7.txt"]))


def main():
    extracted = os.path.join(ROOT, "extracted")
    with open(os.path.join(extracted, "info.json")) as fh:
        info = json.load(fh)
    region = pick_region(info)
    print(f"ROM {info['gamecode']} ({region['label']})")
    arm9 = translate_arm9(extracted, info, region)
    translate_overlays(extracted, info, set(arm9.funcs), region)
    translate_arm7(extracted, info)


if __name__ == "__main__":
    main()
