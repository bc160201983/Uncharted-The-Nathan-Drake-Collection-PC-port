// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: user settings changed at run time from the in-game menu (gow3_overlay.h) and kept
// in gow3.ini (GOW3_CONFIG overrides the path). Environment variables override the file at
// start. Readers load the atomics every frame; writers are the menu and Load().

#pragma once

#include "../../src/cheat_hook.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Gow3Settings {

enum Upscaler : int { UpscalerOff = 0, UpscalerFsr3 = 1, UpscalerFsr4 = 2, UpscalerFsr411 = 3,
                      UpscalerTaa = 4, UpscalerDlss = 5, UpscalerCount };
/// FSR 4 v07 or FSR 4.1.1: the same inputs, settings and placement in the frame.
inline bool IsFsr4(int upscaler) {
    return upscaler == UpscalerFsr4 || upscaler == UpscalerFsr411;
}
enum Preset : int { NativeAA = 0, Quality, Balanced, Performance, UltraPerformance, PresetCount };
enum DebugView : int { DebugNone = 0, DebugReactive = 1, DebugMotion = 2, DebugViewCount };

/// Live output resolutions: the upscaler's output and the UI host targets.
inline constexpr int OutputWidths[] = {1280, 1920, 2560, 3840};
inline constexpr int OutputHeights[] = {720, 1080, 1440, 2160};
inline constexpr int OutputCount = 4;
inline constexpr int OutputDefault = 1; ///< 1920x1080, the game's own size

struct Values {
    std::atomic<int> upscaler{UpscalerFsr3};
    std::atomic<int> preset{NativeAA};
    std::atomic<bool> sharpen{true};
    std::atomic<float> sharpness{0.3f};
    std::atomic<bool> jitter{true};
    std::atomic<bool> reactive{false};
    std::atomic<bool> object_motion{true};
    std::atomic<float> reactive_scale{1.0f};
    std::atomic<float> reactive_threshold{0.2f};
    std::atomic<float> reactive_max{0.9f};
    std::atomic<int> debug_view{DebugNone};
    std::atomic<bool> show_fps{false};
    /// Performance overlay (gow3_perf_stats.h): items shown, font size and background opacity
    /// in percent, corner (0 top left, 1 top right, 2 bottom left, 3 bottom right), layout
    /// (0 one line, 1 one item per line). menu_tab: the menu's last tab.
    std::atomic<unsigned> overlay_items{7};
    std::atomic<int> overlay_scale{100}, overlay_corner{1}, overlay_opacity{50}, overlay_layout{0};
    std::atomic<int> menu_tab{0};
    std::atomic<float> red_orb_multiplier{1.0f};
    std::atomic<bool> red_orbs_supported{false};
    /// Damage and orb pickup multipliers (src/damage_hook.h, src/orb_hook.h), 0.1x to 100x.
    /// The game's code reads these floats directly.
    std::atomic<float> damage_dealt{1.0f}, damage_taken{1.0f};
    std::atomic<float> green_orb_multiplier{1.0f}, blue_orb_multiplier{1.0f}, gold_orb_multiplier{1.0f};
    std::atomic<bool> damage_supported{false}, orb_pickup_supported{false};
    std::atomic<bool> cheats[GOW3_CHEAT_COUNT]{};
    std::atomic<uint32_t> cheats_supported{0};
    /// Damage hook (src/actor_hook.h): enemy health bar, off by default.
    std::atomic<bool> enemy_health_bar{false};
    std::atomic<bool> actor_watch_supported{false};
    // Graphics (gow3_graphics.h). Live: applied on the next frame.
    std::atomic<int> display_mode{0};
    std::atomic<bool> vsync{true};
    std::atomic<int> fps_limit{60};
    std::atomic<bool> async_shaders{true};
    std::atomic<bool> fsr1{false};
    std::atomic<bool> rcas{true};
    std::atomic<int> rcas_strength{75};
    /// Set by the presenter: sharpening ran on the last frame (it needs FSR 1 when the game's
    /// image is smaller than the window).
    std::atomic<bool> rcas_applied{false};
    /// Set by the presenter: the game's image and the window it is shown in, in pixels.
    std::atomic<int> image_width{0}, image_height{0}, window_width{0}, window_height{0};
    std::atomic<int> frames_queued{1};
    /// HDR10 output (Rec. 2020 PQ swapchain), live. Brightness in nits: the peak the image may
    /// reach, the game's diffuse white (SDR 1.0) and the HUD/menus.
    std::atomic<bool> hdr{false};
    std::atomic<int> hdr_peak_nits{1000};
    std::atomic<int> hdr_game_nits{203};
    std::atomic<int> hdr_ui_nits{203};
    /// Set by the presenter: the display accepts HDR10 (the swapchain offers Rec. 2020 PQ).
    std::atomic<bool> hdr_supported{false};
    // Startup: run.py turns them into patches and environment variables; "Apply and restart".
    std::atomic<int> render_resolution{0};
    std::atomic<int> engine_fps{120};
    std::atomic<bool> deferred_readback{true};
    std::atomic<bool> stale_readback{true};
    /// Set before an "Apply and restart"; cleared once the new launch shows the game. run.py
    /// restores the previous startup options when a launch never cleared it.
    std::atomic<bool> restart_unconfirmed{false};
    int startup_render_resolution = 0, startup_engine_fps = 120;
    /// The engine rate this launch really runs at (run.py's GOW3_ENGINE_FPS): 60 when the frame
    /// rate patch does not apply to this game build.
    std::atomic<int> applied_engine_fps{120};
    bool startup_deferred_readback = true, startup_stale_readback = true;
    // FSR 4 checks (menu): the provider's auto exposure, the jitter sign it is given.
    std::atomic<bool> fsr4_auto_exposure{true};
    std::atomic<bool> fsr4_invert_jitter{false};
    std::atomic<int> active_render_width{1920}, active_render_height{1080};
    std::atomic<int> output_res{OutputDefault}; ///< index into OutputWidths
    /// Live resolution and preset changes (run.sh): 0 off by default (startup patch, fastest
    /// on the Steam Deck and older GPUs), -1 auto (strong discrete GPUs), 1 on. On restart.
    std::atomic<int> live_resolution{0};
    /// Why FSR 4 cannot run (assets, device features), or null. Set by the renderer.
    std::atomic<const char*> fsr4_problem{nullptr};
    std::atomic<bool> fsr4_supported{false}, fsr411_supported{false};
    /// DLSS (gow3_dlss.dll, NVIDIA RTX) is ready, or why not (null before the device exists).
    std::atomic<bool> dlss_supported{false};
    std::atomic<const char*> dlss_problem{nullptr};
    /// Set by the renderer: temporal anti-aliasing can run in this game (Uncharted: the camera
    /// from the motion blur's velocity pass; off with UNCHARTED_TEMPORAL_AA=0).
    std::atomic<bool> temporal_aa_available{false};

    /// Controls (gow3gpu.h Gow3Controls; the menu's Controls tab). The maps are guarded by
    /// ControlsMutex(); a change calls ControlsChanged() so the pad reapplies them.
    std::string pad_map, key_map, mouse_map;
    std::atomic<bool> mouse{true}, mouse_invert{false}, mouse_invert_x{false};
    std::atomic<float> mouse_sens{1.0f}, stick_speed{1.0f};
    /// gow3 20 Oct 2026: mouse sensitivity while aiming (times mouse_sens) and up/down speed
    /// compared with left/right.
    std::atomic<float> mouse_aim_sens{1.0f}, mouse_vertical{1.0f};
    std::atomic<int> prompts{-1};
    std::atomic<unsigned> controls_generation{0};
    /// The controller shortcut for the in-game menu (GitHub #15: LT + R3 is also the sniper
    /// zoom): 0 LT + right stick click, 1 View + Menu together, 2 none (Insert on the keyboard).
    std::atomic<int> menu_shortcut{0};
    /// gow3 18 Oct (GitHub #15, #19): menu_shortcut 3 = this custom controller shortcut, one or
    /// two SDL gamepad button names ("lefttrigger"/"righttrigger" for the triggers) joined by
    /// '+'; and the keyboard key that opens the menu (SDL key name). Guarded by ControlsMutex().
    std::string menu_pad = "lefttrigger+rightstick", menu_key = "Insert";
    /// The games' own draw distances in percent (Uncharted 2 and 3, src/runtime_gamesettings.c):
    /// object fade distance, sun and point/spot light shadow distances.
    std::atomic<int> fade_distance{100}, sun_shadow_distance{100}, light_shadow_distance{100};

    /// Startup settings for the explicit GOW3_RENDER_RES compatibility patch only.
    int startup_preset = NativeAA;
    int startup_upscaler = UpscalerFsr3;
    bool startup_object_motion = true;
    int startup_output_res = OutputDefault;
    int startup_live_resolution = 0;
};

Values& Get();

/// Guards the control maps (pad_map, key_map, mouse_map).
std::mutex& ControlsMutex();
/// After a change to the controls: the pad reapplies them on its next read.
void ControlsChanged();
/// The prompts setting as the file holds it ("auto", "keyboard", ...) and parsed (-1 auto).
const char* PromptsName(int prompts);
int ParsePrompts(const std::string& value);

/// Reads the file, then the environment overrides. Called once at start.
void Load();
/// Checks the loaded choice before the first frame; unsupported FSR 4 uses FSR 3.1.
void ConfigureUpscalerSupport(bool fsr4, bool fsr411);
/// After device creation: DLSS availability; a DLSS setting falls back to FSR 3.1 without it.
void ConfigureDlssSupport(bool available, const char* problem);
/// Startup-patched scene dimensions cannot change until run.sh prepares a new image.
bool FixedRenderSession();
int RenderPreset();
bool ResolutionNeedsRestart();
/// A startup graphics option differs from the one this launch started with.
bool GraphicsNeedRestart();
/// Writes the file (menu changes).
/// Key and value lines as the settings file holds them.
using KeyValues = std::vector<std::pair<std::string, std::string>>;
/// The current settings as file lines.
KeyValues Current();
void Save();

/// Render resolution divisor of a preset (1.0 native, 1.5 quality, ...).
float PresetScale(int preset);
const char* PresetName(int preset);
const char* UpscalerName(int upscaler);

} // namespace Gow3Settings
