# Cross-compile Ridge Racer Marathon for 64-bit Windows with mingw-w64 (GCC).
#
#   macOS:          brew install mingw-w64 ninja
#   Debian/Ubuntu:  sudo apt install mingw-w64 ninja-build   (uses the -posix thread variant)
#
#   cmake -S . -B build-win -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
#
# The top-level CMakeLists links libgcc/libstdc++/winpthread statically for MinGW, so the
# resulting .exe only depends on DLLs that ship with Windows.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(RRM_MINGW_TRIPLE x86_64-w64-mingw32 CACHE STRING "mingw-w64 target triple")
set(_rrm_tc_hints
    /opt/homebrew/bin /usr/local/bin /usr/bin
    /opt/homebrew/opt/mingw-w64/bin /usr/local/opt/mingw-w64/bin)

# Debian/Ubuntu ship win32- and posix-thread flavours; std::thread needs the posix one.
find_program(CMAKE_C_COMPILER NAMES ${RRM_MINGW_TRIPLE}-gcc-posix ${RRM_MINGW_TRIPLE}-gcc
             HINTS ${_rrm_tc_hints} REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES ${RRM_MINGW_TRIPLE}-g++-posix ${RRM_MINGW_TRIPLE}-g++
             HINTS ${_rrm_tc_hints} REQUIRED)
find_program(CMAKE_RC_COMPILER NAMES ${RRM_MINGW_TRIPLE}-windres windres
             HINTS ${_rrm_tc_hints} REQUIRED)
find_program(CMAKE_AR NAMES ${RRM_MINGW_TRIPLE}-gcc-ar ${RRM_MINGW_TRIPLE}-ar HINTS ${_rrm_tc_hints})
find_program(CMAKE_RANLIB NAMES ${RRM_MINGW_TRIPLE}-gcc-ranlib ${RRM_MINGW_TRIPLE}-ranlib HINTS ${_rrm_tc_hints})
find_program(CMAKE_STRIP NAMES ${RRM_MINGW_TRIPLE}-strip HINTS ${_rrm_tc_hints})

# Search headers/libraries only inside the mingw sysroot, programs only on the host.
get_filename_component(_rrm_tc_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
foreach(_root
        "${_rrm_tc_bin}/../${RRM_MINGW_TRIPLE}"
        "${_rrm_tc_bin}/../toolchain-x86_64/${RRM_MINGW_TRIPLE}"
        "/usr/${RRM_MINGW_TRIPLE}")
    if(EXISTS "${_root}")
        get_filename_component(_root "${_root}" REALPATH)
        list(APPEND CMAKE_FIND_ROOT_PATH "${_root}")
    endif()
endforeach()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
