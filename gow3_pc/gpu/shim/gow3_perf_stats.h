// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: numbers for the performance overlay (gow3_overlay.cpp). A thread started once samples
// CPU, RAM, GPU use and their temperatures (Windows counters, NVIDIA's driver) and VRAM
// (VK_EXT_memory_budget) once a second while the overlay shows one of them (20 Oct 2026;
// GOW3_PERF_SAMPLER_PAUSE=0: twice a second, always); a value that cannot be measured stays
// negative and shows as "n/a".
//
// 24 Oct 2026: GPU and CPU temperatures next to their use (nvml.dll for the graphics card, the
// ACPI thermal zone for the processor), and the graphics card shows as "GPU" instead of its
// full name so the counter fits one line.
//
// 25 Oct 2026: RAM is the system's memory in use, not this process's working set, and it is
// shown without the total ("RAM 11.2 GB") so the line stays short.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace Vulkan {
class Instance;
}

namespace Gow3PerfStats {

/// Overlay items (Gow3Settings::Values::overlay_items).
enum Item : unsigned {
    Fps = 1u << 0,
    FrameTime = 1u << 1,
    Upscaler = 1u << 2,
    Gpu = 1u << 3,
    Cpu = 1u << 4,
    Ram = 1u << 5,
    Vram = 1u << 6,
    AllItems = (1u << 7) - 1,
};
inline constexpr unsigned DefaultItems = Fps | FrameTime | Gpu | Cpu | Ram;

struct Sample {
    float fps = 0.0f, frame_ms = 0.0f;
    std::string upscaler;
    std::string gpu_name;
    float gpu_percent = -1.0f, cpu_percent = -1.0f;
    float gpu_temp_c = -1.0f, cpu_temp_c = -1.0f;
    // RAM is the whole system (not this process); VRAM is the card's memory budget.
    double ram_used_gb = -1.0, ram_total_gb = -1.0, vram_used_gb = -1.0, vram_total_gb = -1.0;
};

/// Process CPU time over wall time on every core, 0 to 100; -1 without a wall interval.
inline float CpuPercent(uint64_t process_delta, uint64_t wall_delta, unsigned cores) {
    if (!wall_delta || !cores) {
        return -1.0f;
    }
    return std::clamp(float(double(process_delta) * 100.0 / (double(wall_delta) * cores)), 0.0f, 100.0f);
}

/// The overlay text for the items in `mask`: one line each, or one line when `row`.
inline std::string Format(const Sample& s, unsigned mask, bool row) {
    std::string out;
    char part[160];
    const auto add = [&] {
        if (!out.empty()) {
            out += row ? "   " : "\n";
        }
        out += part;
    };
    const auto pair = [](char* to, size_t size, const char* label, double used, double total) {
        if (used < 0.0) {
            std::snprintf(to, size, "%s n/a", label);
        } else if (total < 0.0) {
            std::snprintf(to, size, "%s %.1f GB", label, used);
        } else {
            std::snprintf(to, size, "%s %.1f / %.1f GB", label, used, total);
        }
    };
    // "°C" as UTF-8; the overlay's DejaVu Sans font has it.
    constexpr const char* degree = "\xc2\xb0" "C";
    if (mask & Fps) {
        std::snprintf(part, sizeof(part), "%.0f FPS", s.fps);
        add();
    }
    if (mask & FrameTime) {
        std::snprintf(part, sizeof(part), "%.1f ms", s.frame_ms);
        add();
    }
    if (mask & Upscaler) {
        std::snprintf(part, sizeof(part), "%s", s.upscaler.empty() ? "Native" : s.upscaler.c_str());
        add();
    }
    // The card's name is not shown (24 Oct 2026): "GPU" keeps the one-line counter short.
    if (mask & Gpu) {
        if (s.gpu_percent < 0.0f) {
            std::snprintf(part, sizeof(part), "GPU n/a");
        } else if (s.gpu_temp_c < 0.0f) {
            std::snprintf(part, sizeof(part), "GPU %.0f%%", s.gpu_percent);
        } else {
            std::snprintf(part, sizeof(part), "GPU %.0f%% %.0f%s", s.gpu_percent, s.gpu_temp_c, degree);
        }
        add();
    }
    if (mask & Cpu) {
        if (s.cpu_percent < 0.0f) {
            std::snprintf(part, sizeof(part), "CPU n/a");
        } else if (s.cpu_temp_c < 0.0f) {
            std::snprintf(part, sizeof(part), "CPU %.0f%%", s.cpu_percent);
        } else {
            std::snprintf(part, sizeof(part), "CPU %.0f%% %.0f%s", s.cpu_percent, s.cpu_temp_c, degree);
        }
        add();
    }
    if (mask & Ram) {
        // The system's RAM in use, without the total (24 Oct 2026).
        pair(part, sizeof(part), "RAM", s.ram_used_gb, -1.0);
        add();
    }
    if (mask & Vram) {
        pair(part, sizeof(part), "VRAM", s.vram_used_gb, s.vram_total_gb);
        add();
    }
    return out;
}

/// Starts the sampling thread the first time. Every call also keeps it sampling: it sleeps
/// once no call came for 2 s (the overlay calls it on each frame that shows a sampled item).
void Start(const Vulkan::Instance& instance);
/// The latest CPU, RAM, GPU and VRAM values, with their temperatures (the caller fills FPS,
/// frametime and upscaler).
Sample Latest();

} // namespace Gow3PerfStats
