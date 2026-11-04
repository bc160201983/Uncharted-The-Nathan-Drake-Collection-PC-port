#!/usr/bin/env python3
"""Convert a PS3 Uncharted 3 UI texture/material package (net.pak, also works for other
MATERIAL_TABLE_1 + VRAM_DESC only paks such as hud.pak) into a PS4 / Nathan Drake
Collection package.

What gets written (page 0 unless noted):
  TEXTURE_TABLE           {count, u32 hash[], char* name[], VramDesc* desc[] (inline)}
  RAW_DATA + VRAM_DESC    per texture (short name string + PS4 descriptor/full path)
  RAW_DATA x2             TEXTURE_TABLE name and hash arrays
  EFFECT_TABLE + EFFECTs  transplanted verbatim from the PS4 reference pak (menu.pak),
                          placed at the same page offset modulo 0x1000 (GCN code alignment)
  RAW_DATA (geometry strings / textures / bindings / params / materials)
  GEOMETRY_1 (v0x10)      no meshes; one texture entry per texture and one material
                          instance per PS3 material: name = PS3 material name (so the
                          menu's sid lookups such as 0x7e2c2757 match), shader
                          hud/textured-2d.fx, effect = reference "menu-logo" effect,
                          bindings g_tColorMap / g_tTransparencyMap -> PS3 textures.
  page 1: PAK_LOGIN_TABLE {EFFECT_TABLE, TEXTURE_TABLE, GEOMETRY_1} + VRAM desc table
  VRAM:   PS3 linear DXT texels re-laid out as PS4 1D-thin 8x8-block micro tiles.
"""
import argparse, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ndpak import Pak, sid
from ps3mat import read_materials
from ps4writer import PakWriter, Blob, Item, RawSpan, make_login_and_vram_page
import texcodec as tc

PS4_COOK_PREFIX = 'd:/dev/CP9/Data/U3/CookData/ND/build/texture2/'
PS3_COOK_MARK = 'texture1/'

EFFECTS = {   # material instances in PS4 menu.pak whose effect we reuse
    'logo': 'general/menu/menu-logo:naughtydog',   # textured-2d.fx, ND_TRANSPARENCY, color+transparency
    'edge': 'general/menu/menu-edge:default',      # textured-2d.fx, ND_MENU3D + ND_TRANSPARENCY
}


# ------------------------------------------------------------------ reference pak
def extract_effect_span(ref: Pak):
    """Return RawSpan with EFFECT_TABLE + every item it reaches (EFFECTs, RAW_DATA)."""
    et = ref.items_of('EFFECT_TABLE')[0]
    pg = et.page
    items = sorted([i for i in ref.items if i.page == pg], key=lambda i: i.off)

    def container(off):
        for it in items:
            if it.off <= off < it.off + 0x20 + it.size:
                return it
        return None
    seen, todo = {}, [et]
    while todo:
        it = todo.pop()
        if it.off in seen:
            continue
        seen[it.off] = it
        for o in range(it.data_off, it.data_off + it.size, 8):
            r = ref.resolve(pg, o)
            if r:
                if r[0] != pg:
                    raise ValueError('effect data points into another page')
                c = container(r[1])
                if c is None:
                    raise ValueError('effect pointer to loose data %#x' % r[1])
                todo.append(c)
    span_items = sorted(seen.values(), key=lambda i: i.off)
    start = span_items[0].off
    last = span_items[-1]
    end = last.data_off + last.size
    # include the trailing type/name strings
    end = ref.d.find(b'\0', ref.pbase(pg) + end) - ref.pbase(pg) + 1   # type string
    end = ref.d.find(b'\0', ref.pbase(pg) + end) - ref.pbase(pg) + 1   # name string
    # all items in [start,end) must be part of the closure
    inside = [i for i in items if start <= i.off < end]
    if [i.off for i in inside] != [i.off for i in span_items]:
        raise ValueError('effect closure is not contiguous')
    base = ref.pbase(pg)
    data = ref.d[base + start: base + end]
    internal = []
    for (a, b, off) in ref.fixups:
        if a == pg and start <= off < end:
            if b != pg:
                raise ValueError('cross page pointer inside effect span')
            tgt = ref.pptr(pg, off)
            if not (start <= tgt < end):
                raise ValueError('effect pointer leaves span: %#x -> %#x' % (off, tgt))
            internal.append((off - start, tgt - start))
    headers = [(i.off - start, i.type) for i in span_items]
    # retail ResItem name/type pointers are page relative -> make them span relative
    data = bytearray(data)
    for ipos, _ in headers:
        nm, ty = struct.unpack_from('<QQ', data, ipos)
        struct.pack_into('<QQ', data, ipos, nm - start, ty - start)
    for pos, tgt in internal:
        struct.pack_into('<Q', data, pos, 0)
    entries = []
    for i in span_items:
        nm = struct.unpack_from('<Q', data, i.off - start)[0]
        entries.append((i.off - start, nm))
    return RawSpan(bytes(data), internal, headers, entries, modulo=0x1000, phase=start % 0x1000), span_items


def effect_hash_for(ref: Pak, material_name):
    """find the effect hash string used by a material instance of the reference GEOMETRY_1"""
    g = ref.items_of('GEOMETRY_1')[0]
    pg, b = g.page, g.data_off
    n = ref.pu32(pg, b + 0x48)
    mats = ref.resolve(pg, b + 0x50)[1]
    for i in range(n):
        m = mats + i * 0x48
        nm = ref.pstr(pg, ref.resolve(pg, m)[1])
        if nm.split(',')[0] == material_name:
            sh = ref.pstr(pg, ref.resolve(pg, m + 8)[1])
            eh = ref.pstr(pg, ref.resolve(pg, m + 0x18)[1])
            np_ = ref.pu32(pg, m + 0x30)
            pl = ref.resolve(pg, m + 0x10)
            params = [ref.pstr(pg, ref.resolve(pg, pl[1] + 8 * k)[1]) for k in range(np_)] if pl else []
            return sh, eh, params
    raise KeyError(material_name)


# ------------------------------------------------------------------ conversion
def classify_usage(mats, ntex):
    usage = [set() for _ in range(ntex)]
    for m in mats:
        for slot, t in enumerate(m['textures']):
            if t is not None:
                usage[t].add('color' if slot == 0 else 'trans')
    return usage


def ps4_format_for(name, usage, srgb_color=True):
    alphaish = any(k in name for k in ('-trans', '_trans', '-alpha', '_alpha', '-mask'))
    if 'trans' in usage or (not usage and alphaish) or not srgb_color:
        return 0x47      # BC1_UNORM
    return 0x48          # BC1_UNORM_SRGB


def convert(ps3_path, ref_path, out_path, effect='logo', foreground=False, srgb_color=True,
            stream_flags=3, verbose=True):
    src = Pak(ps3_path)
    ref = Pak(ref_path)
    assert src.platform == 'ps3' and ref.platform == 'ps4'
    mats, mtex = read_materials(src)
    texs = src.textures()
    by_hash = {t['hash']: t for t in texs}
    # map material-table texture index -> VRAM_DESC
    for t in mtex:
        if t['hash'] not in by_hash:
            raise ValueError('material texture %s has no VRAM_DESC' % t['name'])
    usage_by_hash = {}
    for t, u in zip(mtex, classify_usage(mats, len(mtex))):
        usage_by_hash[t['hash']] = u

    w = PakWriter()
    p0 = w.page()
    p1 = w.page()

    # ---------------- textures
    vram = bytearray()
    tex_entries = []   # dict(hash, short, full, desc_item, name_item)
    ttab = p0.item('TEXTURE_TABLE')
    for t in texs:
        full3 = t['path']
        short = full3.split(PS3_COOK_MARK, 1)[1] if PS3_COOK_MARK in full3 else full3
        if sid(short) != t['hash']:
            raise ValueError('hash mismatch for %s' % short)
        fmt3 = tc.ps3_format(t)
        if fmt3 is None or not fmt3[2]:
            raise NotImplementedError('PS3 texture format %#x (%s)' % (t['gcmRemapCtl'], short))
        kind, bpb, comp = fmt3
        texels = src.vram_bytes(t['vramOffset'], t['vramSize'])
        if tc.ps3_linear_size(t['width'], t['height'], t['mips'], bpb) != len(texels):
            raise ValueError('unexpected PS3 size for %s' % short)
        tiled = tc.ps3_to_ps4_texels(texels, t['width'], t['height'], t['mips'], bpb)
        if kind == 'DXT1':
            fmt4 = ps4_format_for(short, usage_by_hash.get(t['hash'], set()), srgb_color)
        elif kind == 'DXT5':
            fmt4 = 0x4d if 'trans' in usage_by_hash.get(t['hash'], set()) or not srgb_color else 0x4e
        elif kind == 'DXT3':
            fmt4 = 0x4a if not srgb_color else 0x4b
        else:
            raise NotImplementedError(kind)
        while len(vram) % 0x100:
            vram.append(0)
        voff = len(vram)
        vram += tiled
        nameb = Blob('texname')
        nameb.cstr(short)
        name_item = p0.item('RAW_DATA', nameb)
        full4 = PS4_COOK_PREFIX + short
        db = Blob('vramdesc')
        db.u64(0).u64(voff).u32(len(tiled)).u32(t['hash'])
        db.u32(1).u32(fmt4)
        db.u32(0x010f0000 if fmt4 in (0x48, 0x4b, 0x4e) else 0x01000000)
        db.u32(t['mips']).u32(t['width']).u32(t['height']).u32(0).u32(stream_flags)
        pathpos = 0x40
        db.ptr(db, pathpos)
        assert db.pos == pathpos
        db.cstr(full4)
        desc_item = p0.item('VRAM_DESC', db)
        tex_entries.append(dict(hash=t['hash'], short=short, full=full4, desc=desc_item, name=name_item,
                                fmt=fmt4, w=t['width'], h=t['height'], mips=t['mips']))
    # texture table: sorted by hash (retail order is arbitrary; consumers scan linearly)
    order = sorted(tex_entries, key=lambda e: e['hash'])
    namesb = Blob('ttnames')
    for e in order:
        namesb.ptr(e['name'].data)
    names_item = p0.item('RAW_DATA', namesb)
    hashb = Blob('tthash')
    for e in order:
        hashb.u32(e['hash'])
    hash_item = p0.item('RAW_DATA', hashb)
    tb = ttab.data
    tb.u64(len(order)).ptr(hash_item.data).ptr(names_item.data)
    tb.ptr(tb, 0x20)
    for e in order:
        tb.ptr(e['desc'].data)

    # ---------------- effects (verbatim from the PS4 reference)
    span, span_items = extract_effect_span(ref)
    p0.span(span)
    shader, effect_hash, eparams = effect_hash_for(ref, EFFECTS[effect])

    # ---------------- geometry (materials only)
    strs = Blob('geostrings')
    spool = {}

    def S(s):
        if s not in spool:
            spool[s] = strs.cstr(s)
        return spool[s]
    texb = Blob('geotextures')
    tex_index = {}
    for i, e in enumerate(tex_entries):
        tex_index[e['hash']] = i
        texb.ptr(strs, S(e['full'])).u32(e['hash']).u32(0x200).u32(0).i32(-1).u32(0).u32(4).u64(0)
    paramsb = Blob('geoparams')
    param_off = paramsb.pos
    for prm in eparams:
        paramsb.ptr(strs, S(prm))
    bindb = Blob('geobindings')
    matb = Blob('geomaterials')
    skipped = []
    for m in mats:
        tl = [x for x in m['textures'] if x is not None]
        if not tl:
            skipped.append(m['name'])
            continue
        color = mtex[tl[0]]
        trans = mtex[tl[1]] if len(tl) > 1 else mtex[tl[0]]
        boff = bindb.pos
        for pname, tt in (('g_tColorMap', color), ('g_tTransparencyMap', trans)):
            e = tex_entries[[k for k, x in enumerate(tex_entries) if x['hash'] == tt['hash']][0]]
            shortname = e['short'].rsplit('/', 1)[0] if e['short'].endswith('.ndb') else e['short']
            bindb.ptr(strs, S(pname)).ptr(strs, S(shortname)).ptr(strs, S(e['full']))
            bindb.i32(-1).u32(0).u64(0)
        matb.ptr(strs, S(m['name'])).ptr(strs, S(shader)).ptr(paramsb if eparams else None, param_off)
        matb.ptr(strs, S(effect_hash)).u64(0).ptr(bindb, boff)
        matb.u64(len(eparams)).u64(2).u64(0)
    nmat_out = len(mats) - len(skipped)
    it_strs = p0.item('RAW_DATA', strs)
    it_tex = p0.item('RAW_DATA', texb)
    it_par = p0.item('RAW_DATA', paramsb) if eparams else None
    it_bind = p0.item('RAW_DATA', bindb)
    it_mat = p0.item('RAW_DATA', matb)
    gb = Blob('geometry')
    gb.u32(0x10).u32(1 if foreground else 0)
    gb.u64(0).ptr(None)        # submeshes
    gb.u64(0).ptr(None)        # prototypes
    gb.u64(0).ptr(None)        # instances
    gb.u64(len(tex_entries)).ptr(texb)
    gb.u64(nmat_out).ptr(matb)
    gb.u64(0).u64(0)
    geo = p0.item('GEOMETRY_1', gb)

    # ---------------- page 1 + login
    make_login_and_vram_page(w, p1, [None] * 3, [e['desc'] for e in tex_entries])
    w.vram = bytes(vram)

    class SpanItem:
        def __init__(self, span, pos):
            self.span, self.pos = span, pos

        @property
        def page(self):
            return self.span.page

        @property
        def off(self):
            return self.span.off + self.pos
    et_pos = [i.off for i in span_items if i.type == 'EFFECT_TABLE'][0] - span_items[0].off
    # login order as in retail menu.pak: EFFECT_TABLE before GEOMETRY_1 (the geometry
    # login binds its materials against the level's effect/material table)
    w.login_targets = [SpanItem(span, et_pos), ttab, geo]
    data = w.build()
    open(out_path, 'wb').write(data)
    if verbose:
        print('wrote %s: %d bytes, %d textures, %d materials (%d skipped), vram %#x, effect %s %s' % (
            out_path, len(data), len(tex_entries), nmat_out, len(skipped), len(vram), effect, effect_hash))
    return dict(textures=tex_entries, materials=mats, skipped=skipped, size=len(data))


def copy_ingame12(ps4_menu_ingame12, out_path, ps3_net_ingame12=None):
    """net-ingame12.pak: on PS3 it is byte-identical to menu-ingame12.pak apart from the
    embedded source path string (LEVEL_INFO_1 / SPLINE_TABLE / TAG_INT coverActionPackCount),
    so the PS4 menu-ingame12.pak (which has no path string) is reused verbatim."""
    ref = Pak(ps4_menu_ingame12)
    assert ref.platform == 'ps4' and not ref.validate()
    if ps3_net_ingame12:
        a = Pak(ps3_net_ingame12)
        types = sorted(i.type for i in a.items if i.type not in ('PAK_LOGIN_TABLE', 'VRAM_DESC_TABLE'))
        assert types == sorted(i.type for i in ref.items), types
    open(out_path, 'wb').write(ref.d)
    print('wrote %s (%d bytes, copy of PS4 menu-ingame12.pak)' % (out_path, len(ref.d)))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('ps3pak')
    ap.add_argument('out')
    ap.add_argument('--ref', required=True, help='PS4 menu.pak (source of EFFECT_TABLE/EFFECTs)')
    ap.add_argument('--effect', choices=sorted(EFFECTS), default='logo')
    ap.add_argument('--foreground', action='store_true', help='mark GEOMETRY_1 as foreground (level type 2)')
    ap.add_argument('--linear-color', action='store_true', help='use BC1_UNORM for color maps instead of sRGB')
    ap.add_argument('--stream-flags', type=lambda x: int(x, 0), default=3)
    ap.add_argument('--ingame12', nargs=2, metavar=('PS4_MENU_INGAME12', 'OUT'),
                    help='also write net-ingame12.pak (copy of the PS4 menu-ingame12.pak)')
    a = ap.parse_args()
    convert(a.ps3pak, a.ref, a.out, a.effect, a.foreground, not a.linear_color, a.stream_flags)
    if a.ingame12:
        copy_ingame12(a.ingame12[0], a.ingame12[1])


if __name__ == '__main__':
    main()
