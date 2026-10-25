"""The launcher's HDR at the game's own size (probe_uncharted.native_hdr_composite)."""
import os
import unittest
from unittest import mock

from probe_uncharted import native_hdr_composite

OPT_OUTS = {"UNCHARTED_TEMPORAL_AA": "1", "UNCHARTED_HDR_NATIVE": "1"}


class NativeHdrCompositeTests(unittest.TestCase):
    def run_it(self, env, game="u2", hdr_on=True, **environ):
        with mock.patch.dict(os.environ, {**OPT_OUTS, **environ}):
            return native_hdr_composite(env, game, (1920, 1080), hdr_on)

    def test_all_three_games_take_the_1_to_1_path(self):
        for game in ("u1", "u2", "u3"):
            env = {}
            self.assertTrue(self.run_it(env, game))
            self.assertEqual(env["GOW3_RENDER_RES"], "1920x1080")
            self.assertEqual(env["GOW3_OUTPUT_RES"], "1920x1080")
            self.assertEqual(env["GOW3_COMPOSITE_NATIVE"], "1")
            self.assertEqual(env["GOW3_UI_COMPOSITE_PS"], "749bbec9")
            self.assertEqual(env["GOW3_UI_TONEMAP_PS"], "484cdbf5")

    def test_lists_only_the_variables_it_set(self):
        # The renderer removes the listed ones without HDR output (vk_swapchain.cpp): a value the
        # player set must survive that.
        env = {"GOW3_UI_TONEMAP_PS": "12345678"}
        self.assertTrue(self.run_it(env))
        self.assertEqual(env["GOW3_UI_TONEMAP_PS"], "12345678")
        self.assertEqual(env["GOW3_COMPOSITE_NATIVE_ENV"].split(","),
                         ["GOW3_RENDER_RES", "GOW3_OUTPUT_RES", "GOW3_UI_COMPOSITE_PS"])

    def test_not_taken(self):
        cases = [
            ({}, {"hdr_on": False}),
            ({"GOW3_RENDER_RES": "1280x720"}, {}),  # upscaling already chosen
            ({}, {"game": "other"}),
            ({}, {"UNCHARTED_HDR_NATIVE": "0"}),
            ({}, {"UNCHARTED_TEMPORAL_AA": "0"}),
        ]
        for env, options in cases:
            before = dict(env)
            with self.subTest(env=env, options=options):
                self.assertFalse(self.run_it(env, **options))
                self.assertEqual(env, before)


if __name__ == "__main__":
    unittest.main()
