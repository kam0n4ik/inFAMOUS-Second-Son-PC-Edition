# Copies SRC to DST only when DST does not exist (keeps a user-edited config next to the exe).
#   cmake -DSRC=<file> -DDST=<file> -P copy_if_missing.cmake
if(NOT EXISTS "${DST}")
    configure_file("${SRC}" "${DST}" COPYONLY)
endif()
