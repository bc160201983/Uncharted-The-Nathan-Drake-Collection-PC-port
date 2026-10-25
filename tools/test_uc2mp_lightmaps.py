import struct
import unittest
from uc2mp_lightmaps import morton, tile


def chain(size, mips, bsize):
    """A linear mip chain whose blocks hold their own (mip, x, y)."""
    out = bytearray()
    for m in range(mips):
        b = max(1, (size >> m) // 4)
        for y in range(b):
            for x in range(b):
                out += struct.pack('<HHH', m, x, y).ljust(bsize, b'\0')
    return bytes(out)


class LightmapTilingTests(unittest.TestCase):
    def test_sizes_match_the_collection_lightmaps(self):
        # village-multi.pak: indirect.rgbmax BC3 0x556800, indirect.rgbmaxdelta BC1 0x2ab400
        self.assertEqual(len(tile(chain(2048, 12, 16), 2048, 12, 16)), 0x556800)
        self.assertEqual(len(tile(chain(2048, 12, 8), 2048, 12, 8)), 0x2ab400)

    def test_morton_inside_a_tile_and_tiles_in_rows(self):
        self.assertEqual([morton(x, y) for x, y in ((1, 1), (3, 1), (3, 2), (5, 0), (0, 5))], [3, 7, 13, 17, 34])
        tiled = tile(chain(64, 2, 8), 64, 2, 8)  # 16x16 blocks: 2x2 tiles, then mip 1 padded
        at = lambda i: struct.unpack_from('<HHH', tiled, 8 * i)
        self.assertEqual(at(13), (0, 3, 2))
        self.assertEqual(at(64), (0, 8, 0))       # second tile of the first row
        self.assertEqual(at(128), (0, 0, 8))      # first tile of the second row
        self.assertEqual(at(256 + 7), (1, 3, 1))  # mip 1 starts after four tiles

    def test_wrong_chain_rejected(self):
        with self.assertRaises(ValueError):
            tile(chain(2048, 11, 8), 2048, 12, 8)


if __name__ == '__main__':
    unittest.main()
