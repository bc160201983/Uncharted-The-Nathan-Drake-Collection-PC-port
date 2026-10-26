// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

// gow3: the Uncharted 2 HUD at ultrawide sizes (GOW3_HUD_ASPECT, GOW3_HUD_MODE; launcher ->
// Display -> HUD). The game lays its HUD and menus out in a 1280x720 UI space that it maps to the
// whole target, so past 16:9 they were stretched. Each HUD draw (Tracker) is placed by its own
// horizontal extent (read from its vertices): across the whole width it keeps the full width
// (fades, backdrops), otherwise it is drawn into the 16:9 area: in the middle, or in the wide
// mode at the left or right screen edge when it lies wholly in that third of the UI space. The
// placement is the viewport (and a scissor narrower than the target), so nothing the game draws
// is cut: each draw's 16:9 area is where its content belongs.
namespace Vulkan::HudLayout {

/// The horizontal scale of the HUD pass at a `width` x `height` target (1: no change).
inline float Factor(float hud_aspect, uint32_t width, uint32_t height) {
    if (hud_aspect <= 0.0f || width == 0 || height == 0) {
        return 1.0f;
    }
    const float aspect = float(width) / float(height);
    return aspect > hud_aspect * 1.005f ? hud_aspect / aspect : 1.0f;
}

/// An IEEE half float (binary16) as a float.
inline float HalfToFloat(uint16_t h) {
    const uint32_t exponent = (h >> 10) & 0x1f;
    const uint32_t mantissa = h & 0x3ff;
    float value;
    if (exponent == 0) {
        value = std::ldexp(float(mantissa), -24);
    } else if (exponent == 31) {
        value = mantissa ? std::nanf("") : HUGE_VALF;
    } else {
        value = std::ldexp(float(mantissa | 0x400), int(exponent) - 25);
    }
    return (h & 0x8000) ? -value : value;
}

/// Where a HUD draw goes.
enum class Anchor : uint8_t { Centre, Left, Right, Full };

/// A draw's anchor from the horizontal extent of its vertex positions: NDC, or (beyond +-4) the
/// 1280-wide UI space. No extent (unknown vertex layout): the middle.
inline Anchor Classify(bool valid, float min_x, float max_x, bool wide) {
    if (!valid || !(min_x <= max_x)) {
        return Anchor::Centre;
    }
    if (std::max(std::fabs(min_x), std::fabs(max_x)) > 4.0f) {
        min_x = min_x / 640.0f - 1.0f;
        max_x = max_x / 640.0f - 1.0f;
    }
    if (min_x <= -0.995f && max_x >= 0.995f) {
        return Anchor::Full;
    }
    // By the centre: the parts of one element (icon, bar, text) then go to the same side.
    const float centre = (min_x + max_x) * 0.5f;
    if (wide && centre <= -1.0f / 3.0f) {
        return Anchor::Left;
    }
    if (wide && centre >= 1.0f / 3.0f) {
        return Anchor::Right;
    }
    return Anchor::Centre;
}

/// Width of the area the Left and Right anchors fill: a centred area of `area_aspect` (clamped to
/// the target; 0: the whole target).
inline float WideArea(float target_width, float target_height, float area_aspect) {
    if (area_aspect <= 0.0f) {
        return target_width;
    }
    return std::min(target_width, target_height * area_aspect);
}

/// Target x (viewport or scissor edge) of a draw at x in the full target, for its anchor. Left
/// and Right put the 16:9 layout's edge at the edge of the wide area.
inline float Place(float x, float target_width, float target_height, float k, Anchor anchor,
                   float area_aspect = 0.0f) {
    const float area = WideArea(target_width, target_height, area_aspect);
    switch (anchor) {
    case Anchor::Left:
        return (target_width - area) * 0.5f + x * k;
    case Anchor::Right:
        return (target_width + area) * 0.5f - (target_width - x) * k;
    case Anchor::Full:
        return x;
    case Anchor::Centre:
    default:
        return target_width * 0.5f + (x - target_width * 0.5f) * k;
    }
}

/// Viewport x and width, squeezed by k around the target's centre.
struct Span {
    float x, width;
};
inline Span Viewport(float x, float width, float target_width, float k) {
    const float centre = target_width * 0.5f;
    return {centre + (x - centre) * k, width * k};
}

/// Scissor columns [x0, x1): a scissor across the whole target stays (full-width quads are drawn
/// wider by the game and must not be cut); a narrower one is squeezed like the viewport.
struct Columns {
    int32_t x0, x1;
};
inline Columns Scissor(int32_t x0, int32_t x1, uint32_t target_width, uint32_t target_height,
                       float k, Anchor anchor = Anchor::Centre, float area_aspect = 0.0f) {
    if (k >= 1.0f || anchor == Anchor::Full || (x0 <= 0 && x1 >= int32_t(target_width))) {
        return {x0, x1};
    }
    const auto place = [&](int32_t v) {
        return int32_t(std::lround(Place(float(v), float(target_width), float(target_height), k, anchor,
                                        area_aspect)));
    };
    return {place(x0), place(x1)};
}

/// Wide mode draws a HUD draw three times, once per anchor; a pass keeps only the screen columns
/// where the third of the layout it belongs to is shown: [0, W/3) at the left edge, the middle
/// third in the middle, the last third at the right edge.
struct Pass {
    Anchor anchor;
    Columns columns;
};
inline Columns PassColumns(Anchor anchor, int32_t game_x0, int32_t game_x1, uint32_t target_width,
                           uint32_t target_height, float k, float area_aspect = 0.0f) {
    const int32_t index = anchor == Anchor::Left ? 0 : anchor == Anchor::Centre ? 1 : 2;
    const float w = float(target_width);
    const auto edge = [&](int32_t i) {
        return int32_t(std::lround(Place(w * float(i) / 3.0f, w, float(target_height), k, anchor,
                                         area_aspect)));
    };
    const Columns game = Scissor(game_x0, game_x1, target_width, target_height, k, anchor,
                                 area_aspect);
    const Columns out{std::max(game.x0, edge(index)), std::min(game.x1, edge(index + 1))};
    return {out.x0, std::max(out.x0, out.x1)};
}
inline std::array<Pass, 3> Split(int32_t game_x0, int32_t game_x1, uint32_t target_width,
                                 uint32_t target_height, float k, float area_aspect = 0.0f) {
    const auto pass = [&](Anchor anchor) {
        return Pass{anchor, PassColumns(anchor, game_x0, game_x1, target_width, target_height, k,
                                        area_aspect)};
    };
    return {pass(Anchor::Left), pass(Anchor::Centre), pass(Anchor::Right)};
}

/// GOW3_HUD_MODE: "wide" moves side elements to the screen edges (default: the 16:9 area).
inline bool WideFromEnv() {
    const char* env = std::getenv("GOW3_HUD_MODE");
    return env && std::string_view{env} == "wide";
}

/// GOW3_HUD_ASPECT (or another variable: GOW3_HUD_WIDE_ASPECT) as a float or W:H; 0 when unset.
inline float AspectFromEnv(const char* name = "GOW3_HUD_ASPECT") {
    const char* env = std::getenv(name);
    if (!env || !*env) {
        return 0.0f;
    }
    float w = 0.0f, h = 0.0f;
    char colon = 0;
    if (std::sscanf(env, "%f%c%f", &w, &colon, &h) == 3 && colon == ':' && h > 0.0f) {
        return w / h;
    }
    return std::max(0.0f, float(std::atof(env)));
}

/// Which draws are the HUD's. Uncharted finishes a frame with the tonemap draw
/// (GOW3_UI_TONEMAP_PS, 484cdbf5) into its target, draws the in-game HUD, subtitles and prompts
/// over that target, copies it into a display buffer (GOW3_HUD_COMPOSITE_PS, 749bbec9), and
/// draws menus into the display buffer after the copy. HUD draws are therefore:
/// - over the frame: indexed draws into the tonemap target after the tonemap draw, until the copy
///   (the HUD's quads and text are indexed; a full-screen pass is not and keeps the full width);
/// - after the copy: draws into a display buffer, until the next tonemap draw.
struct Tracker {
    uint64_t tonemap_ps = 0x484cdbf5ull;
    uint64_t composite_ps = 0x749bbec9ull;
    uint64_t frame_target = 0; ///< the tonemap draw's target
    bool over_frame = false;   ///< after the tonemap draw, before the copy
    bool armed = false;        ///< the copy was the last display draw
    bool after_copy = false;   ///< display draws after the copy
    bool last_over_frame = false; ///< the last Draw was a HUD draw over the frame (not a menu)
    // Trace counters (GOW3_HUD_TRACE).
    uint64_t frames = 0, over_frame_draws = 0, over_frame_skipped = 0, after_copy_draws = 0;

    /// One draw: its pixel shader, colour target and whether it is indexed; true for a HUD draw.
    bool Draw(uint64_t ps, uint64_t target, bool display, bool indexed) {
        last_over_frame = false;
        if (ps == tonemap_ps) {
            ++frames;
            frame_target = target;
            over_frame = true;
            armed = after_copy = false;
            return false;
        }
        if (display && ps == composite_ps) {
            over_frame = after_copy = false;
            armed = true;
            return false;
        }
        if (over_frame && target == frame_target) {
            (indexed ? over_frame_draws : over_frame_skipped)++;
            last_over_frame = indexed;
            return indexed;
        }
        if (display) {
            if (armed) {
                after_copy = true;
                armed = false;
            }
            after_copy_draws += after_copy ? 1 : 0;
            return after_copy;
        }
        return false; // other targets (the scene, offscreen HUD work) leave the phases alone
    }
};

inline uint64_t TonemapFromEnv() {
    const char* env = std::getenv("GOW3_UI_TONEMAP_PS");
    return env && *env ? std::strtoull(env, nullptr, 16) : 0x484cdbf5ull;
}

inline uint64_t CompositeFromEnv() {
    const char* env = std::getenv("GOW3_HUD_COMPOSITE_PS");
    return env && *env ? std::strtoull(env, nullptr, 16) : 0x749bbec9ull;
}

} // namespace Vulkan::HudLayout
