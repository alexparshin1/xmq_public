# This directory can also be built without the broker sources, against its installed SDK.
IF (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    CMAKE_MINIMUM_REQUIRED(VERSION 3.20)
    PROJECT(xmq_ldap LANGUAGES CXX)
    SET(CMAKE_CXX_STANDARD 20)
    SET(CMAKE_CXX_STANDARD_REQUIRED ON)
    FIND_PATH(XMQ_EXTENSION_INCLUDE_DIR extension/xmq_extension.h PATH_SUFFIXES include/xmq REQUIRED)
    FIND_FILE(XMQ_EXTENSION_CMAKE XmqExtension.cmake PATH_SUFFIXES share/xmq/cmake REQUIRED)
    INCLUDE("${XMQ_EXTENSION_CMAKE}")
ENDIF ()

# Optional: ordinary broker builds do not acquire a mandatory LDAP dependency.
FIND_PACKAGE(OpenSSL REQUIRED)
IF (WIN32)
    XMQ_ADD_EXTENSION(xmq_ldap SOURCES ${CMAKE_CURRENT_LIST_DIR}/Ldap.cpp
            ${CMAKE_CURRENT_LIST_DIR}/LdapPlatform.cpp OPENSSL
            UI "${CMAKE_CURRENT_LIST_DIR}/ldap.ui.json")
    TARGET_LINK_LIBRARIES(xmq_ldap wldap32 crypt32)
    INSTALL(TARGETS xmq_ldap LIBRARY DESTINATION lib/xmq-extensions
            RUNTIME DESTINATION lib/xmq-extensions)
ELSE ()
    FIND_PATH(LDAP_INCLUDE_DIR ldap.h)
    FIND_LIBRARY(LDAP_LIBRARY NAMES ldap)
    FIND_LIBRARY(LDAP_LBER_LIBRARY NAMES lber)
    IF (LDAP_INCLUDE_DIR AND LDAP_LIBRARY AND LDAP_LBER_LIBRARY)
        XMQ_ADD_EXTENSION(xmq_ldap SOURCES ${CMAKE_CURRENT_LIST_DIR}/Ldap.cpp
                ${CMAKE_CURRENT_LIST_DIR}/LdapPlatform.cpp OPENSSL
                UI "${CMAKE_CURRENT_LIST_DIR}/ldap.ui.json")
        TARGET_INCLUDE_DIRECTORIES(xmq_ldap PRIVATE ${LDAP_INCLUDE_DIR})
        TARGET_LINK_LIBRARIES(xmq_ldap ${LDAP_LIBRARY} ${LDAP_LBER_LIBRARY})
        INSTALL(TARGETS xmq_ldap LIBRARY DESTINATION lib/xmq-extensions
                RUNTIME DESTINATION lib/xmq-extensions)
        ADD_EXECUTABLE(xmq_ldap_tests EXCLUDE_FROM_ALL tests/LdapTests.cpp)
        TARGET_SOURCES(xmq_ldap_tests PRIVATE ${CMAKE_CURRENT_LIST_DIR}/LdapPlatform.cpp)
        TARGET_INCLUDE_DIRECTORIES(xmq_ldap_tests PRIVATE ${XMQ_EXTENSION_INCLUDE_DIR} ${CMAKE_CURRENT_LIST_DIR} ${LDAP_INCLUDE_DIR})
        TARGET_LINK_LIBRARIES(xmq_ldap_tests ${LDAP_LIBRARY} ${LDAP_LBER_LIBRARY} OpenSSL::Crypto)
    ELSE ()
        IF (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
            MESSAGE(FATAL_ERROR "LDAP development headers and libldap/liblber are required")
        ENDIF ()
        MESSAGE(STATUS "LDAP development library not found: xmq_ldap extension is not built")
    ENDIF ()
ENDIF ()
