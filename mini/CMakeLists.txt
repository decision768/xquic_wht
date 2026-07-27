### hq mini client/server ###
    set(XQC_ZLOG_ROOT_X86_64
        "/data/quic/zlog-1.2.18"
        CACHE PATH "zlog root for native Linux x86_64 mini builds")
    set(XQC_ZLOG_ROOT_AARCH64
        "/home/admin123/zlog-1.2.18"
        CACHE PATH "zlog root for Linux AArch64 mini builds")

    if(NOT ZLOG_INCLUDE_DIR OR NOT ZLOG_STATIC_LIB)
        if(XQC_LINUX_ARCH STREQUAL "aarch64")
            set(_xqc_zlog_root "${XQC_ZLOG_ROOT_AARCH64}")
            set(_xqc_zlog_build_dir "build_aarch64")
        else()
            set(_xqc_zlog_root "${XQC_ZLOG_ROOT_X86_64}")
            set(_xqc_zlog_build_dir "build_x86_64")
        endif()
        if(NOT ZLOG_INCLUDE_DIR)
            set(ZLOG_INCLUDE_DIR "${_xqc_zlog_root}/src")
        endif()
        if(NOT ZLOG_STATIC_LIB)
            set(ZLOG_STATIC_LIB
                "${_xqc_zlog_root}/${_xqc_zlog_build_dir}/lib/libzlog.a")
        endif()
        unset(_xqc_zlog_root)
        unset(_xqc_zlog_build_dir)
    endif()
    message(STATUS "XQUIC mini zlog include: ${ZLOG_INCLUDE_DIR}")
    message(STATUS "XQUIC mini zlog library: ${ZLOG_STATIC_LIB}")
    set(
        HQ_SOURCES
        "../demo/xqc_hq_ctx.c"
        "../demo/xqc_hq_conn.c"
        "../demo/xqc_hq_request.c"
    )

    set(
        MINI_CLIENT_SOURCES
        ${HQ_SOURCES}
        "common.c"
        "mini_client.c"
        "mini_client_cb.c"
        "mini_client_main.c"
    )

    set(
        MINI_SERVER_SOURCES
        ${HQ_SOURCES}
        "common.c"
        "mini_server.c"
        "mini_server_cb.c"
        "mini_server_main.c"
    )


    if(${CMAKE_SYSTEM_NAME} MATCHES "Windows")
        set(MINI_SERVER_SOURCES
            ${MINI_SERVER_SOURCES}
            ${GETOPT_SOURCES}
        )

        set(MINI_CLIENT_SOURCES
            ${MINI_CLIENT_SOURCES}
            ${GETOPT_SOURCES}
        )
    endif()

    include_directories(
        "${CMAKE_SOURCE_DIR}/"
        "${CMAKE_SOURCE_DIR}/include"
        ${CMAKE_SOURCE_DIR}
        ${CUNIT_INCLUDE_DIRS}
        ${LIBEVENT_INCLUDE_DIR}
    )

    add_executable(mini_server ${MINI_SERVER_SOURCES})
    add_executable(mini_client ${MINI_CLIENT_SOURCES})
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(mini_client PRIVATE _GNU_SOURCE)
    endif()


    target_include_directories(mini_server PRIVATE
        ${ZLOG_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/src/netins_udp
    )
    target_include_directories(mini_client PRIVATE
        ${ZLOG_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/src/netins_udp
    )

    target_link_libraries(mini_server ${APP_DEPEND_LIBS} xquic-static ${LIBEVENT_LIBRARY} ${LIBEVENT_PTHREADS_LIBRARY} ${ZLOG_STATIC_LIB})
    target_link_libraries(mini_client ${APP_DEPEND_LIBS} xquic-static ${LIBEVENT_LIBRARY} ${LIBEVENT_PTHREADS_LIBRARY} ${ZLOG_STATIC_LIB})
