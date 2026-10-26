#!/usr/bin/env python3
"""Naughty Dog Uncharted 3 package (.pak, version 0x0a79) parser for the
PS3 (big-endian, 32-bit pointers) and PS4 / Nathan Drake Collection
(little-endian, 64-bit pointers) builds.

File layout (both flavours):

  +0x000 PakHeader (0x2c bytes, all u32 in file endianness)
          0x00 u16 magic 0x0a79, u16 flags (bit0: "no VRAM/skip"?, 0 in all samples)
          0x04 hdrSize            size of header block (header + page entries + fixup tables)
          0x08 loginTablePage     page index of PAK_LOGIN_TABLE item
          0x0c loginTableOffset   offset of the login item (its ResItem header) in that page
          0x10 pageCt
          0x14 pageEntryOffset    always 0x2c
          0x18 numFixupTables     must be 8 (PS4 loader asserts "wrong format" otherwise)
          0x1c fixupTableOffset   offset of 8 x {u32 page(0 = header block), u32 offset, u32 count}
          0x20 vramSize           bytes of VRAM (texel) data appended after the last page
          0x24 vramDescTablePage  page of the VRAM_DESC_TABLE item (ResItem header)
          0x28 vramDescTableOffset
  +0x02c PakPageEntry[pageCt] {u32 fileOffset, u32 size, u32 unused}
  +....  fixup-table directory (8 x 12 bytes), then pointer fixup entries
          {u16 srcPage, u16 dstPage, u32 offset}: *(page[src]+offset) += base(page[dst])
          (stored pointer values are offsets relative to the start of dstPage)
  pages  each page starts with ResPageHeader:
          PS3: u32 link, u32 pageSize, u32 pkgIndex, u16 ?, u16 numEntries  (0x10 bytes)
          PS4: u64 link, u32 pkgIndex, u32 pageSize, u16 ?, u16 numEntries  (0x14 bytes)
          followed by ResPageEntry[numEntries] (16 bytes each, both builds):
              u64 name (page-relative), u32 itemOffset, u32 0x00012345
  VRAM   raw texel data, vramSize bytes, VRAM_DESC offsets are relative to its start.

ResItem header (0x20 bytes in both builds):
  u64 name (page relative ptr to C string)   u64 type (page relative ptr to C string)
  u32 dataSize   u32 0x12345   u32 nameSid(runtime)   u32 typeSid(runtime)
  data follows at +0x20.  (PS3 has 0x12345 fill in the runtime slots, PS4 0xaaaaaaaa/0.)
  The PS4 loader rewrites the page entry name / item name / item type pointers by adding
  the page base and stores sid(name) at +0x18 and sid(type) at +0x1c (package-mgr.cpp,
  UpdatePackage_Status_LoadingPages, 0xdd4620..0xdd4680).
"""
import struct, sys, os

MAGIC = 0x0a79


def sid(s):
    """Naughty Dog string id: CRC-32 (poly 0x04C11DB7), MSB first, init 0, no xor."""
    if isinstance(s, str):
        s = s.encode()
    c = 0
    for ch in s:
        c ^= ch << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) & 0xffffffff if c & 0x80000000 else (c << 1) & 0xffffffff
    return c


class Item:
    __slots__ = ('page', 'off', 'name', 'type', 'size', 'listed')

    def __init__(self, page, off, name, type_, size, listed=True):
        self.page, self.off, self.name, self.type, self.size, self.listed = page, off, name, type_, size, listed

    @property
    def data_off(self):
        return self.off + 0x20

    def __repr__(self):
        return '<Item p%d+%#x %s %r size=%#x>' % (self.page, self.off, self.type, self.name, self.size)


class Pak:
    def __init__(self, path_or_bytes, platform=None):
        if isinstance(path_or_bytes, (bytes, bytearray)):
            self.d = bytes(path_or_bytes)
            self.path = '<bytes>'
        else:
            self.path = path_or_bytes
            self.d = open(path_or_bytes, 'rb').read()
        d = self.d
        if platform is None:
            if struct.unpack_from('>H', d, 0)[0] == 0 and struct.unpack_from('>H', d, 2)[0] == MAGIC:
                platform = 'ps3'
            elif struct.unpack_from('<H', d, 0)[0] == MAGIC:
                platform = 'ps4'
            else:
                raise ValueError('not a 0x0a79 pak')
        self.platform = platform
        self.e = '>' if platform == 'ps3' else '<'
        self.ptrsize = 4 if platform == 'ps3' else 8
        self.pagehdr_size = 0x10 if platform == 'ps3' else 0x14
        self._parse()

    # -- primitive readers (file offsets) --
    def u16(self, o): return struct.unpack_from(self.e + 'H', self.d, o)[0]
    def u32(self, o): return struct.unpack_from(self.e + 'I', self.d, o)[0]
    def i32(self, o): return struct.unpack_from(self.e + 'i', self.d, o)[0]
    def u64(self, o): return struct.unpack_from(self.e + 'Q', self.d, o)[0]
    def ptr(self, o): return self.u32(o) if self.ptrsize == 4 else self.u64(o)
    def f32(self, o): return struct.unpack_from(self.e + 'f', self.d, o)[0]

    def cstr(self, o):
        e = self.d.find(b'\0', o)
        return self.d[o:e].decode('latin-1')

    # -- page relative helpers --
    def pbase(self, page):
        return self.pages[page][0]

    def pu32(self, page, off): return self.u32(self.pbase(page) + off)
    def pptr(self, page, off): return self.ptr(self.pbase(page) + off)
    def pstr(self, page, off): return self.cstr(self.pbase(page) + off)

    def _parse(self):
        d = self.d
        h = [self.u32(i * 4) for i in range(11)]
        self.hdr = dict(magic=h[0] & 0xffff if self.e == '<' else h[0] & 0xffff,
                        flags=(h[0] >> 16) if self.e == '<' else (h[0] >> 16),
                        hdrSize=h[1], loginPage=h[2], loginOff=h[3], pageCt=h[4],
                        pageEntryOff=h[5], numFixupTables=h[6], fixupTableOff=h[7],
                        vramSize=h[8], vramDescPage=h[9], vramDescOff=h[10])
        if self.u16(0 if self.e == '<' else 2) != MAGIC:
            raise ValueError('bad magic')
        self.pages = []
        for i in range(h[4]):
            o = h[5] + i * 12
            self.pages.append((self.u32(o), self.u32(o + 4), self.u32(o + 8)))
        # fixups
        self.fixup_dirs = []
        self.fixups = []  # (srcPage, dstPage, offset)
        for i in range(h[6]):
            o = h[7] + i * 12
            pg, off, cnt = self.i32(o), self.i32(o + 4), self.i32(o + 8)
            self.fixup_dirs.append((pg, off, cnt))
            if cnt <= 0:
                continue
            base = 0 if pg == 0 else None
            if base is None:
                raise ValueError('fixup directory in a non-header page not supported (%d)' % pg)
            for j in range(cnt):
                fo = base + off + j * 8
                a, b = self.u16(fo), self.u16(fo + 2)
                self.fixups.append((a, b, self.u32(fo + 4)))
        self.ptrmap = {}  # (srcPage, off) -> dstPage
        for a, b, off in self.fixups:
            self.ptrmap[(a, off)] = b
        last = self.pages[-1]
        self.vram_base = (last[0] + last[1] + 15) & ~15 if self.pages else h[1]
        # sanity: vram should end exactly at EOF
        self.vram_base = len(d) - h[8]
        # page headers / entries / items
        self.items = []
        self.page_entries = []
        for pi, (po, ps, _) in enumerate(self.pages):
            if self.platform == 'ps3':
                psize = self.u32(po + 4)
            else:
                psize = self.u32(po + 0xc)
            n = self.u16(po + self.pagehdr_size - 2)
            ents = []
            for j in range(n):
                eo = po + self.pagehdr_size + j * 16
                nm = self.u64(eo)
                io = self.u32(eo + 8)
                ents.append((nm, io))
                self.items.append(self._item(pi, io))
            self.page_entries.append(dict(size=psize, entries=ents))
        self.login = self._login()

    def _item(self, page, off, listed=True):
        b = self.pbase(page) + off
        nm, ty, sz = self.u64(b), self.u64(b + 8), self.u32(b + 16)
        return Item(page, off, self.pstr(page, nm), self.pstr(page, ty), sz, listed)

    def _login(self):
        it = self._item(self.hdr['loginPage'], self.hdr['loginOff'])
        b = self.pbase(it.page) + it.data_off
        n = self.u32(b)
        ents = [(self.u32(b + 8 + i * 8), self.u32(b + 12 + i * 8)) for i in range(n)]
        return dict(item=it, entries=ents)

    def item_at(self, page, off):
        for it in self.items:
            if it.page == page and it.off == off:
                return it
        return self._item(page, off, listed=False)

    def items_of(self, type_):
        return [i for i in self.items if i.type == type_]

    def is_ptr(self, page, off):
        return (page, off) in self.ptrmap

    def resolve(self, page, off):
        """Read pointer at page:off; returns (dstPage, dstOff) or None for null/unfixed."""
        if (page, off) not in self.ptrmap:
            return None
        return self.ptrmap[(page, off)], self.pptr(page, off)

    # -- VRAM descriptors --
    def vram_desc_table(self):
        if self.hdr['vramSize'] == 0 and self.platform == 'ps4' and self.hdr['vramDescPage'] == 0 and self.hdr['vramDescOff'] == 0:
            return []
        pg, off = self.hdr['vramDescPage'], self.hdr['vramDescOff']
        it = self.item_at(pg, off)
        b = it.data_off
        n = self.pu32(pg, b)
        out = []
        if self.platform == 'ps3':
            for i in range(n):
                o = b + 4 + i * 4
                out.append(self.resolve(pg, o) or (None, self.pu32(pg, o)))
        else:
            for i in range(n):
                o = b + 8 + i * 8
                out.append(self.resolve(pg, o) or (None, self.pptr(pg, o)))
        return out

    def vram_desc(self, page, off):
        """Parse a VRAM_DESC body (off = data offset, i.e. item+0x20)."""
        b = self.pbase(page) + off
        if self.platform == 'ps3':
            r = dict(runtime=self.u32(b), vramOffset=self.u32(b + 4), vramSize=self.u32(b + 8),
                     hash=self.u32(b + 0xc), type=self.u32(b + 0x10), gcmRemapCtl=self.u32(b + 0x14),
                     gcmFormatCtl=self.u32(b + 0x18), mips=self.u32(b + 0x1c), width=self.u32(b + 0x20),
                     height=self.u32(b + 0x24), depth=self.u32(b + 0x28), unk2c=self.u32(b + 0x2c))
            r['path'] = self.pstr(page, self.u32(b + 0x30))
        else:
            r = dict(runtime=self.u64(b), vramOffset=self.u64(b + 8), vramSize=self.u32(b + 0x10),
                     hash=self.u32(b + 0x14), type=self.u32(b + 0x18), format=self.u32(b + 0x1c),
                     unk20=self.u32(b + 0x20), mips=self.u32(b + 0x24), width=self.u32(b + 0x28),
                     height=self.u32(b + 0x2c), unk30=self.u32(b + 0x30), unk34=self.u32(b + 0x34))
            r['path'] = self.pstr(page, self.u64(b + 0x38))
        return r

    def textures(self):
        out = []
        for res in self.vram_desc_table():
            if res[0] is None:
                continue
            pg, off = res          # points at the VRAM_DESC ResItem header
            t = self.vram_desc(pg, off + 0x20)
            t['page'], t['off'] = pg, off
            out.append(t)
        return out

    def vram_bytes(self, off, size):
        return self.d[self.vram_base + off:self.vram_base + off + size]

    # -- validation --
    def validate(self):
        errs = []
        d = self.d
        h = self.hdr
        if h['numFixupTables'] != 8:
            errs.append('numFixupTables != 8')
        end = h['hdrSize']
        for i, (po, ps, _) in enumerate(self.pages):
            if po < end:
                errs.append('page %d overlaps previous data' % i)
            if self.page_entries[i]['size'] != ps:
                errs.append('page %d header size %#x != entry %#x' % (i, self.page_entries[i]['size'], ps))
            end = po + ps
        if end > self.vram_base:
            errs.append('pages overlap vram data')
        for a, b, off in self.fixups:
            if a >= len(self.pages) or b >= len(self.pages):
                errs.append('fixup page out of range %r' % ((a, b, off),))
                continue
            if off + self.ptrsize > self.pages[a][1]:
                errs.append('fixup offset out of page %r' % ((a, b, off),))
                continue
            v = self.pptr(a, off)
            if v > self.pages[b][1]:
                errs.append('pointer value out of dst page %r -> %#x' % ((a, b, off), v))
        for pg, off in self.login['entries']:
            if pg >= len(self.pages) or off >= self.pages[pg][1]:
                errs.append('login entry out of range %r' % ((pg, off),))
        for t in self.textures():
            if t['vramOffset'] + t['vramSize'] > h['vramSize']:
                errs.append('texture %s vram range out of bounds' % t['path'])
        return errs


def summary(p, out=sys.stdout, items=True):
    h = p.hdr
    w = out.write
    w('%s  [%s]  size=%#x\n' % (p.path, p.platform, len(p.d)))
    w('  header: ' + ' '.join('%s=%#x' % kv for kv in h.items()) + '\n')
    for i, pg in enumerate(p.pages):
        w('  page %d: file %#x size %#x  entries=%d\n' % (i, pg[0], pg[1], len(p.page_entries[i]['entries'])))
    w('  fixups: %d  vram base %#x size %#x\n' % (len(p.fixups), p.vram_base, h['vramSize']))
    w('  login table (%d): %s\n' % (len(p.login['entries']), ', '.join(
        '%s@p%d+%#x' % (p.item_at(pg, off).type, pg, off) for pg, off in p.login['entries'])))
    from collections import Counter
    c = Counter(i.type for i in p.items)
    w('  item types: ' + ', '.join('%s x%d' % kv for kv in sorted(c.items())) + '\n')
    if items:
        for it in p.items:
            w('    p%d+%06x %-22s size %#7x  %s\n' % (it.page, it.off, it.type, it.size, it.name))
    errs = p.validate()
    w('  validate: %s\n' % ('OK' if not errs else '; '.join(errs[:10])))


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser(description='dump an Uncharted 3 .pak (PS3 or PS4)')
    ap.add_argument('pak', nargs='+')
    ap.add_argument('--textures', action='store_true')
    ap.add_argument('--noitems', action='store_true')
    a = ap.parse_args()
    for f in a.pak:
        p = Pak(f)
        summary(p, items=not a.noitems)
        if a.textures:
            for t in p.textures():
                print('    tex %08x %4dx%-4d mips %2d vram %#9x +%#8x %s %s' % (
                    t['hash'], t['width'], t['height'], t['mips'], t['vramOffset'], t['vramSize'],
                    ('fmt=%#x' % t['format']) if 'format' in t else ('gcm=%08x/%08x' % (t['gcmRemapCtl'], t['gcmFormatCtl'])),
                    t['path']))
