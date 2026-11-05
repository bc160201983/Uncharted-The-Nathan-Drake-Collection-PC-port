"""Check the shipped U2 HDR shader ABI against the port's tone-draw contract.

Requires spirv-cross and spirv-val on PATH, or SPIRV_CROSS / SPIRV_VAL overrides.
Run from the repository: python -m unittest discover -s gow3_pc/tests -p test_hdr_shader.py
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import unittest

SHADER = Path(__file__).resolve().parent.parent / 'hdr/fs_3371ae8d_hdr.spv'
CROSS = os.environ.get('SPIRV_CROSS') or shutil.which('spirv-cross')
VALIDATOR = os.environ.get('SPIRV_VAL') or shutil.which('spirv-val')


@unittest.skipUnless(CROSS and VALIDATOR, 'SPIR-V tools are required')
class HdrShaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run([VALIDATOR, '--target-env', 'vulkan1.3', str(SHADER)], check=True)
        cls.reflection = json.loads(subprocess.check_output(
            [CROSS, str(SHADER), '--reflect'], text=True))

    def test_live_settings_do_not_use_specialization_constants(self):
        self.assertFalse(self.reflection.get('specialization_constants'))
        buffers = {b['binding']: b for b in self.reflection['ssbos']}
        self.assertEqual(set(buffers), {0, 1, 12})
        self.assertEqual(buffers[12]['name'], 'HdrSettings')
        self.assertTrue(buffers[12]['readonly'])
        self.assertEqual(buffers[12]['set'], 0)

    def test_guest_resources_keep_their_bindings(self):
        self.assertEqual({x['binding'] for x in self.reflection['separate_images']}, set(range(2, 8)))
        self.assertEqual({x['binding'] for x in self.reflection['separate_samplers']}, {8, 9})
        for category in ('ssbos', 'separate_images', 'separate_samplers'):
            self.assertTrue(all(x['set'] == 0 for x in self.reflection[category]))

    def test_push_constants_still_fit_the_guest_128_byte_layout(self):
        block, = self.reflection['push_constants']
        members = self.reflection['types'][block['type']]['members']
        self.assertEqual([m['offset'] for m in members],
                         [0, 4, 8, 12, 16, 32, 48, 64, 80, 96, 112, 120, 124])
        self.assertEqual([m['type'] for m in members],
                         ['float'] * 4 + ['uvec4'] * 6 + ['uvec2', 'uint', 'uint'])

    def test_one_fragment_output_and_expected_entry_point(self):
        self.assertEqual(self.reflection['entryPoints'], [{'name': 'main', 'mode': 'frag'}])
        output, = self.reflection['outputs']
        self.assertEqual((output['type'], output['location']), ('vec4', 0))


if __name__ == '__main__':
    unittest.main()
