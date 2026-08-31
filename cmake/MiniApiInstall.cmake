if(XQC_ENABLE_MINI_API)
    include(GNUInstallDirs)

    if(TARGET xquic AND NOT XQC_NO_SHARED)
        set_target_properties(xquic PROPERTIES
            VERSION "1.0.0"
            SOVERSION "1")
    endif()

    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/include/xquic/netins_udp.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/xquic"
        COMPONENT mini_api)

    foreach(library_target xquic-static xquic)
        if(TARGET ${library_target})
            install(TARGETS ${library_target}
                ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
                LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
                RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
                COMPONENT mini_api)
        endif()
    endforeach()

    foreach(demo_target
        netins_udp_mini_api_client
        netins_udp_mini_api_server
        netins_udp_stream_client
        netins_udp_stream_server)
        if(TARGET ${demo_target})
            install(TARGETS ${demo_target}
                RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
                COMPONENT mini_api)
        endif()
    endforeach()

    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
        DESTINATION "."
        COMPONENT mini_api)

    install(PROGRAMS
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/mini_api/check_abi.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/mini_api/check_stream_isolation.py"
        DESTINATION "scripts/mini_api"
        COMPONENT mini_api)
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/xquic.lds"
        DESTINATION "scripts"
        COMPONENT mini_api)
endif()
