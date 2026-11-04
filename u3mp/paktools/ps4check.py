#!/usr/bin/env python3
"""Structural self-check of a PS4 U3 pak, mirroring what the PS4 loader does:

  * header / page / fixup sanity (ndpak.Pak.validate)
  * every listed item resolvable, type strings known
  * VRAM desc table -> VRAM_DESC items, desc fields sane, tiled size matches format/mips,
    hash == sid(short path) and TEXTURE_TABLE consistency
  * EFFECT_TABLE: version 6/7, effect entries + hash list
  * GEOMETRY_1 (v0x10): GeometryDesc::ResolveReferences emulation - every material binding's
    full texture path must hash to a geometry texture entry (else the game asserts
    "Could not find texture '%s' in material instance '%s'"), every texture entry hash must be
    a VRAM_DESC in this pak (or already resident), every material effect hash must exist in
    the EFFECT_TABLE, and material name sids (substring before ',') are listed.
  * optional: --png DIR renders textures (de-tiled) for visual inspection
  * optional: --want HASH ... checks that material sids exist (e.g. UI icon hashes)
"""
import argparse, os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ndpak import Pak, sid
import texcodec as tc

KNOWN = {'TEXTURE_TABLE', 'VRAM_DESC', 'RAW_DATA', 'EFFECT_TABLE', 'EFFECT', 'GEOMETRY_1', 'LEVEL_INFO_1',
         'LEVEL_PATS_1', 'DEBUG_INFO_1', 'ATTACHED_FG_MAPPING', 'TAG_FLOAT', 'TAG_INT', 'SPLINE_TABLE'}


def check(path, want=(), png=None, quiet=False):
    p = Pak(path)
    assert p.platform == 'ps4'
    errs = list(p.validate())
    info = []
    for it in p.items:
        if it.type not in KNOWN:
            errs.append('unknown item type %s' % it.type)
    # textures
    texs = p.textures()
    by_hash = {}
    for t in texs:
        if t['format'] not in tc.DXGI:
            errs.append('unknown format %#x %s' % (t['format'], t['path']))
            continue
        name, bpb = tc.DXGI[t['format']]
        comp = name.startswith('BC')
        exp = tc.ps4_tiled_size(t['width'], t['height'], t['mips'], bpb, comp)
        if exp != t['vramSize']:
            errs.append('tiled size %#x != %#x for %s' % (t['vramSize'], exp, t['path']))
        short = t['path'].split('texture2/', 1)[-1]
        if sid(short) != t['hash']:
            errs.append('hash != sid(short) %s' % short)
        if t['type'] != 1:
            errs.append('type %d' % t['type'])
        by_hash[t['hash']] = t
    info.append('%d textures, formats %s' % (len(texs), sorted({hex(t['format']) for t in texs})))
    # texture table
    for tt in p.items_of('TEXTURE_TABLE'):
        b = tt.data_off
        n = p.pu32(tt.page, b)
        hp = p.resolve(tt.page, b + 8)
        npp = p.resolve(tt.page, b + 0x10)
        dp = p.resolve(tt.page, b + 0x18)
        for i in range(n):
            h = p.pu32(hp[0], hp[1] + 4 * i)
            nm = p.resolve(npp[0], npp[1] + 8 * i)
            d = p.resolve(dp[0], dp[1] + 8 * i)
            desc = p.vram_desc(d[0], d[1])
            if desc['hash'] != h or sid(p.pstr(*nm)) != h:
                errs.append('TEXTURE_TABLE entry %d inconsistent' % i)
        info.append('TEXTURE_TABLE %d entries' % n)
    # effects
    eff_hashes = {}
    for et in p.items_of('EFFECT_TABLE'):
        b = et.data_off
        n, ver = p.pu32(et.page, b), p.pu32(et.page, b + 4)
        if ver not in (6, 7):
            errs.append('EFFECT_TABLE version %d' % ver)
        arr = p.resolve(et.page, b + 8)
        fx = p.resolve(et.page, b + 0x10)
        nfx = p.pu32(fx[0], fx[1])
        fxn = [p.pstr(*p.resolve(fx[0], fx[1] + 8 + 8 * k)) for k in range(nfx)]
        for i in range(n):
            e = arr[1] + 0x18 * i
            ep = p.resolve(arr[0], e)
            idx = p.pu32(arr[0], e + 8)
            h = p.u64(p.pbase(arr[0]) + e + 0x10)
            it = p.item_at(ep[0], ep[1] - 0x20)
            if it.type != 'EFFECT':
                errs.append('effect entry %d does not point at an EFFECT' % i)
            eff_hashes[h] = fxn[idx] if idx < len(fxn) else '?'
        info.append('EFFECT_TABLE v%d: %d effects %s' % (ver, n, ', '.join('%016X=%s' % kv for kv in eff_hashes.items())))
    # geometry
    mat_sids = {}
    for g in p.items_of('GEOMETRY_1'):
        pg, b = g.page, g.data_off
        ver, flags = p.pu32(pg, b), p.pu32(pg, b + 4)
        if ver != 0x10:
            errs.append('GEOMETRY_1 version %#x' % ver)
        ntex = p.pu32(pg, b + 0x38)
        tp = p.resolve(pg, b + 0x40)
        texmap = {}
        for i in range(ntex):
            e = tp[1] + 0x28 * i
            full = p.pstr(*p.resolve(tp[0], e))
            h = p.pu32(tp[0], e + 8)
            texmap[sid(full)] = (i, h, full)
            if h not in by_hash:
                errs.append('geometry texture %s (%08x) not in this pak' % (full, h))
        nmat = p.pu32(pg, b + 0x48)
        mp = p.resolve(pg, b + 0x50)
        nbind = 0
        for i in range(nmat):
            m = mp[1] + 0x48 * i
            name = p.pstr(*p.resolve(mp[0], m))
            eh_ptr = p.resolve(mp[0], m + 0x18)
            if eh_ptr:
                eh = int(p.pstr(*eh_ptr), 16)
                if eh not in eff_hashes:
                    errs.append('material %s: effect %016X missing' % (name, eh))
            nb = p.pu32(mp[0], m + 0x38)
            bp = p.resolve(mp[0], m + 0x28)
            for k in range(nb):
                be = bp[1] + 0x28 * k
                full = p.pstr(*p.resolve(bp[0], be + 0x10))
                if sid(full) not in texmap:
                    errs.append("Could not find texture '%s' in material instance '%s'" % (full, name))
                nbind += 1
            mat_sids[sid(name.split(',')[0])] = name
        info.append('GEOMETRY_1 v%#x flags %d: %d textures, %d materials, %d bindings' % (ver, flags, ntex, nmat, nbind))
    for h in want:
        if h in mat_sids:
            info.append('want %08x -> %s' % (h, mat_sids[h]))
        else:
            errs.append('wanted material sid %08x not present' % h)
    if png:
        os.makedirs(png, exist_ok=True)
        for t in texs:
            name, bpb = tc.DXGI[t['format']]
            kind = {'BC1': 'BC1', 'BC2': 'BC2', 'BC3': 'BC3'}.get(name[:3])
            if not kind:
                continue
            d = p.vram_bytes(t['vramOffset'], t['vramSize'])
            lin = tc.ps4_mip0_linear(d, t['width'], t['height'], bpb)
            img = tc.decode(lin, t['width'], t['height'], kind)
            tc.save_png(img[::-1].copy(), os.path.join(png, t['path'].split('/')[-2] + '.png'))
    if not quiet:
        print(path)
        for l in info:
            print('  ' + l)
        print('  RESULT: %s' % ('OK' if not errs else '%d problems' % len(errs)))
        for e in errs[:20]:
            print('   - ' + e)
    return errs, info, mat_sids


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('pak', nargs='+')
    ap.add_argument('--want', nargs='*', default=[], type=lambda x: int(x, 16))
    ap.add_argument('--png')
    a = ap.parse_args()
    bad = 0
    for f in a.pak:
        e, _, _ = check(f, a.want, a.png)
        bad += bool(e)
    sys.exit(1 if bad else 0)
