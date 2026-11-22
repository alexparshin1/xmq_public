# The first data-integration connector: a message tap that posts JSON to an HTTP(S) endpoint.
# Sockets come from Winsock on Windows and from the POSIX API elsewhere; TLS and the signature are
# OpenSSL on both, so the extension links libssl as well as libcrypto. On Linux the SSL symbols
# used to resolve at run time out of the broker, which hid the missing dependency; a Windows import
# library carries no such transitivity and the link failed, which is why it is named here.
XMQ_ADD_EXTENSION(xmq_webhook SOURCES ${CMAKE_CURRENT_LIST_DIR}/Webhook.cpp OPENSSL
                  UI "${CMAKE_CURRENT_LIST_DIR}/webhook.ui.json")

IF (NOT TARGET OpenSSL::SSL)
    MESSAGE(FATAL_ERROR "xmq_webhook needs OpenSSL::SSL for HTTPS; FIND_PACKAGE(OpenSSL) has to "
                        "have found the SSL library, not only crypto.")
ENDIF ()

XMQ_ADD_EXTENSION(xmq_message_export SOURCES ${CMAKE_CURRENT_LIST_DIR}/Webhook.cpp OPENSSL
                  UI "${CMAKE_CURRENT_LIST_DIR}/message_export.ui.json")

XMQ_ADD_EXTENSION(xmq_log_export SOURCES ${CMAKE_CURRENT_LIST_DIR}/Webhook.cpp OPENSSL
                  UI "${CMAKE_CURRENT_LIST_DIR}/log_export.ui.json")

TARGET_COMPILE_DEFINITIONS(xmq_message_export PRIVATE XMQ_MESSAGES_WEBHOOK)
TARGET_COMPILE_DEFINITIONS(xmq_log_export PRIVATE XMQ_LOG_WEBHOOK)

XMQ_ADD_EXTENSION(xmq_http_sink SOURCES ${CMAKE_CURRENT_LIST_DIR}/HttpSink.cpp OPENSSL
                  UI "${CMAKE_CURRENT_LIST_DIR}/http_sink.ui.json")
TARGET_COMPILE_DEFINITIONS(xmq_http_sink PRIVATE XMQ_SINK_BUILD)

XMQ_ADD_EXTENSION(xmq_db_sink SOURCES ${CMAKE_CURRENT_LIST_DIR}/DbSink.cpp SPTK
                  UI "${CMAKE_CURRENT_LIST_DIR}/db_sink.ui.json")

FOREACH (target xmq_webhook xmq_message_export xmq_log_export xmq_http_sink)
    TARGET_LINK_LIBRARIES(${target} PRIVATE OpenSSL::SSL)
    TARGET_LINK_LIBRARIES(${target} PRIVATE ${CMAKE_DL_LIBS})
    IF (WIN32)
        TARGET_LINK_LIBRARIES(${target} PRIVATE ws2_32)
    ENDIF ()
ENDFOREACH ()

INSTALL(TARGETS xmq_webhook xmq_message_export xmq_log_export xmq_http_sink xmq_db_sink
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
