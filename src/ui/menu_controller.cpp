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
#include "engine/render/render_resolution.h"
#include "engine/ui/game_menu.h"
#include "input/device_tracker.h"
#include "input/keyboard_mouse.h"
#include "runtime/gpu/menu_focus.h"
#include "runtime/gpu/native_backend.h"
#include "runtime/gpu/upscale/upscale_settings.h"
#include "runtime/pad/pad.h"
#include "settings/registry.h"
#include "settings/settings_service.h"
#include "ui/overlay_win32.h"
#include "ui/prompt_service.h"
#include "ui/settings_menu.h"

namespace SecondSon::Ui {

namespace {

constexpr u64 kRepeatDelayNs = 400'000'000;
constexpr u64 kRepeatPeriodNs = 110'000'000;
constexpr u64 kRenderGapNs = 12'000'000; // at most ~80 renders/s: a dragged slider does not flood the main thread
constexpr u64 kChordNs = 1'000'000'000;  // L3 + R3 held this long opens the menu
constexpr unsigned kTimerMs = 50;
constexpr u32 kGamePollMs = 33; // how often the game's own menus are looked at (pause, its Settings screen)
// The game's Settings screen (1920x1080 frame, measured 2026-10-15): the list's items, their values, the row pitch
constexpr float kListLeft = 330, kListRight = 1000, kListTop = 215, kListBottom = 990;
constexpr float kValueLeft = 690;  // values / volume bars start here (a click there: left / right)
constexpr float kValueMiddle = 838;
constexpr float kRowPitch = 35;    // between two items of a section
constexpr float kLastRowY = 959;   // the list's last item when it is scrolled to its end
constexpr float kThumbAtEnd = 955; // the scroll bar's thumb ends below this: the list's end is shown
constexpr u64 kHoverRestNs = 90'000'000; // the pointer rests this long before the focus follows it
constexpr int kMaxBurst = 6;       // d-pad presses at once before the focus is looked at again

struct State {
    MenuController::Options opt;
    HWND hwnd = nullptr;
    Win32Overlay overlay;
    bool overlay_ok = false;
    std::string overlay_error;
    bool reported_unavailable = false;
    std::unique_ptr<SettingsMenu> menu;
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
    std::vector<std::string> display_labels; // R11: the displays as the Monitor list shows them
    std::string primary_label;
    std::string present_now; // R11: the presenter's mode the menu last saw (redrawn when it changes)
    std::vector<std::pair<u32, u32>> window_sizes;
    std::string fsr_why; // FSR 3.1 cannot run: its DLL is not next to the exe (looked at when the menu opens)
    // PC Edition: the PC settings are an entry of the game's own Settings screen (Esc / Options during play)
    u32 poll_event = 0;          // SDL user event of the game-menu poll timer
    u32 prompt_event = 0;        // SDL user event of the button icons' timer (ui/prompt_service.h)
    bool dev_f1 = false;         // SSPC_DEV_F1=1 (development): F1 opens the menu anywhere
    bool game_menu_open = false; // one of the game's pausing screens is shown
    bool entry_shown = false;    // the game's Settings screen is in front (its "Additional settings" item opens ours)
    bool hover_pending = false;  // the pointer moved over the list: the focus follows once it rests
    float hover_target = 0;      // frame y
    u64 hover_since_ns = 0;
    // the mouse in the game's Settings list: its focus (the yellow row, Gpu::MenuFocus) is moved under the cursor
    bool nav_active = false;
    bool nav_click = false;      // then pressed (nav_button) once it is there
    u32 nav_button = 0;
    float nav_target = 0;        // frame y
    int nav_rounds = 0;
    bool nav_draining = false;   // presses queued: wait for them to be read
    u64 nav_wait_seq = 0;        // then for a frame read after them
    float nav_last_center = -1;
    int nav_last_dir = 0;
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
        break;
    }
    // types SDL does not name (a generic mapping): by maker - Sony, Microsoft
    switch (SDL_GetGamepadVendorForID(id)) {
    case 0x054C:
        return Input::Device::PlayStation;
    case 0x045E:
        return Input::Device::Xbox;
    default:
        return Input::Device::OtherGamepad;
    }
}

/// The game window's input feeds the device tracker (the game's button icons and the menu's hints): keys, mouse
/// buttons and the wheel at once, mouse movement past a threshold, controller buttons and sticks/triggers past half
/// way - not while the window is in the background (controllers report there too), not a controller's first second
/// (connecting one is not using it), never its motion sensors.
void FeedDevices(const SDL_Event& e, u64 now) {
    auto& d = Input::Devices();
    const auto& st = S();
    const bool focused = st.opt.test_mode || st.opt.window == nullptr ||
                         (SDL_GetWindowFlags(st.opt.window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_WHEEL:
        d.KeyOrMouseButton(now);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        d.MouseMotion(e.motion.xrel, e.motion.yrel, now);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        d.PadConnected(now);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (focused) {
            d.PadButton(PadKind(e.gbutton.which), now);
        }
        break;
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
        if (focused) {
            d.PadButton(PadKind(e.gtouchpad.which), now);
        }
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (focused) {
            d.PadAxis(PadKind(e.gaxis.which), std::abs(static_cast<double>(e.gaxis.value) / 32767.0), now);
        }
        break;
    default:
        break;
    }
}

u32 PromptTimer(void*, SDL_TimerID, u32 interval) {
    SDL_Event ev{};
    ev.type = S().prompt_event;
    SDL_PushEvent(&ev);
    return interval;
}

/// The system's displays with the names [Display] monitor stores: SDL's name, and for a second display of the same name
/// (two monitors of one model) "<name> (2)", "<name> (3)"... in SDL's order.
std::vector<std::pair<SDL_DisplayID, std::string>> Displays() {
    std::vector<std::pair<SDL_DisplayID, std::string>> out;
    int n = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&n);
    for (int i = 0; ids != nullptr && i < n; ++i) {
        const char* dn = SDL_GetDisplayName(ids[i]);
        std::string name = dn != nullptr ? dn : "?";
        const auto same = std::ranges::count_if(out, [&](const auto& d) {
            return d.second == name || d.second.starts_with(name + " (");
        });
        out.emplace_back(ids[i], same == 0 ? name : fmt::format("{} ({})", name, same + 1));
    }
    SDL_free(ids);
    return out;
}

SDL_DisplayID FindDisplay(const std::string& name) {
    if (!name.empty()) {
        for (const auto& [id, n] : Displays()) {
            if (n == name) {
                return id;
            }
        }
    }
    return SDL_GetPrimaryDisplay();
}

/// R11: a display as the Monitor list shows it: "<name> — 1920 × 1080, 165 Hz (primary)".
std::string DisplayLabel(SDL_DisplayID id, const std::string& name, bool ru, bool mark_primary = true) {
    const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(id);
    std::string label = name;
    if (m != nullptr && m->w > 0 && m->h > 0) {
        // (the desktop mode in pixels: SDL's w/h are in points with a display scale)
        const int w = static_cast<int>(std::lround(m->w * (m->pixel_density > 0 ? m->pixel_density : 1.0f)));
        const int h = static_cast<int>(std::lround(m->h * (m->pixel_density > 0 ? m->pixel_density : 1.0f)));
        label += m->refresh_rate > 0 ? fmt::format(" — {} × {}, {:.0f} {}", w, h, m->refresh_rate, ru ? "Гц" : "Hz")
                                     : fmt::format(" — {} × {}", w, h);
    }
    if (mark_primary && id == SDL_GetPrimaryDisplay()) {
        label += ru ? " (основной)" : " (primary)";
    }
    return label;
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

Rect GameFrame();

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
    if (st.hwnd != nullptr) {
        if (const UINT dpi = GetDpiForWindow(st.hwnd); dpi >= 96) {
            env.dpi_scale = static_cast<float>(dpi) / 96.0f; // the system's scale for this window's display
        }
    }
    env.prompts = Input::ResolvePrompts(cfg.pc.prompts, Input::Devices().Active());
    env.confirm_is_circle = !cfg.enter_button_cross;
    env.device_label = Input::Devices().Active() == Input::Device::KeyboardMouse
                           ? std::string(env.ru ? "Клавиатура и мышь" : "Keyboard and mouse")
                           : (st.pad_name.empty() ? std::string(env.ru ? "Контроллер" : "Controller") : st.pad_name);
    const bool bridge = (st.opt.capabilities & Settings::Capability::BridgePresenter) != 0;
    env.backend_label = bridge ? std::string(env.ru ? "мост shadPS4 (--gpu-backend bridge)"
                                                    : "shadPS4 bridge (--gpu-backend bridge)")
                               : std::string(env.ru ? "собственный Vulkan SecondSonPC" : "SecondSonPC's own Vulkan");
    env.displays = st.displays;
    env.display_labels = st.display_labels;
    env.primary_label = st.primary_label;
    if (st.opt.window != nullptr) {
        if (const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(st.opt.window));
            m != nullptr && m->w > 0 && m->h > 0) {
            const float density = m->pixel_density > 0 ? m->pixel_density : 1.0f;
            env.display_size = {static_cast<u32>(std::lround(m->w * density)),
                                static_cast<u32>(std::lround(m->h * density))};
        }
    }
    if (!bridge) {
        // R11: the vertical sync row - the present modes of the presenter's surface and the swapchain's
        const auto pm = Gpu::Native::GetPresentModes();
        env.present_known = pm.known;
        env.present_immediate = pm.immediate;
        env.present_mailbox = pm.mailbox;
        env.present_supported = pm.supported;
        env.present_now = pm.current;
    }
    env.audio_devices = st.audio_devices;
    env.window_sizes = st.window_sizes;
    auto& service = Settings::Instance();
    if (service.File().empty()) {
        env.save_note = env.ru ? "Изменения в этом запуске не сохраняются (файл настроек недоступен)."
                               : "Changes are not saved in this run (the settings file is not available).";
    } else if (const auto error = service.LastSaveError(); !error.empty()) {
        env.save_note = (env.ru ? "Не удалось сохранить настройки: " : "Settings not saved: ") + error;
    }
    if (!service.File().empty()) {
        const auto u8 = service.File().u8string(); // (UTF-8: a user name outside the ANSI code page)
        env.settings_file.assign(reinterpret_cast<const char*>(u8.data()), u8.size());
    }
    // upscaling (the native backend): the scene's size now, why a technology cannot run here
    if (!bridge) {
        namespace Up = Gpu::Upscale;
        const auto us = Up::GetStatus();
        u32 rw = 0, rh = 0;
        Up::GetRenderSize(rw, rh);
        const Engine::Render::RenderSize ds = Engine::Render::DisplaySize();
        if (rw != 0 && rh != 0 && (rw != ds.width || rh != ds.height)) {
            env.render_label = fmt::format("{} × {} → {} × {}", rw, rh, ds.width, ds.height);
        } else {
            env.render_label = fmt::format("{} × {} ({})", ds.width, ds.height,
                                           env.ru ? "без масштабирования" : "native");
        }
        env.dlss_why = us.dlss_why;
        env.fsr_fg_why = us.fsr_fg_why;
        env.fsr_why = st.fsr_why;
        if (cfg.pc.upscaler != "off" && us.active == Up::Backend::Off && !us.reason.empty()) {
            env.upscale_reason = us.reason;
        }
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
    if ((Settings::Instance().Capabilities() & Settings::Capability::PresentModeSwitch) != 0) {
        // R11: the vertical sync row's "Now:" - the presenter switches at its next present, after the render of the
        // change: drawn again once its swapchain's mode is another
        const std::string now_mode = Gpu::Native::GetPresentModes().current;
        st.dirty = st.dirty || now_mode != st.present_now;
        st.present_now = now_mode;
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
    }
    if (st.block_until_release && st.pad_held == 0 && !(st.menu && st.menu->IsOpen())) {
        st.block_until_release = false;
        Pad::SetMenuBlocksInput(false); // the button that closed the menu is up: the game gets the pad again
    }
}

u32 GamePollTimer(void*, SDL_TimerID, u32 interval) {
    SDL_Event ev{};
    ev.type = S().poll_event;
    SDL_PushEvent(&ev);
    return interval;
}

/// The game's picture inside the window's client area (the presenter keeps its proportions, centred).
Rect GameFrame() {
    RECT cr{};
    const Engine::Render::RenderSize ds = Engine::Render::DisplaySize();
    if (S().hwnd == nullptr || !GetClientRect(S().hwnd, &cr) || cr.right <= 0 || cr.bottom <= 0 || ds.width == 0 ||
        ds.height == 0) {
        return {};
    }
    const float aspect = static_cast<float>(ds.width) / static_cast<float>(ds.height);
    const float pw = static_cast<float>(cr.right), ph = static_cast<float>(cr.bottom);
    const float fw = std::min(pw, ph * aspect), fh = fw / aspect;
    return {std::floor((pw - fw) / 2), std::floor((ph - fh) / 2), fw, fh};
}

/// The game's Settings list shows its last item, "Additional settings" (the PC Edition's text for the PC-less HDR
/// option, runtime/filesystem/content_patch.h), chosen: the list is at its end (the scroll bar's thumb at the bottom)
/// and the yellow row is the last row (frame y ~958).
bool AdditionalItemChosen(const Gpu::MenuFocus::Row& r) {
    if (!r.valid || !r.found || !r.bar || r.thumb_bottom < kThumbAtEnd) {
        return false;
    }
    const float center = (r.top + r.bottom) / 2;
    return std::abs(center - kLastRowY) <= kRowPitch / 2;
}

/// Client pixels -> the game's 1920x1080 menu layout: scaled by the height and centred across a picture that is not
/// 16:9 (docs/RESOLUTION_RESEARCH.md).
std::pair<float, float> ToFrame(float x, float y) {
    const Rect f = GameFrame();
    if (f.w <= 0 || f.h <= 0) {
        return {-1, -1};
    }
    const float s = 1080.0f / f.h;
    return {(x - f.x) * s - (f.w * s - 1920.0f) / 2, (y - f.y) * s};
}

bool InList(float fx, float fy) {
    return fx >= kListLeft && fx < kListRight && fy >= kListTop && fy < kListBottom;
}

void NavTo(float target, bool click, u32 button) {
    auto& st = S();
    st.nav_active = true;
    st.nav_click = click;
    st.nav_button = button;
    st.nav_target = target;
    st.nav_rounds = 0;
    st.nav_last_dir = 0;
    st.nav_last_center = -1;
    st.nav_draining = false;
    st.nav_wait_seq = 0;
}

/// One step of moving the game's focus to the row under the cursor (main thread, each poll): the yellow row of a
/// frame read after the last presses, d-pad presses towards the target, at most a few rounds (a section name or the
/// gap between two items has no item: the focus stops next to it).
void NavStep() {
    auto& st = S();
    if (!st.nav_active) {
        return;
    }
    auto& km = Input::Player1();
    if (km.MenuPressesQueued() > 0) {
        return;
    }
    const auto row = Gpu::MenuFocus::Latest();
    if (st.nav_draining) {
        st.nav_draining = false;
        st.nav_wait_seq = row.seq + 3; // the game shows the presses a frame or two later
        return;
    }
    if (!row.valid || row.seq < st.nav_wait_seq) {
        return;
    }
    if (!row.found) {
        st.nav_active = false;
        return;
    }
    const float center = (row.top + row.bottom) / 2;
    const float d = st.nav_target - center;
    // the list wrapped around (from its last item to its first or back): the target is past its end
    const bool wrapped = st.nav_last_center >= 0 && ((st.nav_last_dir > 0 && center < st.nav_last_center - 120) ||
                                                     (st.nav_last_dir < 0 && center > st.nav_last_center + 120));
    if (wrapped) {
        st.nav_active = false;
        return;
    }
    if (std::abs(d) <= kRowPitch / 2) {
        st.nav_active = false;
        if (st.nav_click) {
            if (st.nav_button == Input::Button::Cross && AdditionalItemChosen(row)) {
                Menu().Open(); // the game's "Additional settings": SecondSonPC's settings
            } else {
                km.MenuPress(st.nav_button);
            }
        }
        return;
    }
    if (++st.nav_rounds > 5) {
        st.nav_active = false; // between two items (a section name): the focus stays on the nearest
        return;
    }
    int n = static_cast<int>(std::lround(d / kRowPitch));
    n = std::clamp(n == 0 ? (d > 0 ? 1 : -1) : n, -kMaxBurst, kMaxBurst);
    for (int i = 0; i < std::abs(n); ++i) {
        km.MenuPress(n > 0 ? Input::Button::Down : Input::Button::Up);
    }
    st.nav_last_dir = n > 0 ? 1 : -1;
    st.nav_last_center = center;
    st.nav_draining = true;
}

/// Every kGamePollMs (main thread): the game's menus as GameMenu sees them.
void OnGamePoll() {
    auto& st = S();
    if (!Engine::GameMenu::Installed()) {
        return;
    }
    const bool open = Engine::GameMenu::Current().open > 0;
    if (open != st.game_menu_open) {
        st.game_menu_open = open;
        Input::Player1().SetGameMenuOpen(open);
    }
    const bool menu_open = st.menu && st.menu->IsOpen();
    const bool settings = Engine::GameMenu::SettingsScreenOnTop();
    Gpu::MenuFocus::SetWanted(settings && !menu_open);
    if (!settings || menu_open) {
        st.nav_active = false;
    }
    st.entry_shown = st.overlay_ok && !menu_open && settings && GetForegroundWindow() == st.hwnd;
    if (!st.entry_shown) {
        st.hover_pending = false;
    }
    // the pointer rested over an item: the focus follows it (not while a click is being carried out)
    if (st.hover_pending && !(st.nav_active && st.nav_click) && NowNs() - st.hover_since_ns >= kHoverRestNs) {
        st.hover_pending = false;
        if (!st.nav_active || std::abs(st.nav_target - st.hover_target) > kRowPitch / 4) {
            NavTo(st.hover_target, false, 0);
        }
    }
    NavStep();
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
    // the game's button icons follow the device in every run (automated ones too)
    PromptService::Install();
    st.prompt_event = SDL_RegisterEvents(1);
    if (st.prompt_event != 0) {
        SDL_AddTimer(100, &PromptTimer, nullptr);
    }
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
    st.poll_event = SDL_RegisterEvents(1);
    {
        char v[4] = {};
        st.dev_f1 = GetEnvironmentVariableA("SSPC_DEV_F1", v, sizeof(v)) == 1 && v[0] == '1';
    }
    if (st.poll_event != 0) {
        SDL_AddTimer(kGamePollMs, &GamePollTimer, nullptr);
    }
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
        SSPC_LOG(App, Info,
                 "PC settings: the game's Settings screen (Esc / Options), its last item \"Additional settings\" (cross: "
                 "its key, the controller's, a click){}{} (overlay: Win32 layered window, Direct2D software; "
                 "capabilities 0x{:x})",
                 cfg.pc.menu_button == "guide"  ? "; controller PS/Guide"
                 : cfg.pc.menu_button == "l3r3" ? "; controller L3+R3 held"
                                                : "",
                 st.dev_f1 ? "; F1 (SSPC_DEV_F1, development)" : "", options.capabilities);
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
    st.entry_shown = false;
    st.hover_pending = false;
    st.nav_active = false;
    st.displays.clear();
    st.display_labels.clear();
    st.primary_label.clear();
    for (const auto& [id, name] : Displays()) {
        st.displays.push_back(name);
        st.display_labels.push_back(DisplayLabel(id, name, Ru(Settings::Instance().Current())));
        if (id == SDL_GetPrimaryDisplay()) {
            st.primary_label = DisplayLabel(id, name, Ru(Settings::Instance().Current()), false); // ("Primary - <it>")
        }
    }
    st.audio_devices = AudioDeviceNames();
    st.window_sizes = WindowSizes(st.opt.window);
    {
        // as Bridge::ApplyUpscaling checks it: FSR 3.1 needs amd_fidelityfx_vk.dll next to SecondSonPC.exe
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::error_code ec;
        st.fsr_why = std::filesystem::is_regular_file(std::filesystem::path(exe).parent_path() / L"amd_fidelityfx_vk.dll",
                                                      ec)
                         ? std::string{}
                         : std::string("amd_fidelityfx_vk.dll is not next to SecondSonPC.exe");
    }
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
    SSPC_LOG(App, Info, "settings menu closed ({} renders, average {:.2f} ms, slowest {:.1f} ms); settings file {}",
             stats.frames, stats.frames != 0 ? stats.total_ms / static_cast<double>(stats.frames) : 0.0, stats.max_ms,
             Settings::Instance().File().empty() ? std::string("not used") : Settings::Instance().File().string());
}

EventRoute MenuController::OnEvent(const SDL_Event& e) {
    if (S().prompt_event != 0 && e.type == S().prompt_event) {
        PromptService::Update(NowNs());
        return EventRoute::Consumed;
    }
    {
        const u32 before = Input::Devices().Switches();
        FeedDevices(e, NowNs());
        if (Input::Devices().Switches() != before) {
            PromptService::Update(NowNs()); // at once (else within kMinGapNs by the timer)
        }
    }
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

    // ---- the game's Settings screen: its "Additional settings" item opens SecondSonPC's settings (cross: its key,
    // the controller's, a click on it); the mouse chooses and clicks the game's items
    if (st.poll_event != 0 && e.type == st.poll_event) {
        OnGamePoll();
        return EventRoute::Consumed;
    }
    // F1 only for development (SSPC_DEV_F1=1), or when the game's screens cannot be seen (another eboot)
    const bool f1_opens = st.dev_f1 || !Engine::GameMenu::Installed();
    if (!open && st.entry_shown) {
        const bool on_item = AdditionalItemChosen(Gpu::MenuFocus::Latest());
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && on_item &&
            Input::Player1().KeyTriggers(static_cast<u16>(e.key.scancode), Input::Action::Cross)) {
            Open();
            return EventRoute::Consumed;
        }
        if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && on_item) {
            const bool cross_confirms = Settings::Instance().Current().enter_button_cross;
            if (e.gbutton.button == (cross_confirms ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST)) {
                TrackPad(e, true);
                Open();
                return EventRoute::BridgeOnly; // the game reads an intercepted controller while the menu is open
            }
        }
        if (e.type == SDL_EVENT_MOUSE_MOTION) {
            const auto [x, y] = ToPixels(e.motion.x, e.motion.y);
            const auto [fx, fy] = ToFrame(x, y);
            if (InList(fx, fy) && Gpu::MenuFocus::Latest().valid) {
                st.hover_pending = true; // followed once the pointer rests (OnGamePoll)
                st.hover_target = fy;
                st.hover_since_ns = now;
            } else {
                st.hover_pending = false;
            }
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
            const auto [x, y] = ToPixels(e.button.x, e.button.y);
            const auto [fx, fy] = ToFrame(x, y);
            if (InList(fx, fy)) {
                    st.hover_pending = false;
                // the item: cross; its value (a choice, a volume bar): left / right
                const u32 button = fx < kValueLeft     ? Input::Button::Cross
                                   : fx < kValueMiddle ? Input::Button::Left
                                                       : Input::Button::Right;
                if (Gpu::MenuFocus::Latest().valid) {
                    NavTo(fy, true, button);
                } else {
                    Input::Player1().MenuPress(button); // no frame reading (the bridge's presenter): the chosen item
                }
                return EventRoute::Consumed;
            }
        }
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
        }
        if (!open) {
            if (!down || e.key.repeat) {
                return EventRoute::Normal;
            }
            if (sc == SDL_SCANCODE_F1 && f1_opens) {
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
            if (sc == SDL_SCANCODE_F1 && !e.key.repeat && f1_opens) {
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
        } else if (e.button.button == SDL_BUTTON_RIGHT && down) {
            st.menu->OnNav(Nav::Back); // as in the game's menus: back (a list, a dialog, then the window)
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
            return open ? EventRoute::BridgeOnly : EventRoute::Normal;
        }
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
