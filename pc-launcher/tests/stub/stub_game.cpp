// inFAMOUS PC Launcher — the stub game of the tests (copied as SecondSonPC.exe / FirstLightPC.exe into a fixture).
// Writes stub_report.txt in its working folder: the command line, the working folder and the environment variables
// SSPC_* / STUB_*; then, as the environment asks:
//   STUB_EXIT=<n>        exit code (default 0)
//   STUB_NO_WINDOW=1     no window (a start that fails early)
//   STUB_LIFETIME_MS=<n> how long the window stays (default 1500)
//   STUB_TITLE=<text>    the window's title (default "inFAMOUS Stub")
#include <windows.h>

#include <string>

namespace {

std::wstring Env(const wchar_t* name) {
    wchar_t buf[4096];
    const DWORD n = GetEnvironmentVariableW(name, buf, 4096);
    return n > 0 && n < 4096 ? std::wstring(buf, n) : std::wstring();
}

void Report() {
    std::wstring text = L"cmdline=";
    text += GetCommandLineW();
    wchar_t cwd[MAX_PATH * 2];
    GetCurrentDirectoryW(MAX_PATH * 2, cwd);
    text += L"\ncwd=";
    text += cwd;
    text += L"\n";
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
            if (_wcsnicmp(p, L"SSPC_", 5) == 0 || _wcsnicmp(p, L"STUB_", 5) == 0) {
                text += L"env:";
                text += p;
                text += L"\n";
            }
        }
        FreeEnvironmentStringsW(block);
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(size_t(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), utf8.data(), bytes, nullptr, nullptr);
    const HANDLE f = CreateFileW(L"stub_report.txt", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(f, utf8.data(), DWORD(utf8.size()), &written, nullptr);
        CloseHandle(f);
    }
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER) {
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    Report();
    const std::wstring exitText = Env(L"STUB_EXIT");
    const int exitCode = exitText.empty() ? 0 : _wtoi(exitText.c_str());
    if (Env(L"STUB_NO_WINDOW") == L"1") return exitCode;
    const std::wstring lifeText = Env(L"STUB_LIFETIME_MS");
    const UINT lifetime = lifeText.empty() ? 1500u : UINT(_wtoi(lifeText.c_str()));
    std::wstring title = Env(L"STUB_TITLE");
    if (title.empty()) title = L"inFAMOUS Stub";

    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"LauncherStubGame";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, 0, 0, 320, 180, nullptr,
                                nullptr, inst, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetTimer(hwnd, 1, lifetime, nullptr);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return exitCode;
}
