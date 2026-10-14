#include "core/crash.h"

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>

#include "core/log.h"

namespace sspc::crash {
namespace {

wchar_t g_dump_dir[MAX_PATH] = L".";

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                         PMINIDUMP_EXCEPTION_INFORMATION,
                                         PMINIDUMP_USER_STREAM_INFORMATION,
                                         PMINIDUMP_CALLBACK_INFORMATION);

LONG WINAPI Filter(EXCEPTION_POINTERS* info) {
    char line[512];
    const auto* rec = info->ExceptionRecord;
    HMODULE module = nullptr;
    char module_name[MAX_PATH] = "?";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCSTR>(rec->ExceptionAddress), &module);
    if (module) GetModuleFileNameA(module, module_name, MAX_PATH);
    const auto base = reinterpret_cast<unsigned long long>(module);
    const auto addr = reinterpret_cast<unsigned long long>(rec->ExceptionAddress);
    std::snprintf(line, sizeof(line),
                  "\n*** CRASH: exception 0x%08lx at %p (%s+0x%llx) thread %lu ***\n",
                  rec->ExceptionCode, rec->ExceptionAddress, module_name, addr - base,
                  GetCurrentThreadId());
    log::EmergencyWrite(line);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\crash_%04u%02u%02u_%02u%02u%02u.dmp", g_dump_dir, st.wYear, st.wMonth,
               st.wDay, st.wHour, st.wMinute, st.wSecond);
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    auto write_dump = dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>(
                                    GetProcAddress(dbghelp, "MiniDumpWriteDump"))
                              : nullptr;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (write_dump && file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), info, FALSE};
        const BOOL ok = write_dump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                   static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                                              MiniDumpWithThreadInfo),
                                   &mei, nullptr, nullptr);
        log::EmergencyWrite(ok ? "*** minidump written\n" : "*** minidump FAILED\n");
    }
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

void Install(const std::wstring& dump_directory) {
    wcsncpy_s(g_dump_dir, dump_directory.c_str(), _TRUNCATE);
    SetUnhandledExceptionFilter(Filter);
}

}  // namespace sspc::crash
