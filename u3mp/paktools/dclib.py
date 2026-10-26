"""Naughty Dog DC (compiled data/script) module reader, PS3 (BE, 32-bit) and PS4 (LE, 64-bit)."""
import struct

def sid(s):
    if isinstance(s, str): s = s.encode()
    h = 0
    for c in s:
        h ^= c << 24
        for _ in range(8):
            h = ((h << 1) ^ 0x04C11DB7) & 0xffffffff if h & 0x80000000 else (h << 1) & 0xffffffff
    return h

class DC:
    def __init__(self, data, name=''):
        self.d = d = bytes(data)
        self.name = name
        if d[:4] == b'DC00':
            self.e, self.ps, self.plat = '>', 4, 'ps3'
        elif d[:4] == b'00CD':
            self.e, self.ps, self.plat = '<', 8, 'ps4'
        else:
            raise ValueError('not a DC module')
        e = self.e
        self.version = self.u32(4)
        if self.ps == 8:
            self.reloc_off = self.u64(8)
        else:
            self.reloc_off = self.u32(8)
            assert self.u32(12) == 0
        self.nsect = self.u32(0x10)
        self.nsym = self.u32(0x14)
        es = 8 + self.ps
        self.symtab_off = 0x18
        self.syms = []
        for i in range(self.nsym):
            o = 0x18 + i * es
            sid_, typ = struct.unpack_from(e + 'II', d, o)
            val = self.ptr(o + 8)
            self.syms.append((sid_, typ, val))
        self.data_start = 0x18 + self.nsym * es
        # relocation bitmap: u32 byte count, then one bit per pointer-size word
        n = self.u32(self.reloc_off)
        self.bitmap = d[self.reloc_off + 4:self.reloc_off + 4 + n]
        self.reloc_tail = d[self.reloc_off + 4 + n:]
        self.ptrs = set()
        for bi, b in enumerate(self.bitmap):
            if not b: continue
            for k in range(8):
                bit = (b >> (7 - k)) & 1 if self.ps == 4 else (b >> k) & 1
                if bit:
                    self.ptrs.add((bi * 8 + k) * self.ps)
        self.symmap = {s: (t, v) for s, t, v in self.syms}

    def u8(self, o): return self.d[o]
    def u16(self, o): return struct.unpack_from(self.e + 'H', self.d, o)[0]
    def u32(self, o): return struct.unpack_from(self.e + 'I', self.d, o)[0]
    def i32(self, o): return struct.unpack_from(self.e + 'i', self.d, o)[0]
    def f32(self, o): return struct.unpack_from(self.e + 'f', self.d, o)[0]
    def u64(self, o): return struct.unpack_from(self.e + 'Q', self.d, o)[0]
    def ptr(self, o): return self.u64(o) if self.ps == 8 else self.u32(o)
    def isptr(self, o): return o in self.ptrs
    def cstr(self, o):
        z = self.d.find(b'\0', o)
        return self.d[o:z]
