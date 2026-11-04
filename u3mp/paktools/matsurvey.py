"""Survey materials in PS4 actor paks and their PS3 counterparts (run on device).
usage: python3 matsurvey.py <ps4 actor33 dir> <ps3 actor33.psarc> <out.jsonl> [name-substring]"""
import sys, os, json, struct, io
sys.path.insert(0, os.path.dirname(__file__))
from ndpak import Pak
import psarc_lib


def ps4_materials(q):
    out = []
    gs = q.items_of('GEOMETRY_1')
    if not gs:
        return out
    g = gs[0]
    pg = g.page
    b = g.data_off
    n = q.pu32(pg, b + 0x48)
    r = q.resolve(pg, b + 0x50)
    if not r:
        return out
    mp, mo = r
    for i in range(n):
        o = mo + i * 0x48
        S = lambda off: (q.pstr(*q.resolve(mp, o + off)) if q.resolve(mp, o + off) else None)
        nf, npar, nb, x3 = struct.unpack_from('<4I', q.d, q.pbase(mp) + o + 0x30)
        tail = q.pu32(mp, o + 0x40)
        feats = []
        fr = q.resolve(mp, o + 0x10)
        if fr:
            for k in range(nf):
                rr = q.resolve(fr[0], fr[1] + 8 * k)
                feats.append(q.pstr(*rr) if rr else None)
        params = []
        pr = q.resolve(mp, o + 0x20)
        if pr:
            for k in range(npar):
                po = pr[1] + 0x18 * k
                nm = q.pstr(*q.resolve(pr[0], po))
                vr = q.resolve(pr[0], po + 8)
                cnt = q.pu32(pr[0], po + 0x10)
                vals = list(struct.unpack_from('<4f', q.d, q.pbase(vr[0]) + vr[1])) if vr else None
                params.append([nm, cnt, vals])
        binds = []
        br = q.resolve(mp, o + 0x28)
        if br:
            for k in range(nb):
                bo = br[1] + 0x28 * k
                binds.append([q.pstr(*q.resolve(br[0], bo)), q.pstr(*q.resolve(br[0], bo + 8))])
        out.append(dict(name=S(0), shader=S(8), hash=S(0x18), feats=feats, params=params, binds=binds,
                        x3=x3, tail=tail))
    return out


def ps3_materials(p):
    out = []
    seen = set()
    for it in p.items_of('PM_FG_PROTOTYPES'):
        n = p.pu32(it.page, it.data_off)
        for k in range(n):
            r = p.resolve(it.page, it.data_off + 4 + 4 * k)
            if not r:
                continue
            pg, proto = r
            nsub = p.u16(p.pbase(pg) + proto + 6)
            t = p.resolve(pg, proto + 0x10)
            if not t:
                continue
            for s in range(nsub):
                sd = p.resolve(t[0], t[1] + 0x10 * s)
                if not sd:
                    continue
                m = p.resolve(sd[0], sd[1])
                if not m or m in seen:
                    continue
                seen.add(m)
                mb = p.pbase(m[0]) + m[1]
                nm = p.pstr(*p.resolve(m[0], m[1]))
                npar = p.u32(mb + 0xc)
                h = struct.unpack_from('>3I', p.d, mb + 0x14)
                out.append(dict(name=nm, nparams=npar, h=['%08x' % x for x in h]))
    return out


def main():
    d4, ps3arc, outp = sys.argv[1:4]
    filt = sys.argv[4] if len(sys.argv) > 4 else ''
    arc = psarc_lib.Psarc(ps3arc)
    idx = {os.path.basename(n): i for i, n in enumerate(arc.names)}
    done = set()
    if os.path.exists(outp):
        for l in open(outp):
            try:
                done.add(json.loads(l)['pak'])
            except Exception:
                pass
    import time
    t0 = time.time()
    with open(outp, 'a') as f:
        for fn in sorted(os.listdir(d4)):
            if not fn.endswith('.pak') or filt not in fn or fn in done:
                continue
            if time.time() - t0 > 150:
                print('TIMEUP')
                break
            rec = dict(pak=fn)
            try:
                q = Pak(os.path.join(d4, fn))
                rec['ps4'] = ps4_materials(q)
            except Exception as e:
                rec['ps4err'] = repr(e)
            if fn in idx:
                try:
                    p = Pak(arc.read(idx[fn] + 1))
                    rec['ps3'] = ps3_materials(p)
                except Exception as e:
                    rec['ps3err'] = repr(e)
            f.write(json.dumps(rec) + '\n')


if __name__ == '__main__':
    main()
