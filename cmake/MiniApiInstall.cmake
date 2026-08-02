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
        netins_udp_mini_api_server)
        if(TARGET ${demo_target})
            install(TARGETS ${demo_target}
                RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
                COMPONENT mini_api)
        endif()
    endforeach()

    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/docs/mini_api/README.md"
        DESTINATION "."
        RENAME "README.md"
        COMPONENT mini_api)
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/NETINS_UDP_API_使用说明.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/NETINS_UDP_API_详细使用说明.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/NETINS_UDP_编译指南.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/RELEASE_NOTES.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
        DESTINATION "."
        COMPONENT mini_api)
endif()
