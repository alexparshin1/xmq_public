# This directory can also be built without the broker sources, against its installed SDK.
IF (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    CMAKE_MINIMUM_REQUIRED(VERSION 3.20)
    PROJECT(xmq_http_auth LANGUAGES CXX)
    SET(CMAKE_CXX_STANDARD 20)
    SET(CMAKE_CXX_STANDARD_REQUIRED ON)
    FIND_PATH(XMQ_EXTENSION_INCLUDE_DIR extension/xmq_extension.h PATH_SUFFIXES include/xmq REQUIRED)
    FIND_FILE(XMQ_EXTENSION_CMAKE XmqExtension.cmake PATH_SUFFIXES share/xmq/cmake REQUIRED)
    INCLUDE("${XMQ_EXTENSION_CMAKE}")
    IF (NOT SPTK_INCLUDE_DIR)
        FIND_PATH(SPTK_INCLUDE_DIR sptk5/net/HttpConnect.h
                HINTS "$ENV{SPTK_ROOT}/include" "$ENV{HOME}/.local/include"
                /usr/local/include /usr/include)
    ENDIF ()
    IF (NOT SPTK_LIBRARY_CORE)
        FIND_LIBRARY(SPTK_LIBRARY_CORE NAMES sputil5
                HINTS "$ENV{SPTK_ROOT}/lib" "$ENV{HOME}/.local/lib"
                /usr/local/lib /usr/lib /usr/lib64)
    ENDIF ()
ENDIF ()

# SPTK owns URL parsing, verified TLS and HTTP; this extension hashes cache keys with OpenSSL's
# crypto library. A build without either simply has no HTTP authenticator rather than failing.
FIND_PACKAGE(OpenSSL 3.0 QUIET)
IF (OPENSSL_FOUND)
    IF (NOT SPTK_INCLUDE_DIR OR NOT SPTK_LIBRARY_CORE)
        MESSAGE(FATAL_ERROR "xmq_http_auth requires SPTK HTTP/TLS support (SPTK_INCLUDE_DIR and SPTK_LIBRARY_CORE)")
    ENDIF ()
    XMQ_ADD_EXTENSION(xmq_http_auth SOURCES ${CMAKE_CURRENT_LIST_DIR}/HttpAuth.cpp OPENSSL
            UI "${CMAKE_CURRENT_LIST_DIR}/http-auth.ui.json")
    TARGET_INCLUDE_DIRECTORIES(xmq_http_auth PRIVATE "${SPTK_INCLUDE_DIR}")
    TARGET_LINK_LIBRARIES(xmq_http_auth "${SPTK_LIBRARY_CORE}")
    FOREACH (sptkDependency PCRE2_INCLUDE_DIR ZLIB_INCLUDE_DIR OPENSSL_INCLUDE_DIR)
        IF (${sptkDependency})
            TARGET_INCLUDE_DIRECTORIES(xmq_http_auth PRIVATE "${${sptkDependency}}")
        ENDIF ()
    ENDFOREACH ()
    IF (UNIX AND NOT APPLE)
        SET_TARGET_PROPERTIES(xmq_http_auth PROPERTIES INSTALL_RPATH "$ORIGIN/..")
    ENDIF ()
    IF (WIN32)
        TARGET_COMPILE_DEFINITIONS(xmq_http_auth PRIVATE NOMINMAX)
    ENDIF ()
    INSTALL(TARGETS xmq_http_auth LIBRARY DESTINATION lib/xmq-extensions
            RUNTIME DESTINATION lib/xmq-extensions)

    ADD_EXECUTABLE(xmq_http_auth_tests EXCLUDE_FROM_ALL tests/HttpAuthTests.cpp)
    TARGET_INCLUDE_DIRECTORIES(xmq_http_auth_tests PRIVATE ${XMQ_EXTENSION_INCLUDE_DIR}
            ${CMAKE_CURRENT_LIST_DIR} "${SPTK_INCLUDE_DIR}")
    TARGET_LINK_LIBRARIES(xmq_http_auth_tests OpenSSL::Crypto "${SPTK_LIBRARY_CORE}")
    FOREACH (sptkDependency PCRE2_INCLUDE_DIR ZLIB_INCLUDE_DIR OPENSSL_INCLUDE_DIR)
        IF (${sptkDependency})
            TARGET_INCLUDE_DIRECTORIES(xmq_http_auth_tests PRIVATE "${${sptkDependency}}")
        ENDIF ()
    ENDFOREACH ()
    IF (WIN32)
        TARGET_COMPILE_DEFINITIONS(xmq_http_auth_tests PRIVATE NOMINMAX)
    ENDIF ()
ELSE ()
    IF (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        MESSAGE(FATAL_ERROR "OpenSSL 3.0 development files are required")
    ENDIF ()
    MESSAGE(STATUS "OpenSSL 3.0 not found: xmq_http_auth extension is not built")
ENDIF ()
