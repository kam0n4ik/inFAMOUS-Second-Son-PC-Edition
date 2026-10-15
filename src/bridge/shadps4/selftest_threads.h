// SecondSonPC — self-test of SecondSonPC's guest thread manager (Phase 2, P2.4;
// docs/PHASE2_P24_NATIVE_THREADS.md). The manager runs on a test host: guest stacks in host memory
// (with a no-access guard) or in the selected guest memory manager (`guest_stacks`: the bridge's in
// --selftest, SecondSonPC's in --selftest --native memory), TCBs of its own. "Guest" code is
// System V functions of the test that call the manager's exports as the game does.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "bridge/shadps4/selftest_vmm.h"
#include "runtime/cpu/fs_patcher.h"

namespace SecondSon::App {
struct Config;
}

namespace SecondSon::Bridge {

std::vector<TestGroup> TestNativeThreads(bool guest_stacks);

/// P3.3 (src/bridge/shadps4/selftest_fs.cpp): SecondSonPC's TCB slot and FS patcher.
std::vector<TestGroup> TestNativeFs(const App::Config& cfg);

/// P3.4 (src/bridge/shadps4/selftest_exceptions.cpp): SecondSonPC's exception dispatcher.
std::vector<TestGroup> TestNativeExceptions(const App::Config& cfg);

/// P3.4 (src/bridge/shadps4/selftest_cpu.cpp): SSE4a emulation of the CPU patcher.
std::vector<TestGroup> TestNativeCpu(const App::Config& cfg);

/// P3.5 (src/bridge/shadps4/selftest_kernel_hle.cpp): the guest errno, libkernel's memory functions.
std::vector<TestGroup> TestKernelHle(const App::Config& cfg);

/// P3.6 (src/bridge/shadps4/selftest_equeue.cpp): the game's event queues, their guest API and contract.
std::vector<TestGroup> TestEventQueues();

/// P3.6 (src/bridge/shadps4/selftest_lifecycle.cpp): the process's end, in child processes (logs in
/// <logs_dir>\selftest_lifecycle\<case>).
std::vector<TestGroup> TestLifecycle(const std::filesystem::path& logs_dir);

/// P4.1 (src/bridge/shadps4/selftest_gpu.cpp): the PM4 decoder and statistics, the guest GPU's owners, the GPU memory
/// contract, the Vulkan foundation.
std::vector<TestGroup> TestGpu(const App::Config& cfg);

/// P4.2 (src/bridge/shadps4/selftest_gpu_cp.cpp): SecondSonPC's GPU command processor.
std::vector<TestGroup> TestGpuCommandProcessor(const App::Config& cfg);
/// P5.3 (same file): the native operations' direct path (no markers) through the processor - also run with
/// SSPC_SELFTEST_ONLY=shader.
std::vector<TestGroup> TestGpuNativeDirect();

/// P4.3 (src/bridge/shadps4/selftest_shader.cpp): SecondSonPC's shader frontend.
std::vector<TestGroup> TestShaderFrontend(const App::Config& cfg);

/// The executable segment of the game's eboot.bin copied into host memory (read/write; trampoline area after
/// it) and its .eh_frame function ranges (selftest_fs.cpp). The game's files are only read.
struct EbootCopy {
    u8* copy = nullptr;
    u64 filesz = 0;          // bytes of code copied
    u64 code_size = 0;       // of the region before the trampolines
    u64 trampoline_size = 0; // of the trampoline area at copy + code_size
    std::vector<u8> original;
    std::vector<Cpu::FunctionRange> ranges;
    u64 fdes = 0;
    u64 dead = 0;            // FDEs inside .eh_frame (discarded by the linker): skipped
};
bool CopyEbootCode(const App::Config& cfg, EbootCopy& out, std::string& error);

} // namespace SecondSon::Bridge
