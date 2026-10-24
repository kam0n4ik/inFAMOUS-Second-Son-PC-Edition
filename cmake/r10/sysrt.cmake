# SecondSonPC R10 sysrt (docs/R10_SYSRT.md): the game's remaining CommonDialog, libSceNet and libSceLibcInternal calls
# on SecondSonPC's own functions (experimental groups sysrt-dialog / sysrt-net / sysrt-libc, SSPC_R10_SYSRT=native).
# SSPC_R10_SYSRT_SOURCES go into sspc_runtime; SSPC_R10_SYSRT_BRIDGE_SOURCES (the adapters and the self-test) into the
# bridge's target (cmake/shadps4_bootstrap.cmake).
set(SSPC_R10_SYSRT_SOURCES
    src/runtime/system/common_dialog.cpp
    src/runtime/net/net_core.cpp
    src/runtime/libc/guest_printf.cpp
    src/runtime/libc/libc_stdio.cpp
)
set(SSPC_R10_SYSRT_BRIDGE_SOURCES
    ${CMAKE_SOURCE_DIR}/src/bridge/shadps4/sysrt_host.cpp
    ${CMAKE_SOURCE_DIR}/src/bridge/shadps4/selftest_sysrt.cpp
)
