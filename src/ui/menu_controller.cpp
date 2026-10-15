// SecondSonPC — the host of the in-game settings menu (see menu_controller.h).
#include "ui/menu_controller.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <vector>

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <windows.h>

#include "base/log.h"
#include "input/device_tracker.h"
#include "input/keyboard_mouse.h"
#include "runtime/gpu/native_backend.h"
#include "runtime/pad/pad.h"
#include "settings/registry.h"
#include "settings/settings_service.h"
#include "ui/overlay_win32.h"
#include "ui/settings_menu.h"

namespace SecondSon::Ui {

namespace {

constexpr u64 kRepeatDelayNs = 400'000'000;
constexpr u64 kRepeatPeriodNs = 110'000'000;
constexpr u64 kRenderGapNs = 12'000'000; // at most ~80 renders/s: a dragged slider does not flood the main thread
constexpr u64 kChordNs = 1'000'000'000;  // L3 + R3 held this long opens the menu
constexpr unsigned kTimerMs = 50;

struct State {
    MenuController::Options opt;
    HWND hwnd = nullptr;
    Win32Overlay overlay;
    bool overlay_ok = false;
    std::string overlay_error;
    bool reported_unavailable = false;
    std::unique_ptr<SettingsMenu> menu;
    Input::DeviceTracker tracker;
    Input::Device pad_kind = Input::Device::OtherGamepad;
    std::string pad_name;
    u32 pad_held = 0;        // SDL gamepad buttons held (bits)
    bool block_until_release = false;
    u64 chord_since_ns = 0;  // L3 and R3 both held since
    u32 chord_event = 0;     // SDL user event type of the chord timer
    std::optional<Nav> held_nav;
    bool held_from_stick = false;
    u8 held_nav_button = 0xFF; // the controller button repeating held_nav
    u64 next_repeat_ns = 0;
    float stick_x = 0, stick_y = 0;
    bool dirty = false;
    u64 last_render_ns = 0;
    u64 closed_ns = 0;
    bool busy = false;             // inside an event/open/close: SDL calls may pump messages (the timer must wait)
    bool exclusive_lifted = false; // exclusive full screen turned borderless while the menu is shown
    std::vector<std::string> displays, audio_devices;
    std::vector<std::pair<u32, u32>> window_sizes;
};

/// Marks the main thread busy in the menu for a scope: WM_TIMER dispatched by SDL calls inside it only sets `dirty`.
struct BusyScope {
    bool was;
    BusyScope();
    ~BusyScope();
};

constexpr u64 kReleaseGuardNs = 2'000'000'000; // a held controller button keeps the game's pad blocked at most this long

State& S() {
    static State state;
    return state;
}

BusyScope::BusyScope() : was{S().busy} {
    S().busy = true;
}
BusyScope::~BusyScope() {
    S().busy = was;
}

/// The controller state the menu keeps is dropped (unplugged, focus lost, a missed button release).
void ForgetPadState() {
    auto& st = S();
    st.pad_held = 0;
    st.held_nav.reset();
    st.held_from_stick = false;
    st.held_nav_button = 0xFF;
    st.stick_x = st.stick_y = 0;
    if (st.block_until_release && !(st.menu && st.menu->IsOpen())) {
        st.block_until_release = false;
        Pad::SetMenuBlocksInput(false);
    }
}

u64 NowNs() {
    return SDL_GetTicksNS();
}

bool Ru(const App::Config& c) {
    return c.pc.ui_language == "ru" || (c.pc.ui_language == "auto" && c.language_id == 8);
}

Input::Device PadKind(SDL_JoystickID id) {
    switch (SDL_GetGamepadTypeForID(id)) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
        return Input::Device::PlayStation;
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE:
        return Input::Device::Xbox;
    default:
        return Input::Device::OtherGamepad;
    }
}

SDL_DisplayID FindDisplay(const std::string& name) {
    int n = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&n);
    SDL_DisplayID found = 0;
    for (int i = 0; ids != nullptr && i < n && !name.empty(); ++i) {
        const char* dn = SDL_GetDisplayName(ids[i]);
        if (dn != nullptr && name == dn) {
            found = ids[i];
            break;
        }
    }
    SDL_free(ids);
    return found != 0 ? found : SDL_GetPrimaryDisplay();
}

std::vector<std::string> DisplayNames() {
    std::vector<std::string> out;
    int n = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&n);
    for (int i = 0; ids != nullptr && i < n; ++i) {
        const char* dn = SDL_GetDisplayName(ids[i]);
        out.emplace_back(dn != nullptr ? dn : "?");
    }
    SDL_free(ids);
    return out;
}

std::vector<std::string> AudioDeviceNames() {
    std::vector<std::string> out;
    int n = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&n); // empty if SDL audio is not started
    for (int i = 0; ids != nullptr && i < n; ++i) {
        const char* dn = SDL_GetAudioDeviceName(ids[i]);
        if (dn != nullptr) {
            out.emplace_back(dn);
        }
    }
    SDL_free(ids);
    return out;
}

Input::GamepadTuning Tuning(const App::Config& c) {
    return {.camera_sensitivity = c.pc.camera_sensitivity,
            .invert_camera_x = c.pc.invert_camera_x,
            .invert_camera_y = c.pc.invert_camera_y,
            .vibration_percent = c.pc.vibration};
}

/// The window's mode, size and display as the settings say (main thread).
void ApplyDisplay(SDL_Window* w, const App::Config& c, bool move_display) {
    if (w == nullptr) {
        return;
    }
    const SDL_DisplayID target = FindDisplay(c.pc.monitor);
    const bool fullscreen_now = (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0;
    const int centred = static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(target));
    if (fullscreen_now && (move_display || !c.fullscreen)) {
        SDL_SetWindowFullscreen(w, false);
    }
    SDL_SetWindowSize(w, static_cast<int>(c.window_width), static_cast<int>(c.window_height));
    if (!c.fullscreen || move_display || !fullscreen_now) {
        SDL_SetWindowPosition(w, centred, centred);
    }
    if (c.fullscreen) {
        // borderless: a window over the desktop; exclusive: the display's own mode, as the bridge sets it at start
        SDL_SetWindowFullscreenMode(w, c.exclusive_fullscreen ? SDL_GetDesktopDisplayMode(target) : nullptr);
        SDL_SetWindowFullscreen(w, true);
    }
    SSPC_LOG(App, Info, "display: {} {}x{} on \"{}\"", c.fullscreen ? (c.exclusive_fullscreen ? "exclusive" : "borderless")
                                                                   : "window",
             c.window_width, c.window_height, c.pc.monitor.empty() ? std::string("primary") : c.pc.monitor);
}

MenuEnv BuildEnv() {
    auto& st = S();
    const auto cfg = Settings::Instance().Current();
    MenuEnv env;
    RECT cr{};
    if (st.hwnd != nullptr && GetClientRect(st.hwnd, &cr) && cr.right > 0 && cr.bottom > 0) {
        env.width = static_cast<float>(cr.right);
        env.height = static_cast<float>(cr.bottom);
    }
    env.ru = Ru(cfg);
    env.prompts = Input::ResolvePrompts(cfg.pc.prompts, st.tracker.Active());
    env.confirm_is_circle = !cfg.enter_button_cross;
    env.device_label = st.tracker.Active() == Input::Device::KeyboardMouse
                           ? std::string(env.ru ? "Клавиатура и мышь" : "Keyboard and mouse")
                           : (st.pad_name.empty() ? std::string(env.ru ? "Контроллер" : "Controller") : st.pad_name);
    const bool bridge = (st.opt.capabilities & Settings::Capability::BridgePresenter) != 0;
    env.backend_label = bridge ? std::string(env.ru ? "мост shadPS4 (по умолчанию)" : "shadPS4 bridge (default)")
                               : std::string(env.ru ? "собственный SecondSonPC (экспериментальный)"
                                                    : "SecondSonPC's own (experimental)");
    env.displays = st.displays;
    env.audio_devices = st.audio_devices;
    env.window_sizes = st.window_sizes;
    auto& service = Settings::Instance();
    if (service.File().empty()) {
        env.save_note = env.ru ? "Изменения в этом запуске не сохраняются (файл настроек недоступен)."
                               : "Changes are not saved in this run (the settings file is not available).";
    } else if (const auto error = service.LastSaveError(); !error.empty()) {
        env.save_note = (env.ru ? "Не удалось сохранить настройки: " : "Settings not saved: ") + error;
    }
    // P4.14: the native backend's shader cache
    if (const auto ci = Gpu::Native::CacheInfo(); ci.enabled) {
        const auto mib = [](u64 b) { return static_cast<double>(b) / (1 << 20); };
        env.shader_cache_size = fmt::format("{:.0f} + {:.0f} MiB (vulkan.bin) / {} MiB", mib(ci.file_bytes),
                                            mib(ci.vulkan_bytes), cfg.pc.shader_cache_max_mib);
        env.shader_cache_content =
            env.ru ? fmt::format("шейдеров: {}, конвейеров: {}", ci.modules, ci.pipelines)
                   : fmt::format("{} shaders, {} pipelines", ci.modules, ci.pipelines);
        if (ci.game_programs != 0) { // (the offline index: how much of the game the cache covers)
            env.shader_cache_content +=
                env.ru ? fmt::format("; программ игры: {} из {}", ci.cached_in_game, ci.game_programs)
                       : fmt::format("; game programs: {} of {}", ci.cached_in_game, ci.game_programs);
        }
        env.shader_cache_run =
            env.ru ? fmt::format("заранее {} из {} ({:.1f} с), из кэша: {}, новых: {}", ci.precompile_built,
                                 ci.precompile_total, ci.precompile_ms / 1000.0, ci.module_loads, ci.translations)
                   : fmt::format("prepared {} of {} ({:.1f} s), read {}, new {}", ci.precompile_built,
                                 ci.precompile_total, ci.precompile_ms / 1000.0, ci.module_loads, ci.translations);
    } else {
        env.shader_cache_size = env.ru ? "выключен (или бэкенд моста)" : "off (or the bridge's backend)";
    }
    env.now_ns = NowNs();
    return env;
}

std::vector<std::pair<u32, u32>> WindowSizes(SDL_Window* window) {
    SDL_Rect usable{0, 0, 7680, 4320};
    if (window != nullptr) {
        SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window), &usable);
    }
    constexpr std::pair<u32, u32> kSizes[] = {{1280, 720},  {1366, 768},  {1600, 900},  {1920, 1080},
                                              {2560, 1440}, {3200, 1800}, {3840, 2160}};
    std::vector<std::pair<u32, u32>> out;
    for (const auto& sz : kSizes) {
        if (static_cast<int>(sz.first) <= usable.w && static_cast<int>(sz.second) <= usable.h) {
            out.push_back(sz);
        }
    }
    if (out.empty()) {
        out.push_back(kSizes[0]);
    }
    return out;
}

/// Layered windows are not reliably shown over an exclusive full-screen swapchain: while the menu is open the window
/// is borderless full screen (the same display mode), and exclusive again when it closes.
void LiftExclusive(bool lift) {
    auto& st = S();
    SDL_Window* w = st.opt.window;
    if (w == nullptr) {
        return;
    }
    const bool fullscreen = (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0;
    const auto cfg = Settings::Instance().Current();
    if (lift && fullscreen && cfg.exclusive_fullscreen && !st.exclusive_lifted) {
        SDL_SetWindowFullscreenMode(w, nullptr);
        st.exclusive_lifted = true;
        SSPC_LOG(App, Info, "settings menu: exclusive full screen shown borderless while the menu is open");
    } else if (!lift && st.exclusive_lifted) {
        st.exclusive_lifted = false;
        if (fullscreen && cfg.fullscreen && cfg.exclusive_fullscreen) {
            SDL_SetWindowFullscreenMode(w, SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(w)));
        }
    }
}

void Render(bool force) {
    auto& st = S();
    if (!st.menu || !st.menu->IsOpen()) {
        return;
    }
    const u64 now = NowNs();
    if (!force && now - st.last_render_ns < kRenderGapNs) {
        st.dirty = true; // the timer renders it
        return;
    }
    st.menu->SetEnv(BuildEnv());
    DrawList list;
    if (const std::string cmd = st.menu->TakeCommand(); !cmd.empty()) {
        // P4.14: a clearing request for the next start (the cache is in use now: nothing is deleted while it runs)
        const auto cfg = Settings::Instance().Current();
        std::error_code ec;
        std::filesystem::create_directories(cfg.shader_cache_dir, ec);
        const auto file = cfg.shader_cache_dir /
                          (cmd == "cmd.clear_shaders" ? "clear_shaders.request" : "clear_pipelines.request");
        std::ofstream(file) << "requested in the settings menu\n";
        SSPC_LOG(App, Info, "settings menu: {} asked for the next start ({})", cmd, file.string());
    }
    st.menu->Build(st.overlay.Measure(), list);
    if (!st.overlay.Present(st.hwnd, list)) {
        // an open menu nobody can see would hold the game's input: closed, the game gets it back
        SSPC_LOG(App, Error, "settings menu: drawing failed - closed");
        Menu().Close();
        return;
    }
    st.last_render_ns = NowNs();
    st.dirty = false;
}

void AfterInput() {
    auto& st = S();
    if (st.menu && st.menu->TakeCloseRequest()) {
        Menu().Close();
        return;
    }
    Render(false);
}

void SendNav(Nav nav) {
    auto& st = S();
    st.menu->SetEnv(BuildEnv());
    st.menu->OnNav(nav);
}

void OnTimer(void*) {
    auto& st = S();
    if (!st.menu || !st.menu->IsOpen()) {
        return;
    }
    if (st.busy) {
        st.dirty = true; // dispatched from inside an SDL call of the menu's own handling: later
        return;
    }
    const BusyScope busy;
    const u64 now = NowNs();
    st.menu->Tick(now);
    if (st.held_nav && now >= st.next_repeat_ns) {
        SendNav(*st.held_nav);
        st.next_repeat_ns = now + kRepeatPeriodNs;
        st.dirty = true;
    }
    if (st.menu->TakeCloseRequest()) {
        Menu().Close();
        return;
    }
    if (st.dirty || st.menu->NeedsTicks()) {
        Render(true);
    }
}

std::optional<Nav> KeyNav(SDL_Scancode sc, bool shift) {
    switch (sc) {
    case SDL_SCANCODE_UP:
    case SDL_SCANCODE_W:
        return Nav::Up;
    case SDL_SCANCODE_DOWN:
    case SDL_SCANCODE_S:
        return Nav::Down;
    case SDL_SCANCODE_LEFT:
    case SDL_SCANCODE_A:
        return Nav::Left;
    case SDL_SCANCODE_RIGHT:
    case SDL_SCANCODE_D:
        return Nav::Right;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
    case SDL_SCANCODE_SPACE:
        return Nav::Accept;
    case SDL_SCANCODE_ESCAPE:
        return Nav::Back;
    case SDL_SCANCODE_DELETE:
        return Nav::Reset;
    case SDL_SCANCODE_BACKSPACE:
        return Nav::Clear;
    case SDL_SCANCODE_TAB:
        return shift ? Nav::PrevTab : Nav::NextTab;
    case SDL_SCANCODE_Q:
        return Nav::PrevTab;
    case SDL_SCANCODE_E:
        return Nav::NextTab;
    case SDL_SCANCODE_PAGEUP:
        return Nav::PageUp;
    case SDL_SCANCODE_PAGEDOWN:
        return Nav::PageDown;
    default:
        return std::nullopt;
    }
}

bool Repeatable(Nav nav) {
    return nav == Nav::Up || nav == Nav::Down || nav == Nav::Left || nav == Nav::Right || nav == Nav::PageUp ||
           nav == Nav::PageDown;
}

std::optional<Nav> PadNav(u8 button, bool confirm_is_circle) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        return Nav::Up;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        return Nav::Down;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        return Nav::Left;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        return Nav::Right;
    case SDL_GAMEPAD_BUTTON_SOUTH:
        return confirm_is_circle ? Nav::Back : Nav::Accept;
    case SDL_GAMEPAD_BUTTON_EAST:
        return confirm_is_circle ? Nav::Accept : Nav::Back;
    case SDL_GAMEPAD_BUTTON_WEST:
        return Nav::Reset;
    case SDL_GAMEPAD_BUTTON_NORTH:
        return Nav::Clear;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        return Nav::PrevTab;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        return Nav::NextTab;
    default:
        return std::nullopt;
    }
}

/// SDL window coordinates -> client pixels (the overlay and the menu work in pixels).
std::pair<float, float> ToPixels(float x, float y) {
    auto& st = S();
    int ww = 0, wh = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(st.opt.window, &ww, &wh);
    SDL_GetWindowSizeInPixels(st.opt.window, &pw, &ph);
    const float sx = ww > 0 ? static_cast<float>(pw) / static_cast<float>(ww) : 1.0f;
    const float sy = wh > 0 ? static_cast<float>(ph) / static_cast<float>(wh) : 1.0f;
    return {x * sx, y * sy};
}

void ToggleBool(std::string_view id) {
    auto& service = Settings::Instance();
    if (const auto* d = Settings::Find(id); d != nullptr && service.Available(*d).available) {
        service.Set(*d, Settings::Value{!std::get<bool>(service.Get(*d))});
    }
}

bool Available(std::string_view id) {
    const auto* d = Settings::Find(id);
    return d != nullptr && Settings::Instance().Available(*d).available;
}

void TrackPad(const SDL_Event& e, bool down) {
    auto& st = S();
    const u8 b = e.gbutton.button;
    if (b < 32) {
        st.pad_held = down ? (st.pad_held | (1u << b)) : (st.pad_held & ~(1u << b));
    }
    if (down) {
        st.pad_kind = PadKind(e.gbutton.which);
        const char* name = SDL_GetGamepadNameForID(e.gbutton.which);
        st.pad_name = name != nullptr ? name : "";
        st.tracker.PadButton(st.pad_kind, NowNs());
    }
    if (st.block_until_release && st.pad_held == 0 && !(st.menu && st.menu->IsOpen())) {
        st.block_until_release = false;
        Pad::SetMenuBlocksInput(false); // the button that closed the menu is up: the game gets the pad again
    }
}

u32 ChordTimer(void*, SDL_TimerID, u32) {
    SDL_Event ev{};
    ev.type = S().chord_event;
    SDL_PushEvent(&ev);
    return 0; // once
}

} // namespace

bool ShowStartupProgress(u32 done, u32 total, u32 failed, double elapsed_s, bool finished) {
    auto& st = S();
    if (!st.overlay_ok || st.hwnd == nullptr) {
        return false;
    }
    if (finished) {
        st.overlay.Hide();
        return false;
    }
    // the start waits on the window's thread: its messages are pumped here (SDL queues its events for later)
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    RECT cr{};
    if (!GetClientRect(st.hwnd, &cr) || cr.right <= 0 || cr.bottom <= 0) {
        return false;
    }
    const float w = static_cast<float>(cr.right), h = static_cast<float>(cr.bottom);
    const float s = std::max(h / 1080.0f, 0.5f);
    const bool ru = Settings::Instance().Initialized() ? Ru(Settings::Instance().Current()) : true;
    const Rect panel{(w - 640 * s) / 2, (h - 170 * s) / 2, 640 * s, 170 * s};
    DrawList list;
    list.Reset(w, h);
    list.SetSurface(panel);
    list.SetDim(96);
    list.FillRect(panel, {22, 24, 30, 240}, 14 * s);
    list.StrokeRect(panel, {255, 255, 255, 40}, 1.0f * s, 14 * s);
    const float x = panel.x + 28 * s, iw = panel.w - 56 * s;
    list.Text({x, panel.y + 20 * s, iw, 34 * s}, ru ? "Подготовка шейдеров" : "Preparing shaders", 26 * s,
              {255, 255, 255, 255}, Align::Left, FontWeight::Semibold);
    const u32 pct = total != 0 ? static_cast<u32>(u64{done} * 100 / total) : 100;
    const auto group = [](u32 v) { // 1 842 (a narrow space between thousands)
        std::string t = std::to_string(v);
        for (int i = static_cast<int>(t.size()) - 3; i > 0; i -= 3) {
            t.insert(static_cast<size_t>(i), "\xE2\x80\xAF");
        }
        return t;
    };
    const std::string line = (ru ? std::string("Подготовлено ") : std::string("Prepared ")) + group(done) + " / " +
                             group(total) + " (" + std::to_string(pct) + "%)";
    list.Text({x, panel.y + 62 * s, iw, 26 * s}, line, 20 * s, {225, 228, 235, 255});
    const Rect bar{x, panel.y + 96 * s, iw, 10 * s};
    list.FillRect(bar, {255, 255, 255, 36}, 5 * s);
    list.FillRect({bar.x, bar.y, bar.w * static_cast<float>(pct) / 100.0f, bar.h}, {88, 166, 255, 255}, 5 * s);
    std::string hint = ru ? "Компиляция графических конвейеров… Esc — начать игру сейчас (остальное — в фоне)"
                          : "Compiling graphics pipelines… Esc - start the game now (the rest in the background)";
    if (failed != 0) {
        hint += (ru ? " · ошибок " : " · failed ") + std::to_string(failed);
    }
    (void)elapsed_s;
    list.Text({x, panel.y + 120 * s, iw, 36 * s}, hint, 15 * s, {170, 176, 190, 255}, Align::Left, FontWeight::Regular,
              true);
    st.overlay.Present(st.hwnd, list);
    // Esc while the game window is in front (pressed since the last call)
    return GetForegroundWindow() == st.hwnd && (GetAsyncKeyState(VK_ESCAPE) & 0x8001) != 0; // (down now, or since)
}

MenuController& Menu() {
    static MenuController controller;
    return controller;
}

void MenuController::Install(const Options& options) {
    auto& st = S();
    st.opt = options;
    if (options.test_mode || options.window == nullptr) {
        SSPC_LOG(App, Info, "settings menu: off ({})", options.test_mode ? "automated run" : "no window");
        return;
    }
    st.hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(options.window),
                                                        SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    st.overlay_ok = st.hwnd != nullptr && st.overlay.Init(st.overlay_error);
    if (st.hwnd == nullptr) {
        st.overlay_error = "no Win32 window handle";
    }
    st.overlay.SetTimer(&OnTimer, nullptr, kTimerMs);
    st.menu = std::make_unique<SettingsMenu>(Settings::Instance());
    st.chord_event = SDL_RegisterEvents(1);
    auto& service = Settings::Instance();
    service.SetCapabilities(options.capabilities);
    // Appliers SecondSonPC owns: keyboard/mouse, real controllers, the window.
    service.AddApplier([](const App::Config& c, std::span<const Settings::Desc* const> changed) {
        bool input = false, pad = false, display = false, monitor = false;
        for (const auto* d : changed) {
            input = input || d->category == Settings::Category::Controls || d->category == Settings::Category::Keyboard;
            pad = pad || d->category == Settings::Category::Gamepad;
            const bool window = d->id == "display.fullscreen" || d->id == "display.fullscreenMode" ||
                                d->id == "display.width" || d->id == "display.height" || d->id == "display.monitor";
            display = display || window;
            monitor = monitor || d->id == "display.monitor";
        }
        if (input) {
            Input::Player1().ApplySettings(c.input);
        }
        if (pad) {
            Pad::SetGamepadTuning(Tuning(c));
        }
        if (display) {
            ApplyDisplay(S().opt.window, c, monitor);
            if (Menu().IsOpen()) {
                S().exclusive_lifted = false; // a mode chosen in the menu: borderless again until it closes
                LiftExclusive(true);
            }
        }
    });
    const auto cfg = service.Current();
    Pad::SetGamepadTuning(Tuning(cfg));
    if (!cfg.pc.monitor.empty()) {
        ApplyDisplay(options.window, cfg, true); // the window opens on the chosen display
    }
    installed_ = true;
    if (st.overlay_ok) {
        SSPC_LOG(App, Info, "settings menu: F1{} (overlay: Win32 layered window, Direct2D software; capabilities 0x{:x})",
                 cfg.pc.menu_button == "guide"  ? ", controller PS/Guide"
                 : cfg.pc.menu_button == "l3r3" ? ", controller L3+R3 held"
                                                : "",
                 options.capabilities);
    } else {
        SSPC_LOG(App, Error, "settings menu: cannot be shown ({}); hotkeys F10/F11 still work", st.overlay_error);
    }
}

bool MenuController::IsOpen() const {
    const auto& st = S();
    return st.menu && st.menu->IsOpen();
}

void MenuController::Open() {
    auto& st = S();
    if (!installed_ || IsOpen()) {
        return;
    }
    if (!st.overlay_ok) {
        if (!st.reported_unavailable) {
            st.reported_unavailable = true;
            SSPC_LOG(App, Warning, "settings menu not shown: {}", st.overlay_error);
        }
        return;
    }
    const BusyScope busy;
    st.displays = DisplayNames();
    st.audio_devices = AudioDeviceNames();
    st.window_sizes = WindowSizes(st.opt.window);
    st.held_nav.reset();
    st.menu->SetEnv(BuildEnv());
    st.menu->Open();
    Input::Player1().SetMenuOpen(true);
    st.block_until_release = false;
    Pad::SetMenuBlocksInput(true);
    LiftExclusive(true);
    Render(true);
    if (IsOpen()) {
        SSPC_LOG(App, Info, "settings menu open (overlay render {:.1f} ms)", st.overlay.GetStats().last_ms);
    }
}

void MenuController::Close() {
    auto& st = S();
    if (!IsOpen()) {
        return;
    }
    const BusyScope busy;
    st.menu->Close();
    st.overlay.Hide();
    st.held_nav.reset();
    st.held_nav_button = 0xFF;
    LiftExclusive(false);
    Input::Player1().SetMenuOpen(false);
    st.closed_ns = NowNs();
    if (st.pad_held != 0) {
        st.block_until_release = true; // until the controller button that closed it is up (at most kReleaseGuardNs)
    } else {
        Pad::SetMenuBlocksInput(false);
    }
    const auto stats = st.overlay.GetStats();
    SSPC_LOG(App, Info, "settings menu closed ({} renders, slowest {:.1f} ms); settings file {}", stats.frames,
             stats.max_ms, Settings::Instance().File().empty() ? std::string("not used")
                                                               : Settings::Instance().File().string());
}

EventRoute MenuController::OnEvent(const SDL_Event& e) {
    if (!installed_) {
        return EventRoute::Normal;
    }
    auto& st = S();
    const BusyScope busy;
    const u64 now = NowNs();
    const bool open = IsOpen();
    if (st.block_until_release && !open && now - st.closed_ns > kReleaseGuardNs) {
        ForgetPadState(); // a release that never came (unplugged, missed event): the game gets the pad back
    }

    // ---- the chord timer (L3 + R3)
    if (st.chord_event != 0 && e.type == st.chord_event) {
        const u32 both = (1u << SDL_GAMEPAD_BUTTON_LEFT_STICK) | (1u << SDL_GAMEPAD_BUTTON_RIGHT_STICK);
        if (!open && (st.pad_held & both) == both && now - st.chord_since_ns >= kChordNs - 50'000'000) {
            Open();
        }
        return EventRoute::Consumed;
    }

    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = e.type == SDL_EVENT_KEY_DOWN;
        const auto sc = e.key.scancode;
        if (down) {
            st.tracker.KeyOrMouseButton(now);
        }
        if (!open) {
            if (!down || e.key.repeat) {
                return EventRoute::Normal;
            }
            if (sc == SDL_SCANCODE_F1) {
                Open();
                return EventRoute::Consumed;
            }
            // F10/F11 through the settings: the choice is kept for the next start
            if (sc == SDL_SCANCODE_F10 && Available("display.showFps")) {
                ToggleBool("display.showFps");
                return EventRoute::Consumed;
            }
            if (sc == SDL_SCANCODE_F11 && Available("display.fullscreen")) {
                ToggleBool("display.fullscreen");
                return EventRoute::Consumed;
            }
            return EventRoute::Normal;
        }
        if (down) {
            if (sc == SDL_SCANCODE_F1 && !e.key.repeat) {
                Close();
                return EventRoute::Consumed;
            }
            st.menu->SetEnv(BuildEnv());
            if (st.menu->Capturing()) {
                if (!e.key.repeat) {
                    if (sc == SDL_SCANCODE_ESCAPE) {
                        st.menu->OnNav(Nav::Back);
                    } else {
                        st.menu->OnCaptureInput({Input::InputCode::Kind::Key, static_cast<u16>(sc)});
                    }
                }
            } else if (const auto nav = KeyNav(sc, (e.key.mod & SDL_KMOD_SHIFT) != 0);
                       nav && (!e.key.repeat || Repeatable(*nav))) {
                st.menu->OnNav(*nav);
            }
            AfterInput();
        }
        return EventRoute::Consumed;
    }
    case SDL_EVENT_MOUSE_MOTION:
        st.tracker.MouseMotion(e.motion.xrel, e.motion.yrel, now);
        if (!open) {
            return EventRoute::Normal;
        }
        {
            const auto [x, y] = ToPixels(e.motion.x, e.motion.y);
            st.menu->OnMouseMove(x, y);
            Render(false);
        }
        return EventRoute::Consumed;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        const bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        if (down) {
            st.tracker.KeyOrMouseButton(now);
        }
        if (!open) {
            return EventRoute::Normal;
        }
        st.menu->SetEnv(BuildEnv());
        const auto [x, y] = ToPixels(e.button.x, e.button.y);
        if (st.menu->Capturing()) {
            if (down) {
                st.menu->OnCaptureInput({Input::InputCode::Kind::MouseButton, e.button.button});
            }
        } else if (e.button.button == SDL_BUTTON_LEFT) {
            st.menu->OnMouseButton(x, y, down);
        }
        AfterInput();
        return EventRoute::Consumed;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!open) {
            return EventRoute::Normal;
        }
        st.menu->SetEnv(BuildEnv());
        {
            const float flip = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
            const float wy = e.wheel.y * flip, wx = e.wheel.x * flip;
            if (wy == 0.0f && wx == 0.0f) {
                return EventRoute::Consumed;
            }
            if (st.menu->Capturing()) {
                const u16 dir = wy > 0 ? 0 : wy < 0 ? 1 : wx < 0 ? 2 : 3;
                st.menu->OnCaptureInput({Input::InputCode::Kind::MouseWheel, dir});
            } else {
                st.menu->OnWheel(wy);
            }
        }
        AfterInput();
        return EventRoute::Consumed;
    case SDL_EVENT_GAMEPAD_REMOVED:
        ForgetPadState(); // its held buttons will never be released
        return EventRoute::Normal;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        ForgetPadState(); // controller events stop in the background: a release would be missed
        if (open && st.menu->Capturing()) {
            st.menu->OnNav(Nav::Back); // a key capture does not survive Alt+Tab
            Render(true);
        }
        return open ? EventRoute::BridgeOnly : EventRoute::Normal;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        return open ? EventRoute::BridgeOnly : EventRoute::Normal; // the menu keeps the cursor free
    case SDL_EVENT_WINDOW_MOVED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
        if (open) {
            Render(true);
        }
        return EventRoute::Normal;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        TrackPad(e, true);
        const u8 b = e.gbutton.button;
        const auto cfg = Settings::Instance().Current();
        if (!open) {
            if (b == SDL_GAMEPAD_BUTTON_GUIDE && cfg.pc.menu_button == "guide") {
                Open();
                return EventRoute::BridgeOnly;
            }
            const u32 both = (1u << SDL_GAMEPAD_BUTTON_LEFT_STICK) | (1u << SDL_GAMEPAD_BUTTON_RIGHT_STICK);
            if (cfg.pc.menu_button == "l3r3" && (st.pad_held & both) == both &&
                (b == SDL_GAMEPAD_BUTTON_LEFT_STICK || b == SDL_GAMEPAD_BUTTON_RIGHT_STICK)) {
                st.chord_since_ns = now;
                SDL_AddTimer(static_cast<u32>(kChordNs / 1'000'000), &ChordTimer, nullptr);
            }
            return EventRoute::Normal;
        }
        if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_GUIDE) {
            Close();
            return EventRoute::BridgeOnly;
        }
        st.menu->SetEnv(BuildEnv());
        if (const auto nav = PadNav(b, !cfg.enter_button_cross)) {
            st.menu->OnNav(*nav);
            if (Repeatable(*nav)) {
                st.held_nav = *nav;
                st.held_from_stick = false;
                st.held_nav_button = b;
                st.next_repeat_ns = now + kRepeatDelayNs;
            }
        }
        AfterInput();
        return EventRoute::BridgeOnly; // the bridge keeps the controller's state; the game reads "intercepted"
    }
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        TrackPad(e, false);
        if (open && st.held_nav && !st.held_from_stick && e.gbutton.button == st.held_nav_button) {
            st.held_nav.reset(); // only the button that repeats stops the repeat
            st.held_nav_button = 0xFF;
        }
        return open ? EventRoute::BridgeOnly : EventRoute::Normal;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const float v = static_cast<float>(e.gaxis.value) / 32767.0f;
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
            st.stick_x = v;
        } else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            st.stick_y = v;
        } else {
            if (std::abs(v) >= Input::DeviceTracker::kStickThreshold) {
                st.tracker.PadAxis(PadKind(e.gaxis.which), std::abs(v), now);
            }
            return open ? EventRoute::BridgeOnly : EventRoute::Normal;
        }
        st.tracker.PadAxis(PadKind(e.gaxis.which), std::max(std::abs(st.stick_x), std::abs(st.stick_y)), now);
        if (!open) {
            return EventRoute::Normal;
        }
        std::optional<Nav> dir;
        if (std::abs(st.stick_y) > 0.6f && std::abs(st.stick_y) >= std::abs(st.stick_x)) {
            dir = st.stick_y < 0 ? Nav::Up : Nav::Down;
        } else if (std::abs(st.stick_x) > 0.6f) {
            dir = st.stick_x < 0 ? Nav::Left : Nav::Right;
        }
        if (dir && (!st.held_nav || *st.held_nav != *dir || !st.held_from_stick)) {
            st.menu->SetEnv(BuildEnv());
            st.menu->OnNav(*dir);
            st.held_nav = dir;
            st.held_from_stick = true;
            st.next_repeat_ns = now + kRepeatDelayNs;
            AfterInput();
        } else if (!dir && st.held_from_stick) {
            st.held_nav.reset();
        }
        return EventRoute::BridgeOnly;
    }
    default:
        return EventRoute::Normal;
    }
}

} // namespace SecondSon::Ui
