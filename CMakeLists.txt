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
    src/engine/game_clock_probe.cpp
    src/engine/game_fixes.cpp
    src/engine/guest_hooks.cpp
    src/engine/guest_probe.cpp
    src/engine/job_system.cpp
    src/engine/mouse_camera.cpp
    src/engine/particles/smoke_quality.cpp
    src/engine/render/dcb_scan.cpp
    src/engine/render/frame_trace.cpp
    src/engine/render/draw_census.cpp
    src/engine/render/draw_runs.cpp
    src/engine/render/native_draw.cpp
    src/engine/render/native_render_ops.cpp
    src/engine/render/native_state.cpp
    src/engine/render/pass_graph.cpp
    src/engine/render/post_effects.cpp
    src/engine/ui/game_menu.cpp
    src/engine/render/display_plan.cpp
    src/engine/render/render_resolution.cpp
    src/native_engine/guest_code_clone.cpp
    src/native_engine/native_engine_install.cpp
    src/native_engine/native_engine_verify.cpp
    src/native_engine/native_function_registry.cpp
    src/native_engine/native_measure.cpp
    src/native_engine/reconstructed/cue_set_buffers.cpp
    src/native_engine/reconstructed/cue_commit.cpp
    src/native_engine/reconstructed/material_bind.cpp
    src/native_engine/reconstructed/frame_scratch.cpp
    src/native_engine/reconstructed/gbuffer_draw_constants.cpp
    src/native_engine/reconstructed/object_constants.cpp
    src/native_engine/reconstructed/shadow_instance_uv.cpp
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
    src/runtime/gpu/menu_focus.cpp
    src/runtime/filesystem/content_patch.cpp
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
    src/runtime/gpu/static/static_draws.cpp
    src/runtime/gpu/static/static_predictor.cpp
    src/runtime/gpu/vulkan/vk_buffer_cache.cpp
    src/runtime/gpu/vulkan/vk_device_context.cpp
    src/runtime/gpu/vulkan/vk_emit.cpp
    src/runtime/gpu/vulkan/vk_diagnostics.cpp
    src/runtime/gpu/vulkan/vk_executor.cpp
    src/runtime/gpu/vulkan/vk_instancing.cpp
    src/runtime/gpu/vulkan/vk_formats.cpp
    src/runtime/gpu/vulkan/vk_capture.cpp
    src/runtime/gpu/vulkan/vk_image_cache.cpp
    src/runtime/gpu/vulkan/vk_pipelines.cpp
    src/runtime/gpu/vulkan/vk_precompile.cpp
    src/runtime/gpu/vulkan/vk_presenter.cpp
    src/runtime/gpu/vulkan/vk_reflex.cpp
    src/runtime/gpu/vulkan/vk_stability_tests.cpp
    src/runtime/gpu/vulkan/vk_resources.cpp
    src/runtime/gpu/upscale/upscalers.cpp
    src/runtime/gpu/upscale/fsr_framegen.cpp
    src/runtime/gpu/vulkan/vk_executor_upscale.cpp
    src/runtime/gpu/vulkan/vk_executor_r11.cpp
    src/runtime/gpu/host_shaders.cpp
    src/runtime/gpu/image_layout.cpp
    src/runtime/gpu/native_backend.cpp
    src/runtime/gpu/parallel_cp.cpp
    src/runtime/gpu/native_gbuffer_prep.cpp
    src/runtime/gpu/cp_mode.cpp
    src/runtime/gpu/cpu_sampler.cpp
    src/runtime/gpu/native_videoout.cpp
    src/runtime/gpu/page_tracker.cpp
    src/runtime/gpu/png_writer.cpp
    src/runtime/gpu/vulkan/vk_foundation.cpp
    src/runtime/kernel/clock.cpp
    src/runtime/kernel/condvar.cpp
    src/runtime/kernel/event_flag.cpp
    src/runtime/kernel/equeue.cpp
    src/runtime/savedata/savedata.cpp
    src/runtime/audio/audio_out.cpp
    src/runtime/media/videodec.cpp
    src/runtime/gpu/gcn/gcn_aux.cpp
    src/runtime/gpu/gcn/gcn_cfg.cpp
    src/runtime/gpu/gcn/gcn_decoder.cpp
    src/runtime/gpu/gcn/gcn_emit.cpp
    src/runtime/gpu/gcn/gcn_inventory.cpp
    src/runtime/gpu/gcn/gcn_resources.cpp
    src/runtime/gpu/gcn/gcn_translator.cpp
    src/runtime/gpu/gcn/gcn_structure.cpp
    src/runtime/gpu/gcn/spirv_builder.cpp
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
    src/runtime/np/np_manager.cpp
    src/runtime/np/offline_services.cpp
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
# Native Engine Reconstruction (src/native_engine): xbyak (BSD-3, the bridge's externals like Zydis) generates the
# signature-independent timing stubs of SSPC_NATIVE_ENGINE_MEASURE; reconstructions of AVX code are built for AVX.
target_link_libraries(sspc_runtime PRIVATE xbyak::xbyak)
set_source_files_properties(src/native_engine/reconstructed/object_constants.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX")
set_source_files_properties(src/native_engine/reconstructed/gbuffer_draw_constants.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX")
set_source_files_properties(src/native_engine/reconstructed/shadow_instance_uv.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX")
# R6 (docs/NATIVE_RENDER_R6_CORE.md §3): each subsystem's sources in a list of its own (cmake/r6/<subsystem>.cmake),
# owned by one branch, so the parallel branches never edit the same lines here
foreach(_r6 core gbuffer shadows cp)
    include(${CMAKE_SOURCE_DIR}/cmake/r6/${_r6}.cmake)
endforeach()
target_sources(sspc_runtime PRIVATE ${SSPC_R6_CORE_SOURCES} ${SSPC_R6_GBUFFER_SOURCES} ${SSPC_R6_SHADOWS_SOURCES}
               ${SSPC_R6_CP_SOURCES})
# R7 (docs/NATIVE_ENGINE_R7_CORE.md §3): the same, one list per R7 branch (cmake/r7/<subsystem>.cmake)
foreach(_r7 core gbuffer shadows resources)
    include(${CMAKE_SOURCE_DIR}/cmake/r7/${_r7}.cmake)
endforeach()
target_sources(sspc_runtime PRIVATE ${SSPC_R7_CORE_SOURCES} ${SSPC_R7_GBUFFER_SOURCES} ${SSPC_R7_SHADOWS_SOURCES}
               ${SSPC_R7_RESOURCES_SOURCES})
# R8 (docs/NATIVE_ENGINE_R8_CORE.md §3): the same, one list per R8 branch (cmake/r8/<subsystem>.cmake)
foreach(_r8 core profile cp render recomp verify)
    include(${CMAKE_SOURCE_DIR}/cmake/r8/${_r8}.cmake)
endforeach()
target_sources(sspc_runtime PRIVATE ${SSPC_R8_CORE_SOURCES} ${SSPC_R8_PROFILE_SOURCES} ${SSPC_R8_CP_SOURCES}
               ${SSPC_R8_RENDER_SOURCES} ${SSPC_R8_RECOMP_SOURCES} ${SSPC_R8_VERIFY_SOURCES})
# R9 (docs/R9_GAMEPLAY_PERFORMANCE.md): the same, one list per R9 part (cmake/r9/<part>.cmake)
foreach(_r9 profile)
    include(${CMAKE_SOURCE_DIR}/cmake/r9/${_r9}.cmake)
endforeach()
target_sources(sspc_runtime PRIVATE ${SSPC_R9_PROFILE_SOURCES})
# R10 sysrt (docs/R10_SYSRT.md): its own list (cmake/r10/sysrt.cmake), the bridge's part in sspc_attach_bootstrap
include(${CMAKE_SOURCE_DIR}/cmake/r10/sysrt.cmake)
target_sources(sspc_runtime PRIVATE ${SSPC_R10_SYSRT_SOURCES})
# R10 sched (docs/R10_CPU_GPU_SCHEDULER.md): the latency probe and the pipeline workers' pool (cmake/r10/sched.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/r10/sched.cmake)
target_sources(sspc_runtime PRIVATE ${SSPC_R10_SCHED_SOURCES})
# R11 main (docs/R11_MAIN_THREAD.md): Game:Main's waits (cmake/r11/main.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/r11/main.cmake)
target_sources(sspc_runtime PRIVATE ${SSPC_R11_MAIN_SOURCES})
# libSceVideodec (runtime/media/videodec.cpp): FFmpeg's H.264 decoder, the library the oracle uses too
target_link_libraries(sspc_runtime PRIVATE FFmpeg::ffmpeg)
# R10 (docs/R10_AJM.md): libSceAjm, SecondSonPC's own (runtime/audio/ajm.cpp, SSPC_R10_AJM=native|verify), on its
# own copy of LibAtrac9 (third_party/LibAtrac9, MIT). Every external symbol of the copy is renamed (SspcAt9_*) by a
# force-included header, so it never meets the bridge's LibAtrac9 at link time and keeps its own global tables.
file(GLOB SSPC_ATRAC9_SOURCES ${CMAKE_SOURCE_DIR}/third_party/LibAtrac9/src/*.c)
add_library(sspc_atrac9 STATIC ${SSPC_ATRAC9_SOURCES})
target_compile_definitions(sspc_atrac9 PRIVATE SSPC_ATRAC9_BUILD _CRT_SECURE_NO_WARNINGS)
target_compile_options(sspc_atrac9 PRIVATE "/FI${CMAKE_SOURCE_DIR}/third_party/LibAtrac9/sspc_atrac9_prefix.h" -w)
target_include_directories(sspc_atrac9 INTERFACE ${CMAKE_SOURCE_DIR}/third_party/LibAtrac9) # <src/libatrac9.h>
target_sources(sspc_runtime PRIVATE src/runtime/audio/ajm.cpp)
target_link_libraries(sspc_runtime PRIVATE sspc_atrac9)
# P4.1: the Vulkan headers (header-only, from the bridge's externals) for SecondSonPC's Vulkan foundation
# (src/runtime/gpu/vulkan). No loader is linked: the system vulkan-1.dll is loaded at run time, as the bridge does.
target_link_libraries(sspc_runtime PRIVATE Vulkan::Headers)
# the Khronos SPIR-V tables of SecondSonPC's own shader translator (runtime/gpu/gcn/spirv_builder)
target_include_directories(sspc_runtime PRIVATE ${CMAKE_SOURCE_DIR}/third_party/spirv_headers/include)
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
# The own GCN translator's generation (src/runtime/gpu/gcn/*: not part of the one above), written beside the
# executable: a cache made by an earlier own translator is told apart by it.
file(GLOB _sspc_own_inputs CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/runtime/gpu/gcn/*.cpp
     ${CMAKE_SOURCE_DIR}/src/runtime/gpu/gcn/*.h ${CMAKE_SOURCE_DIR}/src/runtime/gpu/gcn/*.inc)
list(SORT _sspc_own_inputs)
set(_sspc_own_acc "")
foreach(_f IN LISTS _sspc_own_inputs)
    file(SHA256 "${_f}" _h)
    get_filename_component(_n "${_f}" NAME)
    string(APPEND _sspc_own_acc "|${_n}:${_h}")
endforeach()
string(SHA256 _sspc_own_full "${_sspc_own_acc}")
string(SUBSTRING "${_sspc_own_full}" 0 16 SSPC_OWN_TRANSLATOR_GENERATION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_sspc_own_inputs})
file(WRITE ${CMAKE_BINARY_DIR}/bin/own_translator_generation.txt "${SSPC_OWN_TRANSLATOR_GENERATION}
")

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
    src/settings/experiments.cpp
    src/settings/registry.cpp
    src/settings/settings_checks.cpp
    src/settings/settings_service.cpp
    src/settings/user_store.cpp
    src/ui/overlay_win32.cpp
    src/ui/prompt_icons.cpp
    src/ui/prompt_textures.cpp
    src/ui/settings_menu.cpp
)
# Settings and draw commands are written with designated initializers that leave the other fields at their defaults.
set_source_files_properties(${SSPC_PC_SETTINGS_SOURCES} src/ui/menu_controller.cpp
                            PROPERTIES COMPILE_OPTIONS "-Wno-missing-designated-field-initializers")
target_sources(sspc_runtime PRIVATE ${SSPC_PC_SETTINGS_SOURCES} src/ui/menu_controller.cpp src/ui/prompt_service.cpp)
target_link_libraries(sspc_runtime PUBLIC d2d1 dwrite windowscodecs ole32)
target_sources(shadps4 PRIVATE ${CMAKE_SOURCE_DIR}/src/bridge/shadps4/settings_host.cpp) # the bridge's live settings

# The standalone tests (sspc_settings_tests, sspc_engine_tests, sspc_native_engine_tests: the PC settings, and the
# reconstructed engine code against the game's eboot.bin) are not in this tree: tag archive/tests-20261023.

# Upscaling and frame generation (docs/UPSCALING_FRAME_GENERATION.md). The vendors' SDKs are not part of the sources:
# they are put into third_party/upscalers by hand (README.md, Building from source: NVIDIA DLSS SDK headers +
# nvsdk_ngx_s.lib, AMD FidelityFX API headers, NVIDIA Streamline headers, and the runtime DLLs copied next to
# SecondSonPC.exe).
set(SSPC_UPSCALERS_DIR "${CMAKE_SOURCE_DIR}/third_party/upscalers" CACHE PATH "Staged upscaler SDKs")
if(EXISTS "${SSPC_UPSCALERS_DIR}/ngx/include/nvsdk_ngx_vk.h" AND EXISTS "${SSPC_UPSCALERS_DIR}/ngx/lib/nvsdk_ngx_s.lib")
    # sspc_dlss.dll: the only binary that links NVIDIA's NGX SDK (static CRT, as nvsdk_ngx_s.lib). SecondSonPC.exe
    # loads it at run time; without it DLSS is unavailable and everything else runs.
    add_library(sspc_dlss SHARED src/dlss_bridge/sspc_dlss_bridge.cpp src/dlss_bridge/sspc_dlss_bridge.h)
    set_target_properties(sspc_dlss PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded"
                          RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    target_include_directories(sspc_dlss PRIVATE "${SSPC_UPSCALERS_DIR}/ngx/include" "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(sspc_dlss PRIVATE Vulkan::Headers "${SSPC_UPSCALERS_DIR}/ngx/lib/nvsdk_ngx_s.lib")
    target_compile_definitions(sspc_dlss PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    add_dependencies(shadps4 sspc_dlss)
    message(STATUS "SecondSonPC: DLSS bridge (sspc_dlss.dll) from ${SSPC_UPSCALERS_DIR}/ngx")
else()
    message(STATUS "SecondSonPC: no NGX SDK in ${SSPC_UPSCALERS_DIR}/ngx - sspc_dlss.dll is not built (DLSS unavailable)")
endif()
if(EXISTS "${SSPC_UPSCALERS_DIR}/ffx-api/include/ffx_api/ffx_api.h")
    target_include_directories(sspc_runtime PRIVATE "${SSPC_UPSCALERS_DIR}/ffx-api/include")
    target_compile_definitions(sspc_runtime PRIVATE SSPC_HAVE_FFX_API=1)
endif()
if(EXISTS "${SSPC_UPSCALERS_DIR}/streamline/include/sl.h")
    target_include_directories(sspc_runtime PRIVATE "${SSPC_UPSCALERS_DIR}/streamline/include")
    target_compile_definitions(sspc_runtime PRIVATE SSPC_HAVE_STREAMLINE=1)
endif()
# The vendors' runtime DLLs next to the executable (each is loaded only when its feature is used).
file(GLOB _sspc_upscaler_dlls "${SSPC_UPSCALERS_DIR}/bin/*.dll")
if(_sspc_upscaler_dlls)
    add_custom_target(sspc_upscaler_dlls ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/bin
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_sspc_upscaler_dlls} ${CMAKE_BINARY_DIR}/bin
        COMMENT "SecondSonPC: upscaler runtime DLLs")
    add_dependencies(shadps4 sspc_upscaler_dlls)
endif()
# The game's menu faces for the PC Edition's menus (ui/overlay_win32.cpp: <exe dir>onts; OFL / Apache 2.0, licences
# beside them).
file(GLOB _sspc_fonts "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/*")
add_custom_target(sspc_fonts ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/bin/fonts
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_sspc_fonts} ${CMAKE_BINARY_DIR}/bin/fonts
    COMMENT "SecondSonPC: menu fonts")
add_dependencies(shadps4 sspc_fonts)
