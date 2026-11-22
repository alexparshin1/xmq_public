# Exporters and sinks. The legacy webhook and the HTTP sink share the SPTK-based
# HTTP peer, while the two exporters load their selected sinks at runtime.
SET(XMQ_WEBHOOK_SOURCES "${CMAKE_CURRENT_LIST_DIR}/Webhook.cpp"
                        "${CMAKE_CURRENT_LIST_DIR}/HttpPeer.cpp"
                        "${CMAKE_CURRENT_LIST_DIR}/SinkModule.cpp")
XMQ_ADD_EXTENSION(xmq_webhook SOURCES ${XMQ_WEBHOOK_SOURCES} SPTK OPENSSL
                  UI "${CMAKE_CURRENT_LIST_DIR}/webhook.ui.json")

IF (NOT TARGET OpenSSL::SSL)
    MESSAGE(FATAL_ERROR "xmq_webhook needs OpenSSL::SSL for HTTPS; FIND_PACKAGE(OpenSSL) has to "
                        "have found the SSL library, not only crypto.")
ENDIF ()

XMQ_ADD_EXTENSION(xmq_message_export SOURCES ${XMQ_WEBHOOK_SOURCES} SPTK OPENSSL
                  UI "${CMAKE_CURRENT_LIST_DIR}/message_export.ui.json")

XMQ_ADD_EXTENSION(xmq_log_export SOURCES ${XMQ_WEBHOOK_SOURCES} SPTK OPENSSL
                  UI "${CMAKE_CURRENT_LIST_DIR}/log_export.ui.json")

TARGET_COMPILE_DEFINITIONS(xmq_message_export PRIVATE XMQ_MESSAGES_WEBHOOK)
TARGET_COMPILE_DEFINITIONS(xmq_log_export PRIVATE XMQ_LOG_WEBHOOK)

# Older builds of the same SPTK release predate the JsonSchema API.
INCLUDE(CheckCXXSourceCompiles)
INCLUDE(CMakePushCheckState)
CMAKE_PUSH_CHECK_STATE(RESET)
SET(CMAKE_REQUIRED_INCLUDES "${SPTK_INCLUDE_DIR}")
SET(CMAKE_REQUIRED_LIBRARIES "${SPTK_LIBRARY_CORE}")
UNSET(XMQ_SPTK_JSON_SCHEMA CACHE)
CHECK_CXX_SOURCE_COMPILES([=[
    #include <sptk5/xdoc/JsonSchema.h>
    int main() { return sptk::xdoc::JsonSchema::available() ? 0 : 1; }
]=] XMQ_SPTK_JSON_SCHEMA)
CMAKE_POP_CHECK_STATE()
IF (XMQ_SPTK_JSON_SCHEMA)
    TARGET_COMPILE_DEFINITIONS(xmq_server_core PRIVATE XMQ_SPTK_JSON_SCHEMA)
    IF (TARGET xmq_unit_tests)
        TARGET_COMPILE_DEFINITIONS(xmq_unit_tests PRIVATE XMQ_SPTK_JSON_SCHEMA)
    ENDIF ()
ENDIF ()

FIND_PACKAGE(SQLite3 QUIET)
IF (TARGET SQLite::SQLite3)
    FOREACH (target xmq_message_export xmq_log_export)
        TARGET_LINK_LIBRARIES(${target} PRIVATE SQLite::SQLite3)
        TARGET_COMPILE_DEFINITIONS(${target} PRIVATE XMQ_PERSISTENT_DELIVERY)
    ENDFOREACH ()
ENDIF ()

XMQ_ADD_EXTENSION(xmq_http_sink SOURCES "${CMAKE_CURRENT_LIST_DIR}/HttpSink.cpp"
                                       "${CMAKE_CURRENT_LIST_DIR}/HttpPeer.cpp" SPTK OPENSSL
                  UI "${CMAKE_CURRENT_LIST_DIR}/http_sink.ui.json")

XMQ_ADD_EXTENSION(xmq_db_sink SOURCES ${CMAKE_CURRENT_LIST_DIR}/DbSink.cpp SPTK
                  UI "${CMAKE_CURRENT_LIST_DIR}/db_sink.ui.json")

IF (TARGET PkgConfig::RDKAFKA)
    XMQ_ADD_EXTENSION(xmq_kafka_sink SOURCES ${CMAKE_CURRENT_LIST_DIR}/KafkaSink.cpp
                      UI "${CMAKE_CURRENT_LIST_DIR}/kafka_sink.ui.json")
    TARGET_LINK_LIBRARIES(xmq_kafka_sink PRIVATE PkgConfig::RDKAFKA)
    XMQ_ADD_EXTENSION(xmq_kafka_source SOURCES ${CMAKE_CURRENT_LIST_DIR}/KafkaSource.cpp)
    TARGET_LINK_LIBRARIES(xmq_kafka_source PRIVATE PkgConfig::RDKAFKA)
ENDIF ()

FOREACH (target xmq_webhook xmq_message_export xmq_log_export xmq_http_sink)
    TARGET_LINK_LIBRARIES(${target} PRIVATE OpenSSL::SSL)
    TARGET_LINK_LIBRARIES(${target} PRIVATE ${CMAKE_DL_LIBS})
    IF (WIN32)
        TARGET_LINK_LIBRARIES(${target} PRIVATE ws2_32)
    ENDIF ()
ENDFOREACH ()

SET(XMQ_EXPORT_TARGETS xmq_webhook xmq_message_export xmq_log_export xmq_http_sink xmq_db_sink)
IF (TARGET xmq_kafka_sink)
    LIST(APPEND XMQ_EXPORT_TARGETS xmq_kafka_sink xmq_kafka_source)
ENDIF ()
INSTALL(TARGETS ${XMQ_EXPORT_TARGETS}
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
