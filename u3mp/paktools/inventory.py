#!/usr/bin/env python3
"""Inventory of a PS3 UI pak (net.pak etc.): every item by type, every texture
(hash, short name, format, size, mips) and every material (name, sid, textures), with
cross references of material sids into binaries / DC files given with --xref."""
import argparse, os, struct, sys
from collections import Counter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ndpak import Pak
from ps3mat import read_materials
import texcodec as tc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pak')
    ap.add_argument('--xref', nargs='*', default=[], help='files to search for material sids (name=path)')
    ap.add_argument('--tsv', help='write material/texture table here')
    a = ap.parse_args()
    p = Pak(a.pak)
    print('# %s (%s)' % (a.pak, p.platform))
    print('items:', ', '.join('%s x%d' % kv for kv in sorted(Counter(i.type for i in p.items).items())))
    print('login:', ', '.join(p.item_at(pg, o).type for pg, o in p.login['entries']))
    texs = p.textures()
    fm = Counter((tc.ps3_format(t) or ('?',))[0] for t in texs) if p.platform == 'ps3' else Counter(t['format'] for t in texs)
    print('textures: %d, formats %s, vram %#x' % (len(texs), dict(fm), p.hdr['vramSize']))
    xr = []
    for spec in a.xref:
        nm, path = spec.split('=', 1)
        xr.append((nm, open(path, 'rb').read()))
    mats, mtex = read_materials(p) if p.platform == 'ps3' else ([], [])
    rows = []
    for m in mats:
        refs = []
        for nm, d in xr:
            if d.find(struct.pack('>I', m['sid'])) >= 0 or d.find(struct.pack('<I', m['sid'])) >= 0:
                refs.append(nm)
        tn = [mtex[t]['name'] if t is not None else '-' for t in m['textures']]
        rows.append((m['name'], m['sid'], m['blockFlags'], tn, refs))
    if mats:
        print('materials: %d' % len(mats))
        groups = Counter(r[0].rsplit('/', 1)[0] for r in rows)
        print('material groups:', ', '.join('%s x%d' % kv for kv in sorted(groups.items())))
        for nm, _ in xr:
            print('materials whose sid occurs in %s: %d' % (nm, sum(nm in r[4] for r in rows)))
    if a.tsv:
        with open(a.tsv, 'w') as f:
            f.write('kind\tname\thash\tformat\tsize\tmips\tvram_bytes\textra\n')
            for t in texs:
                short = t['path'].split('texture1/', 1)[-1].split('texture2/', 1)[-1]
                fmt = (tc.ps3_format(t) or ('?',))[0] if p.platform == 'ps3' else tc.DXGI.get(t['format'], ('?',))[0]
                f.write('texture\t%s\t%08x\t%s\t%dx%d\t%d\t%d\t\n' % (short, t['hash'], fmt, t['width'], t['height'],
                                                                      t['mips'], t['vramSize']))
            for nm, h, bf, tn, refs in rows:
                f.write('material\t%s\t%08x\t\t\t\t\tflags=%s textures=%s refs=%s\n' % (
                    nm, h, '%#x' % bf if bf is not None else '-', '|'.join(tn), ','.join(refs)))
        print('wrote', a.tsv)


if __name__ == '__main__':
    main()
