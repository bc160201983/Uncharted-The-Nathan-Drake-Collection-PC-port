import math
from pathlib import Path
import struct
import tempfile
import unittest
from uc2mp_extract_spawns import records, sid, extract


def package(ps3=False, bad_sid=False, position=1.0):
    endian, pointer, width = ('>', '>I', 4) if ps3 else ('<', '<Q', 8)
    raw = bytearray(0x300)
    struct.pack_into(endian + 'I', raw, 0, 0xa79)
    struct.pack_into(endian + 'II', raw, 0x10, 1, 0x2c)
    struct.pack_into(endian + 'III', raw, 0x2c, 0x100, 0x200, 0)
    struct.pack_into(pointer, raw, 0x100 + width, 0x1c0)
    struct.pack_into(endian + 'I', raw, 0x100 + (0xc if ps3 else 0x14), 0x12345)
    struct.pack_into(pointer, raw, 0x100 + (0x1c if ps3 else 0x28), 0x100)
    struct.pack_into(endian + 'I', raw, 0x100 + (0x20 if ps3 else 0x30), 1)
    struct.pack_into(pointer, raw, 0x200, 0x120)
    struct.pack_into(endian + '8f', raw, 0x220, position, 2, 3, 0, 0, 0, 0, 1)
    struct.pack_into(pointer, raw, 0x240, 0x1a0)
    struct.pack_into(endian + 'I', raw, 0x220 + (0x24 if ps3 else 0x28), 0 if bad_sid else sid('online-spawner-1'))
    raw[0x2a0:0x2a0 + 17] = b'online-spawner-1\0'
    raw[0x2c0:0x2c0 + 14] = b'SPAWNER_GROUP\0'
    return raw


class ExtractTests(unittest.TestCase):
    def test_endian_variants_preserve_xyz_quaternion(self):
        with tempfile.TemporaryDirectory() as root:
            for ps3 in (False, True):
                path = Path(root) / str(ps3)
                path.write_bytes(package(ps3))
                row = records(path, ps3)['records'][0]
                self.assertEqual(row['position_xyz'], (1, 2, 3))
                self.assertEqual(row['quaternion'], (0, 0, 0, 1))

    def test_truncated_chunk_and_invalid_metadata_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'asset'
            for data in (package()[:-8], package(bad_sid=True), package(position=math.nan)):
                path.write_bytes(data)
                with self.assertRaises(ValueError):
                    records(path)

    def test_unknown_binary_contract_rejected_before_asset_reads(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            elf = root / 'elf'
            elf.write_bytes(b'unknown revision')
            with self.assertRaisesRegex(ValueError, 'ELF hash'):
                extract(root / 'base', None, root / 'cache', elf)


if __name__ == '__main__':
    unittest.main()
