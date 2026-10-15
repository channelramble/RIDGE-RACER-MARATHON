# Catalog smoke test: runs `launch_tool matrix` on an empty games folder with a fresh data
# folder (dry run: nothing is installed, written or started) and checks that it exits 0, lists
# every game in catalog/games.json and reports no launch failures. Same on every OS:
#
#   cmake -DLAUNCH_TOOL=build/launch_tool -DCATALOG=catalog -DWORK=build/catalog-smoke \
#         -P .github/scripts/catalog-smoke.cmake
#
# Relative paths are taken from the current directory. WORK is deleted and recreated.
cmake_minimum_required(VERSION 3.24)

foreach(_var LAUNCH_TOOL CATALOG WORK)
    if(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
        message(FATAL_ERROR "catalog-smoke: -D${_var}=... is required")
    endif()
    file(TO_CMAKE_PATH "${${_var}}" ${_var})
    get_filename_component(${_var} "${${_var}}" ABSOLUTE)
endforeach()
if(NOT EXISTS "${LAUNCH_TOOL}" AND EXISTS "${LAUNCH_TOOL}.exe")
    set(LAUNCH_TOOL "${LAUNCH_TOOL}.exe")
endif()
if(NOT EXISTS "${LAUNCH_TOOL}")
    message(FATAL_ERROR "catalog-smoke: ${LAUNCH_TOOL} not found (build with -DRRM_BUILD_TESTS=ON)")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/games" "${WORK}/data")

execute_process(
    COMMAND "${LAUNCH_TOOL}" matrix "${WORK}/games" --catalog "${CATALOG}" --data "${WORK}/data"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
message("${_out}")
if(NOT _err STREQUAL "")
    message("stderr:\n${_err}")
endif()

set(_problems "")
if(NOT _rc STREQUAL "0")
    list(APPEND _problems "launch_tool exited with ${_rc}")
endif()

# Every game must be listed, and with an empty folder none may be recognised.
file(READ "${CATALOG}/games.json" _json)
string(JSON _count LENGTH "${_json}" games)
if(_count EQUAL 0)
    list(APPEND _problems "catalog lists no games")
else()
    math(EXPR _last "${_count} - 1")
    foreach(_i RANGE ${_last})
        string(JSON _title GET "${_json}" games ${_i} title)
        string(FIND "${_out}" "== ${_title}: (no files found)" _pos)
        if(_pos EQUAL -1)
            list(APPEND _problems "game not listed: ${_title}")
        endif()
    endforeach()
endif()
string(FIND "${_out}" "\n0 launch failure(s)" _pos)
if(_pos EQUAL -1)
    list(APPEND _problems "launch failures reported")
endif()

if(_problems)
    list(JOIN _problems "\n  " _text)
    message(FATAL_ERROR "catalog-smoke failed:\n  ${_text}")
endif()
message(STATUS "catalog-smoke: ${_count} games listed, exit 0")
