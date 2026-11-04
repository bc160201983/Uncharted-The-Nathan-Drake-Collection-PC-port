"""Decoder for Uncharted 3 PS3 foreground (actor) geometry.

Structures were reverse engineered against the PS4 (Nathan Drake Collection)
build of the same assets (rosetta pairs); vertex order and triangle order are
identical between the two builds.

Geometry block (one per submesh; found via PM_FG_PROTOTYPES -> prototype+0x14):
  +0x00 ptr  stream table   {ptr data, u32 -1} per attribute
  +0x04 ptr  index info     {u16 ntris, u16 0, u16 size, u16 0, ptr data, -1}
  +0x0c ptr  skin info      (0 = rigid)
  +0x20 u32  0x04000100
  +0x24 ptr  {u32 nverts, u32 1}
  +0x28 ptr  vertex format
Vertex format: +0 u16 offset-to-data(unused), then attributes (8 bytes):
  u8 id (1 pos, 0x11 uv0, 2 normal, 3 tangent ...), u8 fmt, u16 off-to-params,
  u8 bits[4]
  fmt 0x4N : N components, unsigned fixed point, params = N floats a, N floats b,
             value = a*(1 + v*2^-21) + b
  fmt 0x6N : as 0x4N plus N u32 mantissa offsets I after the floats:
             value = a*(1 + (4v + I)*2^-23) + b
  fmt 0x0N : N components, signed normalised, value = v*2/(2^bits-1) - 1
Each attribute is its own big-endian bit stream (components packed MSB first).

Indices (compressed): header {u16 bits, u16 nExplicit, u16 nSlots, u16 start},
  then three 2-byte aligned bit streams:
  explicit indices (nExplicit x bits), triangle configs (2 bits / tri),
  slot flags (1 bit / emitted slot, 0 = take running counter).
  config 0: (p0, p2, new)   1: (p2, p1, new)   3: three new slots.
Skinning (skin info): u16 sz1, sz2, sz3, sz4, nRanges, ?, ptr s1, s2, s3, s4, 0,
  ptr ranges {u16 first joint, u16 count}; palette = concatenated ranges.
  Vertices in groups of 4. s1 = one code byte per group:
    4/6 : new joint list from s2 (u16 = pal*48 + 4 if more follow)
    2/10: per-lane joint rows from s3 (4 u16 per row, lane order 1,0,3,2;
          rows end when all four lane flags are 0)
    0   : repeat the previous group (joints and weights)
    8   : keep previous joints, new weights
    6/8/10 carry weights (rows = joints per lane), else weight 255 on the first
  s4 = u8 weights, rows x 4 lanes per group.
"""
import struct
import numpy as np


def _bits_be(buf, widths, count):
    v = int.from_bytes(buf, 'big')
    tot = len(buf) * 8
    out = np.zeros((count, len(widths)), dtype=np.int64)
    pos = 0
    for i in range(count):
        for c, w in enumerate(widths):
            out[i, c] = (v >> (tot - pos - w)) & ((1 << w) - 1)
            pos += w
    return out


def _align(nbits, a=16):
    return (nbits + a - 1) // a * a


def decode_indices(raw, ntris):
    bits, nexp, nslots, start = struct.unpack_from('>4H', raw, 0)
    bs = ''.join(format(x, '08b') for x in raw[8:])
    p = 0
    vals = [int(bs[p + i * bits:p + (i + 1) * bits], 2) for i in range(nexp)]
    p = _align(nexp * bits)
    cfg = [int(bs[p + 2 * i:p + 2 * i + 2], 2) for i in range(ntris)]
    p += _align(ntris * 2)
    flags = bs[p:p + nslots]
    vi = si = 0
    counter = start
    tris = []

    def take():
        nonlocal vi, si, counter
        f = flags[si]
        si += 1
        if f == '1':
            x = vals[vi]
            vi += 1
            return x
        x = counter
        counter += 1
        return x

    for i in range(ntris):
        c = cfg[i]
        if i == 0 or c == 3:
            t = (take(), take(), take())
        else:
            pv = tris[-1]
            if c == 0:
                t = (pv[0], pv[2], take())
            elif c == 1:
                t = (pv[2], pv[1], take())
            else:
                raise ValueError('index config 2 not seen yet (tri %d)' % i)
        tris.append(t)
    assert vi == nexp and si == nslots, (vi, nexp, si, nslots)
    return tris


def decode_skin(p, page, info, nverts):
    b = p.pbase(page)
    sz = struct.unpack_from('>6H', p.d, b + info)
    ptrs = [(p.resolve(page, info + 0xc + 4 * k) or (0, None))[1] for k in range(4)]
    rng = p.resolve(page, info + 0x20)[1]
    pal = []
    for k in range(sz[4]):
        j0, n = struct.unpack_from('>2H', p.d, b + rng + 4 * k)
        pal += list(range(j0, j0 + n))
    def arr(k, fmt):
        if not sz[k] or ptrs[k] is None:
            return b'' if fmt is None else ()
        if fmt is None:
            return p.d[b + ptrs[k]:b + ptrs[k] + sz[k]]
        return struct.unpack_from('>%dH' % (sz[k] // 2), p.d, b + ptrs[k])
    S1, S2, S3, S4 = arr(0, None), arr(1, 'H'), arr(2, 'H'), arr(3, None)
    ngroups = (nverts + 3) // 4
    p2 = p3 = wp = 0
    out = []
    cols = None
    prev = (0, [])
    for gi in range(ngroups):
        f = S1[gi]
        if f not in (0, 2, 4, 6, 8, 10):
            raise ValueError('skin group flags %d' % f)
        hasw = f in (6, 8, 10)
        if f in (2, 10):       # per-lane joint rows from s3
            Jr = []
            while True:
                row = S3[p3:p3 + 4]
                p3 += 4
                Jr.append([row[1] // 48, row[0] // 48, row[3] // 48, row[2] // 48])
                if all(x % 48 == 0 for x in row):
                    break
            cols = [[Jr[k][v] for k in range(len(Jr))] for v in range(4)]
        elif f in (4, 6):      # shared joint list from s2
            L = []
            while True:
                x = S2[p2]
                p2 += 1
                L.append(x // 48)
                if x % 48 == 0:
                    break
            cols = [L] * 4
        if f == 0:             # repeat previous group (joints and weights)
            r, Wr = prev
        else:
            r = len(cols[0]) if hasw else 0
            Wr = [S4[wp + k * 4:wp + k * 4 + 4] for k in range(r)]
            wp += 4 * r
        prev = (r, Wr)
        for v in range(4):
            if gi * 4 + v >= nverts:
                break
            if r == 0:
                out.append([(pal[cols[v][0]], 255)])
            else:
                d = {}
                for k in range(r):
                    w = Wr[k][v]
                    if w:
                        j = pal[cols[v][k]]
                        d[j] = d.get(j, 0) + w
                out.append(sorted(d.items(), key=lambda x: x[1]))
    assert (p2 * 2, p3 * 2, wp) == (sz[1], sz[2], sz[3]), ((p2 * 2, p3 * 2, wp), sz)
    return out, pal


def decode_block(p, page, blk, skin=True):
    """Decode one geometry block -> dict(pos, uv, normal, tangent, tris, skin)."""
    b = p.pbase(page)
    cnt = p.resolve(page, blk + 0x24)[1]
    nv = p.pu32(page, cnt)
    fmt = p.resolve(page, blk + 0x28)[1]
    st = p.resolve(page, blk)[1]
    res = {'nverts': nv}
    k = 0
    o = fmt + 8
    attrs = []
    while True:
        aid, af, aoff = struct.unpack_from('>BBH', p.d, b + o)
        if aid == 0 or o >= fmt + 8 + 8 * 8:
            break
        widths = [x for x in p.d[b + o + 4:b + o + 8]]
        attrs.append((aid, af, aoff, widths))
        o += 8
        # stop when the next word does not look like an attribute (params begin)
        nxt = p.d[b + o]
        if nxt not in (1, 2, 3, 4, 5, 6, 7, 8, 0x10, 0x11, 0x12, 0x13, 0x14):
            break
    names = {1: 'pos', 2: 'normal', 3: 'tangent', 0x11: 'uv0', 0x12: 'uv1', 0x13: 'uv2', 4: 'color0', 5: 'color1'}
    for ai, (aid, af, aoff, widths) in enumerate(attrs):
        n = af & 0xf
        w = widths[:n]
        if not p.is_ptr(page, st + 8 * ai):
            break
        data = p.resolve(page, st + 8 * ai)[1]
        V = _bits_be(p.d[b + data:b + data + (nv * sum(w) + 7) // 8], w, nv).astype(np.float64)
        if af & 0xf0 == 0x40:
            pa = struct.unpack_from('>%df' % (2 * n), p.d, b + fmt + aoff)
            a = np.array(pa[:n])
            bb = np.array(pa[n:])
            val = a * (1.0 + V * 2.0 ** -21) + bb
        elif af & 0xf0 == 0x60:
            pa = struct.unpack_from('>%df' % (2 * n), p.d, b + fmt + aoff)
            off = np.array(struct.unpack_from('>%dI' % n, p.d, b + fmt + aoff + 8 * n), dtype=np.float64)
            a = np.array(pa[:n])
            bb = np.array(pa[n:])
            val = a * (1.0 + (4.0 * V + off) * 2.0 ** -23) + bb   # max err ~6e-5 vs PS4
        elif af & 0xf0 == 0x00:
            val = np.stack([V[:, c] * 2.0 / max((1 << w[c]) - 1, 1) - 1.0 for c in range(n)], 1)
        else:
            raise ValueError('attr fmt %#x' % af)
        res[names.get(aid, 'attr%x' % aid)] = val
    ii = p.resolve(page, blk + 4)[1]
    ntris, _, isz, _ = struct.unpack_from('>4H', p.d, b + ii)
    idat = p.resolve(page, ii + 8)[1]
    res['tris'] = decode_indices(p.d[b + idat:b + idat + isz], ntris)
    if skin and p.is_ptr(page, blk + 0xc):
        res['skin'], res['palette'] = decode_skin(p, page, p.resolve(page, blk + 0xc)[1], nv)
    return res


def prototypes(p):
    """Map FOREGROUND_SECTION_2 name -> list of (page, block offset) per submesh."""
    out = {}
    for it in p.items_of('FOREGROUND_SECTION_2'):
        pg, proto = p.resolve(it.page, it.data_off)
        nsub = p.u16(p.pbase(pg) + proto + 6)
        tpg, tbl = p.resolve(pg, proto + 0x14)
        blocks = []
        for k in range(nsub):
            r = p.resolve(tpg, tbl + 4 * k)
            if r:
                blocks.append(r)
        out[it.name] = dict(proto=(pg, proto), blocks=blocks)
    return out
