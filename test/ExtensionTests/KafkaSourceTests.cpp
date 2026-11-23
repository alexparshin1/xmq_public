#ifdef XMQ_TEST_KAFKA_SOURCE_LIBRARY

#include "server/Extensions/ExtensionHost.h"

#include <gtest/gtest.h>
#include <librdkafka/rdkafka.h>
#include <sptk5/FileLogEngine.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace {
using namespace std::chrono_literals;

xmq::ExtensionHost::Configured source(const char* brokers, const char* topic, const std::string& group)
{
    return {.m_name = "kafka_source", .m_library = XMQ_TEST_KAFKA_SOURCE_LIBRARY,
            .m_settings = {{"bootstrap_servers", brokers}, {"kafka_topic", topic},
                           {"group_id", group}, {"mqtt_topic", "test/from-kafka"},
                           {"security_protocol", "PLAINTEXT"}}};
}

TEST(KafkaSourceTests, refusesWildcardDestination)
{
    auto configuration = source("localhost:9092", "test", "test");
    configuration.m_settings["mqtt_topic"] = "test/#";
    configuration.m_required = true;
    sptk::FileLogEngine log("kafka-source-test.log");
    xmq::ExtensionHost host(log, "test", [](auto, auto, auto) { return true; });
    EXPECT_ANY_THROW(host.start({configuration}));
}

TEST(KafkaSourceTests, importsBinaryValueAndRetriesRefusedPublish)
{
    const auto* brokers = std::getenv("XMQ_TEST_KAFKA_SOURCE_BROKERS");
    const auto* topic = std::getenv("XMQ_TEST_KAFKA_SOURCE_TOPIC");
    if (!brokers || !*brokers || !topic || !*topic)
        GTEST_SKIP() << "Set XMQ_TEST_KAFKA_SOURCE_BROKERS and XMQ_TEST_KAFKA_SOURCE_TOPIC to a dedicated test topic";

    const auto id = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto payload = id + std::string("\0\xff", 2);
    char detail[256]{};
    auto* options = rd_kafka_conf_new();
    ASSERT_EQ(RD_KAFKA_CONF_OK, rd_kafka_conf_set(options, "bootstrap.servers", brokers, detail, sizeof detail));
    auto* producer = rd_kafka_new(RD_KAFKA_PRODUCER, options, detail, sizeof detail);
    if (!producer)
        rd_kafka_conf_destroy(options);
    ASSERT_NE(nullptr, producer) << detail;
    const std::unique_ptr<rd_kafka_t, decltype(&rd_kafka_destroy)> handle(producer, rd_kafka_destroy);
    const std::unique_ptr<rd_kafka_topic_t, decltype(&rd_kafka_topic_destroy)> target(
        rd_kafka_topic_new(producer, topic, nullptr), rd_kafka_topic_destroy);
    ASSERT_NE(nullptr, target);
    ASSERT_EQ(0, rd_kafka_produce(target.get(), 0, RD_KAFKA_MSG_F_COPY,
                                  payload.data(), payload.size(), nullptr, 0, nullptr));
    ASSERT_EQ(RD_KAFKA_RESP_ERR_NO_ERROR, rd_kafka_flush(producer, 5000));

    std::atomic<unsigned> attempts{0};
    std::atomic<bool> accepted{false};
    sptk::FileLogEngine log("kafka-source-test.log");
    xmq::ExtensionHost host(log, "test", [&](std::string_view from, std::string_view mqttTopic, std::string_view body)
    {
        if (body != payload)
            return true;
        if (from != "kafka_source" || mqttTopic != "test/from-kafka" || attempts.fetch_add(1) == 0)
            return false;
        accepted = true;
        return true;
    });
    auto configuration = source(brokers, topic, "xmq-source-test-" + id);
    configuration.m_required = true;
    host.start({configuration});
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (!accepted && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(20ms);
    host.stop();
    EXPECT_TRUE(accepted);
    EXPECT_EQ(2u, attempts.load());
}

} // namespace
#endif
