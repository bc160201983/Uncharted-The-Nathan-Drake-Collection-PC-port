"""Map cards for the multiplayer loading screen, from the Collection's own net.pak.

net.pak keeps the PS3 multiplayer art: art/general/net/maps/<name>-color.tga (BC1) and
<name>-trans.tga (BC4, the alpha), 1024x512, tiled for the PS4 GPU and stored upside down.
This writes <overlay>/uc2mp/loading/<map key>.png for every lobby map, which the overlay's
match loading screen (gow3_overlay.cpp MatchLoadingScreen) draws. Reads the player's own game;
writes only <overlay>.

usage: python tools/uc2mp_loading_art.py <PS4 update folder> <base game folder> --overlay <dir>
"""
import argparse
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

from uc2mp_lightmaps import Pak, morton

NET = Path('u2data/build/main/pak22/ps4/net.pak')
OUT = Path('uc2mp/loading')
# lobby map key -> net.pak card name
CARDS = {'village-multi': 'village', 'warzone-multi': 'warzone', 'gompa-multi': 'gompa',
         'ice-cave-multi': 'ice-cave', 'shambhala-multi': 'shambhala', 'the-fort-multi': 'fort',
         'train-wreck-multi': 'train-wreck', 'treasure-multi': 'treasure', 'stage-multi': 'stage'}


def untile(data, width, height, bsize):
    """Mip 0 of a tiled block texture as row-major blocks (8x8-block tiles, Morton inside)."""
    bw, bh = width // 4, height // 4
    tw = (bw + 7) // 8
    out = bytearray(bw * bh * bsize)
    for y in range(bh):
        for x in range(bw):
            s = (((y >> 3) * tw + (x >> 3)) * 64 + morton(x & 7, y & 7)) * bsize
            d = (y * bw + x) * bsize
            out[d:d + bsize] = data[s:s + bsize]
    return np.frombuffer(bytes(out), np.uint8).reshape(-1, bsize)


def bc1(blocks):
    c0 = blocks[:, 0].astype(np.int32) | (blocks[:, 1].astype(np.int32) << 8)
    c1 = blocks[:, 2].astype(np.int32) | (blocks[:, 3].astype(np.int32) << 8)

    def rgb(c):
        return np.stack([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31], -1)
    a, b = rgb(c0), rgb(c1)
    four = (c0 > c1)[:, None]
    palette = np.stack([a, b, np.where(four, (2 * a + b) // 3, (a + b) // 2),
                        np.where(four, (a + 2 * b) // 3, 0)], 1)
    bits = blocks[:, 4:8].astype(np.uint32)
    bits = bits[:, 0] | (bits[:, 1] << 8) | (bits[:, 2] << 16) | (bits[:, 3] << 24)
    index = np.stack([(bits >> (2 * i)) & 3 for i in range(16)], 1).astype(np.int64)
    return np.take_along_axis(palette, index[:, :, None], 1)  # n, 16, 3


def bc4(blocks):
    a0, a1 = blocks[:, 0].astype(np.int32), blocks[:, 1].astype(np.int32)
    eight = a0 > a1
    palette = [a0, a1]
    for i in range(1, 7):
        palette.append(np.where(eight, ((7 - i) * a0 + i * a1) // 7,
                                ((5 - i) * a0 + i * a1) // 5 if i < 5 else (0 if i == 5 else 255)))
    palette = np.stack(palette, 1)
    bits = np.zeros(len(blocks), np.uint64)
    for i in range(6):
        bits |= blocks[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    index = np.stack([(bits >> np.uint64(3 * i)) & np.uint64(7) for i in range(16)], 1).astype(np.int64)
    return np.take_along_axis(palette, index, 1)  # n, 16


def card(pak, slots, name):
    def texture(suffix, dxgi):
        key = next((k for k in slots if k.startswith(f'{name}-{suffix}.tga/')), None)
        if key is None:
            raise ValueError(f'net.pak has no {name}-{suffix}')
        desc = slots[key]
        offset, size = struct.unpack_from('<I', pak.data, desc + 8)[0], struct.unpack_from('<I', pak.data, desc + 0x10)[0]
        fmt, width, height = struct.unpack_from('<I', pak.data, desc + 0x1c)[0], *struct.unpack_from('<II', pak.data, desc + 0x28)
        if fmt != dxgi:
            raise ValueError(f'{name}-{suffix}: unexpected format')
        start = pak.tex_base + offset
        return untile(pak.data[start:start + size], width, height, 8), width, height
    colour, width, height = texture('color', 0x47)
    alpha, _, _ = texture('trans', 0x50)
    pixels = np.concatenate([bc1(colour), bc4(alpha)[:, :, None]], 2)
    pixels = pixels.reshape(height // 4, width // 4, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(height, width, 4)
    return Image.fromarray(pixels[::-1].astype(np.uint8), 'RGBA')  # stored upside down


def build(game_roots, overlay):
    source = next((root / NET for root in game_roots if (root / NET).is_file()), None)
    if source is None:
        raise ValueError(f'{NET} not found in the game folders')
    pak = Pak(source.read_bytes())
    slots = pak.texture_slots()
    destination = overlay / OUT
    destination.mkdir(parents=True, exist_ok=True)
    for key, name in CARDS.items():
        card(pak, slots, name).save(destination / f'{key}.png')
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', type=Path, nargs='+', help='PS4 update folder first, then the base game')
    parser.add_argument('--overlay', type=Path, required=True)
    args = parser.parse_args()
    print(f'wrote {build([g.resolve() for g in args.game], args.overlay)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
