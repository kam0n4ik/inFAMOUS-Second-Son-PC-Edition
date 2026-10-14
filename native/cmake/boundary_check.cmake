# Fails configuration if any native source reaches into the reference hybrid port or shadPS4.
# The native runtime must stay a separate program (docs/native/ARCHITECTURE.md, "Boundary").
function(sspc_boundary_check root)
    file(GLOB_RECURSE sources "${root}/engine/*" "${root}/game/*" "${root}/tools/*.cpp" "${root}/tools/*.h"
         "${root}/tests/*" "${root}/shaders/*")
    set(violations "")
    foreach(f ${sources})
        file(STRINGS "${f}" lines REGEX "^[ \t]*#[ \t]*include")
        foreach(l ${lines})
            string(TOLOWER "${l}" ll)
            # sce*.h: PS4 SDK header names, matched only at the start of a file name.
            if(ll MATCHES "shadps4|third_party|\\.\\./src|bridge/|runtime/(kernel|memory|cpu|gpu|loader)|[<\"/]sce[a-z0-9_]*\\.h|orbis")
                list(APPEND violations "${f}: ${l}")
            endif()
        endforeach()
    endforeach()
    if(violations)
        string(REPLACE ";" "\n  " msg "${violations}")
        message(FATAL_ERROR "Native boundary violated (includes from the reference port / shadPS4):\n  ${msg}")
    endif()
    list(LENGTH sources n)
    message(STATUS "Native boundary check: ${n} files, no references to src/, third_party/ or shadPS4")
endfunction()
