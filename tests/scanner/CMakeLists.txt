# Private build of the scanner module and its tests (not used by the main build):
#   cmake -S tests/scanner -B build-scanner && cmake --build build-scanner -j
#   build-scanner/scanner_unit_tests
#   build-scanner/scan_tool catalog <folder> [--verify] [--rebuild-mame rave-racer /tmp/raverace.zip]
cmake_minimum_required(VERSION 3.24)
project(rrm_scanner_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

get_filename_component(RRM_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
find_package(SDL3 CONFIG REQUIRED)  # Platform.cpp uses SDL

add_library(scan_miniz STATIC ${RRM_ROOT}/third_party/miniz/miniz.c)
target_include_directories(scan_miniz PUBLIC ${RRM_ROOT}/third_party)

file(GLOB RRM_FORMAT_SOURCES CONFIGURE_DEPENDS ${RRM_ROOT}/src/core/formats/*.cpp)
set(RRM_SCANNER_SOURCES
    ${RRM_ROOT}/src/core/Scanner.cpp
    ${RRM_ROOT}/src/core/RomSet.cpp
    ${RRM_FORMAT_SOURCES})
add_library(scan_core STATIC
    ${RRM_ROOT}/src/core/Catalog.cpp
    ${RRM_ROOT}/src/core/Json.cpp
    ${RRM_ROOT}/src/core/Library.cpp
    ${RRM_ROOT}/src/core/Settings.cpp
    ${RRM_ROOT}/src/core/Platform.cpp
    ${RRM_SCANNER_SOURCES})
target_include_directories(scan_core PUBLIC ${RRM_ROOT}/src ${RRM_ROOT}/third_party)
target_link_libraries(scan_core PUBLIC scan_miniz SDL3::SDL3)

if(MSVC)
    set(RRM_WARNINGS /W4)
else()
    set(RRM_WARNINGS -Wall -Wextra -Wshadow)
endif()
set_source_files_properties(${RRM_SCANNER_SOURCES} ${RRM_ROOT}/tests/scan_tool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/unit_tests.cpp PROPERTIES COMPILE_OPTIONS "${RRM_WARNINGS}")

add_executable(scan_tool ${RRM_ROOT}/tests/scan_tool.cpp)
target_link_libraries(scan_tool PRIVATE scan_core)

add_executable(scanner_unit_tests ${CMAKE_CURRENT_SOURCE_DIR}/unit_tests.cpp)
target_link_libraries(scanner_unit_tests PRIVATE scan_core)

enable_testing()
add_test(NAME scanner_unit_tests COMMAND scanner_unit_tests)
