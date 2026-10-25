import tempfile
from pathlib import Path
import unittest
from uc2mp_stage_wrapper import build_overlay, validate_and_append, RELATIVE_MANIFEST, RELATIVE_PACKAGE


class StageWrapperTests(unittest.TestCase):
    def test_append_exactly_once_and_preserve_original_bytes(self):
        original = b'level-name stage-multi \r\nlevel-id stage-multi\r\npackage stage-multi\r\n'
        output, added = validate_and_append(original)
        self.assertTrue(added)
        self.assertTrue(output.startswith(original))
        self.assertEqual(validate_and_append(output), (output, False))
        self.assertEqual(output.count(b'level-name stage-multi-pak\r\n'), 1)

    def test_duplicate_and_conflicting_blocks_rejected(self):
        with self.assertRaises(ValueError):
            validate_and_append(b'level-name stage-multi\nlevel-name stage-multi\n')
        with self.assertRaises(ValueError):
            validate_and_append(b'level-name stage-multi\nlevel-name stage-multi-pak\npackage wrong\n')

    def test_effective_patch_is_read_only_and_output_conflicts_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            base, patch, overlay = (root / name for name in ('base', 'patch', 'overlay'))
            for tree, suffix in ((base, b'base'), (patch, b'patch')):
                target = tree / RELATIVE_MANIFEST
                target.parent.mkdir(parents=True)
                target.write_bytes(b'level-name stage-multi\npackage stage-multi\n#' + suffix + b'\n')
            package = base / RELATIVE_PACKAGE
            package.parent.mkdir(parents=True)
            package.write_bytes(bytes.fromhex('790a0000') + bytes(32))
            before = (patch / RELATIVE_MANIFEST).read_bytes()
            report = build_overlay(base, patch, overlay)
            self.assertEqual(report['source'], str(patch / RELATIVE_MANIFEST))
            self.assertEqual(build_overlay(base, patch, overlay), report)
            self.assertEqual((patch / RELATIVE_MANIFEST).read_bytes(), before)
            (overlay / RELATIVE_MANIFEST).write_text('other overlay')
            with self.assertRaises(ValueError):
                build_overlay(base, patch, overlay)
            with self.assertRaises(ValueError):
                build_overlay(base, patch, base / 'unsafe')


if __name__ == '__main__':
    unittest.main()
