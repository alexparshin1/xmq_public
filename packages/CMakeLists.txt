set(CPACK_PACKAGE_NAME "XMQ-server")
set(CPACK_PACKAGE_VENDOR "sptk.net")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Cross-platform MQTT server")
set(CPACK_PACKAGE_VERSION ${VERSION})
set(CPACK_PACKAGE_VERSION_MAJOR ${VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${VERSION_PATCH})
set(CPACK_PACKAGE_INSTALL_DIRECTORY "packages")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://www.sptk.net")

set(CPACK_VERBATIM_VARIABLES YES)

SET(CMAKE_INSTALL_RPATH ${CMAKE_INSTALL_PREFIX}/lib)
SET(CPACK_PACKAGING_INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX})

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

# What a freshly installed broker still needs doing, in one place for all three package formats.
# The port is read out of the configuration that is installed beside it rather than written out a
# second time here: a message naming a port nothing listens on is worse than no message at all.
FILE(READ "${CMAKE_CURRENT_LIST_DIR}/../xmq_server.conf" xmqServerConfiguration)
STRING(REGEX MATCH "\"listener_port\"[ \t]*:[ \t]*([0-9]+)" matchedPort "${xmqServerConfiguration}")
SET(XMQ_WEB_INTERFACE_PORT "${CMAKE_MATCH_1}")
IF (NOT XMQ_WEB_INTERFACE_PORT)
    MESSAGE(FATAL_ERROR "No web_service.listener_port in xmq_server.conf to tell the installer about.")
ENDIF ()

# Twice through configure_file: once to put the port into the message, and once to put the
# finished message into the script that prints it.
CONFIGURE_FILE("${CMAKE_CURRENT_LIST_DIR}/FirstStartMessage.txt"
               "${CMAKE_CURRENT_BINARY_DIR}/FirstStartMessage.txt" @ONLY)
FILE(READ "${CMAKE_CURRENT_BINARY_DIR}/FirstStartMessage.txt" XMQ_FIRST_START_MESSAGE)
STRING(STRIP "${XMQ_FIRST_START_MESSAGE}" XMQ_FIRST_START_MESSAGE)
# Named "postinst" because dpkg picks maintainer scripts out of the control archive by name and
# will not look at anything called otherwise. RPM takes it by path and does not care.
CONFIGURE_FILE("${CMAKE_CURRENT_LIST_DIR}/PostInstall.sh.in"
               "${CMAKE_CURRENT_BINARY_DIR}/postinst" @ONLY)

IF (OS_FLAVOUR STREQUAL "ol" OR OS_FLAVOUR STREQUAL "fedora" OR OS_FLAVOUR STREQUAL "redhat")
    SET(CPACK_GENERATOR "RPM")
    SET(CPACK_RPM_PACKAGE_SHLIBDEPS TRUE)
    SET(CPACK_RPM_PACKAGE_GENERATE_SHLIBS ON)
    SET(CPACK_RPM_PACKAGE_MAINTAINER "alexeyp@gmail.com")
    SET(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "${CMAKE_CURRENT_BINARY_DIR}/postinst")
    # Listed here so that none of these five is declared %config. CPack's RPM generator marks every
    # file installed to an absolute destination as %config on its own, and it has no setting to stop
    # that; naming a file in this list is the one thing that takes it out. They are the only absolute
    # destinations in the tree: the three templates, whose directory is outside the prefix on a
    # packaged Linux broker (the user-database one is written by its install script), and the two
    # units daemon/CMakeLists.txt installs.
    #
    # Each entry needs a directive. A bare path breaks CPackRPM itself: it strips the directive prefix
    # unquoted, and an empty prefix leaves string(STRIP) one argument short, failing the package.
    # %attr(-,root,root) keeps the installed mode and changes nothing else.
    #
    # %config is what made the rpm own them. The first layer of acceptance forbids any declared
    # configuration, because the live files are the broker's and the operator's, not the package's -
    # and every 0.9.16 rpm carried all of them, while the .deb and the FreeBSD package carried none.
    SET(CPACK_RPM_USER_FILELIST
        "%attr(-,root,root) ${XMQ_CONF_DIRECTORY}/xmq_server.conf.template"
        "%attr(-,root,root) ${XMQ_CONF_DIRECTORY}/xmq_extensions.conf.template"
        "%attr(-,root,root) ${XMQ_CONF_DIRECTORY}/xmq_extensions.d/50-user-database.conf.template"
        "%attr(-,root,root) /etc/systemd/system/xmq_server.service"
        "%attr(-,root,root) /etc/systemd/system/xmq_test_suite.service")
ENDIF ()

IF (OS_FLAVOUR STREQUAL "debian" OR OS_FLAVOUR STREQUAL "linuxmint" OR OS_FLAVOUR STREQUAL "ubuntu")
    SET(CPACK_GENERATOR "DEB")
    SET(CPACK_DEBIAN_PACKAGE_SHLIBDEPS TRUE)
    SET(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS ON)
    SET(CPACK_DEBIAN_PACKAGE_MAINTAINER "alexeyp@gmail.com")
    SET(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_CURRENT_BINARY_DIR}/postinst")
    # So the script arrives executable: dpkg refuses to run a maintainer script that is not.
    SET(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION TRUE)
ENDIF ()

IF (BSD)
    # pkg(8), through CPack's own generator - it links libpkg, so the result is a real package with
    # a manifest and dependencies, not a tarball that happens to unpack in the right place.
    #
    # This branch is chosen by BSD rather than by OS_FLAVOUR like the two above: OS_FLAVOUR is read
    # out of /etc/os-release, which FreeBSD does not have, so it is empty here and would match
    # nothing.
    SET(CPACK_GENERATOR "FREEBSD")
    SET(CPACK_FREEBSD_PACKAGE_MAINTAINER "alexeyp@gmail.com")
    SET(CPACK_FREEBSD_PACKAGE_LICENSE "MPL20")
    SET(CPACK_FREEBSD_PACKAGE_ORIGIN "net/xmq-server")
    SET(CPACK_FREEBSD_PACKAGE_CATEGORIES "net")
    SET(CPACK_FREEBSD_PACKAGE_WWW "${CPACK_PACKAGE_HOMEPAGE_URL}")
    SET(CPACK_FREEBSD_PACKAGE_COMMENT "${CPACK_PACKAGE_DESCRIPTION_SUMMARY}")
    SET(CPACK_FREEBSD_PACKAGE_DESCRIPTION
        "XMQ is an MQTT 5.0, 3.1.1 and 3.1 message broker with a web interface, "
        "optional Redis-backed session persistence, bridging and clustering.")

    # Origins, the way pkg names its dependencies, and only the C++ runtime needs naming here - pkg
    # works the rest out from the binaries. A GCC build takes that runtime from the compiler port,
    # and the package has to say so: one that installs and then cannot find libstdc++.so.6 is worse
    # than one that refuses to install.
    #
    # A Clang build needs nothing. libc++ and libcxxrt are part of the base system, so the package
    # depends on no port at all - which is the difference between 401 MB and 16 MB on a machine that
    # only wants to run a broker, and the reason the FreeBSD builds moved to Clang.
    IF (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        SET(CPACK_FREEBSD_PACKAGE_DEPS "lang/gcc15")
    ENDIF ()

    # Ownership, which CPack has no setting for - see the script for what it does and why cpack has
    # to run as root for it to work.
    SET(CPACK_PRE_BUILD_SCRIPTS "${CMAKE_CURRENT_LIST_DIR}/FreeBSDOwnership.cmake")

    # Nor has it a setting for package scripts, so this one is written out here and put into the
    # manifest by build_freebsd_package.sh - in the xmq_build_farm repository - which is already
    # unpacking and rebuilding the package to add the first-start message. It makes the account the broker runs as and gives it the
    # directories it has to write.
    CONFIGURE_FILE("${CMAKE_CURRENT_LIST_DIR}/FreeBSDPostInstall.sh.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/FreeBSDPostInstall.sh" @ONLY)
ENDIF ()

# Strip at packaging time, not at link time. The -s flag lives in CMAKE_CXX_FLAGS_RELEASE only,
# so a package built in any other configuration ships full debug info: that is how the 0.9.15
# .deb grew from 8 MB to 122 MB without anyone touching the packaging.
SET(CPACK_STRIP_FILES TRUE)

INCLUDE(CPack)
