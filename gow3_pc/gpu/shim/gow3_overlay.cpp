// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_overlay.h"

#include <algorithm>
#include <array>
#include <bit>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>
#include "gow3_settings.h"
#include "gow3_trophies.h"
#include "core/emulator_settings.h"
#include "gow3_graphics.h"
#include "gow3_perf_stats.h"

extern "C" void runtime_restart(void); // src/probe.c: starts run.py again, ends this process
extern "C" int runtime_gamesettings_active(void); // src/runtime_gamesettings.c
// src/runtime_pad.c: the complete maps (every button listed) the given maps give on the defaults.
extern "C" void runtime_pad_effective_maps(const char* pad_spec, const char* key_spec, const char* mouse_spec,
                                           char* pad_out, char* key_out, char* mouse_out, size_t size);
#include "common/elf_info.h"
#include "imgui.h"
#include "imgui_internal.h" // RegisterUserTexture (save pictures)
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global gow3_font_ttf\n"
    "gow3_font_ttf:\n"
    ".incbin \"" GOW3_FONT_PATH "\"\n"
    ".global gow3_font_ttf_end\n"
    "gow3_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden gow3_font_ttf\n"
    ".global gow3_font_ttf\n"
    "gow3_font_ttf:\n"
    ".incbin \"" GOW3_FONT_PATH "\"\n"
    ".hidden gow3_font_ttf_end\n"
    ".global gow3_font_ttf_end\n"
    "gow3_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char gow3_font_ttf[];
extern "C" const unsigned char gow3_font_ttf_end[];

namespace Gow3Overlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
// gow3 18 Oct (GitHub #15, #19): the controller buttons held now (bit = SDL gamepad button; the
// triggers, pressed past half, as the two top bits). The menu opens when every button of its
// shortcut is held: LT + right stick click, View + Menu, or the player's own one or two buttons.
constexpr u32 LeftTriggerBit = 1u << 30, RightTriggerBit = 1u << 31;
u32 pad_held = 0;

u32 PadTokenBit(std::string_view token) {
    if (token == "lefttrigger") return LeftTriggerBit;
    if (token == "righttrigger") return RightTriggerBit;
    const SDL_GamepadButton button = SDL_GetGamepadButtonFromString(std::string(token).c_str());
    return button > SDL_GAMEPAD_BUTTON_INVALID && button < 30 ? 1u << button : 0;
}

/// The custom shortcut's buttons ("a+b" -> mask).
u32 PadComboMask(const std::string& combo) {
    u32 mask = 0;
    for (size_t at = 0; at <= combo.size();) {
        const size_t end = std::min(combo.find('+', at), combo.size());
        mask |= PadTokenBit(std::string_view(combo).substr(at, end - at));
        at = end + 1;
    }
    return mask;
}

/// The buttons of the selected shortcut, 0 for none.
u32 MenuPadMask() {
    switch (Gow3Settings::Get().menu_shortcut) {
    case 0: return LeftTriggerBit | (1u << SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    case 1: return (1u << SDL_GAMEPAD_BUTTON_BACK) | (1u << SDL_GAMEPAD_BUTTON_START);
    case 3: {
        std::scoped_lock lock{Gow3Settings::ControlsMutex()};
        return PadComboMask(Gow3Settings::Get().menu_pad);
    }
    default: return 0;
    }
}

/// The keyboard key that opens the menu (Insert unless changed).
SDL_Keycode MenuKey() {
    std::string name;
    {
        std::scoped_lock lock{Gow3Settings::ControlsMutex()};
        name = Gow3Settings::Get().menu_key;
    }
    const SDL_Keycode key = SDL_GetKeyFromName(name.c_str());
    return key != SDLK_UNKNOWN ? key : SDLK_INSERT;
}

const char* PadLabel(const std::string& name, bool playstation);
bool PlayStationController();

/// "LT + RS" (Xbox) or "L2 + R3" (PlayStation) for a shortcut mask.
std::string PadComboLabel(u32 mask, bool playstation) {
    std::string text;
    const auto add = [&](const std::string& part) { text += (text.empty() ? "" : " + ") + part; };
    if (mask & LeftTriggerBit) add(playstation ? "L2" : "LT");
    if (mask & RightTriggerBit) add(playstation ? "R2" : "RT");
    for (int b = 0; b < 30; ++b) {
        if (mask & (1u << b)) {
            if (const char* name = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(b))) {
                add(PadLabel(name, playstation));
            }
        }
    }
    return text;
}

// The menu's shortcuts as the window title shows them (Gow3Settings::menu_shortcut, menu_key).
std::string ShortcutText() {
    std::string text = SDL_GetKeyName(MenuKey());
    if (const u32 mask = MenuPadMask()) {
        text += " / " + PadComboLabel(mask, PlayStationController());
    }
    return text;
}
std::atomic<bool> loading{false};
std::atomic<u32> loading_done{0}, loading_total{0};
std::atomic<u32> background_done{0}, background_total{0};
bool dirty = false; // settings changed while open: saved on close
int requested_tab = -1; // switch on the next frame (L1/R1, Q/E, menu opened)
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
bool text_entry_active = false;
std::string text_entry_prompt, text_entry_text;
// The game's save list (sceSaveDataDialog): a choice made with the arrows/D-pad.
std::atomic<bool> choice_active{false};
std::string choice_title;
std::vector<std::string> choice_items;
int choice_index = 0;
std::atomic<int> choice_result{-1}; // -1 open, -2 cancelled, else the chosen index
// Save pictures: decoded by BeginChoice, made textures on the present thread (null = none).
// Closed lists' textures wait in choice_retired until the backend has destroyed them.
std::vector<Gow3Png::Image> choice_pictures;
std::vector<ImTextureData*> choice_textures, choice_retired;

// Trophy popups (ShowTrophy) and the Trophies tab's pictures, guarded by imgui_mutex. A picture
// becomes a texture before the next frame (UpdateTrophyTextures); retired ones are freed there.
struct TrophyPopupItem {
    Gow3Png::Image picture;
    ImTextureData* tex = nullptr;
    std::string title, text;
    std::chrono::steady_clock::time_point shown{};
};
std::deque<TrophyPopupItem> trophy_popups;
std::vector<ImTextureData*> trophy_retired;
std::map<int, ImTextureData*> trophy_tab_textures; // by trophy id, of trophy_tab_list
int trophy_tab_list = 0;
bool trophy_tab_shown = false; // drawn this frame: its missing pictures load before the next
float fps_top_right_height = 0.0f; // the FPS counter in the top right corner: popups go below it
std::atomic<bool> choice_retiring{false};
std::atomic<bool> trophy_showing{false}; // a popup is up or its texture still to free (Visible)
float base_scale = 1.0f;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

void CancelCapture();
void ReloadControls();

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    ImGui::GetIO().MouseDrawCursor = value;
    CancelCapture();
    if (value) {
        requested_tab = std::clamp(Gow3Settings::Get().menu_tab.load(), 0, 4);
        ReloadControls();
    }
    if (!value && dirty) {
        dirty = false;
        Gow3Settings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_Q: return ImGuiKey_Q;
    case SDLK_E: return ImGuiKey_E;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    const bool changed = ImGui::Checkbox(label, &v);
    Store(value, v, changed);
}

// A combo over `count` labels storing an index or a value from `values`.
void Choice(const char* label, std::atomic<int>& target, const char* const* labels, const int* values,
            int count) {
    int current = 0;
    for (int i = 0; i < count; ++i) {
        if ((values ? values[i] : i) == target.load()) current = i;
    }
    if (ImGui::Combo(label, &current, labels, count)) {
        Store(target, values ? values[current] : current, true);
    }
}

// The note for the item under the mouse or the gamepad/keyboard focus, shown in the footer.
const char* footer_help = nullptr;

void Help(const char* text) {
    if (text && (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || ImGui::IsItemFocused())) {
        footer_help = text;
    }
}

// Two-column rows: the name on the left, the control on the right.
bool BeginRows(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) {
        return false;
    }
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.52f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.48f);
    return true;
}

void Row(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(label);
}

void CheckRow(const char* label, std::atomic<bool>& value, const char* help = nullptr) {
    Row(label);
    Checkbox("##value", value);
    Help(help);
    ImGui::PopID();
}

void ChoiceRow(const char* label, std::atomic<int>& target, const char* const* labels, const int* values,
               int count, const char* help = nullptr) {
    Row(label);
    Choice("##value", target, labels, values, count);
    Help(help);
    ImGui::PopID();
}

void InfoRow(const char* label, const char* text) {
    Row(label);
    ImGui::TextDisabled("%s", text);
    ImGui::PopID();
}

void SliderRow(const char* label, std::atomic<int>& value, int lo, int hi, const char* format, const char* help) {
    Row(label);
    int v = value;
    if (ImGui::SliderInt("##value", &v, lo, hi, format, ImGuiSliderFlags_AlwaysClamp)) {
        Store(value, std::clamp(v, lo, hi), true);
    }
    Help(help);
    ImGui::PopID();
}

const char* UpscalerLabel(int upscaler) {
    switch (upscaler) {
    case Gow3Settings::UpscalerFsr3: return "FSR 3.1";
    case Gow3Settings::UpscalerFsr4: return "FSR 4";
    case Gow3Settings::UpscalerFsr411: return "FSR 4.1.1";
    case Gow3Settings::UpscalerTaa: return "TAA";
    case Gow3Settings::UpscalerDlss: return "DLSS";
    default: return "";
    }
}

const Vulkan::Instance* overlay_instance = nullptr; // for the overlay's measurements

// The performance overlay's text with the current settings (also the menu's preview).
std::string OverlayText() {
    namespace P = Gow3PerfStats;
    const auto& s = Gow3Settings::Get();
    const unsigned items = s.overlay_items;
    if ((items & (P::Gpu | P::Cpu | P::Ram | P::Vram)) && overlay_instance) {
        P::Start(*overlay_instance);
    }
    P::Sample sample = P::Latest();
    sample.fps = frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f;
    sample.frame_ms = frame_ms_avg;
    sample.upscaler = UpscalerLabel(s.upscaler);
    return P::Format(sample, items, s.overlay_layout == 0);
}

void ImageTab() {
    namespace G = Gow3Graphics;
    auto& s = Gow3Settings::Get();
    ImGui::SeparatorText("Display");
    if (BeginRows("##display")) {
        ChoiceRow("Display mode", s.display_mode, G::DisplayModeNames.data(), nullptr, G::DisplayModeCount,
                  "Windowed, borderless window or exclusive fullscreen.");
        CheckRow("VSync", s.vsync, "Waits for the monitor: no tearing, a little more input delay.");
        ImGui::BeginDisabled(!s.hdr_supported);
        CheckRow("HDR", s.hdr,
                 s.hdr_supported ? "HDR10 output. Turn on HDR in Windows display settings first."
                                 : "Needs a display with HDR turned on in Windows display settings.");
        ImGui::BeginDisabled(!s.hdr);
        SliderRow("Peak brightness", s.hdr_peak_nits, 400, 4000, "%d nits",
                  "The brightest highlights; set it to your display's peak.");
        SliderRow("Game brightness", s.hdr_game_nits, 80, 500, "%d nits",
                  "The brightness of white surfaces in the game (203 = the HDR reference).");
        SliderRow("HUD brightness", s.hdr_ui_nits, 80, 500, "%d nits",
                  "The brightness of the HUD, menus and this overlay.");
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    const int iw = s.image_width, ih = s.image_height, ww = s.window_width, wh = s.window_height;
    const bool upscaling = iw < ww && ih < wh;
    const bool downscaling = iw > ww || ih > wh;
    const bool rcas_blocked = s.rcas && !s.rcas_applied;
    if (s.temporal_aa_available) {
        // Temporal anti-aliasing at the game's render size (Native AA; the launcher's Render
        // resolution sets the size). Applied on the next frame.
        ImGui::SeparatorText("Anti-aliasing");
        if (BeginRows("##aa")) {
            const char* labels[5];
            int values[5];
            int count = 0;
            labels[count] = "Off (the game's own)";
            values[count++] = Gow3Settings::UpscalerOff;
            labels[count] = "TAA";
            values[count++] = Gow3Settings::UpscalerTaa;
            labels[count] = "AMD FSR 3.1 Native AA";
            values[count++] = Gow3Settings::UpscalerFsr3;
            if (s.fsr4_supported) {
                labels[count] = "AMD FSR 4 Native AA";
                values[count++] = Gow3Settings::UpscalerFsr4;
            }
            if (s.dlss_supported) {
                labels[count] = "NVIDIA DLAA";
                values[count++] = Gow3Settings::UpscalerDlss;
            }
            ChoiceRow("Anti-aliasing", s.upscaler, labels, values, count,
                      "Smooths jagged and flickering edges using earlier frames. TAA is the lightest; FSR "
                      "and DLAA keep more detail. Costs a little GPU time.");
            if (!s.dlss_supported) {
                const char* problem = s.dlss_problem.load();
                InfoRow("NVIDIA DLAA", problem ? problem : "needs an NVIDIA RTX graphics card");
            } else {
                // NVIDIA's DLSS license asks for its attribution where DLSS is offered.
                InfoRow("NVIDIA DLAA", "uses NVIDIA DLSS");
                Help("NVIDIA, RTX and DLSS are trademarks of NVIDIA Corporation.");
            }
            const int upscaler = s.upscaler;
            const bool sharpens = upscaler == Gow3Settings::UpscalerFsr3 ||
                                  Gow3Settings::IsFsr4(upscaler) ||
                                  upscaler == Gow3Settings::UpscalerDlss;
            ImGui::BeginDisabled(!sharpens);
            CheckRow("Anti-aliasing sharpening", s.sharpen,
                     "Sharpens the anti-aliased image (FSR and DLAA; TAA has none).");
            ImGui::BeginDisabled(!s.sharpen);
            Row("Anti-aliasing sharpness");
            int sharpness = int(std::lround(s.sharpness.load() * 100.0f));
            const bool moved =
                ImGui::SliderInt("##value", &sharpness, 0, 200, "%d%%", ImGuiSliderFlags_AlwaysClamp);
            Store(s.sharpness, std::clamp(sharpness, 0, 200) / 100.0f, moved);
            Help("How strong the sharpening is (30% by default).");
            ImGui::PopID();
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::EndTable();
        }
    }
    ImGui::SeparatorText("Scaling and sharpening");
    if (BeginRows("##scaling")) {
        CheckRow("Upscale with FSR 1", s.fsr1,
                 "Upscales the game's image: before the HUD with the launcher's Upscaling (FSR 1), else a "
                 "smaller image to the window.");
        ImGui::BeginDisabled(rcas_blocked);
        CheckRow("Sharpening (RCAS)", s.rcas,
                 rcas_blocked && downscaling ? "No effect: the game's image is larger than the window."
                 : rcas_blocked              ? "Needs FSR 1: the game's image is smaller than the window."
                                             : "Sharpens the final image.");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!s.rcas || rcas_blocked);
        SliderRow("Sharpening strength", s.rcas_strength, 0, 100, "%d%%", "How strong the sharpening is.");
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Draw distance (experimental)");
    if (BeginRows("##draw_distance")) {
        if (!runtime_gamesettings_active()) {
            InfoRow("Draw distances", "Uncharted 2 and 3 only (found once a game has started)");
        }
        ImGui::BeginDisabled(!runtime_gamesettings_active());
        SliderRow("Object fade distance", s.fade_distance, 100, 400, "%d%%",
                  "How far away objects and details fade in (100% = the game's own). Higher costs some FPS.");
        SliderRow("Sun shadow distance", s.sun_shadow_distance, 100, 300, "%d%%",
                  "How far the sun's shadows reach (100% = the game's 40 m).");
        SliderRow("Light shadow distance", s.light_shadow_distance, 100, 300, "%d%%",
                  "How far shadows of lamps and torches reach (100% = the game's 40 m).");
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Information");
    if (BeginRows("##image_info")) {
        char text[96];
        std::snprintf(text, sizeof(text), "%d x %d", iw, ih);
        InfoRow("Game image", text);
        std::snprintf(text, sizeof(text), "%d x %d", ww, wh);
        InfoRow("Window", text);
        // The launcher's Upscaling draws the game below the output size (GOW3_RENDER_RES): FSR 1
        // upscales it before the HUD (TemporalUpscaler::OnComposite) unless an upscaler runs.
        const char* scaled = std::getenv("GOW3_RENDER_RES");
        InfoRow("FSR 1", !s.fsr1 ? "off"
                         : scaled && *scaled ? "upscales the game before the HUD"
                         : upscaling ? "active"
                                     : "no effect (image not smaller than window)");
        InfoRow("Sharpening", s.rcas_applied ? "applied" : "not applied");
        ImGui::EndTable();
    }
}

void PerformanceTab() {
    namespace G = Gow3Graphics;
    auto& s = Gow3Settings::Get();
    ImGui::SeparatorText("Live");
    if (BeginRows("##live")) {
        static const char* const fps_labels[] = {"30", "40", "45", "50", "55", "60", "75", "90", "100", "120"};
        static_assert(std::size(fps_labels) == G::FpsLimits.size());
        ChoiceRow("Frame rate limit", s.fps_limit, fps_labels, G::FpsLimits.data(), int(G::FpsLimits.size()),
                  "Caps the frame rate, live. 60 is the PS4's; above 120 the game's movement breaks. With VSync, "
                  "a limit at or above the display's refresh rate makes the game follow the display (75 on a 75 Hz "
                  "monitor): the steadiest frame times. 40-55 are for tests.");
        // At a 60 Hz vblank the game waits for the next vblank: above 60 FPS needs the faster
        // vblank, which the launcher sets at start when its limit is above 60.
        if (EmulatorSettings.GetVblankFrequency() <= 60 && (s.fps_limit == 0 || s.fps_limit > 60)) {
            InfoRow("Effective limit", "60 FPS until the game is started with 120");
        }
        static const char* const queued_labels[] = {"1 (recommended)", "2 (faster; U2 NPCs float)", "Unlimited"};
        ChoiceRow("Frames queued", s.frames_queued, queued_labels, G::FramesQueued.data(),
                  int(G::FramesQueued.size()), "More frames queued: steadier FPS, slower response to input.");
        CheckRow("Compile new shaders in the background", s.async_shaders,
                 "Fewer stutters in new areas; an object or effect may appear a moment later.");
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Applied on restart");
    if (BeginRows("##restart")) {
        CheckRow("Game reads GPU data without waiting", s.stale_readback,
                 "Big FPS gain. Switch it off if lighting or objects look wrong.");
        CheckRow("Deferred GPU readbacks", s.deferred_readback,
                 "Fewer waits for the GPU. Switch it off if textures or shadows look wrong.");
        ImGui::EndTable();
    }
}

void OverlayTab() {
    namespace P = Gow3PerfStats;
    auto& s = Gow3Settings::Get();
    if (BeginRows("##overlay_on")) {
        CheckRow("Show overlay", s.show_fps, "The performance overlay in a corner of the screen.");
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(!s.show_fps);
    ImGui::SeparatorText("What to show");
    if (ImGui::BeginTable("##overlay_items", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableNextColumn();
        static const char* const names[] = {"FPS", "Frametime", "Upscaler", "GPU (name and use)", "CPU (game)",
                                            "RAM (game / total)", "VRAM (used / available)"};
        for (unsigned item = 0; item < 7; ++item) {
            const unsigned bit = 1u << item;
            bool on = (s.overlay_items & bit) != 0;
            if (ImGui::Checkbox(names[item], &on)) {
                s.overlay_items = on ? (s.overlay_items | bit) : (s.overlay_items & ~bit);
                dirty = true;
            }
        }
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Preview");
        ImGui::BeginChild("##preview", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        ImGui::SetWindowFontScale(s.overlay_scale / 100.0f);
        const std::string text = OverlayText();
        ImGui::TextUnformatted(text.empty() ? "(nothing selected)" : text.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::EndChild();
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Appearance");
    if (BeginRows("##appearance")) {
        SliderRow("Font size", s.overlay_scale, 50, 300, "%d%%", "Size of the overlay text.");
        static const char* const corners[] = {"Top left", "Top right", "Bottom left", "Bottom right"};
        ChoiceRow("Corner", s.overlay_corner, corners, nullptr, 4, "Where the overlay sits.");
        SliderRow("Background opacity", s.overlay_opacity, 0, 100, "%d%%", "0% is no background.");
        static const char* const layouts[] = {"One line", "One item per line"};
        ChoiceRow("Layout", s.overlay_layout, layouts, nullptr, 2, "Items side by side or stacked.");
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    ImGui::SeparatorText("About");
    if (BeginRows("##about")) {
        const auto& elf = Common::ElfInfo::Instance();
        char text[160];
        std::snprintf(text, sizeof(text), "%.*s v%.*s", int(elf.GameSerial().size()), elf.GameSerial().data(),
                      int(elf.AppVer().size()), elf.AppVer().data());
        InfoRow("Game", text);
        const u32 total = background_total;
        if (total) {
            std::snprintf(text, sizeof(text), "loading %u%%", u32(u64(background_done) * 100 / total));
        } else {
            std::snprintf(text, sizeof(text), "ready");
        }
        InfoRow("Shader cache", text);
        ImGui::EndTable();
    }
}

// ---- Controls tab -------------------------------------------------------------------------
// The keyboard, mouse and controller sources of each PS4 button and stick direction
// (src/runtime_pad.c's maps), the mouse, the controller's camera speed and the button icons.
// Kept in the settings file with the rest (the launcher's Controls tab edits the same keys); the
// pad applies a change at once (Gow3Settings::ControlsChanged). Guarded by imgui_mutex.

struct ControlAction {
    const char* id;    // the maps' name
    const char* label;
    const char* fixed_pad[2]; // the controller's own source (Xbox, PlayStation), not rebindable
    bool stick;        // a stick direction: keys only
};
constexpr ControlAction control_actions[] = {
    {"ls_up", "Move forward", {"Left stick", "Left stick"}, true},
    {"ls_down", "Move back", {"Left stick", "Left stick"}, true},
    {"ls_left", "Move left", {"Left stick", "Left stick"}, true},
    {"ls_right", "Move right", {"Left stick", "Left stick"}, true},
    {"rs_up", "Look up", {"Right stick", "Right stick"}, true},
    {"rs_down", "Look down", {"Right stick", "Right stick"}, true},
    {"rs_left", "Look left", {"Right stick", "Right stick"}, true},
    {"rs_right", "Look right", {"Right stick", "Right stick"}, true},
    {"left", "D-pad left", {}, false},
    {"right", "D-pad right", {}, false},
    {"up", "D-pad up", {}, false},
    {"down", "D-pad down", {}, false},
    {"cross", "Jump, climb", {}, false},
    {"circle", "Cover, roll, drop", {}, false},
    {"square", "Melee", {}, false},
    {"triangle", "Reload, pick up", {}, false},
    {"l2", "Aim", {"LT", "L2"}, false},
    {"r2", "Fire", {"RT", "R2"}, false},
    {"l1", "Grenade aim", {}, false},
    {"r1", "Grenade throw", {}, false},
    {"l3", "Left stick press", {}, false},
    {"r3", "Right stick press", {}, false},
    {"options", "Pause menu", {}, false},
    {"touchpad_left", "Touchpad left", {"LB + View", "L1 + Share"}, false},
    {"touchpad_right", "Touchpad right", {}, false},
};
constexpr int ControlCount = int(std::size(control_actions));
// Group headings before these rows.
constexpr std::pair<int, const char*> control_groups[] = {{0, "Movement and camera"}, {8, "Weapons (d-pad)"},
                                                          {12, "Actions"}};

struct ControlBinding {
    std::vector<std::string> keys; // SDL key names
    std::string mouse, pad;        // runtime_pad.c's mouse names, SDL gamepad button names
};
std::array<ControlBinding, ControlCount> control_bindings;
bool control_bindings_loaded = false;
bool control_playstation = false; // controller button names: PlayStation or Xbox
bool control_reset_confirm = false;

enum Capture : int { CaptureNone, CaptureKey, CaptureMouse, CapturePad, CaptureMenuPad, CaptureMenuKey };
constexpr int MenuBindingAction = -2; // capture_action of the menu's own shortcut
u32 capture_pad_mask = 0;             // CaptureMenuPad: the buttons held so far
int capture = CaptureNone, capture_action = -1;
std::chrono::steady_clock::time_point capture_until{};
const char* control_note = nullptr; // what the last choice did (shown above the table)

void CancelCapture() {
    capture = CaptureNone;
    capture_action = -1;
    capture_pad_mask = 0;
}

void ReloadControls() {
    control_bindings_loaded = false;
    control_reset_confirm = false;
    control_note = nullptr;
}

int ControlIndex(std::string_view id) {
    for (int i = 0; i < ControlCount; ++i) {
        if (id == control_actions[i].id) return i;
    }
    return -1;
}

// kind: 0 keyboard, 1 mouse, 2 controller.
void ParseControlMap(std::string_view spec, int kind) {
    while (!spec.empty()) {
        const size_t comma = spec.find(',');
        const std::string_view entry = spec.substr(0, comma);
        spec = comma == std::string_view::npos ? std::string_view{} : spec.substr(comma + 1);
        const size_t eq = entry.find('=');
        if (eq == std::string_view::npos) continue;
        const int index = ControlIndex(entry.substr(0, eq));
        const std::string source{entry.substr(eq + 1)};
        if (index < 0 || source.empty()) continue;
        ControlBinding& binding = control_bindings[index];
        if (kind == 0) {
            binding.keys.push_back(source);
        } else if (kind == 1) {
            if (binding.mouse.empty()) binding.mouse = source;
        } else if (binding.pad.empty()) {
            binding.pad = source;
        }
    }
}

bool PlayStationController() {
    const int prompts = Gow3Settings::Get().prompts;
    if (prompts == 0 || prompts == 2) return prompts == 0;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    bool playstation = false;
    if (ids && count > 0) {
        const SDL_GamepadType type = SDL_GetGamepadTypeForID(ids[0]);
        playstation = type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5;
    }
    SDL_free(ids);
    return playstation;
}

void LoadControlBindings() {
    auto& s = Gow3Settings::Get();
    std::string pad, keys, mouse;
    {
        std::scoped_lock lock{Gow3Settings::ControlsMutex()};
        pad = s.pad_map;
        keys = s.key_map;
        mouse = s.mouse_map;
    }
    static char pad_out[2048], key_out[2048], mouse_out[2048];
    runtime_pad_effective_maps(pad.c_str(), keys.c_str(), mouse.c_str(), pad_out, key_out, mouse_out,
                               sizeof(pad_out));
    for (ControlBinding& binding : control_bindings) {
        binding = {};
    }
    ParseControlMap(key_out, 0);
    ParseControlMap(mouse_out, 1);
    ParseControlMap(pad_out, 2);
    control_playstation = PlayStationController();
    control_bindings_loaded = true;
}

// Complete maps (every action listed, "id=" for none), so they mean the same on any defaults.
void SaveControlBindings() {
    std::string pad, keys, mouse;
    const auto add = [](std::string& out, const char* id, const std::string& source) {
        if (!out.empty()) out += ',';
        out += id;
        out += '=';
        out += source;
    };
    for (int i = 0; i < ControlCount; ++i) {
        const ControlAction& action = control_actions[i];
        const ControlBinding& binding = control_bindings[i];
        if (binding.keys.empty()) add(keys, action.id, "");
        for (const std::string& key : binding.keys) add(keys, action.id, key);
        if (!action.stick) add(mouse, action.id, binding.mouse);
        if (!action.stick && !action.fixed_pad[0]) add(pad, action.id, binding.pad);
    }
    auto& s = Gow3Settings::Get();
    {
        std::scoped_lock lock{Gow3Settings::ControlsMutex()};
        s.pad_map = pad;
        s.key_map = keys;
        s.mouse_map = mouse;
    }
    Gow3Settings::ControlsChanged();
    dirty = true;
}

// A key or mouse button does one thing: it leaves the action that had it (as in the launcher).
// A controller button swaps: the action that had it gets this one's old button. An action keeps
// at most two keyboard and mouse sources (the launcher's limit).
void AssignControl(int action, int kind, const std::string& source) {
    ControlBinding& target = control_bindings[action];
    for (int i = 0; i < ControlCount; ++i) {
        if (i == action) continue;
        ControlBinding& other = control_bindings[i];
        if (kind == CaptureKey) {
            std::erase(other.keys, source);
        } else if (kind == CaptureMouse && other.mouse == source) {
            other.mouse.clear();
        } else if (kind == CapturePad && other.pad == source && !control_actions[i].fixed_pad[0]) {
            other.pad = target.pad;
        }
    }
    if (kind == CaptureKey) {
        target.keys = {source};
    } else if (kind == CaptureMouse) {
        target.mouse = source;
        if (target.keys.size() > 1) target.keys.resize(1);
    } else {
        target.pad = source;
    }
    SaveControlBindings();
    control_note = "Saved.";
}

void ClearControl(int action, int kind) {
    ControlBinding& target = control_bindings[action];
    if (kind == CaptureKey) target.keys.clear();
    if (kind == CaptureMouse) target.mouse.clear();
    if (kind == CapturePad) target.pad.clear();
    SaveControlBindings();
    control_note = "Cleared.";
}

void ResetControls() {
    auto& s = Gow3Settings::Get();
    {
        std::scoped_lock lock{Gow3Settings::ControlsMutex()};
        s.pad_map.clear();
        s.key_map.clear();
        s.mouse_map.clear();
    }
    s.mouse = true;
    s.mouse_sens = 1.0f;
    s.mouse_invert = false;
    s.mouse_invert_x = false;
    s.mouse_aim_sens = 1.0f;
    s.mouse_vertical = 1.0f;
    s.prompts = -1;
    s.stick_speed = 1.0f;
    Gow3Settings::ControlsChanged();
    dirty = true;
    ReloadControls();
    control_note = "Controls reset to the defaults.";
}

// imgui_mutex held: an input event while a binding waits for its key or button. Releases go on
// to ImGui (it saw the press that started the choice); presses choose, Escape cancels, Delete
// clears.
bool HandleMenuCaptureEvent(const SDL_Event& event);

bool HandleCaptureEvent(const SDL_Event& event) {
    if (capture == CaptureMenuPad || capture == CaptureMenuKey) {
        return HandleMenuCaptureEvent(event);
    }
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN: {
        if (event.key.repeat) return true;
        const SDL_Scancode code = event.key.scancode;
        if (code == SDL_SCANCODE_ESCAPE) {
            control_note = "Unchanged.";
        } else if (code == SDL_SCANCODE_DELETE) {
            ClearControl(capture_action, capture);
        } else if (capture == CaptureKey) {
            const char* name = SDL_GetScancodeName(code);
            if (!name || !*name || SDL_GetKeyFromScancode(code, SDL_KMOD_NONE, false) == MenuKey() ||
                code == SDL_SCANCODE_F9 || code == SDL_SCANCODE_COMMA) {
                control_note = "This menu's key, F9 (pad recording) and the comma key cannot be bound.";
            } else {
                AssignControl(capture_action, CaptureKey, name);
            }
        } else {
            return true; // a mouse or controller choice: keys other than Escape and Delete wait
        }
        CancelCapture();
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        static const char* const names[] = {"left", "middle", "right", "x1", "x2"};
        const int button = event.button.button;
        if (capture == CaptureMouse && button >= 1 && button <= 5) {
            AssignControl(capture_action, CaptureMouse, names[button - 1]);
        } else {
            control_note = "Unchanged.";
        }
        CancelCapture();
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (capture == CaptureMouse && event.wheel.y != 0) {
            AssignControl(capture_action, CaptureMouse, event.wheel.y > 0 ? "wheel_up" : "wheel_down");
            CancelCapture();
        }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        if (capture == CapturePad) {
            const char* name = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(event.gbutton.button));
            if (name && *name) AssignControl(capture_action, CapturePad, name);
        } else {
            control_note = "Unchanged."; // a controller button cancels a key or mouse choice
        }
        CancelCapture();
        return true;
    }
    default:
        return false;
    }
}

// The menu's shortcut being chosen: CaptureMenuKey takes the next key (Escape cancels, Delete
// restores Insert); CaptureMenuPad the one or two controller buttons held together (a single
// button only if the game does not use it: Guide, Share, the paddles, the touchpad).
void FinishMenuPadCapture() {
    const u32 mask = capture_pad_mask;
    const u32 free_single = (1u << SDL_GAMEPAD_BUTTON_GUIDE) | (1u << SDL_GAMEPAD_BUTTON_MISC1) |
                            (1u << SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1) | (1u << SDL_GAMEPAD_BUTTON_LEFT_PADDLE1) |
                            (1u << SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2) | (1u << SDL_GAMEPAD_BUTTON_LEFT_PADDLE2) |
                            (1u << SDL_GAMEPAD_BUTTON_TOUCHPAD);
    if (std::popcount(mask) == 1 && !(mask & free_single)) {
        control_note = "Hold two buttons together: a single button is also a game action.";
    } else if (mask) {
        std::string combo;
        if (mask & LeftTriggerBit) combo += "lefttrigger";
        if (mask & RightTriggerBit) combo += std::string(combo.empty() ? "" : "+") + "righttrigger";
        for (int b = 0; b < 30; ++b) {
            if (mask & (1u << b)) {
                if (const char* name = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(b))) {
                    combo += std::string(combo.empty() ? "" : "+") + name;
                }
            }
        }
        {
            std::scoped_lock lock{Gow3Settings::ControlsMutex()};
            Gow3Settings::Get().menu_pad = combo;
        }
        Gow3Settings::Get().menu_shortcut = 3;
        dirty = true;
        control_note = "Menu shortcut changed.";
    }
    CancelCapture();
}

bool HandleMenuCaptureEvent(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN: {
        if (event.key.repeat) return true;
        if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
            control_note = "Unchanged.";
        } else if (capture == CaptureMenuKey) {
            const char* name = event.key.scancode == SDL_SCANCODE_DELETE ? "Insert" : SDL_GetKeyName(event.key.key);
            if (name && *name) {
                {
                    std::scoped_lock lock{Gow3Settings::ControlsMutex()};
                    Gow3Settings::Get().menu_key = name;
                }
                dirty = true;
                control_note = "Menu key changed.";
            }
        } else {
            return true; // a controller choice: other keys wait
        }
        CancelCapture();
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        if (capture != CaptureMenuPad) {
            control_note = "Unchanged.";
            CancelCapture();
            return true;
        }
        const u32 bit = event.gbutton.button < 30 ? 1u << event.gbutton.button : 0;
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            capture_pad_mask |= bit;
            if (std::popcount(capture_pad_mask) >= 2) FinishMenuPadCapture();
        } else if (capture_pad_mask & bit) {
            FinishMenuPadCapture(); // released: the buttons held until now
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (capture != CaptureMenuPad || (event.gaxis.axis != SDL_GAMEPAD_AXIS_LEFT_TRIGGER &&
                                          event.gaxis.axis != SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
            return capture == CaptureMenuPad;
        }
        const u32 bit = event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? LeftTriggerBit : RightTriggerBit;
        if (event.gaxis.value > 16000 && !(capture_pad_mask & bit)) {
            capture_pad_mask |= bit;
            if (std::popcount(capture_pad_mask) >= 2) FinishMenuPadCapture();
        } else if (event.gaxis.value < 8000 && (capture_pad_mask & bit)) {
            FinishMenuPadCapture();
        }
        return true;
    }
    default:
        return false;
    }
}

const char* MouseLabel(const std::string& name) {
    static const std::pair<const char*, const char*> labels[] = {
        {"left", "Left button"}, {"middle", "Middle button"}, {"right", "Right button"}, {"x1", "Back button"},
        {"x2", "Forward button"}, {"wheel_up", "Wheel up"}, {"wheel_down", "Wheel down"}};
    for (const auto& [id, label] : labels) {
        if (name == id) return label;
    }
    return name.c_str();
}

const char* PadLabel(const std::string& name, bool playstation) {
    static const std::array<const char*, 3> labels[] = {
        {"a", "A", "Cross"}, {"b", "B", "Circle"}, {"x", "X", "Square"}, {"y", "Y", "Triangle"},
        {"back", "View", "Share"}, {"guide", "Guide", "PS"}, {"start", "Menu", "Options"},
        {"leftstick", "LS", "L3"}, {"rightstick", "RS", "R3"}, {"leftshoulder", "LB", "L1"},
        {"rightshoulder", "RB", "R1"}, {"dpup", "D-pad up", "D-pad up"}, {"dpdown", "D-pad down", "D-pad down"},
        {"dpleft", "D-pad left", "D-pad left"}, {"dpright", "D-pad right", "D-pad right"},
        {"misc1", "Share", "Mute"}, {"paddle1", "P1", "P1"}, {"paddle2", "P2", "P2"}, {"paddle3", "P3", "P3"},
        {"paddle4", "P4", "P4"}, {"touchpad", "Touchpad", "Touchpad"}};
    for (const auto& label : labels) {
        if (name == label[0]) return label[playstation ? 2 : 1];
    }
    return name.c_str();
}

void BindingButton(int action, int kind, const std::string& text) {
    const bool waiting = capture == kind && capture_action == action;
    const char* shown = waiting ? (kind == CaptureMenuPad ? "Hold one or two buttons..."
                                   : kind == CapturePad   ? "Press a button..."
                                   : kind == CaptureMouse ? "Click or scroll..."
                                                          : "Press a key...")
                        : text.empty() ? "-"
                                       : text.c_str();
    ImGui::PushID(kind);
    if (waiting) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.45f, 0.12f, 1.0f));
    }
    const std::string label = std::string(shown) + "###binding";
    if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0.0f)) && capture == CaptureNone) {
        capture = kind;
        capture_action = action;
        capture_until = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        control_note = nullptr;
    }
    if (waiting) {
        ImGui::PopStyleColor();
    }
    Help(kind == CaptureMenuPad ? "Select, then hold the two controller buttons that should open this menu (a "
                                  "single button only if the game does not use it). Escape cancels."
         : kind == CaptureMenuKey ? "Select, then press the key that opens this menu. Escape cancels, Delete "
                                    "restores Insert."
         : kind == CapturePad ? "Select, then press the controller button for this action (the action that had it "
                                "swaps to this one's old button). Escape cancels, Delete clears."
         : kind == CaptureMouse ? "Select, then click a mouse button or scroll. Escape cancels, Delete clears."
                                : "Select, then press a key. Escape cancels, Delete clears.");
    ImGui::PopID();
}

// A percent slider over a float setting; true when changed.
bool PercentRow(const char* label, std::atomic<float>& value, int lo, int hi, const char* help) {
    Row(label);
    int v = int(std::lround(value.load() * 100.0f));
    const bool changed = ImGui::SliderInt("##value", &v, lo, hi, "%d%%", ImGuiSliderFlags_AlwaysClamp);
    if (changed) {
        value = std::clamp(v, lo, hi) / 100.0f;
    }
    Help(help);
    ImGui::PopID();
    return changed;
}

void ControlsTab() {
    auto& s = Gow3Settings::Get();
    if (!control_bindings_loaded) {
        LoadControlBindings();
    }
    if (capture != CaptureNone && std::chrono::steady_clock::now() > capture_until) {
        CancelCapture();
        control_note = "Nothing pressed: unchanged.";
    }
    bool changed = false;
    ImGui::SeparatorText("Camera and aiming");
    if (BeginRows("##camera")) {
        changed |= PercentRow("Controller camera and aim speed", s.stick_speed, 50, 300,
                              "How fast the right stick turns the camera (Uncharted 2 and 3; not yet in every camera). For "
                              "aiming use the game's own Options > Controls > Aiming > Sensitivity (Uncharted 2: up "
                              "to five times its old maximum).");
        Row("Mouse");
        bool mouse = s.mouse;
        if (ImGui::Checkbox("##value", &mouse)) {
            s.mouse = mouse;
            changed = true;
        }
        Help("The mouse turns the camera and presses its buttons' actions. Off: the mouse is left alone.");
        ImGui::PopID();
        ImGui::BeginDisabled(!s.mouse);
        changed |= PercentRow("Mouse sensitivity", s.mouse_sens, 25, 500, "How far the camera turns per mouse movement.");
        changed |= PercentRow("Mouse aim sensitivity", s.mouse_aim_sens, 25, 1000,
                              "While aiming (the aim button held): the mouse sensitivity times this. Below 100% "
                              "makes aiming finer.");
        changed |= PercentRow("Mouse up/down speed", s.mouse_vertical, 25, 300,
                              "Looking up and down compared with left and right.");
        Row("Invert mouse up/down");
        bool invert = s.mouse_invert;
        if (ImGui::Checkbox("##value", &invert)) {
            s.mouse_invert = invert;
            changed = true;
        }
        Help("Moving the mouse forward looks down.");
        ImGui::PopID();
        Row("Invert mouse left/right");
        bool invert_x = s.mouse_invert_x;
        if (ImGui::Checkbox("##value", &invert_x)) {
            s.mouse_invert_x = invert_x;
            changed = true;
        }
        Help("Moving the mouse left turns right.");
        ImGui::PopID();
        ImGui::EndDisabled();
        static const char* const icon_labels[] = {"Auto (the device you use)", "Keyboard and mouse", "PlayStation",
                                                  "Xbox"};
        static const int icon_values[] = {-1, 1, 0, 2};
        Row("Button icons");
        int icons = 0;
        for (int i = 0; i < 4; ++i) {
            if (icon_values[i] == s.prompts) icons = i;
        }
        if (ImGui::Combo("##value", &icons, icon_labels, 4)) {
            s.prompts = icon_values[icons];
            control_playstation = PlayStationController();
            changed = true;
        }
        Help("The buttons shown in the game's hints and menus. Auto follows the device you play with.");
        ImGui::PopID();
        static const char* const shortcut_labels[] = {"LT + right stick click", "View + Menu together",
                                                      "None (keyboard key only)", "Custom (below)"};
        ChoiceRow("Menu shortcut (controller)", s.menu_shortcut, shortcut_labels, nullptr, 4,
                  "Opens this menu. LT + right stick click is also the sniper zoom in some guns: View + Menu "
                  "or your own buttons avoid that. The keyboard key always works.");
        {
            std::string custom_text, key_text;
            {
                std::scoped_lock lock{Gow3Settings::ControlsMutex()};
                custom_text = PadComboLabel(PadComboMask(s.menu_pad), control_playstation);
                key_text = s.menu_key;
            }
            Row("Custom menu shortcut");
            BindingButton(MenuBindingAction, CaptureMenuPad, custom_text);
            ImGui::PopID();
            Row("Menu key (keyboard)");
            BindingButton(MenuBindingAction, CaptureMenuKey, key_text);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (changed) {
        Gow3Settings::ControlsChanged();
        dirty = true;
    }

    ImGui::SeparatorText("Buttons");
    if (capture != CaptureNone) {
        ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s: %s  (Escape cancels, Delete clears)",
                           capture_action == MenuBindingAction ? "Menu shortcut"
                                                               : control_actions[capture_action].label,
                           capture == CapturePad     ? "press a controller button"
                           : capture == CaptureMouse ? "click a mouse button or scroll"
                                                     : "press a key");
    } else {
        ImGui::TextDisabled("%s", control_note ? control_note
                                               : "Select a slot, then press the new key or button. Changes apply at once.");
    }
    if (ImGui::BeginTable("##bindings", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 0.31f);
        ImGui::TableSetupColumn("Keyboard", ImGuiTableColumnFlags_WidthStretch, 0.25f);
        ImGui::TableSetupColumn("Mouse", ImGuiTableColumnFlags_WidthStretch, 0.22f);
        ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch, 0.22f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < ControlCount; ++i) {
            for (const auto& [first, heading] : control_groups) {
                if (first == i) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", heading);
                }
            }
            const ControlAction& action = control_actions[i];
            const ControlBinding& binding = control_bindings[i];
            ImGui::TableNextRow();
            ImGui::PushID(i);
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(action.label);
            ImGui::TableNextColumn();
            std::string keys;
            for (const std::string& key : binding.keys) {
                keys += (keys.empty() ? "" : " / ") + key;
            }
            BindingButton(i, CaptureKey, keys);
            ImGui::TableNextColumn();
            if (action.stick) {
                ImGui::TextDisabled("%s", action.id[0] == 'r' ? "Moves the camera" : "-");
            } else {
                BindingButton(i, CaptureMouse, binding.mouse.empty() ? "" : MouseLabel(binding.mouse));
            }
            ImGui::TableNextColumn();
            if (action.fixed_pad[0]) {
                ImGui::TextDisabled("%s", action.fixed_pad[control_playstation ? 1 : 0]);
            } else {
                BindingButton(i, CapturePad, binding.pad.empty() ? "" : PadLabel(binding.pad, control_playstation));
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    if (!control_reset_confirm) {
        if (ImGui::Button("Reset controls to defaults")) {
            control_reset_confirm = true;
        }
        Help("Keyboard, mouse and controller buttons, the sensitivities and the button icons.");
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Reset every control of this game?");
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            ResetControls();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep")) {
            control_reset_confirm = false;
        }
    }
}

// Startup options changed since this launch (applied by "Apply and restart").
int RestartChanges() {
    const auto& s = Gow3Settings::Get();
    return (s.deferred_readback != s.startup_deferred_readback) + (s.stale_readback != s.startup_stale_readback);
}

// The running game's trophies: earned pictures bright, the others dimmed.
void TrophiesTab() {
    trophy_tab_shown = true;
    footer_help = "Trophy pictures come from your copy of the game. Their names are encrypted in the "
                  "game files, so they are not shown.";
    const Gow3Trophies::Summary list = Gow3Trophies::List();
    if (list.game.empty()) {
        ImGui::TextWrapped("The game has not opened its trophy list yet.");
        return;
    }
    if (list.list != trophy_tab_list) {
        for (auto& [id, tex] : trophy_tab_textures) {
            if (tex) {
                tex->WantDestroyNextFrame = true;
                trophy_retired.push_back(tex);
            }
        }
        trophy_tab_textures.clear();
        trophy_tab_list = list.list;
    }
    const int earned = int(std::count(list.earned.begin(), list.earned.end(), true));
    const int total = int(list.ids.size());
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", list.game.c_str());
    char progress[64];
    std::snprintf(progress, sizeof(progress), "%d of %d trophies earned", earned, total);
    ImGui::ProgressBar(total ? float(earned) / float(total) : 0.0f, ImVec2(-1.0f, 0.0f), progress);
    ImGui::Spacing();
    const float cell = 72.0f * base_scale;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const int columns = std::max(1, int((ImGui::GetContentRegionAvail().x + spacing) / (cell + spacing)));
    for (int i = 0; i < total; ++i) {
        const int id = list.ids[i];
        const auto it = trophy_tab_textures.find(id);
        ImTextureData* tex = it != trophy_tab_textures.end() ? it->second : nullptr;
        if (tex) {
            ImGui::ImageWithBg(tex->GetTexRef(), ImVec2(cell, cell), ImVec2(0, 0), ImVec2(1, 1),
                               ImVec4(0, 0, 0, 0),
                               list.earned[i] ? ImVec4(1, 1, 1, 1) : ImVec4(0.3f, 0.3f, 0.3f, 0.55f));
        } else {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + cell, at.y + cell),
                                                      IM_COL32(50, 50, 56, 255), 6.0f * base_scale);
            ImGui::Dummy(ImVec2(cell, cell));
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Trophy %d: %s", id, list.earned[i] ? "earned" : "not earned yet");
        }
        if ((i + 1) % columns != 0 && i + 1 < total) {
            ImGui::SameLine();
        }
    }
}

void Menu() {
    auto& s = Gow3Settings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(760.0f * base_scale, viewport->WorkSize.x * 0.95f),
                                    std::min(680.0f * base_scale, viewport->WorkSize.y * 0.9f)),
                             ImGuiCond_Appearing);
    bool keep_open = true;
    // gow3: the game's own title (param.sfo), not a fixed one.
    static const std::string heading = [] {
        const std::string_view title = Common::ElfInfo::Instance().Title();
        return std::string(title.empty() ? "Uncharted: The Nathan Drake Collection" : title);
    }();
    // The shortcut can change in the Controls tab: the title shows it; "###" keeps the window's id.
    const std::string window_title = heading + "  (" + ShortcutText() + ")###gow3_menu";
    if (!ImGui::Begin(window_title.c_str(), &keep_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    footer_help = nullptr;
    constexpr int TabCount = 5;
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput && capture == CaptureNone) {
        const int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
                         : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false)
                             ? -1
                             : 0;
        if (step) {
            requested_tab = (s.menu_tab + step + TabCount) % TabCount;
        }
    }
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetTextLineHeightWithSpacing() * 2.0f;
    if (ImGui::BeginTabBar("##tabs")) {
        static const char* const tabs[TabCount] = {"Image", "Performance", "Overlay", "Controls", "Trophies"};
        for (int tab = 0; tab < TabCount; ++tab) {
            const ImGuiTabItemFlags flags = requested_tab == tab ? ImGuiTabItemFlags_SetSelected : 0;
            if (!ImGui::BeginTabItem(tabs[tab], nullptr, flags)) {
                continue;
            }
            if (s.menu_tab != tab && requested_tab < 0) {
                s.menu_tab = tab;
                dirty = true;
            }
            if (ImGui::BeginChild("##body", ImVec2(0, -footer))) {
                switch (tab) {
                case 0: ImageTab(); break;
                case 1: PerformanceTab(); break;
                case 2: OverlayTab(); break;
                case 3: ControlsTab(); break;
                default: TrophiesTab(); break;
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (requested_tab >= 0) {
            s.menu_tab = requested_tab;
            dirty = true;
            requested_tab = -1;
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", footer_help ? footer_help : "L1 / R1 or Q / E: switch tabs. The launcher keeps the same settings.");
    ImGui::PopTextWrapPos();
    if (const int changes = RestartChanges()) {
        ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%d change%s need%s a restart (unsaved progress is lost).",
                           changes, changes == 1 ? "" : "s", changes == 1 ? "s" : "");
        ImGui::SameLine();
        ImGui::BeginDisabled(!std::getenv("GOW3_RESTART_COMMAND"));
        if (ImGui::Button("Apply and restart")) {
            // The new launch confirms the options once the game shows; run.py restores the
            // previous ones if it never does (restart_unconfirmed).
            s.restart_unconfirmed = true;
            Gow3Settings::Save();
            runtime_restart();
        }
        ImGui::EndDisabled();
    } else {
        ImGui::NewLine();
    }
    const float close_width = ImGui::CalcTextSize("Close").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - close_width);
    if (ImGui::Button("Close")) {
        keep_open = false;
    }
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

// The game's text dialog: what is typed, and how to finish (keyboard or controller).
void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::TextDisabled("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel");
    ImGui::TextDisabled("Controller: Cross (A) = OK, Circle (B) = cancel");
    ImGui::End();
}

void ChoiceBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.36f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, viewport->WorkSize.y * 0.8f));
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::Begin("##choice", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", choice_title.c_str());
    ImGui::Separator();
    // Save pictures are 228x128; rows without one keep the text aligned when others have one.
    const bool pictures = std::any_of(choice_textures.begin(), choice_textures.end(),
                                      [](ImTextureData* tex) { return tex != nullptr; });
    const ImVec2 picture_size(228.0f * base_scale, 128.0f * base_scale);
    for (int i = 0; i < int(choice_items.size()); ++i) {
        const bool selected = i == choice_index;
        ImGui::BeginGroup();
        ImTextureData* tex = i < int(choice_textures.size()) ? choice_textures[i] : nullptr;
        if (tex) {
            ImGui::Image(tex->GetTexRef(), picture_size);
        } else if (pictures) {
            ImGui::Dummy(picture_size);
        }
        if (pictures) {
            ImGui::SameLine();
        }
        if (selected) {
            ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.55f, 1.0f), "> %s", choice_items[i].c_str());
        } else {
            ImGui::Text("  %s", choice_items[i].c_str());
        }
        ImGui::EndGroup();
        if (selected) {
            if (pictures) {
                const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                const float pad = 3.0f * base_scale;
                ImGui::GetWindowDrawList()->AddRect(ImVec2(min.x - pad, min.y - pad),
                                                    ImVec2(max.x + pad, max.y + pad),
                                                    IM_COL32(255, 230, 140, 255), 0.0f, 0,
                                                    2.0f * base_scale);
            }
            ImGui::SetScrollHereY();
        }
    }
    ImGui::Separator();
    ImGui::TextDisabled("Up/Down or D-pad: choose   Enter or Cross (A): OK   Esc or Circle (B): cancel");
    ImGui::End();
}

void LoadingScreen() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.4f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    const u32 done = loading_done, total = loading_total;
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted(total ? "Loading shader cache..." : "Preparing graphics...");
    ImGui::SetWindowFontScale(1.0f);
    if (total) {
        char count[64];
        std::snprintf(count, sizeof(count), "%u / %u (%u%%)", done, total,
                      u32(u64(done) * 100 / total));
        ImGui::ProgressBar(float(done) / float(total), ImVec2(-1.0f, 0.0f), count);
    }
    ImGui::TextDisabled("The first start after an update takes longer.");
    ImGui::End();
}

void FpsCounter() {
    const auto& s = Gow3Settings::Get();
    const std::string text = OverlayText();
    if (text.empty()) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    const int corner = s.overlay_corner;
    const bool right = corner == 1 || corner == 3, bottom = corner >= 2;
    ImGui::SetNextWindowPos(ImVec2(right ? viewport->WorkPos.x + viewport->WorkSize.x - pad : viewport->WorkPos.x + pad,
                                   bottom ? viewport->WorkPos.y + viewport->WorkSize.y - pad : viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(s.overlay_opacity / 100.0f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(s.overlay_scale / 100.0f);
    ImGui::TextUnformatted(text.c_str());
    fps_top_right_height = corner == 1 ? ImGui::GetWindowHeight() : 0.0f;
    ImGui::End();
}

// The front trophy popup: slides in at the top right, stays (shorter while others wait), fades.
void TrophyPopup() {
    TrophyPopupItem& item = trophy_popups.front();
    const auto now = std::chrono::steady_clock::now();
    if (item.shown == std::chrono::steady_clock::time_point{}) {
        item.shown = now;
    }
    const float hold = trophy_popups.size() > 1 ? 3.0f : 6.0f;
    const float t = std::chrono::duration<float>(now - item.shown).count();
    if (t >= hold) {
        if (item.tex) {
            item.tex->WantDestroyNextFrame = true;
            trophy_retired.push_back(item.tex);
        }
        trophy_popups.pop_front();
        return;
    }
    const float in = std::min(t / 0.3f, 1.0f);
    const float alpha = std::min(in, std::clamp((hold - t) / 0.4f, 0.0f, 1.0f));
    const auto a = [&](int value) { return int(float(value) * alpha); };
    const float s = base_scale;
    const float width = 470.0f * s, height = 96.0f * s, pad = 24.0f * s;
    const float slide = (1.0f - in) * (1.0f - in) * (width + pad);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float below_fps = fps_top_right_height > 0.0f ? fps_top_right_height + 8.0f * s : 0.0f;
    const ImVec2 min(viewport->WorkPos.x + viewport->WorkSize.x - pad - width + slide,
                     viewport->WorkPos.y + pad + below_fps);
    const ImVec2 max(min.x + width, min.y + height);
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(min, max, IM_COL32(22, 22, 26, a(238)), 14.0f * s);
    const float icon = height - 20.0f * s;
    const ImVec2 icon_min(min.x + 10.0f * s, min.y + 10.0f * s);
    const ImVec2 icon_max(icon_min.x + icon, icon_min.y + icon);
    if (item.tex) {
        draw->AddImageRounded(item.tex->GetTexRef(), icon_min, icon_max, ImVec2(0, 0), ImVec2(1, 1),
                              IM_COL32(255, 255, 255, a(255)), 8.0f * s);
    } else {
        draw->AddRectFilled(icon_min, icon_max, IM_COL32(60, 60, 66, a(255)), 8.0f * s);
    }
    // A small cup, then the title; below it the text's lines.
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    const float x = icon_max.x + 14.0f * s, y = min.y + 14.0f * s;
    const ImU32 gold = IM_COL32(222, 184, 92, a(255));
    draw->AddRectFilled(ImVec2(x, y + 2.0f * s), ImVec2(x + 12.0f * s, y + 9.0f * s), gold, 3.0f * s);
    draw->AddRectFilled(ImVec2(x + 5.0f * s, y + 9.0f * s), ImVec2(x + 7.0f * s, y + 13.0f * s), gold);
    draw->AddRectFilled(ImVec2(x + 2.0f * s, y + 13.0f * s), ImVec2(x + 10.0f * s, y + 15.0f * s), gold);
    draw->AddText(font, size * 0.95f, ImVec2(x + 18.0f * s, y), IM_COL32(186, 186, 194, a(255)),
                  item.title.c_str());
    const std::string_view text = item.text;
    const size_t newline = text.find('\n');
    const std::string first(text.substr(0, newline));
    draw->PushClipRect(ImVec2(x, min.y), ImVec2(max.x - 10.0f * s, max.y), true);
    draw->AddText(font, size * 1.1f, ImVec2(x, y + 24.0f * s), IM_COL32(255, 255, 255, a(255)),
                  first.c_str());
    if (newline != std::string_view::npos) {
        const std::string second(text.substr(newline + 1));
        draw->AddText(font, size * 0.95f, ImVec2(x, y + 50.0f * s), IM_COL32(170, 170, 178, a(255)),
                      second.c_str());
    }
    draw->PopClipRect();
}

void BackgroundLoading() {
    const u32 done = background_done, total = std::max(background_total.load(), 1u);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + pad,
                                   viewport->WorkPos.y + viewport->WorkSize.y - pad),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("##background_loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextDisabled("Loading shader cache %u%%", u32(u64(done) * 100 / total));
    ImGui::End();
}

} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "gow3";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(gow3_font_ttf),
                                   int(gow3_font_ttf_end - gow3_font_ttf), 18.0f, &font_config);

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    // Font atlas plus the save list pictures (up to 33, and a closed list's until freed).
    info.DescriptorPoolSize = 128;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    overlay_instance = &instance;
    initialized = true;
    std::printf("Overlay: menu ready (%s)\n", ShortcutText().c_str());
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

// imgui_mutex held. The backend frees a texture some frames after its last use.
void RetireChoiceTextures() {
    for (ImTextureData* tex : choice_textures) {
        if (tex) {
            tex->WantDestroyNextFrame = true;
            choice_retired.push_back(tex);
        }
    }
    choice_textures.clear();
    choice_pictures.clear();
    choice_retiring = !choice_retired.empty();
}

ImTextureData* NewTexture(const Gow3Png::Image& picture) {
    if (picture.rgba.empty()) {
        return nullptr;
    }
    ImTextureData* tex = IM_NEW(ImTextureData)();
    tex->Create(ImTextureFormat_RGBA32, picture.width, picture.height);
    std::memcpy(tex->GetPixels(), picture.rgba.data(), picture.rgba.size());
    ImGui::RegisterUserTexture(tex);
    return tex;
}

// imgui_mutex held, before NewFrame: popup pictures and (a few per frame) the Trophies tab's
// become textures; retired ones are freed once the backend let them go.
void UpdateTrophyTextures() {
    for (TrophyPopupItem& item : trophy_popups) {
        if (!item.tex && !item.picture.rgba.empty()) {
            item.tex = NewTexture(item.picture);
            item.picture = {};
        }
    }
    if (trophy_tab_shown) {
        trophy_tab_shown = false;
        const Gow3Trophies::Summary list = Gow3Trophies::List();
        int budget = 6;
        for (const int id : list.ids) {
            if (list.list != trophy_tab_list || budget == 0) {
                break;
            }
            if (!trophy_tab_textures.contains(id)) {
                Gow3Png::Image picture;
                Gow3Trophies::Picture(id, picture);
                trophy_tab_textures[id] = NewTexture(picture);
                --budget;
            }
        }
    }
    std::erase_if(trophy_retired, [](ImTextureData* tex) {
        if (tex->Status != ImTextureStatus_Destroyed) {
            return false;
        }
        ImGui::UnregisterUserTexture(tex);
        IM_DELETE(tex);
        return true;
    });
}

// imgui_mutex held, before NewFrame: new pictures become textures, destroyed ones are freed.
void UpdateChoiceTextures() {
    if (!choice_pictures.empty()) {
        choice_textures.assign(choice_pictures.size(), nullptr);
        for (size_t i = 0; i < choice_pictures.size(); ++i) {
            const Gow3Png::Image& picture = choice_pictures[i];
            if (picture.rgba.empty()) {
                continue;
            }
            ImTextureData* tex = IM_NEW(ImTextureData)();
            tex->Create(ImTextureFormat_RGBA32, picture.width, picture.height);
            std::memcpy(tex->GetPixels(), picture.rgba.data(), picture.rgba.size());
            ImGui::RegisterUserTexture(tex);
            choice_textures[i] = tex;
        }
        choice_pictures.clear();
    }
    std::erase_if(choice_retired, [](ImTextureData* tex) {
        if (tex->Status != ImTextureStatus_Destroyed) {
            return false;
        }
        ImGui::UnregisterUserTexture(tex);
        IM_DELETE(tex);
        return true;
    });
    choice_retiring = !choice_retired.empty();
}

// The save list takes the keys and buttons that move and confirm; the rest stays with the game
// (held neutral by CapturesInput).
bool HandleChoiceEvent(const SDL_Event& event) {
    int move = 0, finish = 0; // finish: 1 accept, 2 cancel
    if (event.type == SDL_EVENT_KEY_DOWN) {
        const SDL_Keycode key = event.key.key;
        move = key == SDLK_UP || key == SDLK_W ? -1 : key == SDLK_DOWN || key == SDLK_S ? 1 : 0;
        finish = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE ? 1
                 : key == SDLK_ESCAPE || key == SDLK_BACKSPACE                   ? 2
                                                                                 : 0;
    } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const u8 button = event.gbutton.button;
        move = button == SDL_GAMEPAD_BUTTON_DPAD_UP ? -1 : button == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? 1 : 0;
        finish = button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : button == SDL_GAMEPAD_BUTTON_EAST ? 2 : 0;
    } else {
        return event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
    }
    const int count = int(choice_items.size());
    if (move && count) {
        choice_index = (choice_index + move + count) % count;
    }
    if (finish) {
        choice_result = finish == 1 && count ? choice_index : -2;
        choice_active = false;
        RetireChoiceTextures();
    }
    return true;
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    if (choice_active && !menu_open) {
        return HandleChoiceEvent(event);
    }
    if (menu_open && capture != CaptureNone && HandleCaptureEvent(event)) {
        return true;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        static const bool capture_key = [] {
            const char* env = std::getenv("GOW3_FRAME_CAPTURE_KEY");
            return env && *env == '1';
        }();
        if (down && !event.key.repeat && capture_key && event.key.key == SDLK_F11) {
            Vulkan::FrameCapture::Request();
            return true;
        }
        const SDL_Keycode menu_key = MenuKey();
        if (down && !event.key.repeat &&
            (event.key.key == menu_key || (is_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(event.key.key == menu_key ? !is_open : false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        // The controller shortcut (MenuPadMask): opens or closes the menu when its last button
        // goes down while the others are held.
        const u32 bit = button < 30 ? 1u << button : 0;
        pad_held = down ? pad_held | bit : pad_held & ~bit;
        if (const u32 mask = MenuPadMask(); down && (mask & bit) && (pad_held & mask) == mask) {
            SetOpen(!is_open);
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
            event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
            const u32 bit =
                event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? LeftTriggerBit : RightTriggerBit;
            const bool down = (pad_held & bit) ? event.gaxis.value > 8000 : event.gaxis.value > 16000;
            const bool pressed = down && !(pad_held & bit);
            pad_held = down ? pad_held | bit : pad_held & ~bit;
            if (const u32 mask = MenuPadMask(); pressed && (mask & bit) && (pad_held & mask) == mask) {
                SetOpen(!is_open);
                return true;
            }
        }
        return false;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

// After "Apply and restart", the new options are kept once the game has shown about ten
// seconds of frames after loading (run.py restores the previous ones otherwise).
void ConfirmRestart() {
    static u32 frames = 0;
    auto& s = Gow3Settings::Get();
    if (!s.restart_unconfirmed || loading || ++frames < 600) {
        return;
    }
    s.restart_unconfirmed = false;
    Gow3Settings::Save();
    std::puts("Settings: graphics options confirmed after restart");
}

void SetBackgroundLoading(u32 done, u32 total) {
    background_done = done;
    background_total = total;
}

void SetLoading(bool active, u32 done, u32 total) {
    loading_done = done;
    loading_total = total;
    loading = active;
}

bool Visible() {
    const auto& s = Gow3Settings::Get();
    return initialized && (loading || background_total || menu_open || text_entry_active || choice_active ||
                           choice_retiring || s.show_fps || trophy_showing);
}

bool CapturesInput() {
    return menu_open || text_entry_active || choice_active;
}

void BeginChoice(const std::string& title, const std::vector<std::string>& items,
                 std::vector<Gow3Png::Image> pictures, int focus) {
    std::scoped_lock lock{imgui_mutex};
    RetireChoiceTextures();
    choice_pictures = std::move(pictures);
    choice_pictures.resize(items.size());
    choice_title = title;
    choice_items = items;
    choice_index = focus >= 0 && focus < int(items.size()) ? focus : 0;
    choice_result = -1;
    choice_active = true;
}

int PollChoice() {
    // GOW3_TEST_CHOICE=<index> (scripted tests, whose pad input does not reach this list): the
    // entry is chosen about 1.5 s after the list opens.
    static const int test_choice = [] {
        const char* env = std::getenv("GOW3_TEST_CHOICE");
        return env && *env ? std::atoi(env) : -1;
    }();
    if (test_choice >= 0 && choice_active) {
        static std::chrono::steady_clock::time_point opened{};
        const auto now = std::chrono::steady_clock::now();
        if (opened == std::chrono::steady_clock::time_point{}) {
            opened = now;
        } else if (now - opened > std::chrono::milliseconds(1500)) {
            std::scoped_lock lock{imgui_mutex};
            opened = {};
            choice_result = test_choice < int(choice_items.size()) ? test_choice : -2;
            choice_active = false;
            RetireChoiceTextures();
        }
    }
    return choice_result;
}

void ShowTrophy(Gow3Png::Image picture, const std::string& title, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    if (trophy_popups.size() >= 32) {
        return;
    }
    trophy_popups.push_back(TrophyPopupItem{std::move(picture), nullptr, title, text, {}});
    trophy_showing = true;
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    ConfirmRestart();
    // GOW3_MENU_AT_START=<tab> (tests and screenshots): opens the menu on that tab once the game
    // has shown ten seconds of frames after loading.
    static int menu_at_start = [] {
        const char* env = std::getenv("GOW3_MENU_AT_START");
        return env && *env ? std::clamp(std::atoi(env), 0, 4) : -1;
    }();
    static u32 shown_frames = 0;
    if (menu_at_start >= 0 && initialized && !loading && ++shown_frames > 600) {
        std::scoped_lock lock{imgui_mutex};
        Gow3Settings::Get().menu_tab = menu_at_start;
        SetOpen(true);
        menu_at_start = -1;
    }
    // GOW3_TEST_TROPHY=<seconds>,<id> (tests and screenshots): that trophy's popup once the game
    // has shown frames that long after loading (marked earned for this session only, not saved).
    static int test_trophy_frames = [] {
        const char* env = std::getenv("GOW3_TEST_TROPHY");
        return env && *env ? std::max(1, int(std::atof(env) * 60.0)) : -1;
    }();
    if (test_trophy_frames > 0 && initialized && !loading && --test_trophy_frames == 0) {
        const char* comma = std::strchr(std::getenv("GOW3_TEST_TROPHY"), ',');
        Gow3Trophies::Earned(comma ? std::atoi(comma + 1) : 1);
    }
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    UpdateChoiceTextures();
    UpdateTrophyTextures();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (loading) {
        LoadingScreen();
    }
    if (menu_open) {
        Menu();
    }
    fps_top_right_height = 0.0f;
    if (Gow3Settings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (background_total && !loading && !menu_open) {
        BackgroundLoading();
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    if (choice_active && !menu_open) {
        ChoiceBox();
    }
    if (!trophy_popups.empty()) {
        TrophyPopup();
    }
    trophy_showing = !trophy_popups.empty() || !trophy_retired.empty();
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace Gow3Overlay
