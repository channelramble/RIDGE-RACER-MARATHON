# Standalone build of the libretro host + lr_smoke (independent of the main project):
#   cmake -S tests/libretro -B build-libretro && cmake --build build-libretro -j
cmake_minimum_required(VERSION 3.24)
project(rrm_libretro_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

get_filename_component(RRM_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
find_package(SDL3 CONFIG REQUIRED)

file(GLOB RRM_LR_SOURCES CONFIGURE_DEPENDS ${RRM_ROOT}/src/libretro/*.cpp)
# Input.cpp uses the shared gamepad logic (player slots, dead zones) from src/core.
add_executable(lr_smoke ${RRM_ROOT}/tests/lr_smoke.cpp ${RRM_LR_SOURCES} ${RRM_ROOT}/src/core/Json.cpp
    ${RRM_ROOT}/src/core/Controllers.cpp ${RRM_ROOT}/src/core/Gamepads.cpp)
target_include_directories(lr_smoke PRIVATE ${RRM_ROOT}/src ${RRM_ROOT}/third_party ${RRM_ROOT}/third_party/libretro)
target_link_libraries(lr_smoke PRIVATE SDL3::SDL3)
if(MSVC)
    target_compile_options(lr_smoke PRIVATE /W4)
else()
    target_compile_options(lr_smoke PRIVATE -Wall -Wextra)
endif()
if(APPLE)
    target_link_libraries(lr_smoke PRIVATE "-framework OpenGL")
elseif(WIN32)
    target_link_libraries(lr_smoke PRIVATE opengl32)
else()
    target_link_libraries(lr_smoke PRIVATE ${CMAKE_DL_LIBS})
endif()
