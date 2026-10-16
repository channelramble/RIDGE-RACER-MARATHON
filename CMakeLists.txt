cmake_minimum_required(VERSION 3.24)

project(RidgeRacerMarathon VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
    # Sources are UTF-8; large template-heavy translation units (json, toml++).
    add_compile_options(/utf-8 /bigobj)
endif()

# SDL3: vendored static build for releases; a system/Homebrew SDL3 is fine for development.
set(RRM_SDL_TAG "release-3.4.18" CACHE STRING "SDL3 git tag used when building SDL from source")
if(ANDROID OR IOS OR WIN32)
    set(_rrm_vendored_default ON)
else()
    set(_rrm_vendored_default OFF)
endif()
option(RRM_VENDORED_SDL "Build SDL3 from source and link it statically" ${_rrm_vendored_default})
option(RRM_BUILD_TESTS "Build developer test tools" ON)

if(NOT RRM_VENDORED_SDL)
    find_package(SDL3 CONFIG QUIET)
    if(NOT SDL3_FOUND)
        message(STATUS "System SDL3 not found; building SDL3 from source")
        set(RRM_VENDORED_SDL ON)
    endif()
endif()

if(RRM_VENDORED_SDL)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG ${RRM_SDL_TAG}
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL3)
    set(RRM_SDL_TARGET SDL3::SDL3-static)
else()
    set(RRM_SDL_TARGET SDL3::SDL3)
endif()

# ---- third party -----------------------------------------------------------------
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
add_library(rrm_imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(rrm_imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(rrm_imgui PUBLIC ${RRM_SDL_TARGET})
if(ANDROID OR IOS)
    target_compile_definitions(rrm_imgui PUBLIC IMGUI_IMPL_OPENGL_ES3)
endif()

# OpenGL / GLES linkage (function pointers come from SDL_GL_GetProcAddress where possible).
if(ANDROID)
    set(RRM_GL_LIBS GLESv3 EGL log android)
elseif(IOS)
    set(RRM_GL_LIBS "-framework OpenGLES" "-framework QuartzCore" "-framework UIKit")
elseif(APPLE)
    set(RRM_GL_LIBS "-framework OpenGL")
elseif(WIN32)
    set(RRM_GL_LIBS opengl32)
else()
    set(RRM_GL_LIBS ${CMAKE_DL_LIBS})
endif()

# ---- Marathon core (no UI) -------------------------------------------------------------
file(GLOB RRM_CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/formats/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/*.cpp)
add_library(rrm_miniz STATIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz/miniz.c)
target_include_directories(rrm_miniz PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz)
if(NOT MSVC)
    target_compile_options(rrm_miniz PRIVATE -w)
endif()

add_library(rrm_core STATIC ${RRM_CORE_SOURCES})
target_include_directories(rrm_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tomlplusplus)
target_link_libraries(rrm_core PUBLIC rrm_miniz)
target_link_libraries(rrm_core PUBLIC ${RRM_SDL_TARGET})
target_compile_definitions(rrm_core PUBLIC RRM_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_compile_definitions(rrm_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

# ---- in-process libretro host ----------------------------------------------------------
file(GLOB RRM_LR_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/libretro/*.cpp)
add_library(rrm_libretro STATIC ${RRM_LR_SOURCES})
target_include_directories(rrm_libretro PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libretro)
target_link_libraries(rrm_libretro PUBLIC rrm_core ${RRM_SDL_TARGET} ${RRM_GL_LIBS})
if(ANDROID OR IOS)
    target_compile_definitions(rrm_libretro PUBLIC RRM_GLES=1)
endif()

# ---- application ---------------------------------------------------------------------------
file(GLOB RRM_APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/app/*.cpp)
set(RRM_RESOURCE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/catalog ${CMAKE_CURRENT_SOURCE_DIR}/assets)
# Licence files shipped with every binary: Marathon's licence, the third-party notices and the
# full third-party licence texts (font licences travel inside assets/fonts).
set(RRM_LEGAL_FILES ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md)
file(GLOB RRM_LICENSE_TEXTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/LICENSES/*.txt)

if(ANDROID)
    # SDL's Android Java glue loads libmain.so.
    add_library(main SHARED ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp ${RRM_APP_SOURCES})
    set(RRM_APP_TARGET main)
else()
    add_executable(RidgeRacerMarathon ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp ${RRM_APP_SOURCES})
    set(RRM_APP_TARGET RidgeRacerMarathon)
endif()
target_link_libraries(${RRM_APP_TARGET} PRIVATE rrm_core rrm_libretro rrm_imgui ${RRM_SDL_TARGET} ${RRM_GL_LIBS})

if(APPLE)
    set(_rrm_res_files)
    foreach(_dir catalog assets)
        file(GLOB_RECURSE _files CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${_dir}/*)
        foreach(_f ${_files})
            file(RELATIVE_PATH _rel ${CMAKE_CURRENT_SOURCE_DIR} ${_f})
            get_filename_component(_reldir ${_rel} DIRECTORY)
            if(IOS)
                set_source_files_properties(${_f} PROPERTIES MACOSX_PACKAGE_LOCATION ${_reldir})
            else()
                set_source_files_properties(${_f} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/${_reldir})
            endif()
            list(APPEND _rrm_res_files ${_f})
        endforeach()
    endforeach()
    target_sources(${RRM_APP_TARGET} PRIVATE ${_rrm_res_files})
    if(IOS)
        set(_plist ${CMAKE_CURRENT_SOURCE_DIR}/platform/ios/Info.plist.in)
        file(GLOB _ios_cores CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/platform/ios/cores/*.framework)
        set_target_properties(${RRM_APP_TARGET} PROPERTIES
            XCODE_EMBED_FRAMEWORKS "${_ios_cores}"
            XCODE_EMBED_FRAMEWORKS_CODE_SIGN_ON_COPY ON
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "io.github.ridgeracermarathon"
            XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "15.0")
    else()
        # Frame generation / capture library injected into external backends (tools/framegrab):
        # shipped in the bundle's Frameworks folder, loaded by those programs, never by Marathon.
        add_library(rrm_present SHARED
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/framegrab/framegrab.mm
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/framegrab/framegen.mm)
        set_source_files_properties(
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/framegrab/framegrab.mm
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/framegrab/framegen.mm
            PROPERTIES COMPILE_FLAGS "-fobjc-arc -Wno-deprecated-declarations")  # (OpenGL capture path)
        target_link_libraries(rrm_present PRIVATE "-framework AppKit" "-framework Foundation" "-framework Metal"
            "-framework QuartzCore" "-framework ImageIO" "-framework CoreGraphics" "-framework OpenGL")
        set_target_properties(rrm_present PROPERTIES CXX_STANDARD 17)
        add_dependencies(${RRM_APP_TARGET} rrm_present)
        # Copied when the app links and again whenever the library alone is rebuilt.
        foreach(_t ${RRM_APP_TARGET} rrm_present)
            add_custom_command(TARGET ${_t} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_CONTENT_DIR:${RRM_APP_TARGET}>/Frameworks
                COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:rrm_present>
                        $<TARGET_BUNDLE_CONTENT_DIR:${RRM_APP_TARGET}>/Frameworks/)
        endforeach()
        set(_plist ${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/Info.plist.in)
        set(_icon ${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/Marathon.icns)
        if(EXISTS ${_icon})
            set_source_files_properties(${_icon} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
            target_sources(${RRM_APP_TARGET} PRIVATE ${_icon})
        endif()
        set_source_files_properties(${RRM_LEGAL_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        set_source_files_properties(${RRM_LICENSE_TEXTS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/LICENSES)
        target_sources(${RRM_APP_TARGET} PRIVATE ${RRM_LEGAL_FILES} ${RRM_LICENSE_TEXTS})
    endif()
    set_target_properties(${RRM_APP_TARGET} PROPERTIES
        MACOSX_BUNDLE TRUE
        OUTPUT_NAME "Ridge Racer Marathon"
        MACOSX_BUNDLE_INFO_PLIST ${_plist}
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.ridgeracermarathon"
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
elseif(WIN32)
    set_target_properties(${RRM_APP_TARGET} PROPERTIES WIN32_EXECUTABLE TRUE)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/marathon.rc)
        target_sources(${RRM_APP_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/marathon.rc)
        # Numeric version for the VERSIONINFO resource.
        target_compile_definitions(${RRM_APP_TARGET} PRIVATE RRM_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
            RRM_VERSION_MINOR=${PROJECT_VERSION_MINOR} RRM_VERSION_PATCH=${PROJECT_VERSION_PATCH})
    endif()
    if(MSVC)
        # MinGW embeds the manifest from marathon.rc; MSVC merges it through the linker.
        target_sources(${RRM_APP_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/marathon.manifest)
    endif()
    if(MINGW)
        # -municode: UNICODE is defined, so SDL_main provides wWinMain instead of WinMain.
        target_link_options(${RRM_APP_TARGET} PRIVATE -static -static-libgcc -static-libstdc++ -municode)
    endif()
    add_custom_command(TARGET ${RRM_APP_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/catalog $<TARGET_FILE_DIR:${RRM_APP_TARGET}>/catalog
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:${RRM_APP_TARGET}>/assets)
elseif(NOT ANDROID)
    add_custom_command(TARGET ${RRM_APP_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/catalog $<TARGET_FILE_DIR:${RRM_APP_TARGET}>/catalog
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:${RRM_APP_TARGET}>/assets)
    include(GNUInstallDirs)
    install(TARGETS ${RRM_APP_TARGET} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY catalog assets DESTINATION ${CMAKE_INSTALL_DATADIR}/ridge-racer-marathon)
    install(FILES platform/linux/io.github.ridgeracermarathon.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications OPTIONAL)
    install(FILES platform/linux/io.github.ridgeracermarathon.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)
    install(FILES ${RRM_LEGAL_FILES} DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/ridge-racer-marathon)
    install(FILES ${RRM_LICENSE_TEXTS} DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/ridge-racer-marathon/LICENSES)
endif()

# ---- developer tools ---------------------------------------------------------------------------
if(RRM_BUILD_TESTS AND NOT ANDROID AND NOT IOS)
    include(CTest)
    file(GLOB RRM_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
    foreach(_t ${RRM_TEST_SOURCES})
        get_filename_component(_name ${_t} NAME_WE)
        add_executable(${_name} ${_t})
        target_link_libraries(${_name} PRIVATE rrm_core rrm_libretro ${RRM_SDL_TARGET} ${RRM_GL_LIBS})
    endforeach()
    # Drives the real UI with virtual gamepads (tests/ui_pad_tool.cpp).
    if(TARGET ui_pad_tool)
        target_sources(ui_pad_tool PRIVATE ${RRM_APP_SOURCES})
        target_link_libraries(ui_pad_tool PRIVATE rrm_imgui)
    endif()
    if(BUILD_TESTING)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME controller_settings COMMAND controllers_test)
        add_test(NAME controller_configs COMMAND controller_configs_test --catalog ${CMAKE_CURRENT_SOURCE_DIR}/catalog)
        add_test(NAME libretro_input COMMAND lr_input_test --catalog ${CMAKE_CURRENT_SOURCE_DIR}/catalog)
        add_test(NAME missing_game_files COMMAND missing_game_files_test)
        add_test(NAME backend_bundle COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_bundle_test.py $<TARGET_FILE:bundle_tool>
            ${CMAKE_CURRENT_SOURCE_DIR}/catalog)
    endif()
    if(APPLE)
        # Offline quality and GPU-time check of the Metal frame generation (tools/framegrab/fg_eval.mm).
        add_executable(fg_eval ${CMAKE_CURRENT_SOURCE_DIR}/tools/framegrab/fg_eval.mm
                               ${CMAKE_CURRENT_SOURCE_DIR}/tools/framegrab/framegen.mm)
        set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/tools/framegrab/fg_eval.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
        target_link_libraries(fg_eval PRIVATE "-framework Foundation" "-framework Metal" "-framework ImageIO"
            "-framework CoreGraphics")
        set_target_properties(fg_eval PROPERTIES CXX_STANDARD 17)
    endif()
endif()
