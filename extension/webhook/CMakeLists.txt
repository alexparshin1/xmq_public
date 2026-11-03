# The first data-integration connector: a message tap that posts JSON to an HTTP(S) endpoint.
# POSIX sockets and OpenSSL; a Windows port needs Winsock and is separate work.
IF (UNIX)
    XMQ_ADD_EXTENSION(xmq_webhook SOURCES ${CMAKE_CURRENT_LIST_DIR}/Webhook.cpp OPENSSL)
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
    INSTALL(FILES "${CMAKE_CURRENT_BINARY_DIR}/50-webhook.conf.template"
            DESTINATION ${XMQ_CONF_INSTALL_DESTINATION}/xmq_extensions.d)
ENDIF ()
