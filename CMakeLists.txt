#[[
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin. All rights reserved.    ║
║  email                alexeyp@gmail.com                                      ║
╚══════════════════════════════════════════════════════════════════════════════╝
#]]

CMAKE_MINIMUM_REQUIRED(VERSION 3.15)

PROJECT(XMQ)

IF (NOT CMAKE_BUILD_TYPE)
    SET(CMAKE_BUILD_TYPE Release)
ENDIF ()

# Not stripped at link time. The packages are stripped by CPack (CPACK_STRIP_FILES in
# packages/CMakeLists.txt), so -s here changed nothing about what ships - it only made every
# binary in the build tree unsymbolised, which is what turned a crash backtrace into five frames
# of "??" when one was finally needed.

set(TARGET_MESSAGES OFF)
set(CMAKE_TARGET_MESSAGES OFF)
set(CMAKE_INSTALL_MESSAGE NEVER)

# The version is read from VERSION.txt, beside this file - one line to change on a release instead of
# the three SET() lines it replaces, which a release script had to edit in step with each other.
FILE(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION.txt" XMQ_VERSION_LINE LIMIT_COUNT 1)
STRING(REPLACE "." ";" XMQ_VERSION_PARTS "${XMQ_VERSION_LINE}")
LIST(GET XMQ_VERSION_PARTS 0 VERSION_MAJOR)
LIST(GET XMQ_VERSION_PARTS 1 VERSION_MINOR)
LIST(GET XMQ_VERSION_PARTS 2 VERSION_PATCH)
SET(VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")
SET(SOVERSION "1")

SET(CMAKE_EXPORT_COMPILE_COMMANDS OFF)

# Place compiled libraries and examples into 'lib'
SET(LIBRARY_OUTPUT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lib" CACHE PATH "Directory for built libraries")
SET(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/")

OPTION(BUILD_WITH_COVERAGE "Build with coverage" OFF)
IF (UNIX)
    OPTION(USE_NEW_ABI "Use new GCC ABI" ON)
ENDIF ()
OPTION(BUILD_PACKAGE "Build OS package" ON)

# Frame pointers, kept. At -O3 the compiler drops them, and perf's default unwinder then walks
# garbage, so a profile of a release build is unreadable. The alternative, --call-graph dwarf,
# unwinds from debug info instead - correct, but heavy enough to distort the very thing being
# measured on a process already saturating its CPU.
#
# On by default, deliberately. It costs about a percent, and the work it enables is measured in
# tens: xmq_scn currently costs twice the broker per message and is what limits the heaviest
# scenarios, and that is not found by guessing. A build nobody can profile hides more than a
# percent. Turn it off for a measurement that has to be free of it.
OPTION(WITH_FRAME_POINTERS "Keep frame pointers, so perf can unwind stacks" ON)


INCLUDE(${CMAKE_ROOT}/Modules/CheckTypeSize.cmake)
INCLUDE(${CMAKE_ROOT}/Modules/TestBigEndian.cmake)
INCLUDE(${CMAKE_ROOT}/Modules/CheckFunctionExists.cmake)

INCLUDE(cmake/OSType.cmake)

# Turns a directory that sits under CMAKE_INSTALL_PREFIX into a destination expressed relative to
# it, and leaves anything else as it is.
#
# It matters because an absolute DESTINATION escapes CPack's staging directory. CPack stages an
# install by pointing the prefix at a directory of its own; a rule that names a full path ignores
# that and writes to the real one - so a packaging build wants a privilege it should not need, and
# the files it wrote are missing from the package that was the point of the run. Every path that
# genuinely lives under the prefix should therefore be named relative to it.
FUNCTION(XMQ_INSTALL_DESTINATION absolutePath outputVariable)
    SET(result "${absolutePath}")
    STRING(FIND "${absolutePath}" "${CMAKE_INSTALL_PREFIX}/" position)
    IF (position EQUAL 0)
        STRING(LENGTH "${CMAKE_INSTALL_PREFIX}/" prefixLength)
        STRING(SUBSTRING "${absolutePath}" ${prefixLength} -1 result)
    ENDIF ()
    SET(${outputVariable} "${result}" PARENT_SCOPE)
ENDFUNCTION()

# DT_RPATH, not DT_RUNPATH, for every binary this project links. The toolchains default to RUNPATH,
# which is searched after LD_LIBRARY_PATH, so the environment could put another SPTK in front of the
# one a broker was installed with; RPATH is searched first, covers the whole dependency tree, and is
# what dlopen consults. Measured on a test host: with LD_LIBRARY_PATH naming another copy, a RUNPATH
# binary loaded that copy and an RPATH binary did not. SPTK links its own libraries the same way.
IF (UNIX AND NOT APPLE)
    STRING(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,--disable-new-dtags")
    STRING(APPEND CMAKE_MODULE_LINKER_FLAGS " -Wl,--disable-new-dtags")
    STRING(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,--disable-new-dtags")
ENDIF ()

# FreeBSD ships its own libstdc++ and one per installed GCC, and the loader's search order is not
# tied to the compiler this was built with, so a binary can end up loading a libstdc++ too old for
# the GLIBCXX symbols it needs - g++15 builds picking up gcc14's copy, say. Pin the RPATH to the
# compiler's own libstdc++ directory rather than depend on that system-wide order. SPTK does the
# same; a build of XMQ that skipped it would not start even against a correctly built SPTK.
IF (BSD AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    EXECUTE_PROCESS(
            COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libstdc++.so
            OUTPUT_VARIABLE GCC_LIBSTDCXX_PATH
            OUTPUT_STRIP_TRAILING_WHITESPACE)
    GET_FILENAME_COMPONENT(GCC_LIBSTDCXX_DIR "${GCC_LIBSTDCXX_PATH}" DIRECTORY)
    GET_FILENAME_COMPONENT(GCC_LIBSTDCXX_DIR "${GCC_LIBSTDCXX_DIR}" REALPATH)
    IF (GCC_LIBSTDCXX_DIR AND EXISTS "${GCC_LIBSTDCXX_DIR}")
        MESSAGE("GCC libstdc++ dir:  ${GCC_LIBSTDCXX_DIR}")
        SET(CMAKE_BUILD_RPATH "${GCC_LIBSTDCXX_DIR}")
        LIST(APPEND CMAKE_INSTALL_RPATH "${GCC_LIBSTDCXX_DIR}")
    ENDIF ()
ENDIF ()

INCLUDE(CheckTypeSize)
CHECK_TYPE_SIZE("void*" SIZEOF_VOID_P BUILTIN_TYPES_ONLY)
SET(BITNESS 32)
IF (SIZEOF_VOID_P EQUAL 8)
    SET(BITNESS 64)
ENDIF ()

MESSAGE("Build type:         ${CMAKE_BUILD_TYPE} ${BITNESS} bit")
MESSAGE("Compiler:           ${CMAKE_CXX_COMPILER_ID}")
MESSAGE("Coverage:           ${BUILD_WITH_COVERAGE}")
IF (WIN32)
    ADD_DEFINITIONS(
            -DHAVE_CONFIG_H
            -DWIN32
            -D_SILENCE_TR1_NAMESPACE_DEPRECATION_WARNING
            -D_CRT_SECURE_NO_WARNINGS
            -DGTEST_LANG_CXX11=1
    )
    # /MP: the Visual Studio generator runs projects side by side, but compiles the files of one
    # project one at a time unless told otherwise - and xmq_server_core alone is most of the code.
    ADD_COMPILE_OPTIONS(/wd4275 /std:c++20 /W4 /arch:AVX2 /MP)
    LINK_LIBRARIES(wsock32 ole32 uuid comctl32 ws2_32 winhttp)
ELSE ()
    IF (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # No -fexperimental-library any more. It was here for std::jthread and <stop_token>, which
        # libc++ still gates behind it in LLVM 19 and which it tags with the libc++ version
        # (abi:se190107 for 19.1.7) - so every part of the process had to carry the flag and a
        # libc++ upgrade was an ABI break. Neither type is used now: sptk::JoiningThread joins on
        # destruction, which is all this code ever wanted from jthread, and the one stop_token is a
        # flag.
        ADD_DEFINITIONS(
                -Wall -pedantic
                -Wno-c++98-compat
                -Wno-c++98-compat-pedantic
                -Wno-cast-align
                -Wno-exit-time-destructors
                -Wno-float-equal
                -Wno-format-nonliteral
                -Wno-global-constructors
                -Wno-implicit-fallthrough
                -Wno-long-long
                -Wno-missing-prototypes
                -Wno-missing-noreturn
                -Wno-padded
                -Wno-shadow
                -Wno-switch-enum
                -Wno-unused-parameter
                -fPIC
        )
        IF (BUILD_WITH_COVERAGE)
            ADD_DEFINITIONS(-fprofile-instr-generate -fcoverage-mapping)
            ADD_DEFINITIONS(-DBUILD_WITH_COVERAGE)
        ENDIF ()
    ELSE ()
        ADD_DEFINITIONS(-Wall -pedantic -Wno-long-long -fPIC -fno-var-tracking-assignments)
        IF (BUILD_WITH_COVERAGE)
            ADD_DEFINITIONS(--coverage)
            ADD_DEFINITIONS(-DBUILD_WITH_COVERAGE)
        ENDIF ()
    ENDIF ()

    # Keep inline functions out of the dynamic symbol tables.
    #
    # Without this, each shared library exports its own weak copy of inline definitions such as
    # std::mutex::lock(). The first library loaded then interposes that symbol for every other one,
    # so a lock can be taken by one library's copy while the matching unlock is inlined into
    # another. On FreeBSD the two copies disagree about libstdc++'s __gthread_active() flag - the
    # interposing copy locks for real, the inlined unlock compiles to nothing - and the mutex is
    # left held; the next acquisition fails with EDEADLK. SPTK carries the same flag for the same
    # reason, and XMQ is built from as many shared libraries as SPTK is.
    ADD_DEFINITIONS(-fvisibility-inlines-hidden)

    IF (CMAKE_BUILD_TYPE STREQUAL "Debug")
        #ADD_DEFINITIONS(-O1 -msse4.2)

        # Enable address sanitizer:
        #ADD_DEFINITIONS(-fsanitize=address)
        #LINK_LIBRARIES(tsan)

        # Enable thread sanitizer:
        #ADD_DEFINITIONS(-fsanitize=thread)
        #LINK_LIBRARIES(tsan)
    ELSE ()
        ADD_DEFINITIONS(-O3 -msse4.2)
    ENDIF ()

    IF (WITH_FRAME_POINTERS)
        ADD_DEFINITIONS(-fno-omit-frame-pointer)
    ENDIF ()

    IF (USE_NEW_ABI)
        SET(NEW_ABI_FLAG "1")
        MESSAGE("GCC ABI:            1")
        ADD_DEFINITIONS(-D_GLIBCXX_USE_CXX11_ABI=1)
    ELSE ()
        SET(NEW_ABI_FLAG "0")
        MESSAGE("GCC ABI:            0")
        ADD_DEFINITIONS(-D_GLIBCXX_USE_CXX11_ABI=0)
    ENDIF ()

    IF (UNIX)
        LINK_LIBRARIES(pthread rt dl)
        IF (BUILD_WITH_COVERAGE)
            LINK_LIBRARIES(gcov)
        ENDIF ()
    ELSE ()
        LINK_LIBRARIES(pthread rt)
    ENDIF ()
ENDIF ()

IF (WIN32)
    SET(WINDIR "$ENV{ProgramW6432}")
    IF (NOT WINDIR)
        SET(WINDIR "$ENV{ProgramFiles}")
    ENDIF ()
    SET(CMAKE_INSTALL_PREFIX "${WINDIR}/XMQ" CACHE PATH "Cmake prefix" FORCE)
    SET(LIBRARY_TYPE SHARED)
    # SPTK is linked dynamically (see FindSPTK.cmake), so its headers must mark their symbols
    # __declspec(dllimport), which is what SP_DLL does. It was off while SPTK was linked statically,
    # because then the same define made the linker warn (LNK4217/LNK4286) on every SPTK symbol.
    # LIBRARY_TYPE above still controls XMQ's own libraries, whose XMQ_EXPORT is independent of it.
    ADD_DEFINITIONS(-DSP_DLL)
ELSE ()
    # CMake's own default prefix is /usr/local, and installing there means changing that directory's
    # ownership or permissions. On Linux that is bad practice one can get away with. On FreeBSD
    # /usr/local is where the ports tree installs everything, and changing its ownership breaks the
    # system. A prefix under the user's own home needs no privilege on either.
    #
    # Only the default is replaced. CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT is set by CMake when
    # the prefix was not given, so -DCMAKE_INSTALL_PREFIX=/usr/local on the command line still wins,
    # which is how the packages are built.
    IF (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT AND NOT "$ENV{HOME}" STREQUAL "")
        SET(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH "Install prefix" FORCE)
    ENDIF ()
    #SET (LIBRARY_TYPE STATIC)
    SET(LIBRARY_TYPE SHARED)
ENDIF ()
MESSAGE("Library type:       " ${LIBRARY_TYPE})
MESSAGE("Install prefix:     " ${CMAKE_INSTALL_PREFIX})

# Where the web interface files are installed, and where the server looks for them.
# Defined after CMAKE_INSTALL_PREFIX is settled, so that it follows the prefix actually used.
# The two platforms disagree on the layout: Windows keeps the GUI beside the binaries, under the
# installation directory, while Unix puts it under lib. On Windows this is only the default -
# the installer lets the installation directory be chosen, and the server resolves the GUI
# relative to its own executable at run time (see DirectoryNames::webfaceDirectory).
IF (WIN32)
    SET(REACT_DIRECTORY ${CMAKE_INSTALL_PREFIX}/react)
ELSE ()
    SET(REACT_DIRECTORY ${CMAKE_INSTALL_PREFIX}/lib/xmq)
ENDIF ()

# The same directory as gui-react's install rule has to name it.
XMQ_INSTALL_DESTINATION("${REACT_DIRECTORY}" XMQ_REACT_INSTALL_DESTINATION)

# Whether this tree installs into the user's own home. It decides two things: where the broker keeps
# its configuration and its log, and whether a package may be built from this tree at all.
SET(XMQ_PREFIX_IN_HOME FALSE)
IF (NOT WIN32 AND NOT "$ENV{HOME}" STREQUAL "")
    STRING(FIND "${CMAKE_INSTALL_PREFIX}" "$ENV{HOME}" xmqPrefixPosition)
    IF (xmqPrefixPosition EQUAL 0)
        SET(XMQ_PREFIX_IN_HOME TRUE)
    ENDIF ()
ENDIF ()

# Where the configuration, the certificates and the log live. Decided here rather than by #ifdef in
# DirectoryNames.cpp, because it is a property of how the broker was installed and not of the system
# it runs on - and because the install rules below have to agree with it. They used to be written
# out twice, once in CMake and once in C++, which is a disagreement waiting to happen.
IF (WIN32)
    # From the environment rather than a literal C:/ProgramData. The install rules already read it
    # from there while DirectoryNames.cpp had the literal, so on a machine whose ProgramData is not
    # on C: the two disagreed: the configuration was installed in one place and looked for in another.
    STRING(REPLACE "\\" "/" ProgramData $ENV{ProgramData})
    SET(XMQ_CONF_DIRECTORY "${ProgramData}/xmq")
    SET(XMQ_LOGS_DIRECTORY "${ProgramData}/xmq/logs")
ELSEIF (XMQ_PREFIX_IN_HOME)
    # Nothing in an installation under somebody's home may need a privilege to write, and both /etc
    # and /var/log do. The way round it on this host had been to leave those directories writable by
    # everyone, which is the same problem wearing a different hat.
    SET(XMQ_CONF_DIRECTORY "${CMAKE_INSTALL_PREFIX}/etc/xmq")
    SET(XMQ_LOGS_DIRECTORY "${CMAKE_INSTALL_PREFIX}/var/log/xmq")
ELSEIF (BSD)
    # A packaged broker on FreeBSD. hier(7) puts the configuration of everything installed from
    # ports under the prefix, not in /etc - /etc there belongs to the base system, and a package
    # writing into it is a package fighting the operating system. The log goes to /var/log like
    # everywhere else, which is where hier(7) puts it too.
    SET(XMQ_CONF_DIRECTORY "${CMAKE_INSTALL_PREFIX}/etc/xmq")
    SET(XMQ_LOGS_DIRECTORY "/var/log/xmq")
ELSE ()
    # A packaged broker: where the filesystem hierarchy standard says, whatever the prefix.
    SET(XMQ_CONF_DIRECTORY "/etc/xmq")
    SET(XMQ_LOGS_DIRECTORY "/var/log/xmq")
ENDIF ()
SET(XMQ_CERTS_DIRECTORY "${XMQ_CONF_DIRECTORY}/certs")

# The account a packaged broker runs as. The packages create it on installation and give it the
# three directories above; the service definitions name it. Not used by a build installed into a
# home directory, which runs as whoever is sitting there.
SET(XMQ_SERVICE_USER "xmq")

# The same directory as the install rules have to name it. A packaged Linux broker keeps its
# configuration in /etc, which is not under the prefix, so there this stays absolute and nothing
# changes.
XMQ_INSTALL_DESTINATION("${XMQ_CONF_DIRECTORY}" XMQ_CONF_INSTALL_DESTINATION)

MESSAGE("Configuration:      " ${XMQ_CONF_DIRECTORY})
MESSAGE("Logs:               " ${XMQ_LOGS_DIRECTORY})
MESSAGE("Service account:    " ${XMQ_SERVICE_USER})


FIND_LIBRARY(SOCKET_LIBRARY NAMES socket PATHS /usr/lib)
IF (NOT SOCKET_LIBRARY)
    SET(SOCKET_LIBRARY "")
ELSE ()
    SET(SOCKET_LIBRARY "-lsocket -lrt -lnsl")
ENDIF ()


# Exactly this SPTK release, and no other. SPTK's soname carries its whole version, so the binaries
# built here name these libraries and load nothing else; the package carries the same release in
# lib/xmq-sptk. Changing it is part of moving XMQ to a new SPTK, together with the CHANGELOG's
# "Requires SPTK" line.
SET(XMQ_SPTK_VERSION "5.6.14")
FIND_PACKAGE(SPTK ${XMQ_SPTK_VERSION} EXACT QUIET)
IF (NOT SPTK_FOUND AND SPTK_NOT_FOUND_REASON)
    MESSAGE(FATAL_ERROR "SPTK library:       ${SPTK_NOT_FOUND_REASON}")
ENDIF ()
IF (SPTK_FOUND)
    MESSAGE("SPTK support:       found ${SPTK_VERSION}")
    # On every compile line, so that moving to another SPTK rebuilds everything. Installing SPTK
    # keeps its headers' timestamps, and those are older than objects compiled yesterday against the
    # previous release's headers - so without this a build after an SPTK upgrade recompiled nothing,
    # linked the old objects against the new libraries, and user-database went on reporting itself
    # as built against 5.6.10 and refusing to start under 5.6.11.
    STRING(REPLACE "." "_" xmqSptkVersionTag "${SPTK_VERSION}")
    ADD_COMPILE_DEFINITIONS(XMQ_BUILT_WITH_SPTK_${xmqSptkVersionTag})
    INCLUDE_DIRECTORIES(${SPTK_INCLUDE_DIR})
    LINK_LIBRARIES(${SPTK_LIBRARIES})
    SET(SPTK_FLAG "1")
    SET(SPTK_DEFINE "#define HAVE_SPTK")
ELSE ()
    MESSAGE(FATAL_ERROR "SPTK library:       not found")
ENDIF ()


FIND_PACKAGE(OpenSSL QUIET)
IF (OPENSSL_FOUND)
    MESSAGE("OpenSSL support:    compile")
    INCLUDE_DIRECTORIES(${OPENSSL_INCLUDE_DIR})
    LINK_LIBRARIES(${OPENSSL_LIBRARIES})
    SET(OPENSSL_FLAG "1")
    SET(OPENSSL_DEFINE "#define HAVE_OPENSSL")
ELSE (OPENSSL_FOUND)
    MESSAGE("OpenSSL support: don't compile")
ENDIF (OPENSSL_FOUND)


FIND_PACKAGE(GTest REQUIRED QUIET)
IF (GTest_FOUND)
    MESSAGE("GTest library:      found")
    SET(GTest_FLAG "1")
ELSE ()
    MESSAGE("GTest support:      not found")
ENDIF ()


IF (WIN32)
    SET(PROGRAM_FILES_86 "PROGRAMFILES(X86)")
    # Windows message resource compiler (included with Windows SDK)
    FIND_PROGRAM(MC_COMPILER mc PATHS
            "$ENV{ProgramFiles}/Windows Kits/*/bin/x64"
            "$ENV{ProgramW6432}/Windows Kits/*/bin/x64"
            "$ENV{ProgramFiles}/Microsoft SDKs/Windows/*/Bin/x64"
            "$ENV{ProgramW6432}/Microsoft SDKs/Windows/*/Bin/x64"
            "$ENV{ProgramFiles}/Windows Kits/*/bin/*/x64"
            "$ENV{ProgramW6432}/Windows Kits/*/bin/*/x64"
            "$ENV{ProgramFiles}/Windows Kits/10/bin/10.0.15063.0/x64"
            "$ENV{${PROGRAM_FILES_86}}/Windows Kits/*/bin/*/x64"
            "$ENV{${PROGRAM_FILES_86}}/Windows Kits/*/bin/*/x86"
    )
    IF (MC_COMPILER)
        MESSAGE("Windows SDK mc:     found")
    ELSE (MC_COMPILER)
        MESSAGE("Windows SDK mc:     not found")
    ENDIF (MC_COMPILER)
    ADD_SUBDIRECTORY(events.w32)
ENDIF (WIN32)


SET(PCRE2_FLAG "0")
SET(PCRE_FLAG "0")
FIND_PACKAGE(PCRE2 QUIET)
IF (PCRE2_FOUND)
    MESSAGE("PCRE2 support:      found")
    INCLUDE_DIRECTORIES(${PCRE2_INCLUDE_DIR})
    SET(PCRE2_FLAG "1")
    SET(PCRE2_DEFINE "#define HAVE_PCRE2")
    LINK_LIBRARIES(${PCRE2_LIBRARY})
ELSE (PCRE2_FOUND)
    MESSAGE(FATAL_ERROR "PCRE2 support:      don't compile")
ENDIF (PCRE2_FOUND)


SET(Brotli_FLAG "0")
FIND_PACKAGE(Brotli QUIET)
IF (BROTLI_FOUND)
    MESSAGE("Brotli support:     found")
    INCLUDE_DIRECTORIES(${Brotli_INCLUDE_DIR})
    SET(BROTLI_FLAG "1")
    SET(BROTLI_DEFINE "#define HAVE_BROTLI")
    LINK_LIBRARIES(${BROTLI_LIBRARIES})
ENDIF ()


SET(ZLIB_FLAG "0")
FIND_PACKAGE(ZLIB QUIET)
IF (ZLIB_FOUND)
    MESSAGE("ZLIB library:       found")
    INCLUDE_DIRECTORIES(${ZLIB_INCLUDE_DIR})
    LINK_LIBRARIES(${ZLIB_LIBRARY})
    SET(ZLIB_FLAG "1")
ELSE (ZLIB_FOUND)
    MESSAGE(FATAL_ERROR "ZLIB library not found")
    SET(ZLIB_FLAG "0")
ENDIF (ZLIB_FOUND)


SET(OPENSSL_FLAG "0")
IF (USE_OPENSSL)
    FIND_PACKAGE(OpenSSL QUIET)
    IF (OPENSSL_FOUND)
        MESSAGE("OpenSSL library:    found")
        INCLUDE_DIRECTORIES(${OPENSSL_INCLUDE_DIR})
        LINK_LIBRARIES(${OPENSSL_LIBRARIES})
        SET(OPENSSL_FLAG "1")
    ELSE (OPENSSL_FOUND)
        MESSAGE("OpenSSL support:    don't compile")
    ENDIF (OPENSSL_FOUND)
ENDIF (USE_OPENSSL)

INCLUDE(cmake/CXX20.cmake)
CHECK_FOR_CXX20_COMPILER(CXX20_COMPILER)
# If a C++20 compiler is available, then set the appropriate flags
IF (CXX20_COMPILER)
    ENABLE_CXX20()
    SET(CXX20_FLAG "1")
ELSE (CXX20_COMPILER)
    MESSAGE(FATAL_ERROR "C++20 compiler is not available")
ENDIF ()

SET(THEMES_PREFIX ${CMAKE_INSTALL_PREFIX})

INCLUDE_DIRECTORIES(${SPTK_SOURCE_DIR})

INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR})

# The build tree too, for the one generated header: base/xmq-config.h is written into <build>/base
# rather than into the sources, and is included as "base/xmq-config.h" like any other.
#
# BEFORE, so that this build's copy wins over anything of the same name left in the source tree by
# an older in-source build. One such leftover named a home directory as the configuration path, and
# the package built from it installed a broker that looked for its configuration there, found none,
# wrote a default one and came up with no users at all - authentication then failed for everybody.
# Nothing in the build said a word about it, because there was nothing wrong with the build.
INCLUDE_DIRECTORIES(BEFORE ${CMAKE_BINARY_DIR})

# Create initial set of service files.
#
# Every generated file it produces has to be named here, not just one of them. The condition used to
# ask about CXmqServiceBase.h alone: when CXmqWSDL.cpp was removed from the repository - reasonably,
# since it is generated - the answer stayed "already there", wsdl2cxx never ran, and the file was
# missing from a source list that is built by glob. The build then linked with no definition of
# xmq_wsdl and said so in the one place nobody reads first, the linker.
# Full paths: CMake's EXISTS is only well defined for those, and a relative one here resolved
# against whatever directory cmake happened to be invoked from. That made the test answer "already
# generated" for a checkout that had nothing in service/ at all, as long as some other tree with
# those files was the current directory - which is exactly what a fresh clone hits.
#
# And it asks about the WSDL's own age, not only about whether the files are there. Absence is not
# the only reason they can be wrong: adding a type to xmq.wsdl leaves both of these present and
# stale, so this said "already generated" and the new class was made by the build rule below -
# after the glob that decides what the library compiles. The class was then missing from the
# library, and the first build after every WSDL change failed at link with unresolved externals
# naming a constructor. A second cmake put it right, which made it look like a flaky build rather
# than a rule asking the wrong question. Found on Windows, where service/ had no earlier copy to
# hide it; the same tree on Linux had built the class into place weeks before.
IF (NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/service/CXmqServiceBase.h OR
        NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/service/CXmqWSDL.cpp OR
        ${CMAKE_CURRENT_SOURCE_DIR}/xmq.wsdl IS_NEWER_THAN ${CMAKE_CURRENT_SOURCE_DIR}/service/CXmqServiceBase.h)
    EXECUTE_PROCESS(COMMAND wsdl2cxx xmq.wsdl -d service -j xmq.json -a bearer -o Login:basic,SetupState:none -n xmq
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
ENDIF ()

# Absolute paths, because wsdl2cxx writes into the source tree while a relative OUTPUT names the
# build tree - so the file the rule promised never appeared where the rule said, and the rule could
# not be satisfied. And a target to hang it on: an ADD_CUSTOM_COMMAND runs only when something
# depends on its output, and nothing did, so editing xmq.wsdl regenerated nothing at all. The
# generated sources were only ever refreshed by deleting them or by running wsdl2cxx by hand.
ADD_CUSTOM_COMMAND(
        OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/service/CXmqServiceBase.h
        ${CMAKE_CURRENT_SOURCE_DIR}/service/CXmqWSDL.cpp
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/xmq.wsdl
        COMMAND wsdl2cxx xmq.wsdl -d service -j xmq.json -a bearer -o Login:basic,SetupState:none -n xmq
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating the service sources from xmq.wsdl"
)

ADD_CUSTOM_TARGET(xmq_service_sources
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/service/CXmqServiceBase.h)

# Defined before any subdirectory, because the test directory builds the sample extensions with it.
SET(XMQ_SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
INCLUDE(cmake/XmqExtension.cmake)

add_subdirectory(base)
add_subdirectory(storage)
add_subdirectory(common)
add_subdirectory(scenario)
add_subdirectory(client)
add_subdirectory(server)
# Keep the test subtree available by target name, but out of the default "all" target. In
# particular, CMake's generated install target first builds "all"; without EXCLUDE_FROM_ALL a
# normal `make install` spends most of its time compiling xmq_unit_tests even though that binary is
# not part of a production installation.
add_subdirectory(test EXCLUDE_FROM_ALL)
add_subdirectory(utilities)
add_subdirectory(daemon)

# CONFIGURE_DEPENDS so that a type added to the WSDL is noticed by a plain build. Without it the
# glob is evaluated once, at configure time, and a newly generated class is missing from the
# library until CMake is run again - which shows up as undefined references, not as a missing file.
FILE(GLOB GENERATED_SERVICE_FILES CONFIGURE_DEPENDS service/*.*)
ADD_LIBRARY(xmq_service ${GENERATED_SERVICE_FILES})
ADD_DEPENDENCIES(xmq_service xmq_service_sources)

ADD_EXECUTABLE(xmq_server server/main.cpp)
TARGET_LINK_LIBRARIES(xmq_server xmq_base xmq_storage xmq_common xmq_service xmq_server_core)

# Every installed program loads SPTK from lib/xmq-sptk, the package's own copy, and from nowhere
# else. It used to be $ORIGIN:$ORIGIN/../lib on the broker alone, which found SPTK in the prefix's
# shared lib/ - where the package put it, and so replaced the SPTK of every other program on the
# machine and collided with SPTK's own packages - and the utilities had no SPTK path at all, finding
# it only through the loader cache. A private directory is on no search path but these, so nothing
# else on the machine sees it, and it holds exactly the release in XMQ_SPTK_VERSION.
#
# Written as DT_RPATH (see the linker flag above), so LD_LIBRARY_PATH cannot put another copy first.
# Setting INSTALL_RPATH on a target *replaces* CMAKE_INSTALL_RPATH rather than adding to it, so the
# pinned libstdc++ directory above has to be carried over explicitly.
IF (UNIX AND NOT APPLE)
    SET(XMQ_INSTALL_RPATH "$ORIGIN/../lib/xmq-sptk")
    IF (GCC_LIBSTDCXX_DIR)
        SET(XMQ_INSTALL_RPATH "${XMQ_INSTALL_RPATH}:${GCC_LIBSTDCXX_DIR}")
    ENDIF ()
    SET_TARGET_PROPERTIES(xmq_server xmq_pub xmq_sub xmq_con xmq_scn xmq_scn_cluster xmq_unit_tests
            PROPERTIES INSTALL_RPATH "${XMQ_INSTALL_RPATH}")
ENDIF ()

# xmq_unit_tests is deliberately not among them. It is of no use to anyone installing the broker -
# it wants a Redis, a mosquitto, a database host and its own data files before it will run - and
# shipping it drags googletest into the runtime dependencies of a production package, which is the
# one library here whose ABI is tied to the compiler that built it. xmq_scn stays: the scenario
# utility is a tool in its own right. xmq_scn_cluster is the same kind of thing - a tool of its own,
# with no dependency the others do not already carry - and it is the only way to run the tests the
# cluster is measured by, so it is installed with them.
INSTALL(TARGETS xmq_server xmq_pub xmq_sub xmq_con xmq_scn xmq_scn_cluster
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
)

# Tests are an opt-in install component. EXCLUDE_FROM_ALL here applies to installation: an ordinary
# `cmake --install` (and therefore CPack) omits the binary, while the install_tests target below can
# request this component explicitly.
INSTALL(TARGETS xmq_unit_tests
        RUNTIME DESTINATION bin
        COMPONENT tests
        EXCLUDE_FROM_ALL
)

INSTALL(DIRECTORY test/ScenarioTests/Basic test/ScenarioTests/Enterprise test/ScenarioTests/Bridges
        DESTINATION share/xmq
)

IF (NOT WIN32)
    # SPTK travels inside this package. It is linked dynamically (see cmake/FindSPTK.cmake), so it
    # has to be here or the broker does not start - but shipping it also keeps the promise the
    # static link used to keep, that a user downloads one file and installs it.
    #
    # One file per library, named by its soname - lib<name>.so.<SPTK version> - into lib/xmq-sptk, the
    # directory the RPATH of every installed program names. The soname carries the whole version, so
    # that file is the only name a binary built here asks for; the development link lib<name>.so is
    # not needed at run time, and SPTK opens its database drivers by the same versioned name.
    # Executable, because rpm derives a library's Provides only from files that are.
    #
    # Only the sqlite3 driver. It is what a standalone broker uses - the deployment expected to have
    # no database server at all - and this version defaults to it. SPTK reaches a driver with dlopen
    # at run time, so it is never a link-time dependency and never appears in ldd: it has to be
    # listed here by hand, or it is silently absent until someone configures a broker to use it.
    #
    # The mysql and postgresql drivers are deliberately not here, and this is a correction rather
    # than an omission. They were shipped on the reasoning that the client libraries are small and
    # come from the distribution's own repositories. Measured on a clean FreeBSD 15.1 rather than on
    # a machine that already had everything, they are not: they turned "one package, 13 MB" into
    # **thirty packages and 356 MB**, dragging in mysql84-client, postgresql14-client,
    # openldap26-client, cyrus-sasl and behind them perl5, icu, groff and curl - onto a machine that
    # only wants to run a broker. What the broker itself needs is openssl, pcre2 and brotli.
    #
    # They belong to the cluster configuration, which is not ready for production use and is an
    # extension; the LDAP client is the same story before 1.0. They ship with the extensions that
    # need them, not with every broker. Oracle and ODBC were never here, for the same reason.
    # By name, not by globbing: an earlier SPTK left behind on the build host sits in the same
    # directory under its own versioned names, and only the release this build was configured
    # against is wanted. A missing file is a configuration error, not a package without a driver.
    GET_FILENAME_COMPONENT(SPTK_LIBRARY_DIR ${SPTK_LIBRARY_CORE} DIRECTORY)
    SET(SPTK_RUNTIME_FILES)
    FOREACH (sptkLibrary sputil5 spwsdl5 spdb5 spdb5_sqlite3)
        SET(sptkRuntimeFile "${SPTK_LIBRARY_DIR}/lib${sptkLibrary}.so.${SPTK_VERSION}")
        IF (NOT EXISTS "${sptkRuntimeFile}")
            MESSAGE(FATAL_ERROR "SPTK runtime library lib${sptkLibrary}.so.${SPTK_VERSION} not found in ${SPTK_LIBRARY_DIR}")
        ENDIF ()
        LIST(APPEND SPTK_RUNTIME_FILES "${sptkRuntimeFile}")
    ENDFOREACH ()
    INSTALL(FILES ${SPTK_RUNTIME_FILES} DESTINATION lib/xmq-sptk
            PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
ENDIF ()

IF (BUILD_PACKAGE)
    # Packages still install to /usr/local, and a package must be built from a tree configured for
    # the prefix it will unpack into - not only because CPACK_PACKAGING_INSTALL_PREFIX follows
    # CMAKE_INSTALL_PREFIX, but because the prefix is compiled into the binaries: REACT_DIRECTORY
    # is where the broker looks for its web interface. Built from a tree configured for a prefix
    # under the builder's home, a package would unpack into /usr/local and then look for its web
    # interface in a directory that exists only on the build machine.
    #
    # So packaging is simply not offered for such a tree, rather than offered and wrong. Configure
    # with -DCMAKE_INSTALL_PREFIX=/usr/local to build one.
    SET(xmqPrefixInHome FALSE)
    IF (NOT WIN32 AND NOT "$ENV{HOME}" STREQUAL "")
        STRING(FIND "${CMAKE_INSTALL_PREFIX}" "$ENV{HOME}" xmqPrefixPosition)
        IF (xmqPrefixPosition EQUAL 0)
            SET(xmqPrefixInHome TRUE)
        ENDIF ()
    ENDIF ()

    IF (xmqPrefixInHome)
        MESSAGE("Packaging:          off, prefix is under $ENV{HOME}")
    ELSE ()
        ADD_SUBDIRECTORY(packages)
    ENDIF ()
ENDIF ()

# One block for both platforms: only the directory differs, and that was settled above - by the same
# variable the broker itself is compiled with, so the configuration cannot be installed in one place
# and looked for in another.
# The configuration carries secrets the broker has to be able to present - the password in a
# database URI, the cluster secret, a bridge's credentials - so it is not for every account on the
# machine to read. CMake installs at 0644 unless told otherwise, and it was not told.
SET(XMQ_CONF_PERMISSIONS PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ)

# The template only. The live configuration is not installed at all, because the broker writes one
# on first start when it finds none - Settings::createConfiguration(), which prefers exactly this
# template when it is beside the file it is about to create.
#
# It used to be installed too, guarded by "IF (NOT EXISTS ${XMQ_CONF_DIRECTORY}/xmq_server.conf)".
# That question was asked while CMake configured the build, on the build machine, where there is no
# installation and the answer is always no - so every package carried a live configuration, and
# installing one over a running broker replaced the operator's settings with the defaults. It left
# nothing behind either: the file is not declared as configuration to dpkg or pkg, so there was no
# .dpkg-old and no .pkgsave. Proved on 2026-09-09 by upgrading 0.9.15 to 0.9.16 on the FreeBSD
# acceptance machine, where a live 1801-byte file came back as the 1682-byte template.
#
# Letting the broker create it is better than declaring it a config file in two packaging systems:
# there is then nothing for a packager to own, and the same thing happens on a platform that has no
# packaging at all.
INSTALL(FILES xmq_server.conf DESTINATION ${XMQ_CONF_INSTALL_DESTINATION} RENAME xmq_server.conf.template
        ${XMQ_CONF_PERMISSIONS})
# The extension list is a template only. A broker with no extensions needs no such file, and
# installing a live one would have every upgrade re-answer a question the operator answered.
# Readable by root and its group only, like the server template beside it: the first layer of
# acceptance requires 0640 for everything under etc/xmq, and this one was the exception in every
# 0.9.16 package, Linux and FreeBSD alike.
INSTALL(FILES xmq_extensions.conf.template DESTINATION ${XMQ_CONF_INSTALL_DESTINATION}
        ${XMQ_CONF_PERMISSIONS})
# Accounts are not installed at all. The file this tree carries is the one the tests and a
# development server run on, and it holds passwords that are in the sources for anyone to read.
# Shipping it made every installation start life with the same three known accounts, one of which
# could sign in over MQTT from anywhere on the network.
#
# The broker writes its own on first start instead: an administrator with no password yet, which
# nothing can authenticate against, plus the cluster account. Until a password is set the web
# interface is bound to the loopback address and admits the administrator without one, so that
# whoever installed the server can set it - see ServerController::controlServiceAddress().


# Closed extensions live in their own repository, outside this tree, and are built only when this
# points at them:
#
#     cmake . -DXMQ_EXTENSIONS_SOURCE_DIR=/path/to/xmq-extensions
#
# Outside rather than inside on purpose. Sources dropped into this tree could include the broker's
# headers and link its libraries, and the first deadline would see someone do it - at which point
# the C ABI stops being a boundary. Kept out, and built through XMQ_ADD_EXTENSION, an extension can
# reach nothing but extension/, which is the promise its documentation makes to anyone else writing
# one. The default build has no extensions at all and must keep working: that is what CI builds.
IF (XMQ_EXTENSIONS_SOURCE_DIR)
    IF (NOT IS_DIRECTORY "${XMQ_EXTENSIONS_SOURCE_DIR}")
        MESSAGE(FATAL_ERROR "XMQ_EXTENSIONS_SOURCE_DIR is not a directory: ${XMQ_EXTENSIONS_SOURCE_DIR}")
    ENDIF ()
    MESSAGE(STATUS "Extensions:        ${XMQ_EXTENSIONS_SOURCE_DIR}")
    ADD_SUBDIRECTORY("${XMQ_EXTENSIONS_SOURCE_DIR}" xmq-extensions)
ENDIF ()

# The authenticator the broker ships with. Inside the tree rather than beside the samples, because
# it is not a sample: without it, or something else declaring XMQ_CAP_AUTHENTICATOR, a standalone
# broker admits nobody. It is still built through XMQ_ADD_EXTENSION, so it reaches no more of the
# broker than an extension written by anyone else.
ADD_SUBDIRECTORY(extension/user-database)

# The extension interface. These two headers are the whole of what an extension compiles against;
# nothing else of the broker is needed, and nothing else should be relied on.
INSTALL(FILES extension/xmq_extension.h extension/XmqExtension.h extension/README.md
        DESTINATION include/xmq/extension)

# The project generator goes beside them, and finds them from its own location: installed at
# include/xmq/extension/, the headers it must point a generated project at are one directory up.
INSTALL(PROGRAMS extension/new-extension.py DESTINATION include/xmq/extension)

# The samples go with the headers: an extension author's first move is to copy one, and they are
# public domain precisely so that copying is the intended use.
INSTALL(DIRECTORY examples/extensions DESTINATION share/xmq/examples)

# And the helper that builds them, so that an extension kept outside this tree is built by the same
# rules as one inside it: against the ABI headers, and against nothing else of the broker.
INSTALL(FILES cmake/XmqExtension.cmake DESTINATION share/xmq/cmake)

ADD_SUBDIRECTORY(Keys)

ADD_SUBDIRECTORY(LoadTest)

# The React sources are staged into the binary directory by the gui-react build itself,
# so that a source edit is picked up by a plain build, without reconfiguring.
ADD_SUBDIRECTORY(gui-react)

# Developer installation with tests. Building the normal "all" target explicitly keeps this
# correct when new production targets are added, without maintaining a second target list here.
# xmq_unit_tests is excluded from "all", so it remains the explicit dependency of this target.
ADD_CUSTOM_TARGET(install_tests
        COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}" --target all --config $<CONFIG>
        COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}" --config $<CONFIG>
        COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}" --config $<CONFIG> --component tests
        DEPENDS xmq_unit_tests
        COMMENT "Building and installing XMQ with unit tests"
        USES_TERMINAL
)
