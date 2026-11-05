"""Reads a Nintendo DS cartridge image and writes out the pieces the translator needs.

What comes out of a ROM:
  - the header fields (game code, load addresses, entry points)
  - the ARM9 binary, already decompressed (main CPU, runs the game)
  - the ARM7 binary (second CPU: sound, touch screen, Wi-Fi chip)
  - every ARM9 overlay (code that the game swaps in and out of memory)
  - info.json, which describes where each piece lives in DS memory

Command line:
    python tools/rom_reader.py rom/game.nds              print a summary
    python tools/rom_reader.py rom/game.nds extracted    also write the pieces into a folder
"""

import json
import os
import struct
import sys
from dataclasses import dataclass, field

# Marks the module parameter block inside the ARM9 binary (two fixed words after the block).
MODULE_PARAMS_MAGIC = struct.pack("<II", 0xDEC00621, 0x2106C0DE)
MODULE_PARAMS_FIELDS = ("autoload_list_start", "autoload_list_end", "autoload_start",
                        "static_bss_start", "static_bss_end", "compressed_static_end")
OVERLAY_ENTRY_SIZE = 32
AUTOLOAD_ENTRY_SIZE = 12
FAT_ENTRY_SIZE = 8


def word_at(buf, offset):
    return struct.unpack_from("<I", buf, offset)[0]


def unpack_backward_lz(packed):
    """Undo the Nintendo SDK "backward LZ" compression.

    The trailer is 8 bytes: a word with the size of the compressed span and the
    length of the trailer itself, then a word with how many bytes the data grows.
    Decoding walks from the end of the buffer toward the start.
    """
    src_all = bytes(packed)
    growth = word_at(src_all, len(src_all) - 4)
    if growth == 0:
        return bytearray(src_all)
    info = word_at(src_all, len(src_all) - 8)
    trailer_len = info >> 24
    span = info & 0xFFFFFF
    plain_end = len(src_all) - span  # everything before this offset is stored as is
    out = bytearray(len(src_all) + growth)
    out[:plain_end] = src_all[:plain_end]

    read = len(src_all) - trailer_len
    write = len(out)
    while write > plain_end and read > plain_end:
        read -= 1
        flag_byte = src_all[read]
        for _ in range(8):
            if write <= plain_end or read <= plain_end:
                break
            if flag_byte & 0x80:
                hi, lo = src_all[read - 1], src_all[read - 2]
                read -= 2
                run = (hi >> 4) + 3
                back = (((hi & 0x0F) << 8) | lo) + 3
                for _ in range(run):
                    write -= 1
                    out[write] = out[write + back]
            else:
                read -= 1
                write -= 1
                out[write] = src_all[read]
            flag_byte = (flag_byte << 1) & 0xFF
    return out


@dataclass
class Rom:
    raw: bytes
    title: str = ""
    gamecode: str = ""
    arm9_ram: int = 0
    arm9_entry: int = 0
    arm7_ram: int = 0
    arm7_entry: int = 0
    arm7_size: int = 0
    arm9_in_rom: bytes = b""
    arm9: bytearray = field(default_factory=bytearray)
    arm9_was_packed: bool = False
    arm7: bytes = b""
    module_params: dict = None
    autoloads: list = field(default_factory=list)
    overlays9: list = field(default_factory=list)


def open_rom(path):
    with open(path, "rb") as fh:
        raw = fh.read()
    rom = Rom(raw=raw)
    rom.title = raw[0:12].rstrip(b"\0").decode("ascii", "replace")
    rom.gamecode = raw[12:16].decode("ascii", "replace")

    # Header words from 0x20: offset, entry, ram address and size for ARM9 then ARM7,
    # then the file name table, the allocation table and the two overlay tables.
    (arm9_off, rom.arm9_entry, rom.arm9_ram, arm9_size,
     arm7_off, rom.arm7_entry, rom.arm7_ram, rom.arm7_size,
     _fnt_off, _fnt_size, fat_off, fat_size,
     ovt9_off, ovt9_size, _ovt7_off, _ovt7_size) = struct.unpack_from("<16I", raw, 0x20)

    rom.arm9_in_rom = raw[arm9_off:arm9_off + arm9_size]
    rom.arm7 = raw[arm7_off:arm7_off + rom.arm7_size]
    file_ranges = [struct.unpack_from("<II", raw, fat_off + n * FAT_ENTRY_SIZE)
                   for n in range(fat_size // FAT_ENTRY_SIZE)]

    load_arm9(rom)
    rom.autoloads = find_autoloads(rom)
    rom.overlays9 = [describe_overlay(raw, ovt9_off + n * OVERLAY_ENTRY_SIZE, file_ranges)
                     for n in range(ovt9_size // OVERLAY_ENTRY_SIZE)]
    return rom


def load_arm9(rom):
    """Fill rom.arm9 (decompressed when needed) and rom.module_params."""
    stored = rom.arm9_in_rom
    rom.arm9 = bytearray(stored)
    at = stored.find(MODULE_PARAMS_MAGIC)
    if at < 0:
        return
    block = at - 0x1C  # six fields and the SDK version sit in front of the magic words
    params = dict(zip(MODULE_PARAMS_FIELDS, struct.unpack_from("<6I", stored, block)))
    params["offset"] = block
    rom.module_params = params
    packed_end = params["compressed_static_end"]
    if not packed_end:
        return
    cut = packed_end - rom.arm9_ram
    rom.arm9 = unpack_backward_lz(stored[:cut]) + bytearray(stored[cut:])
    rom.arm9_was_packed = True
    # Clear the "compressed end" field, as emulators and ndstool do, so the game's own
    # boot code does not try to unpack the data a second time.
    struct.pack_into("<I", rom.arm9, block + 0x14, 0)


def find_autoloads(rom):
    """Blocks the boot code copies into the fast ITCM and DTCM memories."""
    params = rom.module_params
    if not params:
        return []
    found = []
    data_at = params["autoload_start"] - rom.arm9_ram
    cursor = params["autoload_list_start"] - rom.arm9_ram
    stop = params["autoload_list_end"] - rom.arm9_ram
    while cursor < stop:
        ram, size, bss = struct.unpack_from("<III", rom.arm9, cursor)
        found.append({"ram": ram, "size": size, "bss": bss, "offset": data_at})
        data_at += size
        cursor += AUTOLOAD_ENTRY_SIZE
    return found


def describe_overlay(raw, entry_at, file_ranges):
    (ov_id, ram, ram_size, bss, sinit_start, sinit_end, file_id,
     flags) = struct.unpack_from("<8I", raw, entry_at)
    rom_start, rom_end = file_ranges[file_id]
    return {
        "id": ov_id, "ram": ram, "ram_size": ram_size, "bss": bss,
        "sinit_start": sinit_start, "sinit_end": sinit_end, "file": file_id,
        "compressed": bool((flags >> 24) & 1),
        "rom_start": rom_start, "rom_end": rom_end,
    }


def overlay_bytes(rom, overlay):
    stored = rom.raw[overlay["rom_start"]:overlay["rom_end"]]
    return unpack_backward_lz(stored) if overlay["compressed"] else bytearray(stored)


def describe(rom):
    packed_note = " (was compressed)" if rom.arm9_was_packed else ""
    out = [
        f"Game: {rom.title} ({rom.gamecode})",
        f"ARM9: {len(rom.arm9_in_rom)} bytes in the ROM, {len(rom.arm9)} bytes after unpacking"
        f"{packed_note}; loads at {rom.arm9_ram:08X}, starts at {rom.arm9_entry:08X}",
        f"ARM7: {rom.arm7_size} bytes at {rom.arm7_ram:08X}, starts at {rom.arm7_entry:08X}",
    ]
    for block in rom.autoloads:
        out.append(f"  autoload -> {block['ram']:08X}  {block['size']} bytes (+{block['bss']} bss)")
    out.append(f"ARM9 overlays: {len(rom.overlays9)}")
    for ov in rom.overlays9:
        packed = "  compressed" if ov["compressed"] else ""
        out.append(f"  #{ov['id']:3d} at {ov['ram']:08X}  {ov['ram_size']:7d} bytes{packed}")
    return "\n".join(out)


def write_pieces(rom, folder):
    os.makedirs(folder, exist_ok=True)

    def dump(name, blob):
        with open(os.path.join(folder, name), "wb") as fh:
            fh.write(blob)

    dump("arm9.bin", rom.arm9)
    dump("arm7.bin", rom.arm7)
    for ov in rom.overlays9:
        dump(f"overlay9_{ov['id']}.bin", overlay_bytes(rom, ov))
    manifest = {
        "title": rom.title, "gamecode": rom.gamecode,
        "arm9_ram": rom.arm9_ram, "arm9_entry": rom.arm9_entry,
        "arm7_ram": rom.arm7_ram, "arm7_entry": rom.arm7_entry,
        "module_params": rom.module_params, "autoloads": rom.autoloads,
        "overlays9": rom.overlays9,
    }
    with open(os.path.join(folder, "info.json"), "w") as fh:
        json.dump(manifest, fh, indent=2)


if __name__ == "__main__":
    image = open_rom(sys.argv[1])
    print(describe(image))
    if len(sys.argv) > 2:
        write_pieces(image, sys.argv[2])
        print(f"Extracted to {sys.argv[2]}")
