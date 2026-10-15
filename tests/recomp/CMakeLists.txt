# Private test project for the recomp / native-port adapters (not part of the
# main build). Builds tests/recomp_tool.cpp against the core sources it needs.
#
#   cmake -S tests/recomp -B build-recomp -G "Unix Makefiles"
#   cmake --build build-recomp -j8
#   build-recomp/recomp_tool --help
#
# Files that other modules provide but that may not exist yet are replaced by
# TEMPORARY test-only stand-ins from this folder (see the *_temp.cpp files).
cmake_minimum_required(VERSION 3.24)
project(rrm_recomp_test LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

get_filename_component(RRM_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
find_package(SDL3 CONFIG REQUIRED)

set(RRM_CORE
    ${RRM_ROOT}/src/core/Catalog.cpp
    ${RRM_ROOT}/src/core/Json.cpp
    ${RRM_ROOT}/src/core/Library.cpp
    ${RRM_ROOT}/src/core/Settings.cpp
    ${RRM_ROOT}/src/core/Platform.cpp
    ${RRM_ROOT}/src/core/Process.cpp
    ${RRM_ROOT}/src/core/Net.cpp
    ${RRM_ROOT}/src/backends/Backend.cpp)

# The adapters under test (compiled with -Wall -Wextra).
set(RRM_MINE
    ${RRM_ROOT}/src/backends/PsxRecomp.cpp
    ${RRM_ROOT}/src/backends/RageRacerPc.cpp
    ${RRM_ROOT}/src/backends/RaveRacerNative.cpp
    ${RRM_ROOT}/src/backends/Fukami.cpp
    ${RRM_ROOT}/src/backends/Rr6Recomp.cpp
    ${RRM_ROOT}/tests/recomp_tool.cpp)

set(RRM_EXTRA ${CMAKE_CURRENT_SOURCE_DIR}/stubs.cpp)

# Disc / ISO9660 readers (Fukami's first-run disc preparation uses them).
file(GLOB RRM_FORMATS ${RRM_ROOT}/src/core/formats/*.cpp)
list(APPEND RRM_EXTRA ${RRM_FORMATS})

# Config editors: real implementation when present, temporary stand-in otherwise.
file(GLOB RRM_CFG ${RRM_ROOT}/src/core/ConfigFiles.cpp ${RRM_ROOT}/src/core/TomlEdit.cpp)
if(RRM_CFG)
    list(APPEND RRM_EXTRA ${RRM_CFG})
    message(STATUS "recomp test: using real ConfigFiles: ${RRM_CFG}")
else()
    list(APPEND RRM_EXTRA ${CMAKE_CURRENT_SOURCE_DIR}/configfiles_temp.cpp)
    message(STATUS "recomp test: TEMPORARY ConfigFiles stand-in (tests/recomp/configfiles_temp.cpp)")
endif()

# MAME romset rebuilding (Rave Racer).
if(EXISTS ${RRM_ROOT}/src/core/RomSet.cpp AND EXISTS ${RRM_ROOT}/third_party/miniz/miniz.c)
    list(APPEND RRM_EXTRA ${RRM_ROOT}/src/core/RomSet.cpp ${RRM_ROOT}/third_party/miniz/miniz.c)
    set_source_files_properties(${RRM_ROOT}/third_party/miniz/miniz.c PROPERTIES COMPILE_OPTIONS "-w")
    message(STATUS "recomp test: using real RomSet")
else()
    list(APPEND RRM_EXTRA ${CMAKE_CURRENT_SOURCE_DIR}/romset_temp.cpp)
    message(STATUS "recomp test: TEMPORARY RomSet stand-in (tests/recomp/romset_temp.cpp)")
endif()

add_executable(recomp_tool ${RRM_MINE} ${RRM_CORE} ${RRM_EXTRA})
target_include_directories(recomp_tool PRIVATE
    ${RRM_ROOT}/src
    ${RRM_ROOT}/third_party
    ${RRM_ROOT}/third_party/tomlplusplus
    ${RRM_ROOT}/third_party/miniz)
target_compile_definitions(recomp_tool PRIVATE RRM_VERSION="recomp-test"
    RRM_SOURCE_DIR="${RRM_ROOT}")
target_link_libraries(recomp_tool PRIVATE SDL3::SDL3)
set_source_files_properties(${RRM_MINE} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")
