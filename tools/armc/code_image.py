"""The code as it sits in DS memory: a list of (start address, bytes) segments."""

import struct


class CodeImage:
    def __init__(self):
        self.segments = []

    def add(self, start, data):
        self.segments.append((start, bytes(data)))

    def _segment_for(self, addr, length):
        for start, blob in self.segments:
            if start <= addr and addr + length <= start + len(blob):
                return start, blob
        return None

    def has(self, addr, length):
        return self._segment_for(addr, length) is not None

    def half(self, addr):
        start, blob = self._segment_for(addr, 2)
        return struct.unpack_from("<H", blob, addr - start)[0]

    def word(self, addr):
        start, blob = self._segment_for(addr, 4)
        return struct.unpack_from("<I", blob, addr - start)[0]
