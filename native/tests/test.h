// Minimal self-contained test framework for the native runtime (no third-party dependencies).
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace sspc::test {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& Registry() {
    static std::vector<Case> r;
    return r;
}

struct Counters {
    int checks = 0;
    int failures = 0;
    int skipped = 0;
};
inline Counters& Count() {
    static Counters c;
    return c;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { Registry().push_back({name, std::move(fn)}); }
};

inline void Check(bool ok, const char* expr, const char* file, int line) {
    ++Count().checks;
    if (!ok) {
        ++Count().failures;
        std::printf("    FAIL %s:%d: %s\n", file, line, expr);
    }
}

// Marks the current test as skipped (e.g. game data not installed); prints the reason.
inline void Skip(const std::string& why) {
    ++Count().skipped;
    std::printf("    SKIP: %s\n", why.c_str());
}

}  // namespace sspc::test

#define SSPC_CAT2(a, b) a##b
#define SSPC_CAT(a, b) SSPC_CAT2(a, b)
#define TEST(name)                                                                              \
    static void SSPC_CAT(test_fn_, name)();                                                     \
    static ::sspc::test::Registrar SSPC_CAT(test_reg_, name)(#name, &SSPC_CAT(test_fn_, name)); \
    static void SSPC_CAT(test_fn_, name)()
#define CHECK(expr) ::sspc::test::Check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::sspc::test::Check((a) == (b), #a " == " #b, __FILE__, __LINE__)
