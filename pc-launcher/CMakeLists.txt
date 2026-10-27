# inFAMOUS PC Launcher - one native launcher (C++20, Qt 6 Quick) for inFAMOUS: Second Son PC Edition and
# inFAMOUS: First Light PC Edition. docs\BUILD.md.
cmake_minimum_required(VERSION 3.24)
project(inFAMOUSPCLauncher VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
option(LAUNCHER_TESTS "Build the tests and the stub game" OFF)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Concurrent Svg ShaderTools LinguistTools)
qt_standard_project_setup(REQUIRES 6.5)
qt_policy(SET QTP0004 NEW)

if(MSVC)
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /W3 /wd4251)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX)
    # no absolute paths of this machine in a release binary: no PDB path, no __FILE__ in Qt's messages
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_link_options(/DEBUG:NONE /PDBALTPATH:none /Brepro)
        add_compile_options(/Brepro)
    endif()
endif()

configure_file(src/launcher_version.h.in ${CMAKE_BINARY_DIR}/generated/launcher_version.h @ONLY)

# ---- the core: everything but the interface (also linked by the tests)
add_library(launcher_core STATIC
    src/profiles/GameProfile.h src/profiles/GameProfile.cpp
    src/profiles/SecondSonProfile.cpp src/profiles/FirstLightProfile.cpp
    src/detection/ParamSfo.h src/detection/ParamSfo.cpp
    src/detection/GameDetector.h src/detection/GameDetector.cpp
    src/settings/TomlDocument.h src/settings/TomlDocument.cpp
    src/settings/SettingsCatalog.h src/settings/SettingsCatalog.cpp
    src/settings/SettingsManager.h src/settings/SettingsManager.cpp
    src/launch/ProcessManager.h src/launch/ProcessManager.cpp
    src/saves/SaveManager.h src/saves/SaveManager.cpp
    src/diagnostics/DiagnosticsManager.h src/diagnostics/DiagnosticsManager.cpp
    src/system/FileUtils.h src/system/FileUtils.cpp
    src/system/SystemInfo.h src/system/SystemInfo.cpp
    src/system/Log.h src/system/Log.cpp
)
target_include_directories(launcher_core PUBLIC src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(launcher_core PUBLIC Qt6::Core Qt6::Gui Qt6::Concurrent dxgi advapi32 shell32 ole32 user32)

# ---- the icon: generated from assets/brand/launcher.svg at build time (no binary icon in the sources)
qt_add_executable(icongen tools/icongen/main.cpp)
target_link_libraries(icongen PRIVATE Qt6::Gui Qt6::Svg)
set_target_properties(icongen PROPERTIES WIN32_EXECUTABLE OFF RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/generated/launcher.ico
    COMMAND icongen ${CMAKE_CURRENT_SOURCE_DIR}/assets/brand/launcher.svg ${CMAKE_BINARY_DIR}/generated/launcher.ico
    DEPENDS icongen ${CMAKE_CURRENT_SOURCE_DIR}/assets/brand/launcher.svg
    COMMENT "launcher.ico from launcher.svg"
)
add_custom_target(launcher_icon DEPENDS ${CMAKE_BINARY_DIR}/generated/launcher.ico)
set_source_files_properties(resources/launcher.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_BINARY_DIR}/generated/launcher.ico)

# ---- Launcher.exe
qt_add_executable(Launcher WIN32
    src/main.cpp
    src/app/Application.h src/app/Application.cpp
    resources/launcher.rc
)
set_target_properties(Launcher PROPERTIES OUTPUT_NAME "Launcher")
add_dependencies(Launcher launcher_icon)
target_include_directories(Launcher PRIVATE resources ${CMAKE_BINARY_DIR}/generated)
if(MSVC)
    target_link_options(Launcher PRIVATE /MANIFEST:NO)
endif()
target_link_libraries(Launcher PRIVATE launcher_core Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Svg)

file(GLOB_RECURSE LAUNCHER_QML RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS qml/*.qml)
set_source_files_properties(qml/themes/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_add_qml_module(Launcher
    URI Launcher
    VERSION 1.0
    QML_FILES ${LAUNCHER_QML}
)

file(GLOB LAUNCHER_ICONS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS assets/icons/*.svg)
file(GLOB LAUNCHER_ART RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS assets/brand/* assets/second_son/* assets/first_light/*)
qt_add_resources(Launcher "launcher_assets"
    PREFIX "/"
    BASE assets
    FILES ${LAUNCHER_ICONS} ${LAUNCHER_ART} assets/fonts/RobotoCondensed.ttf
)
qt_add_shaders(Launcher "launcher_shaders"
    PREFIX "/"
    BASE shaders
    FILES shaders/ambient.frag shaders/glass.frag
)
qt_add_translations(Launcher
    TS_FILES translations/launcher_ru.ts
    RESOURCE_PREFIX /i18n
    LUPDATE_OPTIONS -no-obsolete
)

if(LAUNCHER_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
