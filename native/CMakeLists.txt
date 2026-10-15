# SecondSonPC native runtime ("Native Rebirth").
#
# Independent of the reference hybrid port in ../src and of third_party/shadPS4: this project
# never includes or links anything from them (enforced by cmake/boundary_check.cmake).
# Toolchain: MSVC (cl/link) + Windows SDK; Vulkan headers and glslc from the LunarG Vulkan SDK.
cmake_minimum_required(VERSION 3.24)
project(SecondSonPCNative LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(NOT WIN32)
    message(FATAL_ERROR "SecondSonPC native targets Windows x64")
endif()

include(cmake/boundary_check.cmake)
sspc_boundary_check(${CMAKE_CURRENT_SOURCE_DIR})

# --- Vulkan SDK (headers + glslc; the loader DLL is loaded at run time) -----------------------
set(SSPC_VULKAN_SDK "$ENV{VULKAN_SDK}" CACHE PATH "LunarG Vulkan SDK root")
if(NOT SSPC_VULKAN_SDK)
    set(SSPC_VULKAN_SDK "E:/VulkanSDK")
endif()
if(NOT EXISTS "${SSPC_VULKAN_SDK}/Include/vulkan/vulkan.h")
    message(FATAL_ERROR "Vulkan SDK headers not found under ${SSPC_VULKAN_SDK}/Include (set SSPC_VULKAN_SDK)")
endif()
find_program(SSPC_GLSLC glslc HINTS "${SSPC_VULKAN_SDK}/Bin" REQUIRED)

# --- Shaders -> SPIR-V -> generated headers ---------------------------------------------------
set(SSPC_GEN_DIR ${CMAKE_BINARY_DIR}/generated)
set(SSPC_SHADER_HEADERS)
function(sspc_shader src symbol header_name)
    set(in ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${src})
    set(spv ${SSPC_GEN_DIR}/shaders/${src}.spv)
    set(hdr ${SSPC_GEN_DIR}/shaders/${header_name})
    add_custom_command(
        OUTPUT ${hdr}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SSPC_GEN_DIR}/shaders
        COMMAND ${SSPC_GLSLC} --target-env=vulkan1.3 -O -Werror ${in} -o ${spv}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${spv} -DOUTPUT=${hdr} -DSYMBOL=${symbol}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake
        DEPENDS ${in} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake
        COMMENT "glslc ${src}"
        VERBATIM)
    set(SSPC_SHADER_HEADERS ${SSPC_SHADER_HEADERS} ${hdr} PARENT_SCOPE)
endfunction()
sspc_shader(mesh.vert kMeshVertSpv mesh_vert_spv.h)
sspc_shader(mesh.frag kMeshFragSpv mesh_frag_spv.h)
sspc_shader(skinned.vert kSkinnedVertSpv skinned_vert_spv.h)
add_custom_target(sspc_shaders DEPENDS ${SSPC_SHADER_HEADERS})

# --- Common compile settings ------------------------------------------------------------------
add_library(sspc_options INTERFACE)
target_compile_definitions(sspc_options INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
if(MSVC)
    target_compile_options(sspc_options INTERFACE /W4 /permissive- /utf-8 /EHsc /Zi /Zc:__cplusplus /MP)
    target_link_options(sspc_options INTERFACE /DEBUG:FULL)
endif()
option(SSPC_ASAN "Build with AddressSanitizer (MSVC /fsanitize=address)" OFF)
if(SSPC_ASAN AND MSVC)
    target_compile_options(sspc_options INTERFACE /fsanitize=address)
    target_compile_definitions(sspc_options INTERFACE _DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION)
endif()

# --- Engine library ---------------------------------------------------------------------------
add_library(sspc_engine STATIC
    engine/core/log.cpp
    engine/core/crash.cpp
    engine/core/config.cpp
    engine/core/file.cpp
    engine/core/clock.cpp
    engine/core/jobs.cpp
    engine/platform/window.cpp
    engine/platform/input.cpp
    engine/resources/kcap.cpp
    engine/resources/gcn_buffer.cpp
    engine/resources/geometry.cpp
    engine/resources/spcmesh.cpp
    engine/renderer/vk_api.cpp
    engine/renderer/renderer.cpp
    engine/gameplay/game_loop.cpp
    engine/gameplay/character.cpp
    engine/scene/primitives.cpp
    engine/scene/character_model.cpp
    engine/scene/world_scene.cpp
    engine/resources/world_chunk.cpp
    engine/resources/texture.cpp
    engine/resources/texture_library.cpp
    engine/resources/skeleton.cpp
    engine/resources/exports.cpp
    engine/scene/test_pose.cpp
    engine/gameplay/terrain.cpp
    engine/resources/anim_clip.cpp
    engine/scene/animator.cpp
)
add_dependencies(sspc_engine sspc_shaders)
target_include_directories(sspc_engine PUBLIC engine ${SSPC_GEN_DIR} "${SSPC_VULKAN_SDK}/Include")
target_link_libraries(sspc_engine PUBLIC sspc_options user32 shell32)

# --- Game -------------------------------------------------------------------------------------
add_executable(SecondSonPC WIN32 game/main.cpp)
target_link_libraries(SecondSonPC PRIVATE sspc_engine)
add_custom_command(TARGET SecondSonPC POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/config/SecondSonPC.ini
            -DDST=$<TARGET_FILE_DIR:SecondSonPC>/SecondSonPC.ini
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_if_missing.cmake)

# --- Tools ------------------------------------------------------------------------------------
add_executable(sspc_inspect tools/asset_inspector/main.cpp)
target_link_libraries(sspc_inspect PRIVATE sspc_engine)
add_executable(sspc_convert tools/asset_converter/main.cpp)
target_link_libraries(sspc_convert PRIVATE sspc_engine)
add_executable(sspc_depaudit tools/dep_audit/main.cpp)
target_link_libraries(sspc_depaudit PRIVATE sspc_options)

# --- Tests ------------------------------------------------------------------------------------
add_executable(sspc_tests
    tests/test_main.cpp
    tests/test_core.cpp
    tests/test_kcap.cpp
    tests/test_geometry.cpp
    tests/test_gameplay.cpp
    tests/test_real_assets.cpp
    tests/test_world.cpp
    tests/test_texture.cpp
    tests/test_skeleton.cpp
    tests/test_animation.cpp
)
target_link_libraries(sspc_tests PRIVATE sspc_engine)
target_include_directories(sspc_tests PRIVATE tests)

enable_testing()
add_test(NAME unit COMMAND sspc_tests)
add_test(NAME dependency_audit_static
         COMMAND sspc_depaudit $<TARGET_FILE:SecondSonPC> --report ${CMAKE_BINARY_DIR}/dependency_audit.md)
