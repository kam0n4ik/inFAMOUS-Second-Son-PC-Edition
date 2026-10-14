#include <cstdio>
#include <cstring>

#include "test.h"

int main(int argc, char** argv) {
    using namespace sspc::test;
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    for (const Case& c : Registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        const int before = Count().failures;
        std::printf("[ RUN  ] %s\n", c.name);
        c.fn();
        std::printf("[ %s ] %s\n", Count().failures == before ? " OK " : "FAIL", c.name);
        ++run;
    }
    std::printf("\n%d tests, %d checks, %d failures, %d skipped\n", run, Count().checks, Count().failures,
                Count().skipped);
    return Count().failures == 0 ? 0 : 1;
}
