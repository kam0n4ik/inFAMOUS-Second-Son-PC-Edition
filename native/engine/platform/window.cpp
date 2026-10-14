#include "platform/window.h"

#include <windows.h>

#include "core/log.h"

namespace sspc {
namespace {
constexpr wchar_t kClassName[] = L"SecondSonPCNativeWindow";
}

Window::~Window() {
    SetMouseCaptured(false);
    if (hwnd_) DestroyWindow(hwnd_);
}

bool Window::Create(const WindowDesc& desc, Input* input) {
    input_ = input;
    instance_ = GetModuleHandleW(nullptr);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = reinterpret_cast<WNDPROC>(&Window::WndProc);
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        log::Error("window", "RegisterClassEx failed: {}", GetLastError());
        return false;
    }

    DWORD style = WS_OVERLAPPEDWINDOW;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    RECT rc{0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height)};
    if (desc.fullscreen) {
        style = WS_POPUP;
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        rc = mi.rcMonitor;
        x = rc.left;
        y = rc.top;
    } else {
        AdjustWindowRect(&rc, style, FALSE);
    }
    hwnd_ = CreateWindowExW(0, kClassName, desc.title.c_str(), style, x, y, rc.right - rc.left,
                            rc.bottom - rc.top, nullptr, nullptr, instance_, this);
    if (!hwnd_) {
        log::Error("window", "CreateWindowEx failed: {}", GetLastError());
        return false;
    }

    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;  // generic desktop
    rid.usUsage = 0x02;      // mouse
    rid.hwndTarget = hwnd_;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        log::Warn("window", "raw mouse input not available: {}", GetLastError());
    }
    ShowWindow(hwnd_, SW_SHOW);
    RECT client;
    GetClientRect(hwnd_, &client);
    width_ = static_cast<u32>(client.right - client.left);
    height_ = static_cast<u32>(client.bottom - client.top);
    log::Info("window", "created {}x{}{}", width_, height_, desc.fullscreen ? " borderless" : "");
    return true;
}

bool Window::PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) closed_ = true;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !closed_;
}

void Window::Close() {
    if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

bool Window::ConsumeResized() {
    const bool r = resized_;
    resized_ = false;
    return r;
}

void Window::SetTitle(const std::wstring& title) {
    if (hwnd_) SetWindowTextW(hwnd_, title.c_str());
}

void Window::ApplyClipCursor() {
    if (mouse_captured_ && focused_ && hwnd_) {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&rc), 2);
        ClipCursor(&rc);
    } else {
        ClipCursor(nullptr);
    }
}

void Window::SetMouseCaptured(bool captured) {
    if (captured == mouse_captured_) return;
    mouse_captured_ = captured;
    ShowCursor(captured ? FALSE : TRUE);
    ApplyClipCursor();
}

long long __stdcall Window::WndProc(HWND__* hwnd, unsigned msg, unsigned long long wp, long long lp) {
    Window* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleMessage(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

long long Window::HandleMessage(unsigned msg, unsigned long long wp, long long lp) {
    switch (msg) {
        case WM_CLOSE:
            closed_ = true;
            return 0;
        case WM_SIZE: {
            const u32 w = LOWORD(lp), h = HIWORD(lp);
            minimized_ = wp == SIZE_MINIMIZED || w == 0 || h == 0;
            if (!minimized_ && (w != width_ || h != height_)) {
                width_ = w;
                height_ = h;
                resized_ = true;
            }
            ApplyClipCursor();
            return 0;
        }
        case WM_ACTIVATE:
            focused_ = LOWORD(wp) != WA_INACTIVE;
            if (!focused_ && input_) input_->OnFocusLost();
            ApplyClipCursor();
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
            u32 vk = static_cast<u32>(wp);
            if (vk == VK_SHIFT) {
                const UINT scan = (static_cast<UINT>(lp) >> 16) & 0xff;
                vk = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX);
                if (input_) input_->OnKey(VK_SHIFT, down);
            }
            if (input_) input_->OnKey(vk, down);
            if (msg == WM_SYSKEYDOWN && wp == VK_F4) closed_ = true;
            return 0;
        }
        case WM_INPUT: {
            RAWINPUT raw;
            UINT size = sizeof(raw);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size,
                                sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1) &&
                raw.header.dwType == RIM_TYPEMOUSE && input_ && focused_ &&
                (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                input_->OnMouseDelta(raw.data.mouse.lLastX, raw.data.mouse.lLastY);
            }
            break;
        }
        case WM_LBUTTONDOWN: if (input_) input_->OnMouseButton(0, true); return 0;
        case WM_LBUTTONUP: if (input_) input_->OnMouseButton(0, false); return 0;
        case WM_RBUTTONDOWN: if (input_) input_->OnMouseButton(1, true); return 0;
        case WM_RBUTTONUP: if (input_) input_->OnMouseButton(1, false); return 0;
        case WM_MBUTTONDOWN: if (input_) input_->OnMouseButton(2, true); return 0;
        case WM_MBUTTONUP: if (input_) input_->OnMouseButton(2, false); return 0;
        default: break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace sspc
