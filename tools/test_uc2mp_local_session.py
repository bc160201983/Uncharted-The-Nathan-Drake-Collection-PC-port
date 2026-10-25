"""Run in a separate process: python tools/test_uc2mp_local_session.py."""
from pathlib import Path
import tempfile
import unittest
from uc2mp_local_session import LocalSession, extract_leaves

BINARY = Path(__file__).resolve().parents[1] / "work/base-1.00/eboot.elf"


class LocalSessionTests(unittest.TestCase):
    def test_unknown_image_rejected_before_execution(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "wrong.elf"
            path.write_bytes(b"wrong image")
            with self.assertRaises(ValueError):
                extract_leaves(path)

    @unittest.skipUnless(BINARY.is_file(), f"needs the player's 1.00 executable at {BINARY}")
    def test_lifecycle_and_original_getters(self):
        with LocalSession(BINARY) as s:
            for member_id in (1, 42, 0xFFFFFFFF):
                state = s.create_local(member_id)
                self.assertEqual(state["native_count"], 1)
                self.assertEqual(state["members"], [{"slot": 0, "id": member_id, "identity": member_id}])
                self.assertEqual(state["selector_a_slot"], 0)
                self.assertEqual(state["selector_b_slot"], 0)
                self.assertEqual(s._call("by_id", member_id), s.address + 0x20C)
                with self.assertRaises(RuntimeError):
                    s.create_local()
                s.close()
                s.close()
                self.assertEqual(s.snapshot()["members"], [])
                self.assertEqual(s.snapshot()["logical_count"], 0)
                self.assertEqual(s._call("by_id", member_id), 0)
                self.assertEqual(s.snapshot()["native_count"], 1)  # original offline fallback

    @unittest.skipUnless(BINARY.is_file(), f"needs the player's 1.00 executable at {BINARY}")
    def test_capacity_and_bounds(self):
        with LocalSession(BINARY) as s:
            s.create_local(100)
            for i in range(1, 10):
                self.assertEqual(s.add_member(100 + i, 200 + i), i)
            self.assertEqual(s.snapshot()["native_count"], 10)
            with self.assertRaises(OverflowError):
                s.add_member(999)
            with self.assertRaises(ValueError):
                s.add_member(100)
            for index in (-1, 10, 2**32, True):
                with self.assertRaises(IndexError):
                    s.member(index)
            s._call("set_selector_a", 205)
            self.assertEqual(s.snapshot()["selector_a_slot"], 5)
            self.assertEqual(s.snapshot()["selector_b_slot"], 0)
            s.close()
            with self.assertRaises(RuntimeError):
                s.add_member(1)
            for value in (-1, 2**32, True):
                with self.assertRaises(ValueError):
                    s.create_local(value)


if __name__ == "__main__":
    unittest.main(verbosity=2)
