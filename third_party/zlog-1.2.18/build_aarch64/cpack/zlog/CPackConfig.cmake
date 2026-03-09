SET(CPACK_CMAKE_GENERATOR "Unix Makefiles")
SET(CPACK_GENERATOR "RPM;DEB")
SET(CPACK_OUTPUT_CONFIG_FILE "/home/admin123/zlog-1.2.18/build_aarch64/cpack/zlog/CPackConfig.cmake")
SET(CPACK_INSTALL_CMAKE_PROJECTS "/home/admin123/zlog-1.2.18/build_aarch64;zlog;zlog;/")

SET(CPACK_PACKAGE_VERSION_MAJOR "1")
SET(CPACK_PACKAGE_VERSION_MINOR "2")
SET(CPACK_PACKAGE_VERSION_PATCH "12")
SET(CPACK_RPM_PACKAGE_RELEASE "1") # release version.
SET(CPACK_PACKAGE_VERSION "1.2.12")

SET(CPACK_PACKAGE_NAME "zlog")
SET(CPACK_PACKAGE_DESCRIPTION_SUMMARY "log component for Linux/Unix/AIX")
SET(CPACK_PACKAGE_DESCRIPTION_FILE "/home/admin123/zlog-1.2.18/README")

SET(CPACK_PACKAGE_VENDOR "zlog")
SET(CPACK_PACKAGING_INSTALL_PREFIX "/usr/local")
SET(CPACK_PACKAGE_CONTACT "HardySimpson1984@gmail.com")

SET(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "zlog 1.2.12")

SET(CPACK_SYSTEM_NAME "Linux-aarch64")
SET(CPACK_TOPLEVEL_TAG "Linux-aarch64")
SET(CPACK_PACKAGE_FILE_NAME "zlog-1.2.12-1-Linux-aarch64")

# SET(CPACK_PACKAGE_EXECUTABLES "ccmake;CMake")
# SET(CPACK_STRIP_FILES "bin/ccmake;bin/cmake;bin/cpack;bin/ctest")
set(CPACK_NSIS_MODIFY_PATH, ON)

SET(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "i386")
SET(CPACK_RPM_PACKAGE_ARCHITECTURE "i386")

SET(CPACK_RPM_PACKAGE_REQUIRES "")
SET(CPACK_DEBIAN_PACKAGE_DEPENDS "")

SET(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "/home/admin123/zlog-1.2.18/build_aarch64/cpack/zlog/preinst;/home/admin123/zlog-1.2.18/build_aarch64/cpack/zlog/postinst;/home/admin123/zlog-1.2.18/build_aarch64/cpack/zlog/prerm;/home/admin123/zlog-1.2.18/build_aarch64/cpack/zlog/postrm")

SET(CPACK_RPM_PRE_INSTALL_SCRIPT_FILE "/home/admin123/zlog-1.2.18/build_aarch64/cpack/zlog/preinst")
SET(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "/home/admin123/zlog-1.2.18/build_aarch64/cpack/zlog/postinst")
SET(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE "/home/admin123/zlog-1.2.18/build_aarch64/cpack/zlog/prerm")
SET(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE "/home/admin123/zlog-1.2.18/build_aarch64/cpack/zlog/postrm")

SET(CPACK_RPM_PACKAGE_LICENSE "Apache License Version 2.0")

SET(CPACK_RPM_PACKAGE_GROUP "System Environment/Base")
