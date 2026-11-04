"""Naughty Dog DC (script data) container reader for PS3 (DC00, BE, 32-bit ptr)
and PS4 (00CD, LE, 64-bit ptr)."""
import struct, zlib

def sid(s):
    """CRC32 poly 0x04C11DB7 MSB-first, init 0 (Naughty Dog string id)."""
    c = 0
    for ch in s.encode():
        c ^= ch << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) & 0xffffffff if c & 0x80000000 else (c << 1) & 0xffffffff
    return c

class DC:
    def __init__(self, path):
        self.d = d = open(path, 'rb').read()
        self.path = path
        if d[:4] == b'DC00':
            self.be, self.ps = True, 4
        elif d[:4] == b'00CD':
            self.be, self.ps = False, 8
        else:
            raise ValueError('not DC: %r' % d[:4])
        e = '>' if self.be else '<'
        self.e = e
        if self.ps == 4:
            self.reloc = struct.unpack_from(e + 'I', d, 8)[0]
        else:
            self.reloc = struct.unpack_from(e + 'Q', d, 8)[0]
        n = struct.unpack_from(e + 'I', d, 0x14)[0]
        self.syms = []
        o = 0x18
        for i in range(n):
            if self.ps == 4:
                s, t, off = struct.unpack_from(e + 'III', d, o); o += 12
            else:
                s, t = struct.unpack_from(e + 'II', d, o); off = struct.unpack_from(e + 'Q', d, o + 8)[0]; o += 16
            self.syms.append((s, t, off))
        self.bysid = {s: (t, off) for s, t, off in self.syms}
        # relocation bitmap
        self.ptrs = set()
        if self.reloc and self.reloc < len(d):
            cnt = struct.unpack_from(e + 'I', d, self.reloc)[0]
            bm = d[self.reloc + 4:self.reloc + 4 + cnt]
            for bi, byte in enumerate(bm):
                if byte:
                    for k in range(8):
                        if byte & (1 << k):
                            self.ptrs.add((bi * 8 + (7 - k if self.be else k)) * self.ps)

    def u32(self, o): return struct.unpack_from(self.e + 'I', self.d, o)[0]
    def i32(self, o): return struct.unpack_from(self.e + 'i', self.d, o)[0]
    def f32(self, o): return struct.unpack_from(self.e + 'f', self.d, o)[0]
    def ptr(self, o):
        return struct.unpack_from(self.e + ('I' if self.ps == 4 else 'Q'), self.d, o)[0]
    def isptr(self, o): return o in self.ptrs
    def cstr(self, o):
        if not o or o >= len(self.d): return None
        z = self.d.find(b'\0', o)
        return self.d[o:z].decode('latin1')

    def sym(self, s):
        if isinstance(s, str): s = sid(s)
        return self.bysid.get(s)

def dump(c, off, n, indent=''):
    """dump n pointer-size words with pointer/string annotation"""
    out = []
    for k in range(0, n * c.ps, c.ps):
        o = off + k
        if c.isptr(o):
            v = c.ptr(o); s = c.cstr(v)
            if s and len(s) > 1 and all(32 <= ord(ch) < 127 for ch in s):
                out.append('%s%05x: -> %05x "%s"' % (indent, o, v, s[:60]))
            else:
                out.append('%s%05x: -> %05x' % (indent, o, v))
        else:
            if c.ps == 8:
                a, b = struct.unpack_from(c.e + 'II', c.d, o)
                out.append('%s%05x: %08x %08x' % (indent, o, a, b))
            else:
                out.append('%s%05x: %08x' % (indent, o, c.u32(o)))
    return '\n'.join(out)
