#include "extension/XmqExtension.h"

#include <librdkafka/rdkafka.h>

#include <charconv>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

constexpr xmq_setting settings[] = {
    {.name = "bootstrap_servers", .label = "Kafka brokers",
     .description = "Comma-separated Kafka host:port addresses.", .type = XMQ_SETTING_STRING, .required = 1},
    {.name = "kafka_topic", .label = "Kafka topic",
     .description = "Read records from this existing Kafka topic.", .type = XMQ_SETTING_STRING, .required = 1,
     .pattern = "[a-zA-Z0-9._-]{1,249}"},
    {.name = "group_id", .label = "Consumer group",
     .description = "Members of the same group share partitions and committed offsets.",
     .type = XMQ_SETTING_STRING, .required = 1},
    {.name = "mqtt_topic", .label = "MQTT topic",
     .description = "Local destination topic, without wildcards or a leading $. The record value becomes the payload.",
     .type = XMQ_SETTING_STRING, .required = 1, .pattern = "[^$+#][^+#]*"},
    {.name = "security_protocol", .label = "Kafka transport",
     .description = "Use TLS with system certificate verification, or unencrypted PLAINTEXT for trusted test networks.",
     .type = XMQ_SETTING_CHOICE, .default_value = "SSL",
     .choices = "SSL,PLAINTEXT,SASL_SSL,SASL_PLAINTEXT"},
    {.name = "sasl_mechanism", .label = "SASL mechanism",
     .description = "SCRAM-SHA-256 or SCRAM-SHA-512. Used only with a SASL transport.",
     .type = XMQ_SETTING_CHOICE, .default_value = "SCRAM-SHA-256", .choices = "SCRAM-SHA-256,SCRAM-SHA-512"},
    {.name = "sasl_username", .label = "Kafka username", .type = XMQ_SETTING_STRING, .default_value = ""},
    {.name = "sasl_password", .label = "Kafka password", .type = XMQ_SETTING_SECRET, .default_value = ""},
    {.name = "session_timeout_ms", .label = "Consumer session timeout, ms",
     .description = "How long Kafka waits for this consumer before rejoining the group. Shorter values make Apply and Disable faster.",
     .type = XMQ_SETTING_INTEGER, .default_value = "10000"},
};

class KafkaSource final : public xmq::XmqExtensionBase
{
    using Consumer = std::unique_ptr<rd_kafka_t, decltype(&rd_kafka_destroy)>;

    struct Config
    {
        std::string brokers, kafkaTopic, group, mqttTopic, protocol;
        std::string mechanism, username, password;
        int sessionTimeoutMs{10000};
    };

public:
    using XmqExtensionBase::XmqExtensionBase;
    ~KafkaSource() override { stop(); }

    bool start() override { return configure(); }
    bool reload() override { return configure(); }

    bool stop() override
    {
        if (m_worker.joinable())
        {
            m_worker.request_stop();
            m_worker.join();
        }
        return true;
    }

private:
    bool configure()
    {
        try
        {
            if (!canPublishLocal())
                throw std::runtime_error("this broker does not support local source publishing");
            Config config{setting("bootstrap_servers"), setting("kafka_topic"), setting("group_id"),
                          setting("mqtt_topic"), setting("security_protocol", "SSL")};
            config.mechanism = setting("sasl_mechanism", "SCRAM-SHA-256");
            config.username = setting("sasl_username");
            config.password = setting("sasl_password");
            const auto timeout = setting("session_timeout_ms", "10000");
            const auto parsed = std::from_chars(timeout.data(), timeout.data() + timeout.size(),
                                                config.sessionTimeoutMs);
            if (parsed.ec != std::errc{} || parsed.ptr != timeout.data() + timeout.size() ||
                config.sessionTimeoutMs < 6000 || config.sessionTimeoutMs > 60000)
                throw std::runtime_error("session_timeout_ms must be an integer from 6000 to 60000");
            if (config.brokers.empty() || config.kafkaTopic.empty() || config.group.empty() || config.mqttTopic.empty())
                throw std::runtime_error("Kafka brokers, Kafka topic, consumer group and MQTT topic are required");
            if (config.kafkaTopic.size() > 249 || config.kafkaTopic == "." || config.kafkaTopic == ".." ||
                config.kafkaTopic.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")
                    != std::string::npos)
                throw std::runtime_error("invalid Kafka topic name");
            if (config.mqttTopic.size() > 65535 || config.mqttTopic.starts_with('$') ||
                config.mqttTopic.find_first_of("+#") != std::string::npos ||
                std::ranges::any_of(config.mqttTopic, [](unsigned char c) { return c < 0x20 || c == 0x7f; }))
                throw std::runtime_error("invalid MQTT destination topic");
            if (config.protocol != "SSL" && config.protocol != "PLAINTEXT" &&
                config.protocol != "SASL_SSL" && config.protocol != "SASL_PLAINTEXT")
                throw std::runtime_error("unknown Kafka transport");
            if (config.mechanism != "SCRAM-SHA-256" && config.mechanism != "SCRAM-SHA-512")
                throw std::runtime_error("SASL mechanism must be SCRAM-SHA-256 or SCRAM-SHA-512");
            if (config.protocol.starts_with("SASL_") &&
                (config.username.empty() || config.password.empty()))
                throw std::runtime_error("SASL requires a Kafka username and password");
            if (!config.protocol.starts_with("SASL_") &&
                (!config.username.empty() || !config.password.empty()))
                throw std::runtime_error("SASL credentials require a SASL transport");
            auto consumer = open(config); // Validate before stopping the current worker on Apply.
            stop();
            m_worker = std::jthread([this, config = std::move(config), consumer = std::move(consumer)]
                                   (std::stop_token stopping) mutable
            {
                run(stopping, config, std::move(consumer));
            });
            return true;
        }
        catch (const std::exception& error)
        {
            log(XMQ_LOG_ERROR, error.what());
            return false;
        }
    }

    static Consumer open(const Config& config)
    {
        std::unique_ptr<rd_kafka_conf_t, decltype(&rd_kafka_conf_destroy)> options(
            rd_kafka_conf_new(), rd_kafka_conf_destroy);
        char detail[256]{};
        const auto set = [&](const char* key, const char* value)
        {
            if (rd_kafka_conf_set(options.get(), key, value, detail, sizeof detail) != RD_KAFKA_CONF_OK)
                throw std::runtime_error(detail);
        };
        set("bootstrap.servers", config.brokers.c_str());
        set("group.id", config.group.c_str());
        set("auto.offset.reset", "earliest");
        set("security.protocol", config.protocol.c_str());
        // Explicit rather than relying on the librdkafka default: older releases leave hostname
        // verification off, while the documentation promises the broker name is checked.
        if (config.protocol == "SSL" || config.protocol == "SASL_SSL")
            set("ssl.endpoint.identification.algorithm", "https");
        // Bounds rd_kafka_consumer_close() in stop(): without it librdkafka waits up to the default
        // 45 s session timeout for the group rebalance.
        set("session.timeout.ms", std::to_string(config.sessionTimeoutMs).c_str());
        if (config.protocol.starts_with("SASL_"))
        {
            set("sasl.mechanism", config.mechanism.c_str());
            set("sasl.username", config.username.c_str());
            set("sasl.password", config.password.c_str());
        }
        set("allow.auto.create.topics", "false");
        set("enable.auto.commit", "false");
        set("enable.auto.offset.store", "false");
        set("socket.timeout.ms", "5000");
        Consumer consumer(rd_kafka_new(RD_KAFKA_CONSUMER, options.get(), detail, sizeof detail), rd_kafka_destroy);
        if (!consumer)
            throw std::runtime_error(detail);
        (void) options.release();
        rd_kafka_poll_set_consumer(consumer.get());
        std::unique_ptr<rd_kafka_topic_partition_list_t, decltype(&rd_kafka_topic_partition_list_destroy)>
            topics(rd_kafka_topic_partition_list_new(1), rd_kafka_topic_partition_list_destroy);
        rd_kafka_topic_partition_list_add(topics.get(), config.kafkaTopic.c_str(), RD_KAFKA_PARTITION_UA);
        if (const auto error = rd_kafka_subscribe(consumer.get(), topics.get()); error != RD_KAFKA_RESP_ERR_NO_ERROR)
            throw std::runtime_error(rd_kafka_err2str(error));
        return consumer;
    }

    void run(std::stop_token stopping, const Config& config, Consumer consumer)
    {
        while (!stopping.stop_requested())
        {
            try
            {
                if (!consumer)
                    consumer = open(config);
                std::unique_ptr<rd_kafka_message_t, decltype(&rd_kafka_message_destroy)>
                    message(rd_kafka_consumer_poll(consumer.get(), 200), rd_kafka_message_destroy);
                if (!message || stopping.stop_requested())
                    continue;
                if (message->err)
                {
                    if (message->err != RD_KAFKA_RESP_ERR__PARTITION_EOF)
                        throw std::runtime_error(rd_kafka_message_errstr(message.get()));
                    continue;
                }
                const auto payload = message->payload
                    ? std::string_view(static_cast<const char*>(message->payload), message->len) : std::string_view{};
                if (!publishLocal(config.mqttTopic, payload))
                    throw std::runtime_error("MQTT publish refused; Kafka offset not advanced");
                if (const auto error = rd_kafka_commit_message(consumer.get(), message.get(), 0);
                    error != RD_KAFKA_RESP_ERR_NO_ERROR)
                    throw std::runtime_error(std::string("Kafka offset commit failed: ") + rd_kafka_err2str(error));
            }
            catch (const std::exception& error)
            {
                log(XMQ_LOG_WARNING, error.what());
                // Rejoin from committed offsets; never commit past a refused record.
                if (consumer)
                    rd_kafka_consumer_close(consumer.get());
                consumer.reset();
                for (int i = 0; i < 10 && !stopping.stop_requested(); ++i)
                    std::this_thread::sleep_for(100ms);
            }
        }
        if (consumer)
            rd_kafka_consumer_close(consumer.get());
    }

    std::jthread m_worker;
};

} // namespace

XMQ_DEFINE_EXTENSION_DESCRIBED(KafkaSource, "kafka_source", "1.2", XMQ_CAP_SOURCE,
                               "Imports Kafka records into a local MQTT topic.", settings)
