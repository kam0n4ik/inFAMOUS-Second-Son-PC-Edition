// sspc_depaudit - dependency audit of a native build.
//
//   sspc_depaudit <exe> [--modules <loaded_modules.txt>] [--report <out.md>]
//
// Evidence collected:
//  1. PE import table and delay-import table of the exe (DLL names + function names).
//  2. Strings embedded in the exe (ASCII and UTF-16) that would indicate console runtime use
//     (eboot loading, PS4 modules, shadPS4, guest paths).
//  3. Optionally the list of DLLs actually loaded by a run (SecondSonPC.exe --modules <file>).
// Each criterion is reported as PASS only when the evidence supports it; otherwise NOT PROVEN/FAIL.
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <format>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {

struct Import {
    std::string dll;
    std::vector<std::string> functions;
    bool delayed = false;
};

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ReadFileBytes(const std::wstring& path, std::vector<unsigned char>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

// Maps an RVA to a file offset through the section table; returns false if outside any section.
bool RvaToOffset(const std::vector<unsigned char>& pe, const IMAGE_NT_HEADERS64* nt, DWORD rva, size_t& off) {
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const DWORD size = std::max(sec[i].Misc.VirtualSize, sec[i].SizeOfRawData);
        if (rva >= sec[i].VirtualAddress && rva < sec[i].VirtualAddress + size) {
            off = static_cast<size_t>(rva - sec[i].VirtualAddress) + sec[i].PointerToRawData;
            return off < pe.size();
        }
    }
    return false;
}

std::string CString(const std::vector<unsigned char>& pe, size_t off) {
    std::string s;
    while (off < pe.size() && pe[off] && s.size() < 512) s.push_back(static_cast<char>(pe[off++]));
    return s;
}

bool ParseImports(const std::vector<unsigned char>& pe, std::vector<Import>& out, std::string& error) {
    if (pe.size() < sizeof(IMAGE_DOS_HEADER)) return error = "file too small", false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(pe.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > pe.size()) {
        return error = "not a PE file", false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(pe.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return error = "not a PE32+ (x64) image", false;
    }
    auto thunk_names = [&](DWORD thunk_rva, std::vector<std::string>& names) {
        size_t t = 0;
        if (!thunk_rva || !RvaToOffset(pe, nt, thunk_rva, t)) return;
        for (; t + 8 <= pe.size(); t += 8) {
            unsigned long long v;
            std::memcpy(&v, pe.data() + t, 8);
            if (!v) break;
            if (v & IMAGE_ORDINAL_FLAG64) {
                names.push_back(std::format("#{}", v & 0xffff));
            } else {
                size_t no = 0;
                if (RvaToOffset(pe, nt, static_cast<DWORD>(v), no)) names.push_back(CString(pe, no + 2));
            }
        }
    };
    const auto& dirs = nt->OptionalHeader.DataDirectory;
    size_t off = 0;
    if (dirs[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress &&
        RvaToOffset(pe, nt, dirs[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress, off)) {
        for (; off + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= pe.size(); off += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
            IMAGE_IMPORT_DESCRIPTOR d;
            std::memcpy(&d, pe.data() + off, sizeof(d));
            if (!d.Name) break;
            Import imp;
            size_t no = 0;
            if (RvaToOffset(pe, nt, d.Name, no)) imp.dll = CString(pe, no);
            thunk_names(d.OriginalFirstThunk ? d.OriginalFirstThunk : d.FirstThunk, imp.functions);
            out.push_back(std::move(imp));
        }
    }
    if (dirs[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress &&
        RvaToOffset(pe, nt, dirs[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress, off)) {
        for (; off + sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) <= pe.size(); off += sizeof(IMAGE_DELAYLOAD_DESCRIPTOR)) {
            IMAGE_DELAYLOAD_DESCRIPTOR d;
            std::memcpy(&d, pe.data() + off, sizeof(d));
            if (!d.DllNameRVA) break;
            Import imp;
            imp.delayed = true;
            size_t no = 0;
            if (RvaToOffset(pe, nt, d.DllNameRVA, no)) imp.dll = CString(pe, no);
            thunk_names(d.ImportNameTableRVA, imp.functions);
            out.push_back(std::move(imp));
        }
    }
    return true;
}

std::vector<std::string> EmbeddedStrings(const std::vector<unsigned char>& pe) {
    std::vector<std::string> out;
    std::string cur;
    for (unsigned char c : pe) {  // ASCII
        if (c >= 0x20 && c < 0x7f) cur.push_back(static_cast<char>(c));
        else {
            if (cur.size() >= 5) out.push_back(cur);
            cur.clear();
        }
    }
    for (size_t parity = 0; parity < 2; ++parity) {  // UTF-16LE
        cur.clear();
        for (size_t i = parity; i + 1 < pe.size(); i += 2) {
            if (pe[i] >= 0x20 && pe[i] < 0x7f && pe[i + 1] == 0) cur.push_back(static_cast<char>(pe[i]));
            else {
                if (cur.size() >= 5) out.push_back(cur);
                cur.clear();
            }
        }
    }
    return out;
}

const char* const kForbiddenModules[] = {"shadps4", "libkernel", "libsce", ".sprx", ".prx", "orbis", "sdl3",
                                         "libc.prx", "fios2"};
const char* const kConsoleStrings[] = {"eboot.bin", "shadps4", "/app0", "libkernel", ".sprx", "sce_module",
                                       "scegnm", "libscegnmdriver", "orbis", "pm4"};

bool IsSystemPath(const std::string& lower_path) {
    return lower_path.find("\\windows\\system32\\") != std::string::npos ||
           lower_path.find("\\windows\\winsxs\\") != std::string::npos ||
           lower_path.find("\\windows\\system32\\driverstore\\") != std::string::npos;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: sspc_depaudit <exe> [--modules <file>] [--report <out.md>]\n");
        return 1;
    }
    std::wstring exe = argv[1], modules_file, report_file;
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::wstring(argv[i]) == L"--modules") modules_file = argv[++i];
        else if (std::wstring(argv[i]) == L"--report") report_file = argv[++i];
    }
    std::vector<unsigned char> pe;
    if (!ReadFileBytes(exe, pe)) {
        std::printf("error: cannot read exe\n");
        return 2;
    }
    std::vector<Import> imports;
    std::string error;
    if (!ParseImports(pe, imports, error)) {
        std::printf("error: %s\n", error.c_str());
        return 2;
    }

    std::string md = "# Dependency audit\n\n";
    bool imports_clean = true, sce_free = true;
    md += "## PE imports\n\n| DLL | delay | functions | verdict |\n|---|---|---|---|\n";
    for (const auto& imp : imports) {
        const std::string l = Lower(imp.dll);
        bool bad = false;
        for (const char* f : kForbiddenModules) bad |= l.find(f) != std::string::npos;
        for (const auto& fn : imp.functions) {
            if (Lower(fn).rfind("sce", 0) == 0) {
                bad = true;
                sce_free = false;
            }
        }
        imports_clean &= !bad;
        md += std::format("| {} | {} | {} | {} |\n", imp.dll, imp.delayed ? "yes" : "", imp.functions.size(),
                          bad ? "FORBIDDEN" : "ok");
    }

    std::set<std::string> hits;
    for (const auto& s : EmbeddedStrings(pe)) {
        const std::string l = Lower(s);
        for (const char* k : kConsoleStrings) {
            // "pm4" alone is too short to be meaningful inside random data; require a word boundary-ish match.
            if (std::string_view(k) == "pm4" ? (l == "pm4" || l.find("pm4 ") != std::string::npos)
                                              : l.find(k) != std::string::npos) {
                hits.insert(s.substr(0, 120));
            }
        }
    }
    md += "\n## Console-runtime strings embedded in the exe\n\n";
    if (hits.empty()) md += "none of: eboot.bin, shadps4, /app0, libkernel, .sprx, sce_module, sceGnm*, orbis\n";
    for (const auto& h : hits) md += "- `" + h + "`\n";

    bool runtime_checked = false, runtime_clean = true;
    std::vector<std::string> nonsystem;
    if (!modules_file.empty()) {
        std::ifstream mf(modules_file);
        std::string line;
        md += "\n## DLLs loaded at run time\n\n";
        while (std::getline(mf, line)) {
            if (line.empty()) continue;
            runtime_checked = true;
            const std::string l = Lower(line);
            bool bad = false;
            for (const char* f : kForbiddenModules) bad |= l.find(f) != std::string::npos;
            runtime_clean &= !bad;
            if (!IsSystemPath(l) && l.find(".exe") == std::string::npos) {
                nonsystem.push_back(line);
            }
            md += std::format("- `{}`{}\n", line, bad ? " **FORBIDDEN**" : "");
        }
    }

    auto verdict = [](bool proven, bool ok) { return !proven ? "NOT PROVEN" : (ok ? "PASS" : "FAIL"); };
    md += "\n## Summary\n\n```text\n";
    md += std::format("Native Windows Runtime (imports are Windows/driver DLLs only): {}\n", verdict(true, imports_clean));
    md += std::format("No sce* imports:                                             {}\n", verdict(true, sce_free));
    md += std::format("No console-runtime strings in the exe:                       {}\n", verdict(true, hits.empty()));
    md += std::format("Run-time module list clean:                                  {}\n",
                      verdict(runtime_checked, runtime_clean));
    md += std::format("PS4 Eboot Execution:                                         {}\n",
                      hits.empty() && imports_clean ? "NO (no loader, no eboot reference)" : "NOT PROVEN");
    md += std::format("shadPS4 Runtime Required:                                    {}\n",
                      imports_clean && (!runtime_checked || runtime_clean) ? "NO" : "NOT PROVEN");
    md += "```\n";
    if (!nonsystem.empty()) {
        md += "\nNon-system DLLs loaded (review): \n";
        for (const auto& n : nonsystem) md += "- `" + n + "`\n";
    }
    std::fwrite(md.data(), 1, md.size(), stdout);
    if (!report_file.empty()) {
        std::ofstream(report_file, std::ios::binary) << md;
    }
    const bool ok = imports_clean && sce_free && hits.empty() && runtime_clean;
    return ok ? 0 : 10;
}
