#!/usr/bin/env python3
"""Merge PS3 HUD material names into the retail PS4 hud.pak.

Finding: every one of the 185 PS3 hud.pak materials already exists in the retail PS4
hud.pak, but under the PS4 cook's name form "dir/file:variant,<guid>" while multiplayer code
(PS4 0x62ea60, 0x76bbf0, 0x5e7fc0 ... ) still searches the hud material table (UI+0x20) for
the PS3 slash-form sid, e.g. 0xc1df5e2e = sid("general/hud/store/purchased") whereas retail
has "general/hud/store:purchased" (0x658daf73).  So the merge adds *alias material
instances*: same shader / params / effect / texture bindings as the retail colon material,
but named with the PS3 name.

Layout of the output (retail data kept byte-identical):
  page 0  retail page 0 verbatim; only GEOMETRY_1 +0x48 (material count) and +0x50 (material
          array pointer, now -> page 2) are changed
  page 1  retail page 1 verbatim (login table, VRAM desc table)
  page 2  new: RAW_DATA alias-name strings, RAW_DATA alias bindings (copies of the retail
          binding arrays, pointing at the retail strings), RAW_DATA material array =
          205 retail entries (copied, pointers into page 0) + N aliases
  VRAM    retail VRAM verbatim
A single GEOMETRY_1 is required: the background-geometry login (0xf48060) reuses
level+0xf0 and ConstructFromBoundMaterialTable (0xe4bf60) asserts m_numMaterials == 0 /
m_aMaterial == NULL, so a second GEOMETRY_1 in the same level would trap.
"""
import argparse, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ndpak import Pak, sid
from ps3mat import read_materials
from ps4writer import PakWriter, Blob


def ps4_geometry(p):
    g = p.items_of('GEOMETRY_1')
    assert len(g) == 1
    return g[0]


def ps4_material_list(p):
    g = ps4_geometry(p)
    pg, b = g.page, g.data_off
    n = p.pu32(pg, b + 0x48)
    mp = p.resolve(pg, b + 0x50)
    out = []
    for i in range(n):
        m = mp[1] + 0x48 * i
        name = p.pstr(*p.resolve(mp[0], m))
        out.append(dict(index=i, off=m, page=mp[0], name=name, base=name.split(',')[0]))
    return out


def colon_to_slash(base):
    a, _, b = base.rpartition(':')
    return a + '/' + b if a else base


# slash-form sids the PS4 executable searches for (or holds in data tables) whose material
# lives in the hud level, plus the two emblem quad materials of the PS3 emblem renderer
MIN_SIDS = [0xc1df5e2e, 0xea36b17f, 0x6e9d9193, 0x7f89e267, 0xd41f59a1,
            0x37547fdd, 0x61b60951, 0xcce16bda,                      # trophy outline/solid/blank
            0xd75be949, 0x576ed245, 0xe1e040aa, 0x1f621808,          # mode-buttons ? / ! / warning / check
            0x772e04ce, 0x56f1b7f5,                                  # flag-icons/treasure, treasure-chest
            0xd2e394f5, 0xa0cf6221]                                  # alpha-only-icon, alpha-mask-icon
# LevelMemAlloc (Package::AllocateLoginData) cannot satisfy an allocation larger than a run of
# free 32 KiB sub-pages; the bound-material array is 0x70 bytes per material (0xf63000), so
# keep 0x70 * count well inside one sub-page.
MAX_MATERIALS = (0x8000 - 0x24) // 0x70


def merge(ps4_hud, ps3_paks, out_path, extra_all=False, verbose=True, only_sids=None, layout='page2'):
    ref = Pak(ps4_hud)
    assert ref.platform == 'ps4' and not ref.validate()
    assert ref.hdr['pageCt'] == 2
    mats = ps4_material_list(ref)
    by_base = {m['base']: m for m in mats}
    have_sids = {sid(m['base']) for m in mats}

    # which aliases to add: every PS3 material name whose sid is missing but whose PS4
    # colon-form equivalent exists
    wanted = []   # (alias_name, retail material)
    missing = []
    seen = set()
    for f in ps3_paks:
        for m in read_materials(Pak(f))[0]:
            n = m['name']
            if m['sid'] in have_sids or m['sid'] in seen:
                continue
            if only_sids is not None and m['sid'] not in only_sids:
                continue
            a, _, b = n.rpartition('/')
            eq = by_base.get(a + ':' + b)
            if eq is None:
                missing.append(n)
                continue
            wanted.append((n, eq))
            seen.add(m['sid'])
    if extra_all:   # also slash aliases for PS4-only materials
        for m in mats:
            s = colon_to_slash(m['base'])
            if sid(s) not in have_sids and sid(s) not in seen:
                wanted.append((s, m))
                seen.add(sid(s))

    w = PakWriter()
    pages = []
    for i in range(2):
        po, ps, _ = ref.pages[i]
        fx = [(off, d) for (s, d, off) in ref.fixups if s == i]
        pages.append(w.raw_page(ref.d[po:po + ps], fx))
    p0 = pages[0]
    total = len(mats) + len(wanted)
    if layout == 'append0' and total > MAX_MATERIALS:
        raise ValueError('%d materials would need a %#x byte bound-material array (> one 32 KiB sub-page)'
                         % (total, 0x70 * total))
    strs = Blob('aliasnames')
    names = {}
    for n, _ in wanted:
        names[n] = strs.cstr(n)
    bindb = Blob('aliasbindings')
    matb = Blob('materials')
    pg = mats[0]['page']
    assert pg == 0

    def P(off):     # pointer field in page 0 -> (target RawPage, offset) or None
        r = ref.resolve(0, off)
        if r is None:
            return None
        return pages[r[0]], r[1]

    def copy_entry(m, name_target=None):
        o = m['off']
        if name_target is None:
            t, x = P(o); matb.ptr(t, x)
        else:
            matb.ptr(strs, name_target)
        for k in (8, 0x10, 0x18):
            r = P(o + k)
            matb.ptr(*r) if r else matb.u64(0)
        matb.u64(ref.u64(ref.pbase(0) + o + 0x20))
        nb = ref.pu32(0, o + 0x38)
        if name_target is None:
            r = P(o + 0x28); matb.ptr(*r) if r else matb.u64(0)
        else:   # private copy of the binding array (runtime writes index/hash into it)
            bp = P(o + 0x28)
            if bp is None:
                matb.u64(0)
            else:
                boff = bindb.pos
                for k in range(nb):
                    be = bp[1] + 0x28 * k
                    for f in (0, 8, 0x10):
                        r = P(be + f); bindb.ptr(*r) if r else bindb.u64(0)
                    bindb.raw(ref.d[ref.pbase(0) + be + 0x18: ref.pbase(0) + be + 0x28])
                matb.ptr(bindb, boff)
        for k in (0x30, 0x38, 0x40):
            matb.u64(ref.u64(ref.pbase(0) + o + k))

    for m in mats:
        copy_entry(m)
    for n, m in wanted:
        copy_entry(m, names[n])
    if layout == 'page2':
        p2 = w.page()
        p2.item('RAW_DATA', strs)
        p2.item('RAW_DATA', bindb)
        p2.item('RAW_DATA', matb)
    else:   # keep pageCt = 2: append unlisted data to the end of page 0
        p0.append(matb)
        p0.append(bindb)
        p0.append(strs)

    g = ps4_geometry(ref)
    p0.set_u32(g.data_off + 0x48, len(mats) + len(wanted))
    p0.redirect(g.data_off + 0x50, matb, 0)

    w.login_page = (ref.hdr['loginPage'], ref.hdr['loginOff'])
    w.vram_table_loc = (ref.hdr['vramDescPage'], ref.hdr['vramDescOff'])
    w.vram = ref.d[ref.vram_base:]
    data = w.build()
    open(out_path, 'wb').write(data)
    if verbose:
        print('wrote %s: %d bytes; retail materials %d + %d aliases; no PS4 equivalent for %d: %s' % (
            out_path, len(data), len(mats), len(wanted), len(missing), missing[:10]))
    return wanted, missing


def merge_min(ps4_hud, ps3_paks, out_path, scan_files, sids=MIN_SIDS, alias_page=1, verbose=True):
    """Smallest-footprint variant (keeps pageCt = 2 and the retail page sizes where possible):
      * sid wanted, colon-form sid NOT referenced by the executable / DC files -> rename the
        retail material in place (':' -> '/' in its name string and in the matching submesh
        material-name string: same length, so no layout change, no extra material).
      * colon-form sid referenced (store:purchased / store:locked, SP code 0x5b2740) -> both
        names must exist -> alias entries; the material array is copied (retail order kept)
        and appended to page `alias_page` (default page 1, whose single 32 KiB sub-page has
        room, so no extra loading-heap sub-page is needed)."""
    ref = Pak(ps4_hud)
    assert ref.platform == 'ps4' and not ref.validate() and ref.hdr['pageCt'] == 2
    mats = ps4_material_list(ref)
    by_base = {m['base']: m for m in mats}
    have = {sid(m['base']) for m in mats}
    blobs = [open(f, 'rb').read() for f in scan_files]

    def referenced(h):
        b4, b3 = struct.pack('<I', h), struct.pack('>I', h)
        return any(d.find(b4) >= 0 or d.find(b3) >= 0 for d in blobs)
    ps3names = {}
    for f in ps3_paks:
        for m in read_materials(Pak(f))[0]:
            ps3names[m['sid']] = m['name']
    renames, aliases, skipped = [], [], []
    for h in sids:
        if h in have:
            continue
        n = ps3names.get(h)
        if n is None:
            skipped.append(hex(h)); continue
        a, _, b = n.rpartition('/')
        eq = by_base.get(a + ':' + b)
        if eq is None:
            skipped.append(n); continue
        (aliases if referenced(sid(eq['base'])) else renames).append((n, eq))
    data0 = bytearray(ref.d[ref.pages[0][0]:ref.pages[0][0] + ref.pages[0][1]])
    g = ps4_geometry(ref)
    gb = g.data_off
    nsub, smp = ref.pu32(0, gb + 8), ref.resolve(0, gb + 0x10)
    subnames = {}
    for i in range(nsub):
        r = ref.resolve(0, smp[1] + 0x88 * i + 0x48)
        if r:
            subnames.setdefault(ref.pstr(0, r[1]).split(',')[0], []).append(r[1])
    for n, m in renames:
        colon_pos = len(m['base'].rpartition(':')[0])
        offs = [ref.resolve(0, m['off'])[1]] + subnames.get(m['base'], [])
        for o in offs:
            assert data0[o + colon_pos] == ord(':')
            data0[o + colon_pos] = ord('/')
    w = PakWriter()
    fx0 = [(off, d) for (s, d, off) in ref.fixups if s == 0]
    fx1 = [(off, d) for (s, d, off) in ref.fixups if s == 1]
    p0 = w.raw_page(bytes(data0), fx0)
    p1 = w.raw_page(ref.d[ref.pages[1][0]:ref.pages[1][0] + ref.pages[1][1]], fx1)
    pages = [p0, p1]
    if aliases:
        strs, bindb, matb = Blob('aliasnames'), Blob('aliasbindings'), Blob('materials')

        def P(off):
            r = ref.resolve(0, off)
            return (pages[r[0]], r[1]) if r else None

        def ptr(blob, r):
            blob.ptr(*r) if r else blob.u64(0)
        for m in mats + [x[1] for x in aliases]:
            pass
        for idx, m in enumerate(mats + [x[1] for x in aliases]):
            o = m['off']
            alias = idx >= len(mats)
            if alias:
                matb.ptr(strs, strs.cstr(aliases[idx - len(mats)][0]))
            else:
                ptr(matb, P(o))
            for k in (8, 0x10, 0x18):
                ptr(matb, P(o + k))
            matb.u64(ref.u64(ref.pbase(0) + o + 0x20))
            if alias:
                nb, bp = ref.pu32(0, o + 0x38), P(o + 0x28)
                boff = bindb.pos
                for k in range(nb):
                    be = bp[1] + 0x28 * k
                    for f in (0, 8, 0x10):
                        ptr(bindb, P(be + f))
                    bindb.raw(ref.d[ref.pbase(0) + be + 0x18: ref.pbase(0) + be + 0x28])
                matb.ptr(bindb, boff)
            else:
                ptr(matb, P(o + 0x28))
            for k in (0x30, 0x38, 0x40):
                matb.u64(ref.u64(ref.pbase(0) + o + k))
        tgt = pages[alias_page]
        tgt.append(matb); tgt.append(bindb); tgt.append(strs)
        p0.set_u32(gb + 0x48, len(mats) + len(aliases))
        p0.redirect(gb + 0x50, matb, 0)
    w.login_page = (ref.hdr['loginPage'], ref.hdr['loginOff'])
    w.vram_table_loc = (ref.hdr['vramDescPage'], ref.hdr['vramDescOff'])
    w.vram = ref.d[ref.vram_base:]
    out = w.build()
    open(out_path, 'wb').write(out)
    if verbose:
        print('wrote %s: %d bytes; %d renamed in place, %d aliases (page %d), skipped %s' % (
            out_path, len(out), len(renames), len(aliases), alias_page, skipped))
        for n, m in renames:
            print('  rename %-48s <- %s' % (n, m['base']))
        for n, m in aliases:
            print('  alias  %-48s =  %s (colon sid referenced)' % (n, m['base']))
    return renames, aliases


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('ps4hud')
    ap.add_argument('out')
    ap.add_argument('--ps3', nargs='+', required=True, help='PS3 paks whose material names to alias (hud.pak)')
    ap.add_argument('--all-slash', action='store_true', help='also add slash aliases for PS4-only materials')
    ap.add_argument('--min', action='store_true', help='only alias MIN_SIDS and append to page 0 (2 pages)')
    ap.add_argument('--minimal', action='store_true', help='rename-in-place + aliases only where needed (hud-min)')
    ap.add_argument('--scan', nargs='*', default=[], help='executable / DC files to scan for colon-sid references')
    ap.add_argument('--alias-page', type=int, default=1)
    a = ap.parse_args()
    if a.minimal:
        merge_min(a.ps4hud, a.ps3, a.out, a.scan, alias_page=a.alias_page)
    elif a.min:
        merge(a.ps4hud, a.ps3, a.out, False, only_sids=set(MIN_SIDS), layout='append0')
    else:
        merge(a.ps4hud, a.ps3, a.out, a.all_slash)
