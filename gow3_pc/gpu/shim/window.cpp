// gow3: SDL3 window for the Vulkan swapchain (X11 or Wayland).
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "gow3_overlay.h"
#include "gow3_graphics.h"
#include "gow3_settings.h"
#include "gow3_mouse.h"

namespace Frontend {

namespace {
bool Headless() {
    static const bool headless = [] {
        const char* env = std::getenv("GOW3_HEADLESS");
        return env && std::strcmp(env, "1") == 0;
    }();
    return headless;
}
// GOW3_WINDOW_BACKGROUND=1 (scripted tests while the player uses the PC): the window opens
// without the focus, behind the other windows, and is never raised.
bool Background() {
    static const bool background = [] {
        const char* env = std::getenv("GOW3_WINDOW_BACKGROUND");
        return env && env[0] == '1';
    }();
    return background;
}
// gow3 25 Oct 2026 (local co-op, two game windows on one PC): GOW3_WINDOW_HALF=left|right puts
// the window, borderless, on that half of the primary display's usable area (the taskbar left
// out); GOW3_WINDOW_RECT=x,y,w,h (screen pixels) at that place and size, with its border. Either
// keeps the window windowed whatever the display mode setting says (the setting is not changed).
struct Placement {
    bool on = false, borderless = false;
    int x = 0, y = 0, w = 0, h = 0;
};
const Placement& WindowPlacement() {
    static const Placement placement = [] {
        Placement p;
        if (const char* half = std::getenv("GOW3_WINDOW_HALF"); half && (!std::strcmp(half, "left") || !std::strcmp(half, "right"))) {
            SDL_Rect usable{};
            if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) && usable.w > 1 && usable.h > 0) {
                p.on = p.borderless = true;
                p.w = usable.w / 2;
                p.h = usable.h;
                p.x = usable.x + (half[0] == 'r' ? usable.w - p.w : 0);
                p.y = usable.y;
            }
        } else if (const char* rect = std::getenv("GOW3_WINDOW_RECT")) {
            int x = 0, y = 0, w = 0, h = 0;
            if (std::sscanf(rect, "%d,%d,%d,%d", &x, &y, &w, &h) == 4 && w >= 160 && h >= 90) {
                p.on = true;
                p.x = x; p.y = y; p.w = w; p.h = h;
            }
        }
        if (p.on) {
            std::printf("Window: placed at %d,%d, %dx%d%s (GOW3_WINDOW_%s)\n", p.x, p.y, p.w, p.h,
                        p.borderless ? ", borderless" : "", p.borderless ? "HALF" : "RECT");
            std::fflush(stdout);
        }
        return p;
    }();
    return placement;
}
// gow3 24 Oct 2026: the game window is the foreground window (Windows' own answer, set each
// event poll). The gamepad is read only then (runtime_pad.c, gow3gpu_window_foreground).
std::atomic<bool> window_foreground{true};

void SendToBack(SDL_Window* window) {
#ifdef _WIN32
    if (void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                            SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)) {
        SetWindowPos(static_cast<HWND>(hwnd), HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
#else
    (void)window;
#endif
}
} // namespace

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    // Exclusive fullscreen stays up when another window takes the focus (SDL minimizes it by
    // default; started from the launcher, the game window often starts without the focus).
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    if (Background()) {
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    }
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    // gow3 24 Oct 2026 (user: the controller stopped responding until a key was pressed): SDL
    // drops gamepad events while it thinks no window has the keyboard focus, and its idea of
    // that focus can be lost while the window stays in front (a key press restores it). The
    // events always come now; whether the game is in front is decided from Windows instead
    // (window_foreground; with GOW3_PAD_INDEX, local co-op, each window's own controller works
    // while the other window is in front).
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }
    // Windowed: the requested size, made 16:9 and smaller than the desktop's usable area (a
    // 1920x1080 window on a 1920x1080 screen put its title bar off screen, over the taskbar).
    windowed_width = width_;
    windowed_height = height_;
    if (WindowPlacement().on) {
        windowed_width = WindowPlacement().w;
        windowed_height = WindowPlacement().h;
    } else if (SDL_Rect usable{}; !Headless() && SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) && usable.w > 0 &&
                           usable.h > 0) {
        const int max_w = usable.w * 85 / 100, max_h = usable.h * 85 / 100;
        if (windowed_width > max_w || windowed_height > max_h) {
            windowed_width = std::min(max_w, max_h * 16 / 9);
            windowed_height = windowed_width * 9 / 16;
        }
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER,
                          WindowPlacement().on ? WindowPlacement().x : SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER,
                          WindowPlacement().on ? WindowPlacement().y : SDL_WINDOWPOS_CENTERED);
    if (WindowPlacement().borderless) {
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, true);
    }
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, windowed_width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, windowed_height);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, !Headless());
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, Headless());
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    base_title = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());
    // gow3: the game's own icon (run.py converts sce_sys/icon0.png to BMP, SDL's built-in format).
    if (const char* icon_path = std::getenv("GOW3_WINDOW_ICON")) {
        if (SDL_Surface* icon = SDL_LoadBMP(icon_path)) {
            SDL_SetWindowIcon(window, icon);
            SDL_DestroySurface(icon);
        }
    }

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
#ifdef _WIN32
    if (driver && !std::strcmp(driver, "windows")) {
        window_info.type = WindowSystemType::Windows;
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    } else
#endif
    if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
    // gow3: GOW3_FULLSCREEN=1 (older launchers) still asks for fullscreen.
    if (const char* fullscreen = std::getenv("GOW3_FULLSCREEN"); !Headless() && fullscreen && fullscreen[0] == '1' &&
        Gow3Settings::Get().display_mode == Gow3Graphics::Windowed) {
        Gow3Settings::Get().display_mode = Gow3Graphics::Borderless;
    }
    ApplyDisplayMode(Gow3Settings::Get().display_mode);
    // gow3: the game has no mouse; the in-game menu draws its own cursor (Gow3Overlay::SetOpen).
    if (!Headless()) SDL_HideCursor();
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
}

// Windowed, borderless (desktop-sized, no mode change) or exclusive fullscreen at the desktop
// mode. Window thread only (SDL). The swapchain follows the size events.
void WindowSDL::ApplyDisplayMode(int mode) {
    if (Headless()) {
        display_mode = mode;
        return;
    }
    const bool was_fullscreen = display_mode > Gow3Graphics::Windowed;
    if (WindowPlacement().on) {
        mode = Gow3Graphics::Windowed; // GOW3_WINDOW_HALF / _RECT: always at its place
    }
    display_mode = mode;
    if (mode == Gow3Graphics::Windowed) {
        SDL_SetWindowFullscreen(window, false);
        SDL_SyncWindow(window);
        if (WindowPlacement().on) {
            const Placement& p = WindowPlacement();
            SDL_SetWindowBordered(window, !p.borderless);
            SDL_SetWindowSize(window, p.w, p.h);
            SDL_SetWindowPosition(window, p.x, p.y);
        } else if (was_fullscreen) { // back at the windowed size, centered
            SDL_SetWindowSize(window, windowed_width, windowed_height);
            SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
    } else {
        // Borderless: a desktop-sized window. Fullscreen: exclusive, in the desktop's resolution
        // at its refresh rate.
        const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(window));
        const bool exclusive = mode == Gow3Graphics::Fullscreen && desktop;
        if (!SDL_SetWindowFullscreenMode(window, exclusive ? desktop : nullptr)) {
            LOG_WARNING(Frontend, "Fullscreen mode: {}", SDL_GetError());
        }
        SDL_SetWindowFullscreen(window, true);
    }
    SDL_SyncWindow(window);
    if (Background()) {
        SendToBack(window);
    } else {
        SDL_RaiseWindow(window); // the focus: keyboard input, and no minimized exclusive fullscreen
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    std::printf("Window: %s, %dx%d\n", Gow3Graphics::DisplayModeNames[mode], w, h);
    std::fflush(stdout);
}

WindowSDL::~WindowSDL() {
    SDL_DestroyWindow(window);
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock{text_mutex};
    text = initial;
    text_prompt = prompt;
    text_state = 0;
    text_requested = true;
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

void WindowSDL::UpdateTextTitle() {
    const std::string title = text_active ? base_title + " \u2014 " + text_prompt + ": " + text + "_  (Enter = OK, Esc = cancel)"
                                          : base_title;
    SDL_SetWindowTitle(window, title.c_str());
    Gow3Overlay::SetTextEntry(text_active, text_prompt, text);
}

bool WindowSDL::PollEvents() {
#ifdef _WIN32
    {
        void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                            SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        // gow3 27 Oct 2026 (user: exclusive fullscreen, the keyboard works, the controller
        // doesn't): Windows' foreground window can be another one (an owned or overlay window)
        // while the game has the input. The keyboard focus counts too.
        const HWND foreground = GetForegroundWindow();
        const bool windows_front = !hwnd || foreground == static_cast<HWND>(hwnd) ||
                                   GetAncestor(foreground, GA_ROOTOWNER) == static_cast<HWND>(hwnd);
        const bool front = windows_front || SDL_GetKeyboardFocus() == window;
        window_foreground = front;
        static bool other_reported = false;
        if (front && !windows_front && !other_reported) {
            char name[128] = {};
            GetClassNameA(foreground, name, sizeof(name));
            std::printf("Input: another window is in front (class '%s') while the game has the "
                        "keyboard focus; the controller works\n", name);
            std::fflush(stdout);
        }
        other_reported = other_reported || (front && !windows_front);
        // Diagnosis: in front, but SDL without the keyboard focus (the state that used to stop
        // the controller); once per occurrence.
        static bool reported = false;
        const bool lost = front && SDL_GetKeyboardFocus() != window;
        if (lost && !reported) {
            std::printf("Input: the window is in front but SDL has no keyboard focus; the "
                        "controller keeps working\n");
            std::fflush(stdout);
        }
        reported = lost;
    }
#endif
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) { // SDL text input must be toggled from the window thread
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
    }
    if (!text_active) {
        Gow3Overlay::UpdateTextInput(window);
    }
    // Mouse look: captured while the game has the focus and neither the menu nor a text dialog
    // is open.
    Gow3Mouse::Update(window, !text_active && !Gow3Overlay::CapturesInput());
    // gow3 29 Oct 2026 (GitHub #300: no mouse cursor in the in-game menu): the menu's own drawn
    // cursor follows ImGui's last mouse position, which can be off screen after mouse look let
    // the pointer go. Windows' cursor is shown while the menu or a dialog takes the input,
    // placed in the middle of the window when it appears, and hidden again when the game has it.
    if (!Headless()) {
        static bool cursor_shown = false;
        const bool want_cursor = Gow3Overlay::CapturesInput();
        if (want_cursor != cursor_shown) {
            cursor_shown = want_cursor;
            if (want_cursor) {
                int w = 0, h = 0;
                SDL_GetWindowSize(window, &w, &h);
                SDL_WarpMouseInWindow(window, w * 0.5f, h * 0.5f);
                SDL_ShowCursor();
            } else {
                SDL_HideCursor();
            }
        }
    }
    if (const int mode = Gow3Settings::Get().display_mode; mode != display_mode) {
        ApplyDisplayMode(mode);
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (text_active && (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{text_mutex};
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                text += event.text.text;
            } else if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                size_t cut = text.size() - 1; // drop one UTF-8 code point
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
                text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE) {
                text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                text_active = false;
                SDL_StopTextInput(window);
            }
            UpdateTextTitle();
            continue;
        }
        // The controller finishes the text dialog too: Cross (A) accepts, Circle (B) cancels.
        if (text_active && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
            (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
             event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST)) {
            std::scoped_lock lock{text_mutex};
            text_state = event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : 2;
            text_active = false;
            SDL_StopTextInput(window);
            UpdateTextTitle();
            continue;
        }
        if (Gow3Mouse::OnEvent(event)) {
            continue;
        }
        if (Gow3Overlay::HandleEvent(event)) {
            continue;
        }
        switch (event.type) {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            break;
        }
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            is_open = false;
            break;
        default:
            break;
        }
    }
    return is_open;
}

} // namespace Frontend

extern "C" int gow3gpu_window_foreground(void) {
    return Frontend::window_foreground.load(std::memory_order_relaxed) ? 1 : 0;
}
