// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

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
    // Exactly 0..1: a full-screen quad in unit space (fades), not a 16:9 element.
    if (min_x >= -0.001f && min_x <= 0.001f && max_x >= 0.999f && max_x <= 1.001f) {
        return Anchor::Full;
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

/// The x intervals of a draw's primitives, in the units of its vertex positions (`positions`: x of
/// each vertex in draw order, NaN for one that could not be read). `group` is the vertices per
/// primitive (3: triangle list, 4: quad list) or 0 for a strip (every 3 in a row).
inline std::vector<std::pair<float, float>> PrimitiveIntervals(const std::vector<float>& positions,
                                                               uint32_t group = 3) {
    std::vector<std::pair<float, float>> out;
    const size_t step = group == 0 ? 1 : group;
    const size_t size = group == 0 ? 3 : group;
    for (size_t i = 0; i + size <= positions.size(); i += step) {
        float lo = HUGE_VALF, hi = -HUGE_VALF;
        for (size_t k = 0; k < size; ++k) {
            const float x = positions[i + k];
            if (std::isfinite(x)) {
                lo = std::min(lo, x);
                hi = std::max(hi, x);
            }
        }
        if (lo <= hi) {
            out.emplace_back(lo, hi);
        }
    }
    return out;
}

/// Intervals in NDC: positions beyond +-4 are in the 1280-wide UI space (as in Classify, decided
/// by the draw's whole extent).
inline void NormaliseIntervals(std::vector<std::pair<float, float>>& intervals) {
    float limit = 0.0f;
    for (const auto& [lo, hi] : intervals) {
        limit = std::max({limit, std::fabs(lo), std::fabs(hi)});
    }
    if (limit > 4.0f) {
        for (auto& [lo, hi] : intervals) {
            lo = lo / 640.0f - 1.0f;
            hi = hi / 640.0f - 1.0f;
        }
    }
}

/// Where wide mode cuts a draw into its left, middle and right passes (layout x, -1..1): in the
/// empty columns between its elements, so a line of text or a batch of prompts is never sliced.
/// `intervals` are the x spans (NDC) of the draw's primitives; spans closer than 0.04 (the gaps
/// between letters and words) form one cluster, and a cluster is Left, Centre or Right by its
/// centre like Classify. The cuts are -1 and +1 when no cluster is Left / Right. Without
/// intervals, or when the clusters are not in Left, Centre, Right order, the old thirds.
inline std::pair<float, float> SplitPoints(std::vector<std::pair<float, float>> intervals) {
    constexpr std::pair<float, float> thirds{-1.0f / 3.0f, 1.0f / 3.0f};
    if (intervals.empty()) {
        return thirds;
    }
    std::sort(intervals.begin(), intervals.end());
    struct Cluster {
        float lo, hi;
        int side; // 0 Left, 1 Centre, 2 Right
    };
    std::vector<Cluster> clusters;
    for (const auto& [lo, hi] : intervals) {
        if (!clusters.empty() && lo - clusters.back().hi < 0.04f) {
            clusters.back().hi = std::max(clusters.back().hi, hi);
        } else {
            clusters.push_back({lo, hi, 1});
        }
    }
    int previous = 0;
    for (auto& cluster : clusters) {
        const float centre = (cluster.lo + cluster.hi) * 0.5f;
        cluster.side = centre <= -1.0f / 3.0f ? 0 : centre >= 1.0f / 3.0f ? 2 : 1;
        if (cluster.side < previous) {
            return thirds;
        }
        previous = cluster.side;
    }
    float left = -1.0f, right = 1.0f;
    for (size_t i = 0; i < clusters.size(); ++i) {
        const bool last = i + 1 == clusters.size();
        if (clusters[i].side == 0) {
            left = last ? 1.0f : (clusters[i].hi + clusters[i + 1].lo) * 0.5f;
        }
        if (clusters[i].side == 2 && (i == 0 || clusters[i - 1].side != 2)) {
            right = i == 0 ? -1.0f : (clusters[i - 1].hi + clusters[i].lo) * 0.5f;
        }
    }
    return {left, std::max(left, right)};
}

/// The horizontal scale of a draw that scales itself about its own centre (world and screen
/// anchored shapes whose vertices are final NDC, widened by the game for the screen's aspect on
/// top of the HUD layer's stretch: the aiming reticle, pickup rings): the squeeze k twice, so the
/// shape is as round as at 16:9. 1 when the HUD is not squeezed.
inline float SelfScale(float k) {
    return k >= 1.0f ? 1.0f : k * k;
}

/// The centre of an extent in NDC (positions beyond +-4 are in the 1280-wide UI space).
inline float ExtentCentre(float min_x, float max_x) {
    if (std::max(std::fabs(min_x), std::fabs(max_x)) > 4.0f) {
        min_x = min_x / 640.0f - 1.0f;
        max_x = max_x / 640.0f - 1.0f;
    }
    return (min_x + max_x) * 0.5f;
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

/// An extent in NDC (positions beyond +-4 are in the 1280-wide UI space).
inline void ExtentToNdc(float& min_x, float& max_x) {
    if (std::max(std::fabs(min_x), std::fabs(max_x)) > 4.0f) {
        min_x = min_x / 640.0f - 1.0f;
        max_x = max_x / 640.0f - 1.0f;
    }
}

/// The centre the self-scaling draws of one shape scale about. A reticle is several draws in a
/// row (circle, brackets, ticks), and each scaled about its own centre would keep the gaps
/// between them too wide: a run is the consecutive self-scaling draws of a frame (any other draw
/// ends it), and a draw in run i scales about the centre of run i's whole extent in the frame
/// before (its own centre when there was none: the first frame, a new shape).
struct SelfScaleRuns {
    static constexpr int kMax = 16;
    struct Run {
        float lo = 0.0f, hi = 0.0f;
        bool valid = false;
    };
    Run previous[kMax]{}, current[kMax]{};
    int count = 0;       ///< runs of this frame so far
    bool open = false;   ///< the last draw was a self-scaling one

    void BeginFrame() {
        std::copy(std::begin(current), std::end(current), std::begin(previous));
        std::fill(std::begin(current), std::end(current), Run{});
        count = 0;
        open = false;
    }
    void EndRun() {
        open = false;
    }
    /// A self-scaling draw with a valid extent (NDC): the centre it scales about; `run` gets the
    /// run's index.
    float Draw(float min_x, float max_x, int* run = nullptr) {
        if (!open) {
            open = true;
            ++count;
        }
        const int index = std::min(count, kMax) - 1;
        if (run) {
            *run = count - 1;
        }
        if (count <= kMax) {
            Run& r = current[index];
            r.lo = r.valid ? std::min(r.lo, min_x) : min_x;
            r.hi = r.valid ? std::max(r.hi, max_x) : max_x;
            r.valid = true;
            if (previous[index].valid) {
                return (previous[index].lo + previous[index].hi) * 0.5f;
            }
        }
        return (min_x + max_x) * 0.5f;
    }
};

/// A viewport x and width scaled by s about the NDC x `centre`, which stays where it was.
inline Span SelfScaleViewport(float x, float width, float centre, float s) {
    return {x + width * (centre + 1.0f) * 0.5f * (1.0f - s), width * s};
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
/// where the part of the layout it belongs to is shown: the left part [0, cut_left) at the left
/// edge, the middle part in the middle, the right part [cut_right, W) at the right edge. The cuts
/// are layout x in -1..1 (SplitPoints); by default the thirds.
struct Pass {
    Anchor anchor;
    Columns columns;
};
inline Columns PassColumns(Anchor anchor, int32_t game_x0, int32_t game_x1, uint32_t target_width,
                           uint32_t target_height, float k, float area_aspect = 0.0f,
                           float cut_left = -1.0f / 3.0f, float cut_right = 1.0f / 3.0f) {
    const int32_t index = anchor == Anchor::Left ? 0 : anchor == Anchor::Centre ? 1 : 2;
    const float w = float(target_width);
    const float cuts[4] = {-1.0f, cut_left, cut_right, 1.0f};
    const auto edge = [&](int32_t i) {
        return int32_t(std::lround(Place(w * (cuts[i] + 1.0f) * 0.5f, w, float(target_height), k,
                                         anchor, area_aspect)));
    };
    const Columns game = Scissor(game_x0, game_x1, target_width, target_height, k, anchor,
                                 area_aspect);
    const Columns out{std::max(game.x0, edge(index)), std::min(game.x1, edge(index + 1))};
    return {out.x0, std::max(out.x0, out.x1)};
}
inline std::array<Pass, 3> Split(int32_t game_x0, int32_t game_x1, uint32_t target_width,
                                 uint32_t target_height, float k, float area_aspect = 0.0f,
                                 float cut_left = -1.0f / 3.0f, float cut_right = 1.0f / 3.0f) {
    const auto pass = [&](Anchor anchor) {
        return Pass{anchor, PassColumns(anchor, game_x0, game_x1, target_width, target_height, k,
                                        area_aspect, cut_left, cut_right)};
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
///   non-indexed ones too when `nonindexed` is set and the rasterizer finds an extent short of
///   the full width (Reject otherwise);
/// - after the copy: draws into a display buffer, until the next tonemap draw.
struct Tracker {
    uint64_t tonemap_ps = 0x484cdbf5ull;
    uint64_t composite_ps = 0x749bbec9ull;
    uint64_t frame_target = 0; ///< the tonemap draw's target
    bool over_frame = false;   ///< after the tonemap draw, before the copy
    bool armed = false;        ///< the copy was the last display draw
    bool after_copy = false;   ///< display draws after the copy
    bool last_over_frame = false; ///< the last Draw was a HUD draw over the frame (not a menu)
    bool nonindexed = false;   ///< non-indexed draws over the frame are candidates too (Reject)
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
            const bool hud = indexed || nonindexed;
            (hud ? over_frame_draws : over_frame_skipped)++;
            last_over_frame = hud;
            return hud;
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

    /// The last draw was a non-indexed candidate whose extent is unknown or full width: it is not
    /// placed after all (kept at the full width, counted as skipped).
    void Reject() {
        if (last_over_frame && over_frame_draws > 0) {
            --over_frame_draws;
            ++over_frame_skipped;
        }
        last_over_frame = false;
    }
};

/// GOW3_HUD_SELF_SCALE_VS: vertex shader hashes (hex, comma separated, low 32 bits) of the draws
/// that scale themselves (SelfScale) instead of being placed.
inline std::vector<uint32_t> SelfScaleFromEnv() {
    std::vector<uint32_t> list;
    if (const char* env = std::getenv("GOW3_HUD_SELF_SCALE_VS")) {
        for (const char* p = env; *p;) {
            char* end;
            const uint64_t value = std::strtoull(p, &end, 16);
            if (end == p) {
                break;
            }
            list.push_back(uint32_t(value));
            p = *end == ',' ? end + 1 : end;
        }
    }
    return list;
}

inline uint64_t TonemapFromEnv() {
    const char* env = std::getenv("GOW3_UI_TONEMAP_PS");
    return env && *env ? std::strtoull(env, nullptr, 16) : 0x484cdbf5ull;
}

/// GOW3_HUD_NONINDEXED: non-indexed draws over the frame are placed too when their extent is
/// known and short of the full width (default on; "0" turns it off).
inline bool NonIndexedFromEnv() {
    const char* env = std::getenv("GOW3_HUD_NONINDEXED");
    return !env || env[0] != '0';
}

inline uint64_t CompositeFromEnv() {
    const char* env = std::getenv("GOW3_HUD_COMPOSITE_PS");
    return env && *env ? std::strtoull(env, nullptr, 16) : 0x749bbec9ull;
}

} // namespace Vulkan::HudLayout
