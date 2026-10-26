"""Texture helpers: BCn block decoding (for previews / validation) and the
PS3 (RSX linear) <-> PS4 (GNM 1D-thin micro-tiled) block layout conversion.

Block layouts established from the sample paks (see FINDINGS.md):
  * PS3 RSX DXT textures in these paks are stored linearly (row-major 4x4 blocks,
    pitch = blocksWide * blockBytes), mip chain packed back to back without padding.
  * PS4 textures are stored per mip, each mip padded to a multiple of 8x8 blocks and
    laid out as 8x8-block micro tiles (row-major tile order), each tile's 64 elements
    in "thin/non-displayable" Morton order (x0 y0 x1 y1 x2 y2).
"""
import struct
import numpy as np

# ---------------------------------------------------------------- formats
# DXGI_FORMAT values, which is what the PS4 VRAM_DESC format field holds.
DXGI = {
    0x47: ('BC1_UNORM', 8), 0x48: ('BC1_UNORM_SRGB', 8),
    0x4a: ('BC2_UNORM', 16), 0x4b: ('BC2_UNORM_SRGB', 16),
    0x4d: ('BC3_UNORM', 16), 0x4e: ('BC3_UNORM_SRGB', 16),
    0x50: ('BC4_UNORM', 8), 0x51: ('BC4_SNORM', 8),
    0x53: ('BC5_UNORM', 16), 0x54: ('BC5_SNORM', 16),
    0x1c: ('R8G8B8A8_UNORM', 4), 0x1d: ('R8G8B8A8_UNORM_SRGB', 4),
    0x57: ('B8G8R8A8_UNORM', 4), 0x5b: ('B8G8R8A8_UNORM_SRGB', 4),
}

# PS3 VRAM_DESC word at +0x14: top byte is the GCM texture format class used by
# these paks (observed values); low 16 bits is the GCM component remap.
PS3_FMT = {
    0x06: ('DXT1', 8, True),     # CELL_GCM_TEXTURE_COMPRESSED_DXT1   (size checks match 8 B/block)
    0x07: ('DXT3', 16, True),
    0x08: ('DXT5', 16, True),    # CELL_GCM_TEXTURE_COMPRESSED_DXT45
    0x05: ('A8R8G8B8', 4, False),
}


def ps3_format(desc):
    return PS3_FMT.get(desc['gcmRemapCtl'] >> 24)


def mip_dims(w, h, mips):
    out = []
    for m in range(mips):
        out.append((max(1, w >> m), max(1, h >> m)))
    return out


def blocks(w, h, compressed):
    if compressed:
        return (w + 3) // 4, (h + 3) // 4
    return w, h


def ps3_linear_size(w, h, mips, bpb, compressed=True):
    s = 0
    for mw, mh in mip_dims(w, h, mips):
        bw, bh = blocks(mw, mh, compressed)
        s += bw * bh * bpb
    return s


def ps4_tiled_size(w, h, mips, bpb, compressed=True):
    s = 0
    for mw, mh in mip_dims(w, h, mips):
        bw, bh = blocks(mw, mh, compressed)
        s += ((bw + 7) // 8 * 8) * ((bh + 7) // 8 * 8) * bpb
    return s


# --------------------------------------------------------- micro tiling
def _morton_xy(i):
    x = (i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4)
    y = ((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4)
    return x, y


MORTON8 = [_morton_xy(i) for i in range(64)]


def tile_mip(lin, bw, bh, bpb):
    """linear block array (bw x bh blocks) -> PS4 1D thin micro-tiled bytes (padded)."""
    pw, ph = (bw + 7) // 8 * 8, (bh + 7) // 8 * 8
    src = np.frombuffer(lin, dtype=np.uint8).reshape(bh, bw, bpb)
    pad = np.zeros((ph, pw, bpb), dtype=np.uint8)
    pad[:bh, :bw] = src
    # replicate edge blocks into the padding (keeps bilinear sampling of last mips sane)
    if pw > bw:
        pad[:bh, bw:] = pad[:bh, bw - 1:bw]
    if ph > bh:
        pad[bh:, :] = pad[bh - 1:bh, :]
    tx, ty = pw // 8, ph // 8
    t = pad.reshape(ty, 8, tx, 8, bpb).transpose(0, 2, 1, 3, 4)  # ty,tx,y,x,b
    order_y = np.array([y for x, y in MORTON8])
    order_x = np.array([x for x, y in MORTON8])
    out = t[:, :, order_y, order_x, :]  # ty,tx,64,b
    return out.tobytes()


def detile_mip(data, bw, bh, bpb):
    pw, ph = (bw + 7) // 8 * 8, (bh + 7) // 8 * 8
    tx, ty = pw // 8, ph // 8
    t = np.frombuffer(data[:pw * ph * bpb], dtype=np.uint8).reshape(ty, tx, 64, bpb)
    img = np.zeros((ty, tx, 8, 8, bpb), dtype=np.uint8)
    order_y = np.array([y for x, y in MORTON8])
    order_x = np.array([x for x, y in MORTON8])
    img[:, :, order_y, order_x, :] = t
    img = img.transpose(0, 2, 1, 3, 4).reshape(ph, pw, bpb)
    return img[:bh, :bw].tobytes()


def ps3_to_ps4_texels(data, w, h, mips, bpb, compressed=True):
    out = bytearray()
    o = 0
    for mw, mh in mip_dims(w, h, mips):
        bw, bh = blocks(mw, mh, compressed)
        n = bw * bh * bpb
        out += tile_mip(data[o:o + n], bw, bh, bpb)
        o += n
    return bytes(out)


def ps4_mip0_linear(data, w, h, bpb, compressed=True):
    bw, bh = blocks(w, h, compressed)
    return detile_mip(data, bw, bh, bpb)


# ------------------------------------------------------------ decoding
def _c565(c):
    r = ((c >> 11) & 31) * 255 // 31
    g = ((c >> 5) & 63) * 255 // 63
    b = (c & 31) * 255 // 31
    return np.stack([r, g, b], -1).astype(np.int32)


def decode_bc1(lin, bw, bh, alpha=True):
    a = np.frombuffer(lin[:bw * bh * 8], dtype='<u2').reshape(-1, 4)
    c0, c1 = a[:, 0].astype(np.int32), a[:, 1].astype(np.int32)
    idx = (a[:, 2].astype(np.uint32) | (a[:, 3].astype(np.uint32) << 16))
    p0, p1 = _c565(c0), _c565(c1)
    four = (c0 > c1)[:, None]
    p2 = np.where(four, (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four, (p0 + 2 * p1) // 3, 0)
    pal = np.stack([p0, p1, p2, p3], 1)  # n,4,3
    alp = np.full((len(c0), 4), 255)
    if alpha:
        alp[:, 3] = np.where(four[:, 0], 255, 0)
    sel = np.stack([(idx >> (2 * i)) & 3 for i in range(16)], 1)  # n,16
    rgb = np.take_along_axis(pal, sel[:, :, None].repeat(3, 2), 1)
    al = np.take_along_axis(alp, sel, 1)
    px = np.concatenate([rgb, al[:, :, None]], 2).reshape(bh, bw, 4, 4, 4)
    return px.transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4).astype(np.uint8)


def _bc4_block(a8):
    # a8: n x 8 uint8 -> n x 16 values
    a0, a1 = a8[:, 0].astype(np.int32), a8[:, 1].astype(np.int32)
    bitsv = np.zeros(len(a0), dtype=np.uint64)
    for i in range(6):
        bitsv |= a8[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    sel = np.stack([((bitsv >> np.uint64(3 * i)) & np.uint64(7)).astype(np.int32) for i in range(16)], 1)
    pal = np.zeros((len(a0), 8), dtype=np.int32)
    pal[:, 0], pal[:, 1] = a0, a1
    big = a0 > a1
    for i in range(1, 7):
        pal[:, 1 + i] = np.where(big, ((7 - i) * a0 + i * a1) // 7, 0)
    for i in range(1, 5):
        pal[:, 1 + i] = np.where(big, pal[:, 1 + i], ((5 - i) * a0 + i * a1) // 5)
    pal[:, 6] = np.where(big, pal[:, 6], 0)
    pal[:, 7] = np.where(big, pal[:, 7], 255)
    return np.take_along_axis(pal, sel, 1)


def decode_bc3(lin, bw, bh):
    a = np.frombuffer(lin[:bw * bh * 16], dtype=np.uint8).reshape(-1, 16)
    alpha = _bc4_block(a[:, :8])
    col = decode_bc1(a[:, 8:].tobytes(), bw, bh, alpha=False)
    al = alpha.reshape(bh, bw, 4, 4).transpose(0, 2, 1, 3).reshape(bh * 4, bw * 4)
    col[:, :, 3] = al
    return col


def decode_bc2(lin, bw, bh):
    a = np.frombuffer(lin[:bw * bh * 16], dtype=np.uint8).reshape(-1, 16)
    col = decode_bc1(a[:, 8:].tobytes(), bw, bh, alpha=False)
    al = np.zeros((len(a), 16), dtype=np.int32)
    for i in range(8):
        al[:, 2 * i] = (a[:, i] & 15) * 17
        al[:, 2 * i + 1] = (a[:, i] >> 4) * 17
    col[:, :, 3] = al.reshape(bh, bw, 4, 4).transpose(0, 2, 1, 3).reshape(bh * 4, bw * 4)
    return col


def decode(lin, w, h, kind):
    bw, bh = (w + 3) // 4, (h + 3) // 4
    if kind in ('DXT1', 'BC1'):
        img = decode_bc1(lin, bw, bh)
    elif kind in ('DXT3', 'BC2'):
        img = decode_bc2(lin, bw, bh)
    elif kind in ('DXT5', 'BC3'):
        img = decode_bc3(lin, bw, bh)
    elif kind == 'A8R8G8B8':
        a = np.frombuffer(lin[:w * h * 4], dtype=np.uint8).reshape(h, w, 4)
        return a[:, :, [1, 2, 3, 0]].copy()
    else:
        raise ValueError(kind)
    return img[:h, :w]


def save_png(img, path):
    from PIL import Image
    Image.fromarray(img, 'RGBA').save(path)
