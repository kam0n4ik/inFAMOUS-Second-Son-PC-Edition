// SecondSonPC R10 — the busiest threads spread over the physical cores. See spread.h.
#include "runtime/gpu/r10/spread.h"

#include <windows.h>

#include <bit>
#include <vector>

#include "base/log.h"

namespace SecondSon::Gpu::R10 {

namespace {

struct Cores {
    bool on = false;
    std::vector<u32> first; ///< per physical core: its first logical CPU (processor group 0)
};

const Cores& Get() {
    static const Cores c = [] {
        Cores r;
        char v[4] = {};
        if (!(GetEnvironmentVariableA("SSPC_R10_SPREAD", v, sizeof(v)) == 1 && v[0] == '1')) {
            return r;
        }
        DWORD len = 0;
        GetLogicalProcessorInformation(nullptr, &len);
        std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) + 1);
        len = static_cast<DWORD>(info.size() * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
        if (!GetLogicalProcessorInformation(info.data(), &len)) {
            SSPC_LOG(GPU, Warning, "SSPC_R10_SPREAD=1: GetLogicalProcessorInformation failed ({}) - not applied",
                     GetLastError());
            return r;
        }
        for (DWORD i = 0; i < len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION); ++i) {
            if (info[i].Relationship == RelationProcessorCore && info[i].ProcessorMask != 0) {
                r.first.push_back(static_cast<u32>(std::countr_zero(static_cast<u64>(info[i].ProcessorMask))));
            }
        }
        r.on = !r.first.empty();
        SSPC_LOG(GPU, Warning, "EXPERIMENT: SSPC_R10_SPREAD=1 - the game's main thread and the command processor, "
                               "recording, frontend, emitter and present threads prefer different physical cores ({} "
                               "cores; ideal processor only, no pinning)", r.first.size());
        return r;
    }();
    return c;
}

constexpr const char* kNames[] = {"the game's main thread", "the command processor", "the recording thread",
                                  "the frontend", "the emitter", "the present thread"};

} // namespace

void SpreadThisThread(Heavy h) {
    const Cores& c = Get();
    if (!c.on) {
        return;
    }
    const u32 i = static_cast<u32>(h);
    PROCESSOR_NUMBER pn{};
    pn.Group = 0;
    pn.Number = static_cast<BYTE>(c.first[i % c.first.size()]);
    if (!SetThreadIdealProcessorEx(GetCurrentThread(), &pn, nullptr)) {
        SSPC_LOG(GPU, Warning, "SSPC_R10_SPREAD: {} - SetThreadIdealProcessorEx({}) failed ({})", kNames[i % 6],
                 pn.Number, GetLastError());
        return;
    }
    SSPC_LOG(GPU, Info, "SSPC_R10_SPREAD: {} prefers logical CPU {} (physical core {})", kNames[i % 6], pn.Number,
             i % c.first.size());
}

} // namespace SecondSon::Gpu::R10
