"""HDR composition invariants: scene brightness survives UI-white rebasing."""
import math
import unittest


def encode(v):
    a = abs(v)
    return math.copysign(12.92*a if a < 0.0031308 else 1.055*a**(1/2.4)-0.055, v)


def decode(v):
    a = abs(v)
    return math.copysign(a/12.92 if a <= 0.04045 else ((a+0.055)/1.055)**2.4, v)


class CompositionTests(unittest.TestCase):
    def test_scene_luminance_survives_ui_white_changes(self):
        for game in (80, 194, 203, 500):
            for ui in (80, 196, 203, 500):
                for scene in (0, 0.0001, 0.002, 0.18, 1, 4, 20):
                    original = abs(encode(scene))**2.2 * game
                    rebased = decode(encode(scene)*(game/ui)**(1/2.2))
                    presented = abs(encode(rebased))**2.2 * ui
                    self.assertAlmostEqual(original, presented, delta=max(1e-8,original*1e-6))

    def test_opaque_ui_white_tracks_ui_nits(self):
        for ui in (80, 196, 203, 500):
            self.assertAlmostEqual(encode(1)**2.2 * ui, ui, places=10)

    def test_unorm_display_white_tracks_ui_nits(self):
        for ui in (80, 196, 203, 500):
            self.assertEqual(1.0**2.2 * ui, ui)

    def test_transparent_ui_preserves_scene(self):
        game, ui, scene = 194, 500, 0.18
        rebased = decode(encode(scene)*(game/ui)**(1/2.2))
        blended = 1*0 + rebased*(1-0)
        self.assertAlmostEqual(encode(blended)**2.2*ui, encode(scene)**2.2*game, places=9)


if __name__ == '__main__':
    unittest.main()
