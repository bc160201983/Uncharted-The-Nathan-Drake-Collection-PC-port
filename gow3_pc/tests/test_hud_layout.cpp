// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "gpu/shadps4/video_core/renderer_vulkan/hud_layout.h"

static bool near(float a, float b, float eps = 0.0f) {
    return std::fabs(a - b) < (eps > 0.0f ? eps : 1e-3f);
}

int main() {
    using namespace Vulkan::HudLayout;
    const float hud = 16.0f / 9.0f;
    // 32:9 halves the HUD width, 21:9 (2560x1080) by 0.75; 16:9, 16:10 and an off setting keep it.
    assert(near(Factor(hud, 5120, 1440), 0.5f));
    assert(near(Factor(hud, 3408, 960), hud / (3408.0f / 960.0f)));
    assert(near(Factor(hud, 2560, 1080), 0.75f));
    assert(Factor(hud, 1920, 1080) == 1.0f && Factor(hud, 1280, 800) == 1.0f);
    assert(Factor(0.0f, 5120, 1440) == 1.0f);
    // The full-target viewport becomes the centred 16:9 area: 1280..3840 of 5120.
    const auto full = Viewport(0, 5120, 5120, 0.5f);
    assert(near(full.x, 1280) && near(full.width, 2560));
    // A viewport twice the target (a full-width quad drawn 1/k wider) fills the target again.
    const auto wide = Viewport(-1280, 7680, 5120, 0.5f);
    assert(near(wide.x, 1280 - 1280 * 0.5f) && near(wide.width, 3840));
    // A full scissor stays (full-width quads are drawn wider and must not be cut); a narrower one
    // follows the viewport.
    const auto all = Scissor(0, 5120, 5120, 1440, 0.5f);
    assert(all.x0 == 0 && all.x1 == 5120);
    const auto part = Scissor(0, 2560, 5120, 1440, 0.5f);
    assert(part.x0 == 1280 && part.x1 == 2560);
    const auto none = Scissor(100, 200, 5120, 1440, 1.0f);
    assert(none.x0 == 100 && none.x1 == 200);
    setenv("GOW3_HUD_ASPECT", "16:9", 1);
    assert(near(AspectFromEnv(), hud));
    setenv("GOW3_HUD_ASPECT", "1.7777778", 1);
    assert(near(AspectFromEnv(), hud));
    unsetenv("GOW3_HUD_ASPECT");
    assert(AspectFromEnv() == 0.0f);
    assert(CompositeFromEnv() == 0x749bbec9ull);
    // Which draws are the HUD's: in-game over the tonemapped frame (indexed only), menus after
    // the copy into the display buffer.
    Tracker t;
    const uint64_t frame = 0x1000, display = 0x2000, scene = 0x3000;
    assert(!t.Draw(0xabc, scene, false, true));                 // the scene
    assert(!t.Draw(0x484cdbf5, frame, false, false));           // tonemap
    assert(t.Draw(0x111, frame, false, true));                  // HUD quad over the frame
    assert(!t.Draw(0x222, frame, false, false));                // full-screen pass: kept
    assert(!t.Draw(0x333, scene, false, true));                 // offscreen work: not the HUD
    assert(t.Draw(0x111, frame, false, true));                  // still over the frame
    assert(!t.Draw(0x749bbec9, display, true, false));          // the copy: full width
    assert(t.Draw(0x444, display, true, true));                 // menu after the copy
    assert(!t.Draw(0x333, scene, false, true));
    assert(t.Draw(0x444, display, true, false));                // still after the copy
    assert(!t.Draw(0x484cdbf5, frame, false, false));           // next frame
    assert(!t.Draw(0x444, display, true, true));                // display draw before the copy
    assert(t.frames == 2 && t.over_frame_draws == 2 && t.over_frame_skipped == 1);
    // Anchors from a draw's extent: NDC, or the 1280-wide UI space beyond +-4.
    assert(Classify(true, -1.0f, 1.0f, false) == Anchor::Full);           // fade, backdrop
    assert(Classify(true, 0.0f, 1280.0f, true) == Anchor::Full);          // the same in UI space
    assert(Classify(true, -0.9f, -0.5f, false) == Anchor::Centre);        // 16:9: no edges
    assert(Classify(true, -0.9f, -0.5f, true) == Anchor::Left);           // wide: left third
    assert(Classify(true, 60.0f, 380.0f, true) == Anchor::Left);          // ammo text, UI space
    assert(Classify(true, 0.5f, 0.9f, true) == Anchor::Right);
    assert(Classify(true, -0.5f, 0.5f, true) == Anchor::Centre);          // subtitles
    assert(Classify(true, -0.9f, -0.2f, true) == Anchor::Left);           // a bar: by its centre
    assert(Classify(true, 0.2f, 0.9f, true) == Anchor::Right);
    assert(Classify(true, -0.9f, 0.4f, true) == Anchor::Centre);          // centre -0.25
    assert(Classify(false, 0, 0, true) == Anchor::Centre);                // unknown layout
    // Placement at 32:9 (k = 0.5) in a 5120-wide target.
    assert(near(Place(0, 5120, 1440, 0.5f, Anchor::Centre), 1280) && near(Place(5120, 5120, 1440, 0.5f, Anchor::Centre), 3840));
    assert(near(Place(0, 5120, 1440, 0.5f, Anchor::Left), 0) && near(Place(5120, 5120, 1440, 0.5f, Anchor::Left), 2560));
    assert(near(Place(0, 5120, 1440, 0.5f, Anchor::Right), 2560) && near(Place(5120, 5120, 1440, 0.5f, Anchor::Right), 5120));
    assert(near(Place(1000, 5120, 1440, 0.5f, Anchor::Full), 1000));
    // 32:9 (5120x1440) with a 21:9 area (3360 wide): edges at 880 and 4240.
    const float a219 = 7.0f / 3.0f;
    assert(near(Place(0, 5120, 1440, 0.5f, Anchor::Left, a219), 880));
    assert(near(Place(5120, 5120, 1440, 0.5f, Anchor::Left, a219), 880 + 2560));
    assert(near(Place(5120, 5120, 1440, 0.5f, Anchor::Right, a219), 4240));
    assert(near(Place(0, 5120, 1440, 0.5f, Anchor::Right, a219), 4240 - 2560));
    assert(near(Place(0, 5120, 1440, 0.5f, Anchor::Centre, a219), 1280));
    assert(near(Place(0, 5120, 1440, 0.5f, Anchor::Left, 0.0f), 0));       // screen edges
    // 21:9 screen (3440x1440), k = 0.7757 (16:9 of 21:9): area 0 = the screen, an area wider than
    // the screen is clamped to it.
    const float k219 = Factor(16.0f / 9.0f, 3440, 1440);
    assert(near(Place(0, 3440, 1440, k219, Anchor::Left, 0.0f), 0));
    assert(near(Place(3440, 3440, 1440, k219, Anchor::Right, 0.0f), 3440));
    assert(near(Place(0, 3440, 1440, k219, Anchor::Left, 32.0f / 9.0f), 0));
    assert(near(Place(3440, 3440, 1440, k219, Anchor::Right, 32.0f / 9.0f), 3440));
    setenv("GOW3_HUD_WIDE_ASPECT", "21:9", 1);
    assert(near(AspectFromEnv("GOW3_HUD_WIDE_ASPECT"), 7.0f / 3.0f));
    unsetenv("GOW3_HUD_WIDE_ASPECT");
    assert(AspectFromEnv("GOW3_HUD_WIDE_ASPECT") == 0.0f);
    const auto left = Scissor(0, 2560, 5120, 1440, 0.5f, Anchor::Left);
    assert(left.x0 == 0 && left.x1 == 1280);
    // Half floats.
    assert(HalfToFloat(0x0000) == 0.0f && HalfToFloat(0x3c00) == 1.0f && HalfToFloat(0xbc00) == -1.0f);
    assert(HalfToFloat(0x4000) == 2.0f && HalfToFloat(0x3800) == 0.5f && HalfToFloat(0x5640) == 100.0f);
    assert(HalfToFloat(0x0001) > 0.0f && HalfToFloat(0x0001) < 1e-7f);
    assert(!std::isfinite(HalfToFloat(0x7c00)) && !std::isfinite(HalfToFloat(0x7e00)));
    // Tracker phases: menus after the copy are not "over the frame".
    {
        Tracker t2;
        t2.Draw(0x484cdbf5, 1, false, false);
        assert(t2.Draw(0x111, 1, false, true) && t2.last_over_frame);
        assert(!t2.Draw(0x222, 1, false, false) && !t2.last_over_frame);
        t2.Draw(0x749bbec9, 3, true, false);
        assert(t2.Draw(0x444, 3, true, true) && !t2.last_over_frame);
    }
    // Wide mode's three passes at 32:9 (5120x1440, k = 0.5), screen edges: left third at the left
    // edge, middle third centred, right third at the right edge.
    {
        const auto p = Split(0, 5120, 5120, 1440, 0.5f);
        assert(p[0].anchor == Anchor::Left && p[1].anchor == Anchor::Centre && p[2].anchor == Anchor::Right);
        assert(p[0].columns.x0 == 0 && near(float(p[0].columns.x1), 853.3f, 1.0f));
        assert(near(float(p[1].columns.x0), 1280 + 853.3f, 1.0f) && near(float(p[1].columns.x1), 1280 + 1706.7f, 1.0f));
        assert(near(float(p[2].columns.x0), 5120 - 853.3f, 1.0f) && p[2].columns.x1 == 5120);
        // A game scissor on the left sixth is kept only in the left pass.
        const auto q = Split(0, 800, 5120, 1440, 0.5f);
        assert(q[0].columns.x1 > q[0].columns.x0 && q[1].columns.x1 == q[1].columns.x0 && q[2].columns.x1 == q[2].columns.x0);
        // A 21:9 area (3360 wide): the edges are at 880 and 4240.
        const auto r = Split(0, 5120, 5120, 1440, 0.5f, 7.0f / 3.0f);
        assert(r[0].columns.x0 == 880 && near(float(r[0].columns.x1), 880 + 853.3f, 1.0f));
        assert(r[2].columns.x1 == 4240 && near(float(r[2].columns.x0), 4240 - 853.3f, 1.0f));
        // 21:9 screen (3440x1440): k = 0.7757, area 0.
        const float k = Factor(16.0f / 9.0f, 3440, 1440);
        const auto u = Split(0, 3440, 3440, 1440, k);
        assert(u[0].columns.x0 == 0 && u[2].columns.x1 == 3440);
        assert(u[0].columns.x1 <= u[1].columns.x0 + 1 && u[1].columns.x1 <= u[2].columns.x0 + 1);
        assert(near(float(u[1].columns.x0 + u[1].columns.x1), 3440.0f, 2.0f)); // centred
    }
    std::puts("HUD layout: PASS (factor, viewport, scissor, settings, HUD draws, anchors)");
}
