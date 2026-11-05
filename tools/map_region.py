"""Map the hand-found EUR addresses to another region (USA) using the symbol tables of the
Mario Kart DS decompilation (https://github.com/RonyxDumb/mkds, MIT, config/<region>/arm9/...).

The EUR and USA builds have the same code, but the linker placed things at slightly different
addresses: ARM9 code (0x02000800 - ~0x02150000) is identical, data after it moves by -16/-32 bytes,
and overlay 0/1 move by -32/-64 bytes (differently in different parts of the overlay).  The decomp
has a symbol for the same function in both regions, so aligning the two symbol tables gives an
exact EUR -> USA address for any address inside a known symbol.

    python tools/map_region.py <mkds decomp>/config            -> writes tools/entry_points/usa.txt
    python tools/map_region.py <mkds decomp>/config 0219F3A4        -> maps one code address
    python tools/map_region.py <mkds decomp>/config 021CA410 o1     -> same, naming the image (for data)

The decomp has no ARM7 symbols; the ARM7 of both regions has the same layout (checked against the
USA binary: the OS_*Context hooks and arm7.txt point at the same code), so arm7.txt is shared.

usa.txt lines may start with "oN:" -- "only in overlay N".  Overlay 0 and overlay 1 are loaded
at the same address and the translator used to try every entry in both; a USA entry for overlay 1
would land in the wrong place of overlay 0 (the shift differs), so USA entries say which overlay.
"""

import bisect
import difflib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGES = {"main": "arm9", "o0": "arm9/overlays/ov000", "o1": "arm9/overlays/ov001",
            "o2": "arm9/overlays/ov002", "o3": "arm9/overlays/ov003"}
START_OVERLAYS = 0x02180000  # the ARM9 static image ends here; overlays 0-2 load at 0x021804C0, overlay 3 at 0x021B7B60
AUTO = re.compile(r"^(func|data)_")  # auto-generated names carry the (EUR) address: useless as an alignment key


def read_symbols(file_path):
    out = []
    for line in open(file_path, encoding="utf8"):
        m = re.match(r"(\S+) kind:(\S+) addr:0x([0-9a-fA-F]+)", line)
        if m:
            nbytes = re.search(r"size=0x([0-9a-f]+)", m.group(2))
            out.append((int(m.group(3), 16), m.group(1), m.group(2).split("(")[0], int(nbytes.group(1), 16) if nbytes else None,
                        m.group(2)))
    return sorted(out)


class Map:
    def __init__(self, config):
        self.eur, self.usa, self.pairs = {}, {}, {}
        for img, sub in IMAGES.items():
            e = read_symbols(os.path.join(config, "eur", sub, "symbols.txt"))
            u = read_symbols(os.path.join(config, "usa", sub, "symbols.txt"))
            self.eur[img], self.usa[img] = e, u
            key = lambda s: (s[4], "*" if AUTO.match(s[1]) else s[1])  # kind with ARM/Thumb and size, name (if it has one)
            sm = difflib.SequenceMatcher(None, [key(s) for s in e], [key(s) for s in u], autojunk=False)
            pairs = []
            for a, b, n in sm.get_matching_blocks():
                pairs.extend((e[a + i][0], u[b + i][0]) for i in range(n))
            self.pairs[img] = pairs

    def convert(self, address, img):
        pairs = self.pairs[img]
        i = bisect.bisect_right([p[0] for p in pairs], address) - 1
        return None if i < 0 else pairs[i][1] + address - pairs[i][0]

    def start_function(self, address, img):
        return any(s[0] == address and s[2] == "function" for s in self.eur[img])

    def owners(self, address):
        """Images of the EUR build that an address in an overlay can belong to."""
        if address < START_OVERLAYS:
            return ["main"]
        exact = [i for i in ("o0", "o1", "o3") if self.start_function(address, i)]
        if exact:
            return exact
        return ["o3" if address >= 0x021B7B60 else "o1"]  # inside a function: the Missions overlay (all the rest are there)


def entry_point(line):
    line = line.strip()
    if not line:
        return None
    thumb = line.startswith("T:")
    return int(line[2:] if thumb else line, 16), thumb


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    map = Map(sys.argv[1])
    if len(sys.argv) > 2:  # one address (optionally the image: main, o0, o1, o2, o3 -- needed for data)
        a = int(sys.argv[2], 16)
        for img in (sys.argv[3:4] or map.owners(a)):
            print(f"{a:08X} ({img}) -> {map.convert(a, img):08X}")
        return 0
    outbuf, changes = [], 0
    for line in open(os.path.join(ROOT, "tools", "entry_points", "eur.txt")):
        e = entry_point(line)
        if not e:
            continue
        a, thumb = e
        for img in map.owners(a):
            u = map.convert(a, img)
            if img == "main" and u != a:
                changes += 1
            outbuf.append((f"{img}:" if img != "main" else "") + ("T:" if thumb else "") + f"{u:08X}")
    with open(os.path.join(ROOT, "tools", "entry_points", "usa.txt"), "w") as f:
        f.write("# GENERATED by tools/map_region.py from eur.txt and the symbol tables of the Mario Kart DS decompilation.\n")
        f.write("# Do not edit by hand: put your own lines in usa_extra.txt, which this script never touches.\n")
        f.write("# oN: = only in overlay N (see the docstring of tools/map_region.py).\n")
        f.write("\n".join(outbuf) + "\n")
    print(f"usa.txt: {len(outbuf)} entries ({changes} ARM9 entries moved)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
