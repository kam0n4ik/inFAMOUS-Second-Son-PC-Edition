# inFAMOUS PC Launcher — the tests (Qt Test) and the stub game. They work on fixture folders in %TEMP% only: no real
# game is started, no real settings or saves are written. The param.sfo files of the real games are read (never
# written) to build the fixtures; a test that needs one it cannot find is skipped and says so.
find_package(Qt6 REQUIRED COMPONENTS Test)

set(LAUNCHER_SS_SFO "E:/shdps4/CUSA00004/sce_sys/param.sfo" CACHE FILEPATH "Second Son's param.sfo (read only, for fixtures)")
set(LAUNCHER_FL_SFO "E:/inFAMOUS-First-Light-PC-Edition/sce_sys/param.sfo" CACHE FILEPATH "First Light's param.sfo (read only, for fixtures)")

# the stub game: a window titled like the game's, a report of its arguments / working folder / environment, an exit code
add_executable(stub_game WIN32 stub/stub_game.cpp)
target_compile_definitions(stub_game PRIVATE UNICODE _UNICODE)
target_link_libraries(stub_game PRIVATE user32)

function(launcher_test name)
    add_executable(${name} ${name}.cpp fixtures.h)
    target_link_libraries(${name} PRIVATE launcher_core Qt6::Test)
    target_compile_definitions(${name} PRIVATE
        LAUNCHER_SS_SFO="${LAUNCHER_SS_SFO}" LAUNCHER_FL_SFO="${LAUNCHER_FL_SFO}"
        LAUNCHER_STUB="$<TARGET_FILE:stub_game>" LAUNCHER_EXE="$<TARGET_FILE:Launcher>")
    add_dependencies(${name} stub_game)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

launcher_test(tst_detection)
launcher_test(tst_settings)
launcher_test(tst_saves)
launcher_test(tst_process)
