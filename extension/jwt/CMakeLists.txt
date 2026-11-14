# This directory can also be built without the broker sources, against its installed SDK.
IF (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    CMAKE_MINIMUM_REQUIRED(VERSION 3.20)
    PROJECT(xmq_jwt LANGUAGES CXX)
    SET(CMAKE_CXX_STANDARD 20)
    SET(CMAKE_CXX_STANDARD_REQUIRED ON)
    FIND_PATH(XMQ_EXTENSION_INCLUDE_DIR extension/xmq_extension.h PATH_SUFFIXES include/xmq REQUIRED)
    FIND_FILE(XMQ_EXTENSION_CMAKE XmqExtension.cmake PATH_SUFFIXES share/xmq/cmake REQUIRED)
    INCLUDE("${XMQ_EXTENSION_CMAKE}")
    IF (NOT SPTK_INCLUDE_DIR)
        FIND_PATH(SPTK_INCLUDE_DIR sptk5/JWT.h
                HINTS "$ENV{SPTK_ROOT}/include" "$ENV{HOME}/.local/include"
                /usr/local/include /usr/include)
    ENDIF ()
    IF (NOT SPTK_LIBRARY_CORE)
        FIND_LIBRARY(SPTK_LIBRARY_CORE NAMES sputil5
                HINTS "$ENV{SPTK_ROOT}/lib" "$ENV{HOME}/.local/lib"
                /usr/local/lib /usr/lib /usr/lib64)
    ENDIF ()
ENDIF ()

# Keys from the issuer's key set are imported through provider parameters (EVP_PKEY_fromdata,
# OSSL_PARAM), which is OpenSSL 3: 1.1 is not enough, and a build without OpenSSL 3 simply has no
# JWT authenticator rather than failing to configure.
FIND_PACKAGE(OpenSSL 3.0 QUIET)
IF (OPENSSL_FOUND)
    IF (NOT SPTK_INCLUDE_DIR OR NOT SPTK_LIBRARY_CORE)
        MESSAGE(FATAL_ERROR "xmq_jwt requires SPTK JWT support (SPTK_INCLUDE_DIR and SPTK_LIBRARY_CORE)")
    ENDIF ()
    XMQ_ADD_EXTENSION(xmq_jwt SOURCES ${CMAKE_CURRENT_LIST_DIR}/Jwt.cpp OPENSSL
            UI "${CMAKE_CURRENT_LIST_DIR}/jwt.ui.json")
    # JWKS is fetched using OpenSSL's TLS BIO, not just libcrypto.
    TARGET_LINK_LIBRARIES(xmq_jwt OpenSSL::SSL)
    TARGET_INCLUDE_DIRECTORIES(xmq_jwt PRIVATE "${SPTK_INCLUDE_DIR}")
    TARGET_LINK_LIBRARIES(xmq_jwt "${SPTK_LIBRARY_CORE}")
    FOREACH (sptkDependency PCRE2_INCLUDE_DIR ZLIB_INCLUDE_DIR OPENSSL_INCLUDE_DIR)
        IF (${sptkDependency})
            TARGET_INCLUDE_DIRECTORIES(xmq_jwt PRIVATE "${${sptkDependency}}")
        ENDIF ()
    ENDFOREACH ()
    IF (UNIX AND NOT APPLE)
        SET_TARGET_PROPERTIES(xmq_jwt PROPERTIES INSTALL_RPATH "$ORIGIN/..")
    ENDIF ()
    IF (WIN32)
        TARGET_COMPILE_DEFINITIONS(xmq_jwt PRIVATE NOMINMAX)
    ENDIF ()
    INSTALL(TARGETS xmq_jwt LIBRARY DESTINATION lib/xmq-extensions
            RUNTIME DESTINATION lib/xmq-extensions)

    ADD_EXECUTABLE(xmq_jwt_tests EXCLUDE_FROM_ALL tests/JwtTests.cpp)
    TARGET_INCLUDE_DIRECTORIES(xmq_jwt_tests PRIVATE ${XMQ_EXTENSION_INCLUDE_DIR}
            ${CMAKE_CURRENT_LIST_DIR} "${SPTK_INCLUDE_DIR}")
    TARGET_LINK_LIBRARIES(xmq_jwt_tests OpenSSL::Crypto OpenSSL::SSL)
    TARGET_LINK_LIBRARIES(xmq_jwt_tests "${SPTK_LIBRARY_CORE}")
    FOREACH (sptkDependency PCRE2_INCLUDE_DIR ZLIB_INCLUDE_DIR OPENSSL_INCLUDE_DIR)
        IF (${sptkDependency})
            TARGET_INCLUDE_DIRECTORIES(xmq_jwt_tests PRIVATE "${${sptkDependency}}")
        ENDIF ()
    ENDFOREACH ()

    # Hide deprecated declarations rather than silencing their warnings: accidentally reintroducing
    # a legacy RSA/EC_KEY call must fail compilation, including in the standalone test harness.
    FOREACH (jwtTarget xmq_jwt xmq_jwt_tests)
        TARGET_COMPILE_DEFINITIONS(${jwtTarget} PRIVATE OPENSSL_API_COMPAT=30000 OPENSSL_NO_DEPRECATED)
    ENDFOREACH ()
ELSE ()
    IF (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        MESSAGE(FATAL_ERROR "OpenSSL 3.0 development files are required")
    ENDIF ()
    MESSAGE(STATUS "OpenSSL 3.0 not found: xmq_jwt extension is not built")
ENDIF ()
