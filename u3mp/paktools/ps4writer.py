"""Builder for PS4 (Nathan Drake Collection) Uncharted 3 .pak files (version 0x0a79,
little-endian, 64-bit page-relative pointers + fixup table).  See ndpak.py / FINDINGS.md
for the format.

Usage sketch:
    w = PakWriter()
    p0 = w.page()                     # data page
    it = p0.item('RAW_DATA', b'...')  # returns Item; it.data is a Blob
    blob.ptr(other_blob, off)         # 8-byte pointer, emitted with a fixup
    w.set_login([...items...]); w.set_vram_table([...VRAM_DESC items...]); w.vram = bytes
    data = w.build()
"""
import struct

PAD = 0xaa


def align(n, a):
    return (n + a - 1) // a * a


class Blob:
    """A chunk of bytes with pointer relocations.  Blobs can be embedded inside other blobs."""

    def __init__(self, name=''):
        self.b = bytearray()
        self.relocs = []      # (pos, target_blob, target_off)
        self.parent = None
        self.off_in_parent = 0
        self.name = name

    def __len__(self):
        return len(self.b)

    @property
    def pos(self):
        return len(self.b)

    def u8(self, v): self.b += struct.pack('<B', v); return self
    def u16(self, v): self.b += struct.pack('<H', v); return self
    def u32(self, v): self.b += struct.pack('<I', v & 0xffffffff); return self
    def i32(self, v): self.b += struct.pack('<i', v); return self
    def u64(self, v): self.b += struct.pack('<Q', v); return self
    def raw(self, data): self.b += data; return self

    def pad(self, a, fill=0):
        while len(self.b) % a:
            self.b.append(fill)
        return self

    def ptr(self, target, off=0):
        if target is None:
            self.u64(0)
        else:
            self.relocs.append((len(self.b), target, off))
            self.u64(0)
        return self

    def cstr(self, s):
        """append a NUL terminated string, return its offset in this blob"""
        o = len(self.b)
        self.b += (s.encode() if isinstance(s, str) else s) + b'\0'
        return o

    def embed(self, child, a=16, fill=0):
        self.pad(a, fill)
        child.parent = self
        child.off_in_parent = len(self.b)
        self.b += child.b
        for pos, t, o in child.relocs:
            self.relocs.append((child.off_in_parent + pos, t, o))
        child.relocs = []
        return child.off_in_parent

    def root(self):
        """(root blob, offset of self inside root)"""
        o = 0
        n = self
        while n.parent is not None:
            o += n.off_in_parent
            n = n.parent
        return n, o


class StrPool:
    """String pool helper: returns (blob, offset) of interned strings in a blob."""

    def __init__(self, blob):
        self.blob = blob
        self.map = {}

    def get(self, s):
        if s not in self.map:
            self.map[s] = self.blob.cstr(s)
        return self.map[s]


class Item:
    def __init__(self, type_, name='', data=None):
        self.type = type_
        self.name = name
        self.data = data if data is not None else Blob(type_)
        self.page = None
        self.off = None          # item header offset in page
        self.listed = True

    @property
    def data_off(self):
        return self.off + 0x20


class RawSpan:
    """Verbatim bytes transplanted from another PS4 pak page.

    internal: list of (pos, target_pos) pointer fields pointing inside the span
    headers:  list of (item_pos, type_str) of ResItems inside the span (their name/type
              pointer fields are page relative and get rebased, no fixup)
    """

    def __init__(self, data, internal, headers, entries, modulo=0x1000, phase=0):
        self.data = bytearray(data)
        self.internal = internal
        self.headers = headers
        self.entries = entries   # (item_pos, name_pos) page entries to add
        self.modulo = modulo
        self.phase = phase       # required (offset % modulo)
        self.page = None
        self.off = None


class Page:
    def __init__(self, writer, index):
        self.w = writer
        self.index = index
        self.parts = []   # Items, RawSpans, or ('blob', Blob) for loose data
        self.size = 0
        self.bytes = None

    def item(self, type_, data=None, name='', listed=True):
        it = Item(type_, name, data)
        it.listed = listed
        it.page = self
        self.parts.append(it)
        return it

    def span(self, span):
        span.page = self
        self.parts.append(span)
        return span


class RawPage(Page):
    """A page copied verbatim from an existing PS4 pak (header, entries and items untouched).
    fixups: list of (offset, dstPage) for the pointer fields inside it (original page indices
    must stay valid in the new pak).  redirect() repoints one pointer field at a Blob placed in
    another page; set_u32/set_u64 patch plain values."""

    def __init__(self, writer, index, data, fixups):
        Page.__init__(self, writer, index)
        self.data = bytearray(data)
        self.src_fixups = list(fixups)
        self.redirects = []
        self.appended = []      # Blobs appended after the retail data (unlisted, no page entry)
        self.name = 'rawpage%d' % index

    def append(self, blob):
        self.appended.append(blob)
        return blob

    def root(self):
        return self, 0

    def redirect(self, off, target, toff=0):
        self.redirects.append((off, target, toff))

    def set_u32(self, off, v):
        struct.pack_into('<I', self.data, off, v)

    def set_u64(self, off, v):
        struct.pack_into('<Q', self.data, off, v)


class PakWriter:
    def __init__(self):
        self.pages = []
        self.login = []        # list of Items
        self.vram_items = []   # list of VRAM_DESC Items (pointers to item headers)
        self.vram = b''
        self.login_item = None      # Item holding the PAK_LOGIN_TABLE body
        self.login_targets = []     # Items listed in the login table (in order)
        self.vram_table_item = None
        self.flags = 0

    def page(self):
        p = Page(self, len(self.pages))
        self.pages.append(p)
        return p

    def raw_page(self, data, fixups):
        p = RawPage(self, len(self.pages), data, fixups)
        self.pages.append(p)
        return p

    # ---------------------------------------------------------------- layout
    def _layout_page(self, page):
        if isinstance(page, RawPage):
            red = {o for o, _t, _x in page.redirects}
            page.fixups = [(page.index, d, o) for o, d in page.src_fixups if o not in red]
            page.blob_abs = {id(page): (page, 0)}
            page.bytes = bytearray(page.data)
            for b in page.appended:
                while len(page.bytes) % 16:
                    page.bytes.append(PAD)
                page.blob_abs[id(b)] = (page, len(page.bytes))
                page.bytes += b.b
            if page.appended:   # ResPageHeader.m_pageSize must equal the page table size
                struct.pack_into('<I', page.bytes, 0xc, len(page.bytes))
            page.size = len(page.bytes)
            return
        listed = [x for x in page.parts if isinstance(x, Item) and x.listed]
        nent = len(listed) + sum(len(x.entries) for x in page.parts if isinstance(x, RawSpan))
        out = bytearray()
        out += struct.pack('<QII', 0, 0, 0)         # link, pkg index, size (patched)
        out += struct.pack('<HH', 0, nent)
        out += b'\0' * (16 * max(nent, 1))           # entries patched later (retail keeps one
                                                      # zeroed slot when a page has no entries)
        entries = []
        page.fixups = []      # (pos, target_blob/abs) resolved later
        page.blob_abs = {}    # root blob id -> page offset
        page.loose = []
        for x in page.parts:
            if isinstance(x, Item):
                while len(out) % 16:
                    out.append(PAD)
                x.off = len(out)
                d = x.data
                hdr_pos = len(out)
                out += b'\0' * 0x20
                page.blob_abs[id(d)] = (page, len(out))
                out += d.b
                if x.listed:
                    type_pos = len(out)
                    out += x.type.encode() + b'\0'
                    name_pos = len(out)
                    out += x.name.encode() + b'\0'
                else:
                    type_pos = name_pos = 0
                struct.pack_into('<QQIIII', out, hdr_pos, name_pos, type_pos, len(d.b), 0x12345, 0xaaaaaaaa, 0)
                if x.listed:
                    entries.append((name_pos, hdr_pos))
                page.loose.append(d)
            elif isinstance(x, RawSpan):
                while len(out) % 16 or (len(out) % x.modulo) != x.phase:
                    out.append(PAD)
                base = len(out)
                x.off = base
                data = bytearray(x.data)
                for ipos, _t in x.headers:
                    nm, ty = struct.unpack_from('<QQ', data, ipos)
                    struct.pack_into('<QQ', data, ipos, nm + base, ty + base)
                for pos, tgt in x.internal:
                    struct.pack_into('<Q', data, pos, tgt + base)
                    page.fixups.append((page.index, page.index, base + pos))
                out += data
                for ipos, npos in x.entries:
                    entries.append((base + npos, base + ipos))
            else:
                raise TypeError(x)
        assert len(entries) == nent, (len(entries), nent)
        for i, (nm, io) in enumerate(entries):
            struct.pack_into('<QII', out, 0x14 + 16 * i, nm, io, 0x12345)
        struct.pack_into('<I', out, 0xc, len(out))
        page.bytes = out
        page.size = len(out)

    def build(self):
        self._abs_cache = None
        for p in self.pages:
            self._layout_page(p)
        li = self.login_item
        if li is not None:
            for i, t in enumerate(self.login_targets):
                struct.pack_into('<II', li.page.bytes, li.data_off + 8 + 8 * i, t.page.index, t.off)
            self.login_page = (li.page.index, li.off)
            vt = self.vram_table_item
            self.vram_table_loc = (vt.page.index, vt.off) if vt is not None else None
        else:   # login/vram tables live in a RawPage: caller sets login_page / vram_table_loc
            assert getattr(self, 'login_page', None) is not None
        # resolve relocations
        fix = []
        for p in self.pages:
            fix += p.fixups
        for p in self.pages:
            if isinstance(p, RawPage):
                for pos, tgt, toff in p.redirects:
                    root, roff = tgt.root()
                    tp, tbase = self._all_abs()[id(root)]
                    struct.pack_into('<Q', p.bytes, pos, tbase + roff + toff)
                    fix.append((p.index, tp.index, pos))
                for b in p.appended:
                    _pp, dbase = p.blob_abs[id(b)]
                    for pos, tgt, toff in b.relocs:
                        root, roff = tgt.root()
                        tp, tbase = self._all_abs()[id(root)]
                        struct.pack_into('<Q', p.bytes, dbase + pos, tbase + roff + toff)
                        fix.append((p.index, tp.index, dbase + pos))
                continue
            for x in p.parts:
                if not isinstance(x, Item):
                    continue
                d = x.data
                _pp, dbase = p.blob_abs[id(d)]
                for pos, tgt, toff in d.relocs:
                    root, roff = tgt.root()
                    if id(root) not in self._all_abs():
                        raise ValueError('pointer target blob %r not placed' % tgt.name)
                    tp, tbase = self._all_abs()[id(root)]
                    struct.pack_into('<Q', p.bytes, dbase + pos, tbase + roff + toff)
                    fix.append((p.index, tp.index, dbase + pos))
        self.fixups = fix
        # header
        pageCt = len(self.pages)
        hdr = bytearray()
        fixdir_off = 0x2c + 12 * pageCt
        fix_off = align(fixdir_off + 8 * 12, 16)
        hdr_size = fix_off + 8 * len(fix)
        page_offs = []
        o = align(hdr_size, 16)
        for p in self.pages:
            page_offs.append(o)
            o = align(o + p.size, 16)
        vram_base = o
        lp, lo = self.login_page
        vp, vo = self.vram_table_loc if self.vram_table_loc else (lp, 0)
        hdr += struct.pack('<HHIIIIIIIIII', 0x0a79, self.flags, hdr_size, lp, lo, pageCt, 0x2c, 8,
                           fixdir_off, len(self.vram), vp, vo)
        for p, po in zip(self.pages, page_offs):
            hdr += struct.pack('<III', po, p.size, 0)
        hdr += struct.pack('<III', 0, fix_off, len(fix))
        hdr += struct.pack('<III', 0, 0, 0) * 7
        while len(hdr) < fix_off:
            hdr.append(PAD)
        for s, dpg, off in fix:
            hdr += struct.pack('<HHI', s, dpg, off)
        assert len(hdr) == hdr_size
        out = bytearray(hdr)
        for p, po in zip(self.pages, page_offs):
            while len(out) < po:
                out.append(PAD)
            out += p.bytes
        while len(out) < vram_base:
            out.append(PAD)
        out += self.vram
        return bytes(out)

    def _all_abs(self):
        if not hasattr(self, '_abs_cache') or self._abs_cache is None:
            self._abs_cache = {}
            for p in self.pages:
                self._abs_cache.update(p.blob_abs)
        return self._abs_cache


def make_login_and_vram_page(w, page, login_items, vram_items):
    """Page 1 layout used by the PS4 build: the PAK_LOGIN_TABLE body followed by the
    VRAM desc table (both unlisted ResItems with null name/type, as in retail paks)."""
    lb = Blob('login')
    lb.u32(len(login_items)).u32(0)
    lb.raw(b'\0' * (8 * len(login_items)))      # (page, offset) pairs patched in build()
    login = page.item('PAK_LOGIN_TABLE', lb, listed=False)
    w.login_item, w.login_targets = login, list(login_items)
    if vram_items:
        vb = Blob('vramtable')
        vb.u32(len(vram_items)).u32(0)
        for it in vram_items:
            vb.ptr(it.data, -0x20)                # points at the VRAM_DESC item header
        w.vram_table_item = page.item('VRAM_DESC_TABLE', vb, listed=False)
    return login
