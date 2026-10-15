# SecondSonPC — game-specific PC compatibility runtime for inFAMOUS: Second Son.
cmake_minimum_required(VERSION 3.24)
project(SecondSonPC LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "SecondSonPC requires clang-cl: guest code calls the host with the SysV ABI")
endif()

# TEMPORARY (Phase 0-4): build on top of the shadPS4 core. Removed in Phase 5 (see docs/AUDIT.md).
option(SSPC_SHADPS4_BRIDGE "Temporary bootstrap on the shadPS4 core" ON)
if(NOT SSPC_SHADPS4_BRIDGE)
    message(FATAL_ERROR "A build without the shadPS4 bridge is the Phase 6 goal and not available yet")
endif()
include(cmake/shadps4_bootstrap.cmake) # provides fmt::fmt, toml11::toml11 and the bridge target

# Own code. Rule: nothing outside src/bridge/ may include shadPS4 headers.
add_library(sspc_runtime STATIC
    src/app/config.cpp
    src/app/paths.cpp
    src/base/crash.cpp
    src/base/lifecycle.cpp
    src/base/log.cpp
    src/base/vram_monitor.cpp
    src/engine/guest_hooks.cpp
    src/engine/guest_probe.cpp
    src/engine/job_system.cpp
    src/engine/particles/smoke_quality.cpp
    src/engine/render/dcb_scan.cpp
    src/engine/render/frame_trace.cpp
    src/engine/render/native_draw.cpp
    src/engine/render/native_render_ops.cpp
    src/engine/render/native_state.cpp
    src/engine/render/pass_graph.cpp
    src/input/keyboard_mouse.cpp
    src/input/motion.cpp
    src/input/settings.cpp
    src/loader/bindings.cpp
    src/loader/eh_frame.cpp
    src/loader/image_file.cpp
    src/loader/linker.cpp
    src/loader/module.cpp
    src/loader/rtld.cpp
    src/loader/snapshot.cpp
    src/runtime/app_content/app_content.cpp
    src/runtime/cpu/exceptions.cpp
    src/runtime/cpu/fs_patcher.cpp
    src/runtime/cpu/sse4a.cpp
    src/runtime/cpu/tcb_slot.cpp
    src/runtime/exports.cpp
    src/runtime/filesystem/directory.cpp
    src/runtime/filesystem/file_system.cpp
    src/runtime/filesystem/fs_observer.cpp
    src/runtime/filesystem/vfs.cpp
    src/runtime/gpu/backend.cpp
    src/runtime/gpu/command_processor.cpp
    src/runtime/gpu/cp_registers.cpp
    src/runtime/gpu/cp_replay.cpp
    src/runtime/gpu/cp_shadow.cpp
    src/runtime/gpu/cp_validation.cpp
    src/runtime/gpu/frame_log.cpp
    src/runtime/gpu/inspector.cpp
    src/runtime/gpu/inspector_catalog.cpp
    src/runtime/gpu/stall_trace.cpp
    src/runtime/gpu/shader_trace.cpp
    src/runtime/gpu/frame_pacing.cpp
    src/runtime/gpu/gnm/gnm_driver.cpp
    src/runtime/gpu/pm4.cpp
    src/runtime/gpu/pm4_stats.cpp
    src/runtime/gpu/shader/gcn_program.cpp
    src/runtime/gpu/shader/pipeline_state.cpp
    src/runtime/gpu/shader/runtime_params.cpp
    src/runtime/gpu/shader/shader_cache.cpp
    src/runtime/gpu/shader/shader_capture.cpp
    src/runtime/gpu/shader/shader_disk_cache.cpp
    src/runtime/gpu/shader/game_shader_index.cpp
    src/runtime/gpu/shader/shader_frontend.cpp
    src/runtime/gpu/shader/shader_resources.cpp
    src/runtime/gpu/shader/shader_translator.cpp
    src/runtime/gpu/vulkan/vk_buffer_cache.cpp
    src/runtime/gpu/vulkan/vk_device_context.cpp
    src/runtime/gpu/vulkan/vk_diagnostics.cpp
    src/runtime/gpu/vulkan/vk_executor.cpp
    src/runtime/gpu/vulkan/vk_formats.cpp
    src/runtime/gpu/vulkan/vk_capture.cpp
    src/runtime/gpu/vulkan/vk_image_cache.cpp
    src/runtime/gpu/vulkan/vk_pipelines.cpp
    src/runtime/gpu/vulkan/vk_precompile.cpp
    src/runtime/gpu/vulkan/vk_presenter.cpp
    src/runtime/gpu/vulkan/vk_stability_tests.cpp
    src/runtime/gpu/vulkan/vk_resources.cpp
    src/runtime/gpu/host_shaders.cpp
    src/runtime/gpu/image_layout.cpp
    src/runtime/gpu/native_backend.cpp
    src/runtime/gpu/parallel_cp.cpp
    src/runtime/gpu/cpu_sampler.cpp
    src/runtime/gpu/native_videoout.cpp
    src/runtime/gpu/page_tracker.cpp
    src/runtime/gpu/png_writer.cpp
    src/runtime/gpu/vulkan/vk_foundation.cpp
    src/runtime/kernel/clock.cpp
    src/runtime/kernel/condvar.cpp
    src/runtime/kernel/event_flag.cpp
    src/runtime/kernel/event_queues.cpp
    src/runtime/kernel/guest_errno.cpp
    src/runtime/kernel/kernel_memory.cpp
    src/runtime/kernel/kernel_util.cpp
    src/runtime/kernel/memory.cpp
    src/runtime/kernel/mutex.cpp
    src/runtime/kernel/process.cpp
    src/runtime/kernel/rwlock.cpp
    src/runtime/kernel/semaphore.cpp
    src/runtime/kernel/signal.cpp
    src/runtime/kernel/thread.cpp
    src/runtime/kernel/time.cpp
    src/runtime/language.cpp
    src/runtime/libc/libc_mem.cpp
    src/runtime/memory/direct_memory.cpp
    src/runtime/memory/host_address_space.cpp
    src/runtime/memory/virtual_memory.cpp
    src/runtime/memory/vma_map.cpp
    src/runtime/net/net_util.cpp
    src/runtime/pad/pad.cpp
    src/runtime/playgo/playgo.cpp
    src/runtime/psf/param_sfo.cpp
    src/runtime/sysmodule/sysmodule.cpp
    src/runtime/system/system_service.cpp
    src/runtime/user/user_service.cpp
    src/trace/import_counter.cpp
)
target_include_directories(sspc_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# PRIVATE: must not leak into the shadPS4 sources of the bridge target.
target_compile_definitions(sspc_runtime PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(sspc_runtime PRIVATE /utf-8 /W4)
# SDL3: SecondSonPC's own input layer (src/input) reads SDL events; the library comes from the
# bridge's externals until SecondSonPC has its own window (Phase 4).
target_link_libraries(sspc_runtime PUBLIC fmt::fmt toml11::toml11 dxgi SDL3::SDL3)
# Zydis (x86 decoder, MIT): SecondSonPC's FS patcher (src/runtime/cpu, P3.3). An independent library that
# comes from the bridge's externals until SecondSonPC vendors its own copy (Phase 5).
target_link_libraries(sspc_runtime PRIVATE Zydis::Zydis)
# P4.1: the Vulkan headers (header-only, from the bridge's externals) for SecondSonPC's Vulkan foundation
# (src/runtime/gpu/vulkan). No loader is linked: the system vulkan-1.dll is loaded at run time, as the bridge does.
target_link_libraries(sspc_runtime PRIVATE Vulkan::Headers)
# P4.3: xxHash (BSD-2, from the bridge's externals like Zydis): programs' content identity, cache keys.
target_link_libraries(sspc_runtime PRIVATE xxHash::xxhash)
# P3.4: the commit in fatal exception reports (as of the last configure; "-dirty" with local changes then).
execute_process(COMMAND ${GIT_EXECUTABLE} describe --always --dirty --abbrev=10
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE SSPC_GIT_REV
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
set_source_files_properties(src/runtime/cpu/exceptions.cpp PROPERTIES
    COMPILE_DEFINITIONS "SSPC_GIT_REV=\"${SSPC_GIT_REV}\"")

sspc_attach_bootstrap(sspc_runtime)

# P4.14: the native backend's persistent shader cache generation (src/runtime/gpu/shader/shader_disk_cache.*): the
# bridge's recompiler generation and the contents of SecondSonPC's frontend and translator adapter sources. A change of
# one of them re-runs this configure step; a cache written by another generation is never read.
file(GLOB _sspc_gen_inputs CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/runtime/gpu/shader/*.cpp
     ${CMAKE_SOURCE_DIR}/src/runtime/gpu/shader/*.h)
list(FILTER _sspc_gen_inputs EXCLUDE REGEX "(shader_disk_cache|shader_capture|game_shader_index)\\.(cpp|h)$")
list(APPEND _sspc_gen_inputs ${CMAKE_SOURCE_DIR}/src/bridge/shadps4/shader_translator_host.cpp
     ${CMAKE_SOURCE_DIR}/src/bridge/shadps4/shader_bridge_hooks.h)
list(SORT _sspc_gen_inputs)
set(_sspc_gen_acc "${SSPC_SHADER_CACHE_GENERATION}")
foreach(_f IN LISTS _sspc_gen_inputs)
    file(SHA256 "${_f}" _h)
    get_filename_component(_n "${_f}" NAME)
    string(APPEND _sspc_gen_acc "|${_n}:${_h}")
endforeach()
string(SHA256 _sspc_gen_full "${_sspc_gen_acc}")
string(SUBSTRING "${_sspc_gen_full}" 0 16 SSPC_NATIVE_SHADER_GENERATION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_sspc_gen_inputs})
set_source_files_properties(src/runtime/gpu/shader/shader_disk_cache.cpp PROPERTIES
    COMPILE_DEFINITIONS "SSPC_NATIVE_SHADER_GENERATION=\"${SSPC_NATIVE_SHADER_GENERATION}\"")
message(STATUS "SecondSonPC: native shader cache generation ${SSPC_NATIVE_SHADER_GENERATION}")

# P3.6.6: the SecondSonPC <-> bridge boundary, checked whenever a source, a patch or the allowlist changes
# (cmake/sspc_boundary_check.cmake; report in the build directory). A violation fails the build.
file(GLOB_RECURSE _sspc_boundary_inputs CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/src/*.cpp ${CMAKE_SOURCE_DIR}/src/*.h ${CMAKE_SOURCE_DIR}/src/*.inc)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/sspc_boundary.stamp
    COMMAND ${CMAKE_COMMAND} -DSSPC_SRC=${CMAKE_SOURCE_DIR}/src
            -DSSPC_SHAD=${CMAKE_SOURCE_DIR}/third_party/shadPS4/src
            -DSSPC_PATCHES=${CMAKE_SOURCE_DIR}/patches/shadps4
            -DSSPC_ALLOWLIST=${CMAKE_SOURCE_DIR}/cmake/sspc_boundary_allowlist.txt
            -DSSPC_REPORT=${CMAKE_BINARY_DIR}/sspc_boundary_report.txt
            -DSSPC_STAMP=${CMAKE_BINARY_DIR}/sspc_boundary.stamp
            -P ${CMAKE_SOURCE_DIR}/cmake/sspc_boundary_check.cmake
    DEPENDS ${_sspc_boundary_inputs} ${_sspc_shadps4_patches} ${CMAKE_SOURCE_DIR}/cmake/sspc_boundary_allowlist.txt
            ${CMAKE_SOURCE_DIR}/cmake/sspc_boundary_check.cmake
    COMMENT "SecondSonPC: checking the runtime/bridge boundary"
    VERBATIM)
add_custom_target(sspc_boundary_check DEPENDS ${CMAKE_BINARY_DIR}/sspc_boundary.stamp)
add_dependencies(sspc_runtime sspc_boundary_check)

# PC settings and the in-game settings menu (docs/PC_SETTINGS_IMPLEMENTATION.md): SecondSonPC's own code in one block,
# apart from the runtime's list above. The menu overlay draws with Direct2D/DirectWrite in software (no GPU device).
set(SSPC_PC_SETTINGS_SOURCES
    src/input/binding_rules.cpp
    src/input/device_tracker.cpp
    src/input/gamepad_tuning.cpp
    src/settings/registry.cpp
    src/settings/settings_checks.cpp
    src/settings/settings_service.cpp
    src/settings/user_store.cpp
    src/ui/overlay_win32.cpp
    src/ui/settings_menu.cpp
)
# Settings and draw commands are written with designated initializers that leave the other fields at their defaults.
set_source_files_properties(${SSPC_PC_SETTINGS_SOURCES} src/ui/menu_controller.cpp tools/settings_tests/main.cpp
                            PROPERTIES COMPILE_OPTIONS "-Wno-missing-designated-field-initializers")
target_sources(sspc_runtime PRIVATE ${SSPC_PC_SETTINGS_SOURCES} src/ui/menu_controller.cpp)
target_link_libraries(sspc_runtime PUBLIC d2d1 dwrite)
target_sources(shadps4 PRIVATE ${CMAKE_SOURCE_DIR}/src/bridge/shadps4/settings_host.cpp) # the bridge's live settings

# Standalone tests of the PC settings and screenshots of the menu: SecondSonPC's sources only, no bridge, no game
# (cmake --build <dir> --target sspc_settings_tests; tools/settings_tests/main.cpp).
add_executable(sspc_settings_tests EXCLUDE_FROM_ALL
    tools/settings_tests/main.cpp
    ${SSPC_PC_SETTINGS_SOURCES}
    src/app/config.cpp
    src/base/log.cpp
    src/runtime/language.cpp
    src/input/settings.cpp
    src/input/keyboard_mouse.cpp
    src/input/motion.cpp
)
target_include_directories(sspc_settings_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(sspc_settings_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(sspc_settings_tests PRIVATE /utf-8 /W4)
target_link_libraries(sspc_settings_tests PRIVATE fmt::fmt toml11::toml11 SDL3::SDL3 d2d1 dwrite windowscodecs ole32 uuid)
set_target_properties(sspc_settings_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Standalone tests of the reconstructed engine code (src/engine; docs/native-reconstruction): no bridge, no game
# (cmake --build <dir> --target sspc_engine_tests; tools/engine_tests/main.cpp [--bench]).
add_executable(sspc_engine_tests EXCLUDE_FROM_ALL
    tools/engine_tests/main.cpp
    src/engine/guest_hooks.cpp
    src/engine/guest_probe.cpp
    src/engine/job_system.cpp
    src/engine/particles/smoke_quality.cpp
    src/engine/render/dcb_scan.cpp
    src/engine/render/native_draw.cpp
    src/engine/render/native_render_ops.cpp
    src/engine/render/native_state.cpp
    src/engine/render/pass_graph.cpp
    src/runtime/gpu/pm4.cpp
    src/base/log.cpp
)
target_include_directories(sspc_engine_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(sspc_engine_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(sspc_engine_tests PRIVATE /utf-8 /W4)
target_link_libraries(sspc_engine_tests PRIVATE fmt::fmt)
set_target_properties(sspc_engine_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
