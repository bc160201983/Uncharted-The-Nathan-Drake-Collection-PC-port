// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_settings.h"
#include "gow3_orbs.h"
#include "gow3_graphics.h"
#include "../gow3gpu.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Gow3Settings {

namespace {

const char* Path() {
    const char* env = std::getenv("GOW3_CONFIG");
    return env && env[0] ? env : "gow3.ini";
}

float Clamp(float v, float lo, float hi) {
    return std::clamp(v, lo, hi);
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const float f = float(std::atof(value.c_str()));
    const int i = std::atoi(value.c_str());
    static const char* const overlay_keys[] = {"overlay_fps", "overlay_frametime", "overlay_upscaler",
                                               "overlay_gpu", "overlay_cpu", "overlay_ram", "overlay_vram"};
    for (unsigned item = 0; item < 7; ++item) {
        if (key == overlay_keys[item]) {
            const unsigned bit = 1u << item;
            v.overlay_items = i ? (v.overlay_items | bit) : (v.overlay_items & ~bit);
            return;
        }
    }
    if (key == "overlay_scale") {
        v.overlay_scale = std::clamp(i, 50, 300);
        return;
    }
    if (key == "overlay_corner") {
        v.overlay_corner = i >= 0 && i <= 3 ? i : 1;
        return;
    }
    if (key == "overlay_opacity") {
        v.overlay_opacity = std::clamp(i, 0, 100);
        return;
    }
    if (key == "overlay_layout") {
        v.overlay_layout = i == 1 ? 1 : 0;
        return;
    }
    if (key == "menu_tab") {
        v.menu_tab = i >= 0 && i <= 3 ? i : 0;
        return;
    }
    if (key == "pad_map" || key == "key_map" || key == "mouse_map") {
        std::scoped_lock lock{ControlsMutex()};
        (key == "pad_map" ? v.pad_map : key == "key_map" ? v.key_map : v.mouse_map) = value;
        return;
    }
    if (key == "menu_pad" || key == "menu_key") {
        if (!value.empty()) {
            std::scoped_lock lock{ControlsMutex()};
            (key == "menu_pad" ? v.menu_pad : v.menu_key) = value;
        }
        return;
    }
    if (key == "mouse" || key == "mouse_invert" || key == "mouse_invert_x") {
        (key == "mouse" ? v.mouse : key == "mouse_invert" ? v.mouse_invert : v.mouse_invert_x) = i != 0;
        return;
    }
    if (key == "mouse_aim_sens" || key == "mouse_vertical") {
        (key == "mouse_aim_sens" ? v.mouse_aim_sens : v.mouse_vertical) =
            f > 0.0f ? Clamp(f, 0.1f, 10.0f) : 1.0f;
        return;
    }
    if (key == "mouse_sens") {
        v.mouse_sens = f > 0.0f ? Clamp(f, 0.1f, 10.0f) : 1.0f;
        return;
    }
    if (key == "stick_speed") {
        v.stick_speed = f > 0.0f ? Clamp(f, 0.25f, 4.0f) : 1.0f;
        return;
    }
    if (key == "prompts") {
        v.prompts = ParsePrompts(value);
        return;
    }
    if (key == "fade_distance") {
        v.fade_distance = std::clamp(i, 100, 400);
        return;
    }
    if (key == "sun_shadow_distance" || key == "light_shadow_distance") {
        (key == "sun_shadow_distance" ? v.sun_shadow_distance : v.light_shadow_distance) = std::clamp(i, 100, 300);
        return;
    }
    if (key == "menu_shortcut") {
        v.menu_shortcut = value == "view_menu" ? 1 : value == "keyboard" ? 2 : value == "custom" ? 3 : 0;
        return;
    }
    const char* cheat_keys[] = {"cheat_health", "cheat_magic", "cheat_item", "cheat_rage", "cheat_orbs",
                                "cheat_very_hard", "cheat_double_jumps"};
    for (unsigned cheat = 0; cheat < GOW3_CHEAT_COUNT; ++cheat) {
        if (key == cheat_keys[cheat]) {
            v.cheats[cheat] = i != 0;
            return;
        }
    }
    if (key == "upscaler") {
        for (int u = 0; u < UpscalerCount; ++u) {
            if (value == UpscalerName(u)) {
                v.upscaler = u;
            }
        }
    } else if (key == "preset") {
        v.preset = std::clamp(i, 0, PresetCount - 1);
    } else if (key == "sharpen") {
        v.sharpen = i != 0;
    } else if (key == "sharpness") {
        v.sharpness = Clamp(f, 0.0f, 2.0f);
    } else if (key == "jitter") {
        v.jitter = i != 0;
    } else if (key == "reactive") {
        v.reactive = i != 0;
    } else if (key == "object_motion") {
        v.object_motion = i != 0;
    } else if (key == "reactive_scale") {
        v.reactive_scale = Clamp(f, 0.0f, 16.0f);
    } else if (key == "reactive_threshold") {
        v.reactive_threshold = Clamp(f, 0.0f, 1.0f);
    } else if (key == "reactive_max") {
        v.reactive_max = Clamp(f, 0.0f, 1.0f);
    } else if (key == "debug_view") {
        v.debug_view = std::clamp(i, 0, DebugViewCount - 1);
    } else if (key == "show_fps") {
        v.show_fps = i != 0;
    } else if (key == "display_mode") {
        v.display_mode = Gow3Graphics::ParseDisplayMode(value);
    } else if (key == "vsync") {
        v.vsync = i != 0;
    } else if (key == "fps_limit") {
        v.fps_limit = Gow3Graphics::ParseFpsLimit(i);
    } else if (key == "async_shaders") {
        v.async_shaders = i != 0;
    } else if (key == "fsr1") {
        v.fsr1 = i != 0;
    } else if (key == "rcas") {
        v.rcas = i != 0;
    } else if (key == "rcas_strength") {
        v.rcas_strength = Gow3Graphics::ParseRcasStrength(value.empty() ? -1 : i);
    } else if (key == "frames_queued") {
        v.frames_queued = Gow3Graphics::ParseFramesQueued(value.empty() ? -1 : i);
    } else if (key == "hdr") {
        v.hdr = i != 0;
    } else if (key == "hdr_peak_nits") {
        v.hdr_peak_nits = std::clamp(i, 400, 10000);
    } else if (key == "hdr_game_nits") {
        v.hdr_game_nits = std::clamp(i, 80, 500);
    } else if (key == "hdr_ui_nits") {
        v.hdr_ui_nits = std::clamp(i, 80, 500);
    } else if (key == "render_resolution") {
        v.render_resolution = Gow3Graphics::ParseResolution(value);
    } else if (key == "engine_fps") {
        v.engine_fps = Gow3Graphics::ParseEngineFps(i);
    } else if (key == "deferred_readback") {
        v.deferred_readback = i != 0;
    } else if (key == "stale_readback") {
        v.stale_readback = i != 0;
    } else if (key == "restart_unconfirmed") {
        v.restart_unconfirmed = i != 0;
    } else if (key == "enemy_health_bar") {
        v.enemy_health_bar = i != 0;
    } else if (key == "red_orb_multiplier") {
        v.red_orb_multiplier = Gow3Orbs::ClampMultiplier(f);
    } else if (key == "damage_dealt") {
        v.damage_dealt = Gow3Orbs::ClampMultiplier(f);
    } else if (key == "damage_taken") {
        v.damage_taken = Gow3Orbs::ClampMultiplier(f);
    } else if (key == "green_orb_multiplier") {
        v.green_orb_multiplier = Gow3Orbs::ClampMultiplier(f);
    } else if (key == "blue_orb_multiplier") {
        v.blue_orb_multiplier = Gow3Orbs::ClampMultiplier(f);
    } else if (key == "gold_orb_multiplier") {
        v.gold_orb_multiplier = Gow3Orbs::ClampMultiplier(f);
    } else if (key == "fsr4_auto_exposure") {
        v.fsr4_auto_exposure = i != 0;
    } else if (key == "fsr4_invert_jitter") {
        v.fsr4_invert_jitter = i != 0;
    } else if (key == "live_resolution") {
        v.live_resolution = value == "auto" ? -1 : std::clamp(i, 0, 1);
    } else if (key == "output_res") {
        for (int r = 0; r < OutputCount; ++r) {
            if (value == std::to_string(OutputWidths[r]) + "x" + std::to_string(OutputHeights[r])) {
                v.output_res = r;
            }
        }
    }
}

} // namespace

Values& Get() {
    static Values values;
    return values;
}

namespace {
/// The settings as the game last read or wrote them (see Save).
KeyValues synced;
} // namespace

std::mutex& ControlsMutex() {
    static std::mutex mutex;
    return mutex;
}

void ControlsChanged() {
    ++Get().controls_generation;
}

const char* PromptsName(int prompts) {
    switch (prompts) {
    case 0: return "playstation";
    case 1: return "keyboard";
    case 2: return "xbox";
    case 3: return "controller";
    default: return "auto";
    }
}

int ParsePrompts(const std::string& value) {
    // "1" and "0" are GOW3_PROMPTS's older values: keyboard, and the controller's own.
    if (value == "keyboard" || value == "1") return 1;
    if (value == "playstation") return 0;
    if (value == "xbox") return 2;
    if (value == "controller" || value == "gamepad" || value == "0") return 3;
    return -1;
}

void Load() {
    auto& v = Get();
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[4096]; // the control maps are long lines
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            Set(v, text.substr(0, eq), text.substr(eq + 1));
        }
        std::fclose(file);
        std::printf("Settings: %s\n", Path());
    }
    // Environment overrides (scripts, A/B tests).
    if (const char* env = std::getenv("GOW3_UPSCALER")) {
        v.upscaler = UpscalerOff;
        for (int u = 0; u < UpscalerCount; ++u) {
            if (std::strcmp(env, UpscalerName(u)) == 0) v.upscaler = u;
        }
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"GOW3_FSR_SHARPNESS", "sharpness"},        {"GOW3_JITTER", "jitter"},
        {"GOW3_REACTIVE", "reactive"},              {"GOW3_REACTIVE_SCALE", "reactive_scale"},
        {"GOW3_REACTIVE_THRESHOLD", "reactive_threshold"}, {"GOW3_REACTIVE_MAX", "reactive_max"},
        {"GOW3_UPSCALE_PRESET", "preset"},            {"GOW3_OBJECT_MOTION", "object_motion"},
        {"GOW3_ASYNC_SHADERS", "async_shaders"},      {"GOW3_DEFERRED_READBACK", "deferred_readback"},
        {"GOW3_STALE_READBACK", "stale_readback"},     {"GOW3_FSR1", "fsr1"},
        {"GOW3_RCAS", "rcas"},                        {"GOW3_FRAMES_AHEAD", "frames_queued"},
        {"GOW3_HDR_OUTPUT", "hdr"},
        {"GOW3_PAD_MAP", "pad_map"},                  {"GOW3_KEY_MAP", "key_map"},
        {"GOW3_MOUSE_MAP", "mouse_map"},              {"GOW3_MOUSE", "mouse"},
        {"GOW3_MOUSE_SENS", "mouse_sens"},            {"GOW3_MOUSE_INVERT", "mouse_invert"},
        {"GOW3_MOUSE_AIM_SENS", "mouse_aim_sens"},    {"GOW3_MOUSE_VERTICAL", "mouse_vertical"},
        {"GOW3_MOUSE_INVERT_X", "mouse_invert_x"},
        {"GOW3_PROMPTS", "prompts"},                  {"GOW3_STICK_SPEED", "stick_speed"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env); value && value[0]) {
            Set(v, key, value);
        }
    }
    v.startup_preset = v.preset;
    v.startup_upscaler = v.upscaler;
    v.startup_object_motion = v.object_motion;
    v.startup_output_res = v.output_res;
    v.startup_live_resolution = v.live_resolution;
    v.startup_render_resolution = v.render_resolution;
    v.startup_engine_fps = v.engine_fps;
    const char* applied = std::getenv("GOW3_ENGINE_FPS");
    v.applied_engine_fps = applied && applied[0] ? Gow3Graphics::ParseEngineFps(std::atoi(applied))
                                                 : v.engine_fps.load();
    v.startup_deferred_readback = v.deferred_readback;
    v.startup_stale_readback = v.stale_readback;
    synced = Current();
    ControlsChanged();
}

bool GraphicsNeedRestart() {
    const auto& v = Get();
    return v.deferred_readback != v.startup_deferred_readback ||
           v.stale_readback != v.startup_stale_readback;
}

void ConfigureUpscalerSupport(bool fsr4, bool fsr411) {
    auto& v = Get();
    v.fsr4_supported = fsr4;
    v.fsr411_supported = fsr4 && fsr411;
    const int requested = v.upscaler;
    if ((requested == UpscalerFsr4 && !v.fsr4_supported) ||
        (requested == UpscalerFsr411 && !v.fsr411_supported)) {
        v.fsr4_problem = "GPU does not support the selected FSR 4 shaders; using FSR 3.1";
        std::printf("Upscaler: %s unsupported on this GPU; falling back to FSR 3.1 before the first frame\n",
                    UpscalerName(requested));
        v.upscaler = UpscalerFsr3;
    }
}

void ConfigureDlssSupport(bool available, const char* problem) {
    auto& v = Get();
    v.dlss_supported = available;
    static std::string kept;
    kept = problem ? problem : "";
    v.dlss_problem = available || kept.empty() ? nullptr : kept.c_str();
    if (v.upscaler == UpscalerDlss && !available) {
        std::printf("Upscaler: DLSS unavailable (%s); falling back to FSR 3.1\n",
                    kept.empty() ? "gow3_dlss.dll or nvngx_dlss.dll missing" : kept.c_str());
        v.upscaler = UpscalerFsr3;
    }
}

bool FixedRenderSession() {
    const char* size = std::getenv("GOW3_RENDER_RES");
    return size && size[0];
}

int RenderPreset() {
    const auto& v = Get();
    return FixedRenderSession() ? v.startup_preset :
        v.upscaler == UpscalerTaa ? NativeAA : v.preset.load();
}

bool ResolutionNeedsRestart() {
    const auto& v = Get();
    // TAA needs the live path (native guest targets): run.sh selects it on restart.
    return FixedRenderSession() &&
        (v.preset != v.startup_preset || v.output_res != v.startup_output_res ||
         (v.upscaler == UpscalerOff) != (v.startup_upscaler == UpscalerOff) ||
         (v.upscaler == UpscalerTaa) != (v.startup_upscaler == UpscalerTaa));
}

namespace {

void Append(std::string& out, const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (n > 0) {
        out.append(buffer, std::min<size_t>(size_t(n), sizeof(buffer) - 1));
    }
}

const std::string* Find(const KeyValues& values, const std::string& key) {
    for (const auto& [k, value] : values) {
        if (k == key) return &value;
    }
    return nullptr;
}

KeyValues ReadFile() {
    KeyValues values;
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[4096];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (!text.empty() && text[0] != '#' && eq != std::string::npos) {
                values.emplace_back(text.substr(0, eq), text.substr(eq + 1));
            }
        }
        std::fclose(file);
    }
    return values;
}

} // namespace

KeyValues Current() {
    const auto& v = Get();
    std::string text;
    Append(text,
                 "upscaler=%s\npreset=%d\nsharpen=%d\nsharpness=%.2f\njitter=%d\n"
                 "reactive=%d\nobject_motion=%d\nreactive_scale=%.2f\nreactive_threshold=%.2f\nreactive_max=%.2f\n"
                 "debug_view=%d\nshow_fps=%d\nfsr4_auto_exposure=%d\nfsr4_invert_jitter=%d\n",
                 UpscalerName(v.upscaler), v.preset.load(), int(v.sharpen.load()),
                 v.sharpness.load(), int(v.jitter.load()), int(v.reactive.load()),
                 int(v.object_motion.load()),
                 v.reactive_scale.load(), v.reactive_threshold.load(), v.reactive_max.load(),
                 v.debug_view.load(), int(v.show_fps.load()),
                 int(v.fsr4_auto_exposure.load()), int(v.fsr4_invert_jitter.load()));
    Append(text, "output_res=%dx%d\n", OutputWidths[v.output_res], OutputHeights[v.output_res]);
    Append(text,
                 "overlay_fps=%d\noverlay_frametime=%d\noverlay_upscaler=%d\noverlay_gpu=%d\n"
                 "overlay_cpu=%d\noverlay_ram=%d\noverlay_vram=%d\noverlay_scale=%d\noverlay_corner=%d\n"
                 "overlay_opacity=%d\noverlay_layout=%d\nmenu_tab=%d\n",
                 int(v.overlay_items >> 0 & 1), int(v.overlay_items >> 1 & 1), int(v.overlay_items >> 2 & 1),
                 int(v.overlay_items >> 3 & 1), int(v.overlay_items >> 4 & 1), int(v.overlay_items >> 5 & 1),
                 int(v.overlay_items >> 6 & 1), v.overlay_scale.load(), v.overlay_corner.load(),
                 v.overlay_opacity.load(), v.overlay_layout.load(), v.menu_tab.load());
    Append(text,
                 "display_mode=%s\nvsync=%d\nfps_limit=%d\nasync_shaders=%d\n"
                 "fsr1=%d\nrcas=%d\nrcas_strength=%d\nframes_queued=%d\n"
                 "deferred_readback=%d\nstale_readback=%d\n"
                 "restart_unconfirmed=%d\n",
                 Gow3Graphics::DisplayModeKeys[std::clamp(v.display_mode.load(), 0, 2)].data(),
                 int(v.vsync.load()), v.fps_limit.load(), int(v.async_shaders.load()),
                 int(v.fsr1.load()), int(v.rcas.load()), v.rcas_strength.load(), v.frames_queued.load(),
                 int(v.deferred_readback.load()), int(v.stale_readback.load()),
                 int(v.restart_unconfirmed.load()));
    Append(text, "hdr=%d\nhdr_peak_nits=%d\nhdr_game_nits=%d\nhdr_ui_nits=%d\n", int(v.hdr.load()),
           v.hdr_peak_nits.load(), v.hdr_game_nits.load(), v.hdr_ui_nits.load());
    // Read by run.sh at start.
    Append(text, "live_resolution=%s\n", v.live_resolution < 0 ? "auto"
                                                  : v.live_resolution ? "1" : "0");
    static const char* const shortcuts[] = {"lt_r3", "view_menu", "keyboard", "custom"};
    Append(text, "menu_shortcut=%s\n", shortcuts[std::clamp(v.menu_shortcut.load(), 0, 3)]);
    Append(text, "fade_distance=%d\nsun_shadow_distance=%d\nlight_shadow_distance=%d\n", v.fade_distance.load(),
           v.sun_shadow_distance.load(), v.light_shadow_distance.load());
    Append(text, "mouse=%d\nmouse_sens=%.2f\nmouse_invert=%d\nprompts=%s\nstick_speed=%.2f\n",
           int(v.mouse.load()), v.mouse_sens.load(), int(v.mouse_invert.load()), PromptsName(v.prompts),
           v.stick_speed.load());
    Append(text, "mouse_aim_sens=%.2f\nmouse_vertical=%.2f\nmouse_invert_x=%d\n", v.mouse_aim_sens.load(),
           v.mouse_vertical.load(), int(v.mouse_invert_x.load()));
    {
        std::scoped_lock lock{ControlsMutex()};
        text += "pad_map=" + v.pad_map + "\nkey_map=" + v.key_map + "\nmouse_map=" + v.mouse_map + "\n";
        text += "menu_pad=" + v.menu_pad + "\nmenu_key=" + v.menu_key + "\n";
    }
    KeyValues values;
    for (size_t at = 0; at < text.size();) {
        const size_t line_end = text.find('\n', at);
        const std::string line = text.substr(at, line_end - at);
        const size_t eq = line.find('=');
        values.emplace_back(line.substr(0, eq), line.substr(eq + 1));
        at = line_end + 1;
    }
    return values;
}

void Save() {
    // Keys the in-game menu left alone since the game last read or wrote the file keep the
    // file's value: the launcher may have changed them while the game ran (it used to be
    // overwritten with the game's older value, e.g. a 165 FPS limit after choosing 60).
    const KeyValues ours = Current();
    const KeyValues file_values = ReadFile();
    std::string text = "# Uncharted settings (launcher and in-game menu: Insert / R3+L2)\n";
    for (const auto& [key, value] : ours) {
        const std::string* base = Find(synced, key);
        const std::string* theirs = Find(file_values, key);
        text += key + "=" + (base && *base == value && theirs ? *theirs : value) + "\n";
    }
    for (const auto& [key, value] : file_values) {
        if (!Find(ours, key)) {
            text += key + "=" + value + "\n";
        }
    }
    FILE* file = std::fopen(Path(), "w");
    if (!file) {
        std::printf("Settings: cannot write %s\n", Path());
        return;
    }
    std::fputs(text.c_str(), file);
    std::fclose(file);
    synced = ours;
}

float PresetScale(int preset) {
    static constexpr float scales[PresetCount] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    return scales[std::clamp(preset, 0, PresetCount - 1)];
}

const char* PresetName(int preset) {
    static constexpr const char* names[PresetCount] = {"Native AA", "Quality", "Balanced",
                                                       "Performance", "Ultra Performance"};
    return names[std::clamp(preset, 0, PresetCount - 1)];
}

const char* UpscalerName(int upscaler) {
    static constexpr const char* names[UpscalerCount] = {"off", "fsr3", "fsr4", "fsr411", "taa", "dlss"};
    return names[std::clamp(upscaler, 0, UpscalerCount - 1)];
}

} // namespace Gow3Settings

extern "C" unsigned gow3gpu_controls_generation(void) {
    return Gow3Settings::Get().controls_generation;
}

extern "C" void gow3gpu_controls_get(Gow3Controls* out) {
    const auto& v = Gow3Settings::Get();
    {
        std::scoped_lock lock{Gow3Settings::ControlsMutex()};
        std::snprintf(out->pad_map, sizeof(out->pad_map), "%s", v.pad_map.c_str());
        std::snprintf(out->key_map, sizeof(out->key_map), "%s", v.key_map.c_str());
        std::snprintf(out->mouse_map, sizeof(out->mouse_map), "%s", v.mouse_map.c_str());
    }
    out->mouse = v.mouse ? 1 : 0;
    out->mouse_invert = v.mouse_invert ? 1 : 0;
    out->prompts = v.prompts;
    out->mouse_sens = v.mouse_sens;
    out->stick_speed = v.stick_speed;
}

extern "C" float gow3gpu_controls_mouse_sens(void) {
    return Gow3Settings::Get().mouse_sens;
}

extern "C" int gow3gpu_controls_mouse_invert(void) {
    return Gow3Settings::Get().mouse_invert ? 1 : 0;
}

extern "C" float gow3gpu_controls_mouse_aim_sens(void) {
    return Gow3Settings::Get().mouse_aim_sens;
}

extern "C" float gow3gpu_controls_mouse_vertical(void) {
    return Gow3Settings::Get().mouse_vertical;
}

extern "C" int gow3gpu_controls_mouse_invert_x(void) {
    return Gow3Settings::Get().mouse_invert_x ? 1 : 0;
}

extern "C" float gow3gpu_controls_stick_speed(void) {
    return Gow3Settings::Get().stick_speed;
}

extern "C" int gow3gpu_settings_fade_distance(void) {
    return Gow3Settings::Get().fade_distance;
}

extern "C" int gow3gpu_settings_sun_shadow_distance(void) {
    return Gow3Settings::Get().sun_shadow_distance;
}

extern "C" int gow3gpu_settings_light_shadow_distance(void) {
    return Gow3Settings::Get().light_shadow_distance;
}
