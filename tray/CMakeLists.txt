cmake_minimum_required(VERSION 3.21)
project(xmq_tray VERSION 1.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE
    GROUP_READ GROUP_EXECUTE
    WORLD_READ WORLD_EXECUTE)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    add_executable(xmq_tray WIN32 windows.cpp xmq-tray.rc)
    target_compile_definitions(xmq_tray PRIVATE UNICODE _UNICODE)
    target_link_libraries(xmq_tray PRIVATE shell32 user32)
    if(MINGW)
        target_link_options(xmq_tray PRIVATE -municode)
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
    add_executable(xmq_tray linux.cpp)
    target_link_libraries(xmq_tray PRIVATE PkgConfig::GIO PkgConfig::X11)
endif()

install(TARGETS xmq_tray RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(NOT WIN32)
    install(FILES xmq-tray.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES xmq-tray.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(EXISTS "/etc/os-release")
        file(STRINGS "/etc/os-release" osReleaseIds REGEX "^ID(_LIKE)?=")
        string(JOIN " " osReleaseIds ${osReleaseIds})
        string(TOLOWER "${osReleaseIds}" osReleaseIds)
        string(REGEX REPLACE "[^a-z0-9]+" ";" osReleaseIds "${osReleaseIds}")
    endif()

    if("debian" IN_LIST osReleaseIds OR "ubuntu" IN_LIST osReleaseIds
       OR "linuxmint" IN_LIST osReleaseIds)
        set(CPACK_GENERATOR "DEB")
        set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
        set(CPACK_DEBIAN_PACKAGE_SECTION "net")
        set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS TRUE)
    elseif("fedora" IN_LIST osReleaseIds OR "rhel" IN_LIST osReleaseIds
           OR "redhat" IN_LIST osReleaseIds OR "centos" IN_LIST osReleaseIds
           OR "rocky" IN_LIST osReleaseIds OR "almalinux" IN_LIST osReleaseIds
           OR "ol" IN_LIST osReleaseIds OR "suse" IN_LIST osReleaseIds
           OR "opensuse" IN_LIST osReleaseIds OR "sles" IN_LIST osReleaseIds)
        set(CPACK_GENERATOR "RPM")
        set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
        set(CPACK_RPM_PACKAGE_GROUP "Applications/Internet")
        set(CPACK_RPM_PACKAGE_LICENSE "MPL-2.0")
    endif()

    if(CPACK_GENERATOR)
        set(CPACK_PACKAGE_NAME "xmq-tray")
        set(CPACK_PACKAGE_VENDOR "sptk.net")
        set(CPACK_PACKAGE_CONTACT "alexeyp@gmail.com")
        set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Tray launcher for the XMQ broker console")
        set(CPACK_PACKAGE_HOMEPAGE_URL "https://www.sptk.net")
        set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/local")
        set(CPACK_PACKAGE_RELOCATABLE FALSE)
        set(CPACK_STRIP_FILES TRUE)
        include(CPack)
    endif()
endif()
