#include "platform/input.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

#include "core/config.h"
#include "core/log.h"

namespace sspc {
namespace {

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

XInputGetStateFn LoadXInput() {
    static XInputGetStateFn fn = [] {
        for (const wchar_t* name : {L"xinput1_4.dll", L"xinput9_1_0.dll", L"xinput1_3.dll"}) {
            if (HMODULE m = LoadLibraryW(name)) {
                if (auto f = reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState"))) {
                    log::Info("input", "XInput loaded from {}", log::Narrow(name));
                    return f;
                }
            }
        }
        log::Warn("input", "XInput not available: gamepad disabled");
        return static_cast<XInputGetStateFn>(nullptr);
    }();
    return fn;
}

float Stick(SHORT v, float dead) {
    const float f = std::max(static_cast<float>(v) / 32767.0f, -1.0f);
    const float a = std::abs(f);
    if (a < dead) return 0.0f;
    return std::copysign((a - dead) / (1.0f - dead), f);
}

struct KeyName {
    const char* name;
    u32 vk;
};
constexpr KeyName kKeyNames[] = {
    {"space", VK_SPACE}, {"enter", VK_RETURN}, {"escape", VK_ESCAPE}, {"tab", VK_TAB},
    {"lshift", VK_LSHIFT}, {"rshift", VK_RSHIFT}, {"shift", VK_SHIFT}, {"lctrl", VK_LCONTROL},
    {"ctrl", VK_CONTROL}, {"alt", VK_MENU}, {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT},
    {"right", VK_RIGHT}, {"pageup", VK_PRIOR}, {"pagedown", VK_NEXT}, {"backspace", VK_BACK},
};

}  // namespace

const char* ToString(Action a) {
    static const char* const kNames[] = {"MoveForward", "MoveBack", "MoveLeft", "MoveRight", "Jump", "Sprint", "Walk",
                                         "ToggleMouseCapture", "NextPart", "PrevPart", "TogglePart",
                                         "ToggleFreeCamera", "Quit"};
    return kNames[static_cast<size_t>(a)];
}

u32 Input::KeyFromName(std::string_view name) {
    std::string n(name);
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n.size() == 1) {
        const char c = n[0];
        if (c >= 'a' && c <= 'z') return static_cast<u32>(c - 'a' + 'A');
        if (c >= '0' && c <= '9') return static_cast<u32>(c);
    }
    if (n.size() >= 2 && n[0] == 'f') {
        const int f = std::atoi(n.c_str() + 1);
        if (f >= 1 && f <= 12) return VK_F1 + static_cast<u32>(f - 1);
    }
    for (const auto& k : kKeyNames) {
        if (n == k.name) return k.vk;
    }
    return 0;
}

Input::Input() {
    auto bind = [&](Action a, u32 k0, u32 k1, u16 pad) {
        bindings_[static_cast<size_t>(a)] = {k0, k1};
        pad_bindings_[static_cast<size_t>(a)] = pad;
    };
    bind(Action::MoveForward, 'W', VK_UP, 0);
    bind(Action::MoveBack, 'S', VK_DOWN, 0);
    bind(Action::MoveLeft, 'A', VK_LEFT, 0);
    bind(Action::MoveRight, 'D', VK_RIGHT, 0);
    bind(Action::Jump, VK_SPACE, 0, XINPUT_GAMEPAD_A);
    bind(Action::Sprint, VK_LSHIFT, VK_SHIFT, XINPUT_GAMEPAD_LEFT_THUMB);
    bind(Action::Walk, VK_LCONTROL, VK_CONTROL, 0);
    bind(Action::ToggleMouseCapture, VK_F2, 0, 0);
    bind(Action::NextPart, VK_OEM_6, 0, XINPUT_GAMEPAD_DPAD_RIGHT);   // ]
    bind(Action::PrevPart, VK_OEM_4, 0, XINPUT_GAMEPAD_DPAD_LEFT);    // [
    bind(Action::TogglePart, 'H', 0, XINPUT_GAMEPAD_DPAD_UP);
    bind(Action::ToggleFreeCamera, VK_F3, 0, XINPUT_GAMEPAD_BACK);
    bind(Action::Quit, VK_ESCAPE, 0, 0);
}

void Input::ApplyConfig(const Config& cfg) {
    for (size_t i = 0; i < static_cast<size_t>(Action::Count); ++i) {
        const auto value = cfg.Get("Input", ToString(static_cast<Action>(i)));
        if (!value) continue;
        std::array<u32, 2> keys{};
        size_t n = 0, start = 0;
        const std::string& v = *value;
        while (start <= v.size() && n < 2) {
            size_t comma = v.find(',', start);
            if (comma == std::string::npos) comma = v.size();
            std::string_view token(v.data() + start, comma - start);
            while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
            while (!token.empty() && token.back() == ' ') token.remove_suffix(1);
            if (u32 k = KeyFromName(token)) {
                keys[n++] = k;
            } else if (!token.empty()) {
                log::Warn("input", "unknown key '{}' for {}", token, ToString(static_cast<Action>(i)));
            }
            start = comma + 1;
        }
        if (n > 0) bindings_[i] = keys;
    }
    stick_dead_zone_ = static_cast<float>(cfg.GetFloat("Input", "StickDeadZone", stick_dead_zone_));
    pad_look_speed_ = static_cast<float>(cfg.GetFloat("Input", "GamepadLookSpeed", pad_look_speed_));
    invert_y_ = cfg.GetBool("Input", "InvertY", invert_y_);
}

void Input::OnKey(u32 vk, bool down) {
    if (vk < keys_.size()) keys_[vk] = down;
}

void Input::OnMouseDelta(s32 dx, s32 dy) {
    mouse_dx_ += dx;
    mouse_dy_ += dy;
}

void Input::OnMouseButton(int button, bool down) {
    if (button >= 0 && button < 3) mouse_buttons_[static_cast<size_t>(button)] = down;
}

void Input::OnFocusLost() {
    keys_.fill(false);
    mouse_buttons_.fill(false);
    mouse_dx_ = mouse_dy_ = 0;
}

void Input::BeginFrame() {
    prev_keys_ = keys_;
    frame_mouse_dx_ = mouse_dx_;
    frame_mouse_dy_ = mouse_dy_;
    mouse_dx_ = mouse_dy_ = 0;
    prev_pad_buttons_ = pad_.buttons;
    PollGamepad();
}

void Input::PollGamepad() {
    auto get = LoadXInput();
    pad_ = GamepadState{};
    if (!get) return;
    XINPUT_STATE st{};
    if (get(0, &st) != ERROR_SUCCESS) return;
    pad_.connected = true;
    pad_.left_x = Stick(st.Gamepad.sThumbLX, stick_dead_zone_);
    pad_.left_y = Stick(st.Gamepad.sThumbLY, stick_dead_zone_);
    pad_.right_x = Stick(st.Gamepad.sThumbRX, stick_dead_zone_);
    pad_.right_y = Stick(st.Gamepad.sThumbRY, stick_dead_zone_);
    pad_.left_trigger = st.Gamepad.bLeftTrigger / 255.0f;
    pad_.right_trigger = st.Gamepad.bRightTrigger / 255.0f;
    pad_.buttons = st.Gamepad.wButtons;
}

bool Input::Down(Action a) const {
    const auto i = static_cast<size_t>(a);
    for (u32 k : bindings_[i]) {
        if (k && keys_[k]) return true;
    }
    return pad_bindings_[i] && (pad_.buttons & pad_bindings_[i]);
}

bool Input::Pressed(Action a) const {
    const auto i = static_cast<size_t>(a);
    for (u32 k : bindings_[i]) {
        if (k && keys_[k] && !prev_keys_[k]) return true;
    }
    const u16 b = pad_bindings_[i];
    return b && (pad_.buttons & b) && !(prev_pad_buttons_ & b);
}

void Input::MoveAxis(float& x, float& y) const {
    x = (Down(Action::MoveRight) ? 1.0f : 0.0f) - (Down(Action::MoveLeft) ? 1.0f : 0.0f);
    y = (Down(Action::MoveForward) ? 1.0f : 0.0f) - (Down(Action::MoveBack) ? 1.0f : 0.0f);
    if (pad_.connected && (pad_.left_x != 0 || pad_.left_y != 0)) {
        x = pad_.left_x;
        y = pad_.left_y;
    }
    const float len = std::sqrt(x * x + y * y);
    if (len > 1.0f) {
        x /= len;
        y /= len;
    }
}

void Input::LookDelta(float& dx, float& dy, float frame_seconds) const {
    dx = static_cast<float>(frame_mouse_dx_);
    dy = static_cast<float>(frame_mouse_dy_);
    if (pad_.connected) {
        dx += pad_.right_x * pad_look_speed_ * frame_seconds;
        dy -= pad_.right_y * pad_look_speed_ * frame_seconds;
    }
    if (invert_y_) dy = -dy;
}

}  // namespace sspc
