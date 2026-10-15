// SecondSonPC native runtime - input (keyboard, raw mouse, XInput gamepad, action mapping).
//
// Raw device state is fed by the window procedure (keys, raw mouse deltas) and by PollGamepad
// (XInput, loaded dynamically so the exe has no static XInput import). Game code reads actions
// and axes, never virtual-key codes, so bindings can be remapped from the config file.
#pragma once

#include <array>
#include <string>
#include <string_view>

#include "core/types.h"

namespace sspc {

class Config;

enum class Action : u8 {
    MoveForward,
    MoveBack,
    MoveLeft,
    MoveRight,
    Jump,
    Sprint,
    Walk,
    ToggleMouseCapture,
    NextPart,
    PrevPart,
    TogglePart,
    ToggleFreeCamera,
    ToggleCollisionView,
    Quit,
    Count,
};
const char* ToString(Action a);

struct GamepadState {
    bool connected = false;
    float left_x = 0, left_y = 0, right_x = 0, right_y = 0;  // -1..1 after dead zone
    float left_trigger = 0, right_trigger = 0;
    u16 buttons = 0;  // XINPUT_GAMEPAD_* bits
};

class Input {
public:
    Input();
    void ApplyConfig(const Config& cfg);

    // Called by the window procedure.
    void OnKey(u32 vk, bool down);
    void OnMouseDelta(s32 dx, s32 dy);
    void OnMouseButton(int button, bool down);
    void OnFocusLost();

    // Once per frame, before reading: latches edges, polls the gamepad.
    void BeginFrame();

    bool Down(Action a) const;
    bool Pressed(Action a) const;  // went down this frame
    // Movement in the controller's frame: x = right, y = forward, magnitude <= 1.
    void MoveAxis(float& x, float& y) const;
    // Look delta for this frame in "mouse counts" (gamepad right stick scaled by look speed).
    void LookDelta(float& dx, float& dy, float frame_seconds) const;

    const GamepadState& Gamepad() const { return pad_; }
    bool MouseButton(int b) const { return b >= 0 && b < 3 && mouse_buttons_[b]; }

    static u32 KeyFromName(std::string_view name);  // "W", "Space", "LShift", "F1", ... ; 0 if unknown

private:
    void PollGamepad();

    std::array<bool, 256> keys_{};
    std::array<bool, 256> prev_keys_{};
    std::array<bool, 3> mouse_buttons_{};
    s32 mouse_dx_ = 0, mouse_dy_ = 0;          // accumulated by the window
    s32 frame_mouse_dx_ = 0, frame_mouse_dy_ = 0;  // latched for this frame
    std::array<std::array<u32, 2>, static_cast<size_t>(Action::Count)> bindings_{};
    std::array<u16, static_cast<size_t>(Action::Count)> pad_bindings_{};
    u16 prev_pad_buttons_ = 0;
    GamepadState pad_;
    float stick_dead_zone_ = 0.24f;
    float pad_look_speed_ = 900.0f;  // counts per second at full deflection
    bool invert_y_ = false;
};

}  // namespace sspc
