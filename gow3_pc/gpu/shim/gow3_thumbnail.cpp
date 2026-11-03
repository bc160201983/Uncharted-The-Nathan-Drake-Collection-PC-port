// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_thumbnail.h"
#include "../gow3gpu.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <miniz.h>

namespace Gow3Thumbnail {
namespace {

using Clock = std::chrono::steady_clock;

std::mutex mutex;
std::vector<unsigned char> picture; // RGB8, Width x Height; empty: none yet
Clock::time_point picture_at{};     // when it was captured
Clock::time_point last_request{}, playing_since{};
unsigned last_calls = 0;
// gow3 3 Nov 2026: saves written while the game was not in play (a chapter's first autosave comes
// after its cutscene or loading, when the last picture still shows the end of the chapter before):
// they get the first picture taken once play has gone on for a while.
struct Pending {
    std::string path;
    Clock::time_point since;
};
std::vector<Pending> pending;
// gow3 3 Nov 2026: the camera hooks keep running behind the pause menu, so pictures go on being taken
// there. A save made from the save list (opened from that menu) gets the picture from the moment
// Options was last pressed instead, before the menu was on screen.
std::vector<unsigned char> before_pause;
Clock::time_point save_list_at{};

constexpr auto Fresh = std::chrono::seconds(3);       // a picture this recent shows the save's place
constexpr auto Settled = std::chrono::seconds(2);     // play this long after a break (fade-in over)
constexpr auto GiveUp = std::chrono::minutes(10);     // a waiting save keeps the game's picture

bool WritePng(const std::string& path, const std::vector<unsigned char>& rgb) {
    size_t size = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(rgb.data(), int(Width), int(Height), 3, &size, 6,
                                                           MZ_FALSE);
    if (!png) {
        return false;
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    const bool ok = f && std::fwrite(png, 1, size, f) == size;
    if (f) {
        std::fclose(f);
    }
    mz_free(png);
    if (!ok) {
        std::remove(path.c_str());
    }
    return ok;
}

} // namespace

bool Wanted() {
    // GOW3_SAVE_PICTURE=0: saves keep the game's own picture.
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_SAVE_PICTURE");
        return !(env && env[0] == '0');
    }();
    if (!enabled) {
        return false;
    }
    const auto now = Clock::now();
    if (now - last_request < std::chrono::seconds(1)) {
        return false;
    }
    const unsigned calls = gow3gpu_mouse_look_calls();
    if (calls == last_calls) {
        return false;
    }
    last_calls = calls;
    // A gap since the last request: play (the camera hooks) has just started again.
    if (now - last_request > std::chrono::seconds(2)) {
        std::scoped_lock lock{mutex};
        playing_since = now;
    }
    last_request = now;
    return true;
}

void Store(const unsigned char* rgba) {
    std::vector<unsigned char> rgb(size_t(Width) * Height * 3);
    for (size_t i = 0, n = size_t(Width) * Height; i < n; ++i) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    const auto now = Clock::now();
    std::vector<std::string> due;
    {
        std::scoped_lock lock{mutex};
        picture = rgb;
        picture_at = now;
        if (!pending.empty() && now - playing_since >= Settled) {
            for (const Pending& p : pending) {
                if (now - p.since < GiveUp) {
                    due.push_back(p.path);
                }
            }
            pending.clear();
        }
    }
    if (due.empty()) {
        return;
    }
    // Off the present thread: encoding takes a few milliseconds.
    std::thread([due = std::move(due), rgb = std::move(rgb)] {
        for (const std::string& path : due) {
            if (WritePng(path, rgb)) {
                std::printf("GPU: save picture written once play went on (%s)\n", path.c_str());
            }
        }
    }).detach();
}

} // namespace Gow3Thumbnail

extern "C" int gow3gpu_thumbnail_save(const char* path) {
    using namespace Gow3Thumbnail;
    if (!path || !*path) {
        return 0;
    }
    std::vector<unsigned char> rgb;
    {
        std::scoped_lock lock{mutex};
        const auto now = Clock::now();
        if (save_list_at != Clock::time_point{} && now - save_list_at < std::chrono::minutes(2) &&
            !before_pause.empty()) {
            // Kept for the two minutes: the game may write its profile save around the chosen one.
            rgb = before_pause;
        } else if (picture.empty() || now - picture_at > Fresh || now - playing_since < Settled) {
            // Not in play now: the save waits for the next picture (one entry per save).
            std::erase_if(pending, [&](const Pending& p) { return p.path == path; });
            if (pending.size() < 16) {
                pending.push_back({path, now});
            }
            return 2;
        } else {
            rgb = picture;
        }
    }
    return WritePng(path, rgb) ? 1 : 0;
}

extern "C" void gow3gpu_thumbnail_pause(void) {
    using namespace Gow3Thumbnail;
    std::scoped_lock lock{mutex};
    before_pause = picture;
}

namespace Gow3Thumbnail {
void NoteSaveList() {
    std::scoped_lock lock{mutex};
    save_list_at = Clock::now();
}
} // namespace Gow3Thumbnail
