# Content-address the actual checkout, including dirty and untracked source files.
# Regenerate when a source/header changes; do not identify a dirty binary by HEAD alone.
get_filename_component(XQC_ARRIVAL_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(GLOB_RECURSE XQC_ARRIVAL_SOURCES
    "${XQC_ARRIVAL_ROOT}/src/*.c" "${XQC_ARRIVAL_ROOT}/src/*.h"
    "${XQC_ARRIVAL_ROOT}/include/*.h" "${XQC_ARRIVAL_ROOT}/mini/*.c"
    "${XQC_ARRIVAL_ROOT}/mini/*.h" "${XQC_ARRIVAL_ROOT}/demo/*.c"
    "${XQC_ARRIVAL_ROOT}/demo/*.h")
list(APPEND XQC_ARRIVAL_SOURCES "${XQC_ARRIVAL_ROOT}/CMakeLists.txt"
    "${XQC_ARRIVAL_ROOT}/cmake/adaptive_arrival_build.cmake")
list(SORT XQC_ARRIVAL_SOURCES)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${XQC_ARRIVAL_SOURCES})
set(XQC_ARRIVAL_HASHES "")
set(XQC_ARRIVAL_HASH_JSON "")
foreach(source ${XQC_ARRIVAL_SOURCES})
    file(SHA256 "${source}" hash)
    file(RELATIVE_PATH relative "${XQC_ARRIVAL_ROOT}" "${source}")
    string(APPEND XQC_ARRIVAL_HASHES "${relative} ${hash}\n")
    if(XQC_ARRIVAL_HASH_JSON)
        string(APPEND XQC_ARRIVAL_HASH_JSON ",\n")
    endif()
    string(APPEND XQC_ARRIVAL_HASH_JSON "    \"${relative}\": \"${hash}\"")
endforeach()
execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${XQC_ARRIVAL_ROOT}"
    OUTPUT_VARIABLE XQC_ARRIVAL_HEAD OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git status --porcelain WORKING_DIRECTORY "${XQC_ARRIVAL_ROOT}"
    OUTPUT_VARIABLE XQC_ARRIVAL_DIRTY RESULT_VARIABLE XQC_ARRIVAL_GIT_STATUS ERROR_QUIET)
if(NOT XQC_ARRIVAL_HEAD)
    set(XQC_ARRIVAL_HEAD "unknown-no-git")
endif()
if(NOT XQC_ARRIVAL_GIT_STATUS EQUAL 0)
    set(XQC_ARRIVAL_DIRTY null)
elseif(XQC_ARRIVAL_DIRTY)
    set(XQC_ARRIVAL_DIRTY true)
else()
    set(XQC_ARRIVAL_DIRTY false)
endif()
string(SHA256 XQC_ARRIVAL_DIGEST "${XQC_ARRIVAL_HEAD}\n${XQC_ARRIVAL_HASHES}")
set(XQC_ARRIVAL_BUILD_ID "adaptive-arrival-v1-${XQC_ARRIVAL_DIGEST}")
add_definitions(-DXQC_ARRIVAL_BUILD_ID="${XQC_ARRIVAL_BUILD_ID}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/adaptive_arrival_source_manifest.json"
    "{\n  \"source_build_id\": \"${XQC_ARRIVAL_BUILD_ID}\",\n  \"base_head\": \"${XQC_ARRIVAL_HEAD}\",\n  \"dirty\": ${XQC_ARRIVAL_DIRTY},\n  \"source_sha256\": {\n${XQC_ARRIVAL_HASH_JSON}\n  }\n}\n")
