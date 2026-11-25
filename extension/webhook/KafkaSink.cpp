#include "Sink.h"
#include "extension/XmqExtension.h"

#include <librdkafka/rdkafka.h>

#include <cstdio>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct HandleDeleter { void operator()(rd_kafka_t* p) const { rd_kafka_destroy(p); } };
struct TopicDeleter { void operator()(rd_kafka_topic_t* p) const { rd_kafka_topic_destroy(p); } };
using KafkaHandle = std::unique_ptr<rd_kafka_t, HandleDeleter>;
using KafkaTopic = std::unique_ptr<rd_kafka_topic_t, TopicDeleter>;

struct KafkaDelivery
{
    std::optional<rd_kafka_resp_err_t> report;
    KafkaHandle handle;
    KafkaTopic topic;
    std::string brokers;
    std::string topicName;
    std::string protocol;
    std::string caFile;
    std::string mechanism;
    std::string username;
    std::string password;
    int timeoutMs{5000};

    void connect()
    {
        std::unique_ptr<rd_kafka_conf_t, decltype(&rd_kafka_conf_destroy)> config(
            rd_kafka_conf_new(), rd_kafka_conf_destroy);
        char detail[256]{};
        const auto set = [&](const char* name, const char* value)
        {
            if (rd_kafka_conf_set(config.get(), name, value, detail, sizeof detail) != RD_KAFKA_CONF_OK)
                throw std::runtime_error(detail);
        };
        set("bootstrap.servers", brokers.c_str());
        set("security.protocol", protocol.c_str());
        if (protocol.starts_with("SASL_"))
        {
            set("sasl.mechanism", mechanism.c_str());
            set("sasl.username", username.c_str());
            set("sasl.password", password.c_str());
        }
        if (protocol == "SSL" || protocol == "SASL_SSL")
        {
            // Explicit rather than relying on the librdkafka default: older releases leave
            // hostname verification off, while the documentation promises it is checked.
            set("ssl.endpoint.identification.algorithm", "https");
            if (!caFile.empty())
                set("ssl.ca.location", caFile.c_str());
        }
        set("acks", "all");
        set("enable.idempotence", "true");
        set("message.timeout.ms", std::to_string(timeoutMs).c_str());
        rd_kafka_conf_set_opaque(config.get(), this);
        rd_kafka_conf_set_dr_msg_cb(config.get(), [](rd_kafka_t*, const rd_kafka_message_t* message, void* context)
        {
            static_cast<KafkaDelivery*>(context)->report = message->err;
        });
        KafkaHandle producer(rd_kafka_new(RD_KAFKA_PRODUCER, config.get(), detail, sizeof detail));
        if (!producer)
            throw std::runtime_error(detail);
        (void) config.release();
        KafkaTopic destination(rd_kafka_topic_new(producer.get(), topicName.c_str(), nullptr));
        if (!destination)
            throw std::runtime_error(rd_kafka_err2str(rd_kafka_last_error()));
        handle = std::move(producer);
        topic = std::move(destination);
    }

    void disconnect()
    {
        topic.reset();
        handle.reset();
    }
};

std::string_view get(const char* const* names, const char* const* values, size_t count, const char* key)
{
    for (size_t i = 0; i < count; ++i)
        if (std::strcmp(names[i], key) == 0)
            return values[i];
    return {};
}

int resultFor(rd_kafka_resp_err_t error)
{
    switch (error)
    {
        case RD_KAFKA_RESP_ERR_NO_ERROR:
            return 0;
        case RD_KAFKA_RESP_ERR_MSG_SIZE_TOO_LARGE:
        case RD_KAFKA_RESP_ERR_INVALID_MSG_SIZE:
        case RD_KAFKA_RESP_ERR_TOPIC_EXCEPTION:
        case RD_KAFKA_RESP_ERR_TOPIC_AUTHORIZATION_FAILED:
        case RD_KAFKA_RESP_ERR__INVALID_ARG:
            return 1;
        default:
            return 2;
    }
}

constexpr xmq_setting kafkaSinkSettings[] = {
    {.name = "bootstrap_servers", .label = "Kafka brokers", .type = XMQ_SETTING_STRING, .default_value = ""},
    {.name = "topic", .label = "Kafka topic", .type = XMQ_SETTING_STRING, .default_value = ""},
    {.name = "security_protocol", .label = "Kafka transport",
     .description = "PLAINTEXT is unencrypted. SSL verifies the broker certificate and hostname using the system trust store, or the CA file when set.",
     .type = XMQ_SETTING_CHOICE, .default_value = "PLAINTEXT",
     .choices = "PLAINTEXT,SSL,SASL_PLAINTEXT,SASL_SSL"},
    {.name = "sasl_mechanism", .label = "SASL mechanism",
     .description = "SCRAM-SHA-256 or SCRAM-SHA-512. Used only with a SASL transport.",
     .type = XMQ_SETTING_CHOICE, .default_value = "SCRAM-SHA-256", .choices = "SCRAM-SHA-256,SCRAM-SHA-512"},
    {.name = "sasl_username", .label = "Kafka username", .type = XMQ_SETTING_STRING, .default_value = ""},
    {.name = "sasl_password", .label = "Kafka password", .type = XMQ_SETTING_SECRET, .default_value = ""},
    {.name = "ca_file", .label = "Kafka CA certificate",
     .description = "Absolute path to a PEM CA certificate on the broker. Empty uses the system trust store. Requires SSL transport.",
     .type = XMQ_SETTING_STRING, .default_value = ""},
    {.name = "timeout_ms", .label = "Kafka timeout, ms", .type = XMQ_SETTING_INTEGER, .default_value = "5000"}};

class KafkaSink : public xmq::XmqExtensionBase
{
public:
    using XmqExtensionBase::XmqExtensionBase;
    bool start() override { return false; }
};

} // namespace

XMQ_DEFINE_EXTENSION_IMPL(KafkaSink, "kafka_sink", "1.2", XMQ_CAP_SINK,
                          "Writes exported JSON to a Kafka topic.",
                          kafkaSinkSettings, sizeof(kafkaSinkSettings) / sizeof(kafkaSinkSettings[0]), nullptr, 0, nullptr)

extern "C" XMQ_EXTENSION_EXPORT const XmqSinkV1* xmq_sink_v1()
{
    static const XmqSinkV1 api{
        [](XmqSinkV1::Setting setting, void* context, char* error, size_t capacity) -> void*
        {
            try
            {
                auto state = std::make_unique<KafkaDelivery>();
                state->brokers = setting(context, "bootstrap_servers");
                state->topicName = setting(context, "topic");
                state->protocol = setting(context, "security_protocol");
                state->caFile = setting(context, "ca_file");
                if (state->protocol.empty()) state->protocol = "PLAINTEXT";
                if (state->protocol != "PLAINTEXT" && state->protocol != "SSL" &&
                    state->protocol != "SASL_PLAINTEXT" && state->protocol != "SASL_SSL")
                    throw std::invalid_argument("unknown Kafka transport");
                state->mechanism = setting(context, "sasl_mechanism");
                state->username = setting(context, "sasl_username");
                state->password = setting(context, "sasl_password");
                if (state->mechanism.empty()) state->mechanism = "SCRAM-SHA-256";
                if (state->mechanism != "SCRAM-SHA-256" && state->mechanism != "SCRAM-SHA-512")
                    throw std::invalid_argument("SASL mechanism must be SCRAM-SHA-256 or SCRAM-SHA-512");
                if (state->protocol.starts_with("SASL_") &&
                    (state->username.empty() || state->password.empty()))
                    throw std::invalid_argument("SASL requires a Kafka username and password");
                if (!state->protocol.starts_with("SASL_") &&
                    (!state->username.empty() || !state->password.empty()))
                    throw std::invalid_argument("SASL credentials require a SASL transport");
                if (state->protocol == "PLAINTEXT" && !state->caFile.empty())
                    throw std::invalid_argument("a CA certificate requires SSL transport");
                if (!state->caFile.empty())
                {
                    const std::filesystem::path path(state->caFile);
                    if (!path.is_absolute() || !std::filesystem::is_regular_file(path))
                        throw std::invalid_argument("Kafka CA certificate must be an existing absolute file path");
                }
                if (state->brokers.empty() || state->topicName.empty())
                    throw std::invalid_argument("Kafka brokers and topic are required");
                const std::string_view timeout = setting(context, "timeout_ms");
                if (!timeout.empty())
                {
                    const auto end = timeout.data() + timeout.size();
                    const auto parsed = std::from_chars(timeout.data(), end, state->timeoutMs);
                    if (parsed.ec != std::errc{} || parsed.ptr != end)
                        throw std::invalid_argument("timeout_ms must be an integer");
                }
                if (state->timeoutMs <= 0 || state->timeoutMs > 60000)
                    throw std::invalid_argument("timeout_ms must be 1 to 60000");
                state->connect();
                return state.release();
            }
            catch (const std::exception& e)
            {
                if (error && capacity)
                    snprintf(error, capacity, "%s", e.what());
                return nullptr;
            }
        },
        [](void* instance) { delete static_cast<KafkaDelivery*>(instance); },
        [](void* instance, const char* body, size_t length, const char* const* names,
           const char* const* values, size_t count) -> int
        {
            auto& state = *static_cast<KafkaDelivery*>(instance);
            try
            {
                if (!state.handle)
                    state.connect();
                state.report.reset();
                const auto key = get(names, values, count, "key");
                if (rd_kafka_produce(state.topic.get(), RD_KAFKA_PARTITION_UA, RD_KAFKA_MSG_F_COPY,
                        const_cast<char*>(body), length, key.empty() ? nullptr : key.data(), key.size(), nullptr) != 0)
                    return resultFor(rd_kafka_last_error());
                rd_kafka_flush(state.handle.get(), state.timeoutMs);
                // One in-flight record per worker. Discard the producer on timeout so
                // a late report can never acknowledge the next call's record.
                if (!state.report)
                {
                    state.disconnect();
                    return 2;
                }
                const auto result = resultFor(*state.report);
                if (result == 2)
                    state.disconnect();
                return result;
            }
            catch (...)
            {
                state.disconnect();
                return 2;
            }
        }
    };
    return &api;
}
