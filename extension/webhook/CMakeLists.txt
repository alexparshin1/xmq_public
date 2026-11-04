# The first data-integration connector: a message tap that posts JSON to an HTTP(S) endpoint.
# Sockets come from Winsock on Windows and from the POSIX API elsewhere; TLS and the signature are
# OpenSSL on both, so the extension links libssl as well as libcrypto. On Linux the SSL symbols
# used to resolve at run time out of the broker, which hid the missing dependency; a Windows import
# library carries no such transitivity and the link failed, which is why it is named here.
XMQ_ADD_EXTENSION(xmq_webhook SOURCES ${CMAKE_CURRENT_LIST_DIR}/Webhook.cpp OPENSSL)
IF (NOT TARGET OpenSSL::SSL)
    MESSAGE(FATAL_ERROR "xmq_webhook needs OpenSSL::SSL for HTTPS; FIND_PACKAGE(OpenSSL) has to "
                        "have found the SSL library, not only crypto.")
ENDIF ()
TARGET_LINK_LIBRARIES(xmq_webhook PRIVATE OpenSSL::SSL)
IF (WIN32)
    TARGET_LINK_LIBRARIES(xmq_webhook PRIVATE ws2_32)
ENDIF ()
INSTALL(TARGETS xmq_webhook
        LIBRARY DESTINATION lib/xmq-extensions
        RUNTIME DESTINATION lib/xmq-extensions)

# The fragment is a template: the live file belongs to the operator and says whether the
# extension is on and where it posts.
SET(XMQ_WEBHOOK_LIBRARY_NAME
        "${CMAKE_SHARED_LIBRARY_PREFIX}xmq_webhook${CMAKE_SHARED_LIBRARY_SUFFIX}")
SET(XMQ_WEBHOOK_LIBRARY
        "${CMAKE_INSTALL_PREFIX}/lib/xmq-extensions/${XMQ_WEBHOOK_LIBRARY_NAME}")
CONFIGURE_FILE("${CMAKE_CURRENT_LIST_DIR}/50-webhook.conf.in"
               "${CMAKE_CURRENT_BINARY_DIR}/50-webhook.conf.template" @ONLY)
# The same mode every configuration template gets: it can hold a secret, and the package check
# refuses anything under etc/xmq that another account can read.
INSTALL(FILES "${CMAKE_CURRENT_BINARY_DIR}/50-webhook.conf.template"
        DESTINATION ${XMQ_CONF_INSTALL_DESTINATION}/xmq_extensions.d
        ${XMQ_CONF_PERMISSIONS})
