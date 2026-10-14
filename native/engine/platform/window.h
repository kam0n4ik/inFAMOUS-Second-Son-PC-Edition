// SecondSonPC native runtime - Win32 window.
#pragma once

#include <string>

#include "core/types.h"
#include "platform/input.h"

struct HWND__;
struct HINSTANCE__;

namespace sspc {

struct WindowDesc {
    std::wstring title = L"SecondSonPC";
    u32 width = 1600;
    u32 height = 900;
    bool fullscreen = false;  // borderless window covering the monitor
};

class Window {
public:
    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool Create(const WindowDesc& desc, Input* input);
    // Processes all pending messages; returns false once the window was closed.
    bool PumpMessages();
    void Close();

    HWND__* Handle() const { return hwnd_; }
    HINSTANCE__* Instance() const { return instance_; }
    u32 Width() const { return width_; }
    u32 Height() const { return height_; }
    bool Minimized() const { return minimized_; }
    bool Focused() const { return focused_; }
    // True once after the client area size changed.
    bool ConsumeResized();

    void SetTitle(const std::wstring& title);
    void SetMouseCaptured(bool captured);
    bool MouseCaptured() const { return mouse_captured_; }

private:
    static long long __stdcall WndProc(HWND__* hwnd, unsigned msg, unsigned long long wp, long long lp);
    long long HandleMessage(unsigned msg, unsigned long long wp, long long lp);
    void ApplyClipCursor();

    HWND__* hwnd_ = nullptr;
    HINSTANCE__* instance_ = nullptr;
    Input* input_ = nullptr;
    u32 width_ = 0, height_ = 0;
    bool minimized_ = false;
    bool focused_ = true;
    bool resized_ = false;
    bool closed_ = false;
    bool mouse_captured_ = false;
};

}  // namespace sspc
