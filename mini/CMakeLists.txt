### Shared mini backend and optional command-line programs ###

set(ZLOG_INCLUDE_DIR
    "/home/admin123/zlog-1.2.18/src"
    CACHE PATH "Path to zlog headers")
set(ZLOG_STATIC_LIB
    "/home/admin123/zlog-1.2.18/build_aarch64/lib/libzlog.a"
    CACHE FILEPATH "Path to the zlog static library")

if(NOT EXISTS "${ZLOG_INCLUDE_DIR}/zlog.h")
    message(FATAL_ERROR
        "zlog header not found: ${ZLOG_INCLUDE_DIR}/zlog.h")
endif()
if(NOT EXISTS "${ZLOG_STATIC_LIB}")
    message(FATAL_ERROR
        "zlog library not found: ${ZLOG_STATIC_LIB}")
endif()
message(STATUS "Mini API zlog include: ${ZLOG_INCLUDE_DIR}")
message(STATUS "Mini API zlog library: ${ZLOG_STATIC_LIB}")

set(MINI_BACKEND_INCLUDE_DIRS
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${LIBEVENT_INCLUDE_DIR}"
    "${ZLOG_INCLUDE_DIR}"
)

add_library(mini_common_backend_obj OBJECT
    common.c
    mini_stream_backend.c
    mini_stream_protocol.c
    mini_stream_poc_protocol.c
)
add_library(mini_client_backend_obj OBJECT
    mini_client.c
    mini_client_cb.c
    mini_client_backend.c
    mini_stream_dispatch_client.c
    mini_stream_poc_client.c
)
add_library(mini_server_backend_obj OBJECT
    mini_server.c
    mini_server_cb.c
    mini_server_backend.c
    mini_stream_dispatch_server.c
    mini_stream_poc_server.c
)
set(MINI_BACKEND_OBJECT_TARGETS
    mini_common_backend_obj
    mini_client_backend_obj
    mini_server_backend_obj
)

if(XQC_ENABLE_MINI_API)
    add_library(netins_udp_mini_api_obj OBJECT
        ../src/netins_udp/netins_udp_api.c)
    list(APPEND MINI_BACKEND_OBJECT_TARGETS netins_udp_mini_api_obj)
endif()

foreach(target_name IN LISTS MINI_BACKEND_OBJECT_TARGETS)
    set_target_properties(${target_name} PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
    target_include_directories(${target_name} PRIVATE
        ${MINI_BACKEND_INCLUDE_DIRS})
endforeach()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(mini_client_backend_obj PRIVATE _GNU_SOURCE)
    target_compile_definitions(mini_server_backend_obj PRIVATE _GNU_SOURCE)
    if(XQC_ENABLE_MINI_API)
        target_compile_definitions(netins_udp_mini_api_obj PRIVATE _GNU_SOURCE)
    endif()
endif()

set(MINI_API_OBJECTS
    $<TARGET_OBJECTS:mini_common_backend_obj>
    $<TARGET_OBJECTS:mini_client_backend_obj>
    $<TARGET_OBJECTS:mini_server_backend_obj>
)
if(XQC_ENABLE_MINI_API)
    list(APPEND MINI_API_OBJECTS
        $<TARGET_OBJECTS:netins_udp_mini_api_obj>)
endif()
set(MINI_API_OBJECTS ${MINI_API_OBJECTS} PARENT_SCOPE)

if(XQC_ENABLE_TESTING)
    if(NOT MINI_XQUIC_TARGET)
        set(MINI_XQUIC_TARGET xquic-static)
    endif()
    set(MINI_PLATFORM_SOURCES)
    if(CMAKE_SYSTEM_NAME MATCHES "Windows")
        set(MINI_PLATFORM_SOURCES ${GETOPT_SOURCES})
    endif()

    add_executable(mini_server
        mini_server_main.c
        $<TARGET_OBJECTS:mini_common_backend_obj>
        $<TARGET_OBJECTS:mini_server_backend_obj>
        ${MINI_PLATFORM_SOURCES})
    add_executable(mini_client
        mini_client_main.c
        $<TARGET_OBJECTS:mini_common_backend_obj>
        $<TARGET_OBJECTS:mini_client_backend_obj>
        ${MINI_PLATFORM_SOURCES})

    target_include_directories(mini_server PRIVATE
        ${MINI_BACKEND_INCLUDE_DIRS})
    target_include_directories(mini_client PRIVATE
        ${MINI_BACKEND_INCLUDE_DIRS})

    target_link_libraries(mini_server
        ${APP_DEPEND_LIBS}
        ${MINI_XQUIC_TARGET}
        ${LIBEVENT_LIBRARY}
        ${LIBEVENT_PTHREADS_LIBRARY}
        ${ZLOG_STATIC_LIB})
    target_link_libraries(mini_client
        ${APP_DEPEND_LIBS}
        ${MINI_XQUIC_TARGET}
        ${LIBEVENT_LIBRARY}
        ${LIBEVENT_PTHREADS_LIBRARY}
        ${ZLOG_STATIC_LIB})
endif()

if(XQC_ENABLE_STREAM_POC)
    if(NOT MINI_XQUIC_TARGET)
        set(MINI_XQUIC_TARGET xquic-static)
    endif()

    add_executable(mini_stream_poc_client
        mini_stream_poc_client_main.c
        $<TARGET_OBJECTS:mini_common_backend_obj>
        $<TARGET_OBJECTS:mini_client_backend_obj>)
    add_executable(mini_stream_poc_server
        mini_stream_poc_server_main.c
        $<TARGET_OBJECTS:mini_common_backend_obj>
        $<TARGET_OBJECTS:mini_server_backend_obj>)
    add_executable(mini_stream_poc_protocol_test
        ../tests/mini_stream_poc_protocol_test.c
        mini_stream_poc_protocol.c)

    target_include_directories(mini_stream_poc_client PRIVATE
        ${MINI_BACKEND_INCLUDE_DIRS})
    target_include_directories(mini_stream_poc_server PRIVATE
        ${MINI_BACKEND_INCLUDE_DIRS})
    target_include_directories(mini_stream_poc_protocol_test PRIVATE
        ${MINI_BACKEND_INCLUDE_DIRS})

    target_link_libraries(mini_stream_poc_client
        ${APP_DEPEND_LIBS}
        ${MINI_XQUIC_TARGET}
        ${LIBEVENT_LIBRARY}
        ${LIBEVENT_PTHREADS_LIBRARY}
        ${ZLOG_STATIC_LIB})
    target_link_libraries(mini_stream_poc_server
        ${APP_DEPEND_LIBS}
        ${MINI_XQUIC_TARGET}
        ${LIBEVENT_LIBRARY}
        ${LIBEVENT_PTHREADS_LIBRARY}
        ${ZLOG_STATIC_LIB})
endif()

if(XQC_ENABLE_MINI_API)
    if(NOT MINI_XQUIC_TARGET)
        set(MINI_XQUIC_TARGET xquic-static)
    endif()

    add_executable(netins_udp_mini_api_client
        ../demo/netins_udp_mini_api_client.c)
    add_executable(netins_udp_mini_api_server
        ../demo/netins_udp_mini_api_server.c)
    add_executable(netins_udp_stream_client
        ../demo/netins_udp_stream_client.c)
    add_executable(netins_udp_stream_server
        ../demo/netins_udp_stream_server.c)
    add_executable(mini_stream_protocol_test
        ../tests/mini_stream_protocol_test.c
        mini_stream_protocol.c)

    foreach(target_name
        netins_udp_mini_api_client
        netins_udp_mini_api_server
        netins_udp_stream_client
        netins_udp_stream_server)
        target_include_directories(${target_name} PRIVATE
            "${CMAKE_SOURCE_DIR}/include")
        target_link_libraries(${target_name}
            ${APP_DEPEND_LIBS}
            ${MINI_XQUIC_TARGET}
            ${LIBEVENT_LIBRARY}
            ${LIBEVENT_PTHREADS_LIBRARY}
            ${ZLOG_STATIC_LIB})
    endforeach()
    target_include_directories(mini_stream_protocol_test PRIVATE
        ${MINI_BACKEND_INCLUDE_DIRS})
endif()
