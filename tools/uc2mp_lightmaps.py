"""Restore the indirect lightmaps the Collection dropped from multiplayer Warzone.

The Collection's warzone-multi.pak points its lightmap slots (TEXTURE_TABLE entries) at
32x32 placeholders, art/general/textures/default/default-lightmap0/1.tga, so interiors and
shaded scenery render black. The PS3 warzone-multi.pak (pak22.psarc) still carries the real
ones: indirect.rgbmax (BC3) and indirect.rgbmaxdelta (BC1), 2048x2048, 12 mips, linear. The
Collection stores the same lightmaps for the other maps (village-multi: decoded PS3 and PS4
images match), only tiled for the PS4 GPU. This writes an overlay copy of warzone-multi.pak
with both PS3 lightmaps tiled and appended to the texture block, and the two placeholder
descriptors pointed at them. Reads the player's own PS4 and PS3 files; writes only <overlay>.

Treasure (treasure-multi) has no lightmap on PS3 either: its placeholder default-lightmap0 is
RGBM white with alpha 0, i.e. no indirect light, and the PS4 renderer leaves its interior floors
black. build_neutral() writes an overlay copy whose placeholder carries a low uniform indirect
light instead (needs no PS3 data).

usage: python tools/uc2mp_lightmaps.py <PS4 game or update folder> <PS3 pak22.psarc> <overlay>
"""
import argparse
import re
import struct
import sys
import zlib
from pathlib import Path

LEVEL = 'warzone-multi'
RELATIVE = Path('u2data/build/main/pak22/ps4') / f'{LEVEL}.pak'
# placeholder texture (table name; a BC1 default-lightmap0 variant also exists) ->
# (PS3 lightmap, DXGI format, bytes per 4x4 block)
SLOTS = {'default-lightmap0.tga/7CD7398D61B89710.ndb': ('indirect.rgbmax', 0x4d, 16),
         'default-lightmap1.tga/84D25E61B2920A02.ndb': ('indirect.rgbmaxdelta', 0x47, 8)}
SIZE, MIPS = 2048, 12
ALIGN = 0x10000


def psarc_entry(path, wanted):
    """Bytes of the entry named `wanted` (manifest path) in a PS3 .psarc (zlib blocks)."""
    with open(path, 'rb') as f:
        magic, _, _, comp, toc_len, entry_size, count, block_size, _ = struct.unpack('>4sHH4sIIIII', f.read(32))
        if magic != b'PSAR' or comp != b'zlib':
            raise ValueError(f'{path}: not a zlib PSARC')
        toc = f.read(toc_len - 32)
        entries = []
        for i in range(count):
            e = toc[i * entry_size:(i + 1) * entry_size]
            entries.append((struct.unpack('>I', e[16:20])[0], int.from_bytes(e[20:25], 'big'),
                            int.from_bytes(e[25:30], 'big')))
        width = 2 if block_size <= 0x10000 else 3 if block_size <= 0x1000000 else 4
        raw = toc[count * entry_size:]
        zsizes = [int.from_bytes(raw[i:i + width], 'big') for i in range(0, len(raw) - width + 1, width)]

        def read(i):
            block, usize, offset = entries[i]
            out = bytearray()
            f.seek(offset)
            while len(out) < usize:
                z = zsizes[block] or block_size
                chunk = f.read(z)
                block += 1
                want = min(block_size, usize - len(out))
                out += chunk if z == want and chunk[:1] != b'\x78' else zlib.decompress(chunk)
            return bytes(out[:usize])

        names = read(0).decode('utf-8', 'replace').split('\n')
        for i, name in enumerate(names, 1):
            if name.endswith('/' + wanted) or name == wanted:
                return read(i)
    raise ValueError(f'{wanted} not found in {path}')


def ps3_lightmaps(pak):
    """{name: linear BC data} of the PS3 pak's indirect lightmaps (big endian descriptors)."""
    n, table = struct.unpack_from('>II', pak, 0x10)
    offset, size, _ = struct.unpack_from('>III', pak, table + 12 * (n - 1))
    base = offset + size
    out = {}
    for m in re.finditer(rb'lightmap0/[^\0]*/(indirect\.rgbmax(?:delta)?)\.tga\.ndb', pak):
        s = pak.rfind(b'\x00\x01\x23\x45' * 3, 0, m.start()) + 12
        _, off, length = struct.unpack_from('>III', pak, s)
        out[m.group(1).decode()] = pak[base + off:base + off + length]
    return out


def morton(x, y):
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3)


def tile(linear, size, mips, bsize):
    """PS4 layout of a block-compressed mip chain as the Collection stores it: 8x8-block tiles
    in row-major order, Morton order inside a tile, mips under 8x8 blocks padded to one tile."""
    out, src = bytearray(), 0
    for m in range(mips):
        b = max(1, (size >> m) // 4)
        t = (b + 7) // 8
        level = bytearray(t * t * 64 * bsize)
        for y in range(b):
            row = src + y * b * bsize
            for x in range(b):
                d = (((y >> 3) * t + (x >> 3)) * 64 + morton(x & 7, y & 7)) * bsize
                level[d:d + bsize] = linear[row + x * bsize:row + (x + 1) * bsize]
        src += b * b * bsize
        out += level
    if src != len(linear):
        raise ValueError('PS3 lightmap size does not match a 2048x2048 12-mip chain')
    return bytes(out)


class Pak:
    """PS4 pak pages and pointer fixups (layout after alphaZomega's nd_pak.bt)."""

    def __init__(self, data):
        self.data = data
        if struct.unpack_from('<I', data, 0)[0] != 0xa79:
            raise ValueError('not a PS4 Uncharted 2 pak')
        n, table, nfix, fixtab, self.tex_size = struct.unpack_from('<IIIII', data, 0x10)
        self.pages = [struct.unpack_from('<III', data, table + 12 * i) for i in range(n)]
        self.fix = {}
        for t in range(nfix):
            _, off, cnt = struct.unpack_from('<III', data, fixtab + 12 * t)
            for i in range(cnt):
                p1, p2, o = struct.unpack_from('<HHI', data, off + 8 * i)
                self.fix[(p1, o)] = p2
        self.tex_base = self.pages[-1][0] + self.pages[-1][1]

    def abs(self, page, off):
        return self.pages[page][0] + off

    def ptr(self, page, off):
        dst = self.fix.get((page, off))
        return None if dst is None else (dst, struct.unpack_from('<Q', self.data, self.abs(page, off))[0])

    def cstr(self, page, off):
        a = self.abs(page, off)
        return self.data[a:self.data.find(b'\0', a)].decode('ascii', 'replace')

    def texture_slots(self):
        """{'<file>.tga/<hash>.ndb': absolute offset of its VRAM descriptor} from the TEXTURE_TABLE."""
        for p, (base, _, _) in enumerate(self.pages):
            for e in range(struct.unpack_from('<H', self.data, base + 0x12)[0]):
                item = struct.unpack_from('<I', self.data, base + 0x14 + 0x10 * e + 8)[0]
                kind = struct.unpack_from('<Q', self.data, self.abs(p, item) + 8)[0]  # page-relative
                if self.cstr(p, kind) != 'TEXTURE_TABLE':
                    continue
                count = struct.unpack_from('<i', self.data, self.abs(p, item) + 0x20)[0]
                names = self.ptr(p, item + 0x30)
                out = {}
                for i in range(count):
                    name, desc = self.ptr(names[0], names[1] + 8 * i), self.ptr(p, item + 0x40 + 8 * i)
                    if name and desc:
                        out['/'.join(self.cstr(*name).split('/')[-2:])] = self.abs(*desc)
                return out
        raise ValueError('no TEXTURE_TABLE')


NEUTRAL_LEVELS = ('treasure-multi',)
NEUTRAL_SLOT = 'default-lightmap0.tga/7CD7398D61B89710.ndb'  # BC3 RGBM, 32x32
NEUTRAL_ALPHA = 24  # RGBM multiplier: a low ambient (chosen from an offscreen capture)


def level_path(level):
    return Path('u2data/build/main/pak22/ps4') / f'{level}.pak'


def build_neutral(game_roots, overlay, level):
    """Overlay copy of <level>.pak whose RGBM placeholder lightmap gives a uniform low light."""
    relative = level_path(level)
    source = next((root / relative for root in game_roots if (root / relative).is_file()), None)
    if source is None:
        raise ValueError(f'{relative} not found in the game folders')
    pak = Pak(source.read_bytes())
    desc = pak.texture_slots().get(NEUTRAL_SLOT)
    if desc is None or struct.unpack_from('<I', pak.data, desc + 0x1c)[0] != 0x4d:
        raise ValueError(f'{level}: unexpected placeholder lightmap')
    offset, size = struct.unpack_from('<I', pak.data, desc + 0x08)[0], struct.unpack_from('<I', pak.data, desc + 0x10)[0]
    data = bytearray(pak.data)
    # One BC3 block for every block of the chain: alpha endpoints = NEUTRAL_ALPHA, colour white.
    block = bytes([NEUTRAL_ALPHA, NEUTRAL_ALPHA]) + bytes(6) + struct.pack('<HH', 0xffff, 0xffff) + bytes(4)
    start = pak.tex_base + offset
    data[start:start + size] = block * (size // 16)
    destination = overlay / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)
    return destination


def build(game_roots, ps3_psarc, overlay):
    source = next((root / RELATIVE for root in game_roots if (root / RELATIVE).is_file()), None)
    if source is None:
        raise ValueError(f'{RELATIVE} not found in the game folders')
    pak = Pak(source.read_bytes())
    slots = pak.texture_slots()
    lightmaps = ps3_lightmaps(psarc_entry(ps3_psarc, f'{LEVEL}.pak'))
    data = bytearray(pak.data)
    tex_end = pak.tex_base + pak.tex_size
    if tex_end != len(data):
        raise ValueError('texture block does not end the file')
    for placeholder, (name, dxgi, bsize) in SLOTS.items():
        desc = slots.get(placeholder)
        if desc is None or name not in lightmaps:
            raise ValueError(f'missing {placeholder} slot or PS3 {name}')
        if struct.unpack_from('<I', data, desc + 0x1c)[0] != dxgi:
            raise ValueError(f'{placeholder}: unexpected texture format')
        tiled = tile(lightmaps[name], SIZE, MIPS, bsize)
        data += bytes(-len(data) % ALIGN)
        offset = len(data) - pak.tex_base
        data += tiled
        struct.pack_into('<I', data, desc + 0x08, offset)
        struct.pack_into('<I', data, desc + 0x10, len(tiled))
        struct.pack_into('<III', data, desc + 0x24, MIPS, SIZE, SIZE)
    struct.pack_into('<I', data, 0x20, len(data) - pak.tex_base)
    destination = overlay / RELATIVE
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', type=Path, nargs='+', help='PS4 update folder first, then the base game')
    parser.add_argument('--ps3', type=Path, required=True, help='PS3 USRDIR/build/main/pak22.psarc')
    parser.add_argument('--overlay', type=Path, required=True)
    args = parser.parse_args()
    print(f'wrote {build([g.resolve() for g in args.game], args.ps3, args.overlay)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
