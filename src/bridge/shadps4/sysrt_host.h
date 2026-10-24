// SecondSonPC — R10 sysrt adapters (docs/R10_SYSRT.md §5): what the bridge's libraries that stay the bridge's need to
// know of SecondSonPC's CommonDialog and libSceNet state.
#pragma once

#include <vector>

namespace SecondSon::App {
struct Config;
}

namespace SecondSon::Bridge {

struct TestGroup;

/// For each of the groups sysrt-dialog / sysrt-net that is NATIVE in this run. Before guest code.
void InstallSysrtHost(const App::Config& cfg);

/// The self-test of these groups against the bridge's functions (selftest_sysrt.cpp; SSPC_SELFTEST_ONLY=sysrt).
std::vector<TestGroup> TestSysrt(const App::Config& cfg);

} // namespace SecondSon::Bridge
