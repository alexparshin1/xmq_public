#include "Sink.h"
#include "extension/XmqExtension.h"

#include <librdkafka/rdkafka.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

struct HandleDeleter { void operator()(rd_kafka_t* p) const { rd_kafka_destroy(p); } };
struct TopicDeleter { void operator()(rd_kafka_topic_t* p) const { rd_kafka_topic_destroy(p); } };
using KafkaHandle = std::unique_ptr<rd_kafka_t, HandleDeleter>;
using KafkaTopic = std::unique_ptr<rd_kafka_topic_t, TopicDeleter>;

struct KafkaDelivery
{
    KafkaHandle handle;
    KafkaTopic topic;
    std::string topicName;
    int timeoutMs;
};

constexpr const char* settings[] = {
    "bootstrap_servers", "topic", "timeout_ms"
};

std::string get(const char* const* names, const char* const* values, size_t count, const char* key)
{
    for (size_t i = 0; i < count; ++i)
        if (std::strcmp(names[i], key) == 0)
            return values[i];
    return {};
}

constexpr xmq_setting kafkaSinkSettings[] = {
    {.name = "bootstrap_servers", .label = "Kafka brokers", .type = XMQ_SETTING_STRING, .default_value = ""},
    {.name = "topic", .label = "Kafka topic", .type = XMQ_SETTING_STRING, .default_value = ""},
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
                          kafkaSinkSettings, sizeof(kafkaSinkSettings) / sizeof(kafkaSinkSettings[0]), nullptr, 0)

extern "C" XMQ_EXTENSION_EXPORT const XmqSinkV1* xmq_sink_v1()
{
    static const XmqSinkV1 api{
        [](XmqSinkV1::Setting setting, void* context, char* error, size_t capacity) -> void*
        {
            try
            {
                auto state = std::make_unique<KafkaDelivery>();
                rd_kafka_conf_t* config = rd_kafka_conf_new();
                char detail[256];
                const char* brokers = setting(context, "bootstrap_servers");
                if (rd_kafka_conf_set(config, "bootstrap.servers", brokers, detail, sizeof(detail)) != RD_KAFKA_CONF_OK)
                {
                    rd_kafka_conf_destroy(config);
                    std::snprintf(error, capacity, "%s", detail);
                    return nullptr;
                }
                char errstr[256];
                rd_kafka_t* raw = rd_kafka_new(RD_KAFKA_PRODUCER, config, errstr, sizeof(errstr));
                if (!raw)
                {
                    rd_kafka_conf_destroy(config);
                    std::snprintf(error, capacity, "%s", errstr);
                    return nullptr;
                }
                state->handle.reset(raw);
                state->topicName = setting(context, "topic");
                rd_kafka_topic_conf_t* topicConfig = rd_kafka_topic_conf_new();
                state->topic.reset(rd_kafka_topic_new(raw, state->topicName.c_str(), topicConfig));
                const char* timeout = setting(context, "timeout_ms");
                state->timeoutMs = timeout && *timeout ? std::atoi(timeout) : 5000;
                if (state->timeoutMs <= 0 || state->timeoutMs > 60000)
                    throw std::invalid_argument("timeout_ms must be 1 to 60000");
                return state.release();
            }
            catch (const std::exception& e)
            {
                if (error && capacity)
                    std::snprintf(error, capacity, "%s", e.what());
                return nullptr;
            }
        },
        [](void* instance) { delete static_cast<KafkaDelivery*>(instance); },
        [](void* instance, const char* body, size_t length, const char* const* names,
           const char* const* values, size_t count) -> int
        {
            auto& state = *static_cast<KafkaDelivery*>(instance);
            const auto key = get(names, values, count, "key");
            const int result = rd_kafka_produce(state.topic.get(), RD_KAFKA_PARTITION_UA, RD_KAFKA_MSG_F_COPY,
                const_cast<char*>(body), length, key.empty() ? nullptr : key.data(), key.size(), nullptr);
            if (result != 0)
                return rd_kafka_last_error() == RD_KAFKA_RESP_ERR__QUEUE_FULL ? 2 : 1;
            rd_kafka_poll(state.handle.get(), 0);
            rd_kafka_flush(state.handle.get(), state.timeoutMs);
            return 0;
        }
    };
    return &api;
}
