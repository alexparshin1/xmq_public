#ifdef XMQ_TEST_KAFKA_SINK

#include "server/Extensions/ExtensionHost.h"

#include <gtest/gtest.h>
#include <librdkafka/rdkafka.h>
#include <librdkafka/rdkafka_mock.h>
#include <sptk5/FileLogEngine.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef XMQ_TEST_PERSISTENT_DELIVERY_QUEUE
#include <sqlite3.h>
#endif
using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

struct ConsumerDeleter { void operator()(rd_kafka_t* p) const { rd_kafka_destroy(p); } };
using Consumer = std::unique_ptr<rd_kafka_t, ConsumerDeleter>;

} // namespace

TEST(KafkaSinkTests, exportsMqttMessageToKafkaTopic)
{
    const auto brokers = string(XMQ_TEST_KAFKA_BROKERS);
    const string topic = "xmq-sink-test-" +
        to_string(chrono::steady_clock::now().time_since_epoch().count());
    constexpr int expected = 3;

    rd_kafka_conf_t* config = rd_kafka_conf_new();
    char detail[256];
    ASSERT_EQ(RD_KAFKA_CONF_OK, rd_kafka_conf_set(config, "bootstrap.servers", brokers.c_str(),
                                                  detail, sizeof(detail))) << detail;
    ASSERT_EQ(RD_KAFKA_CONF_OK, rd_kafka_conf_set(config, "auto.offset.reset", "earliest",
                                                  detail, sizeof(detail))) << detail;
    const string group = "xmq-sink-test-" + to_string(chrono::steady_clock::now().time_since_epoch().count());
    ASSERT_EQ(RD_KAFKA_CONF_OK, rd_kafka_conf_set(config, "group.id", group.c_str(),
                                                  detail, sizeof(detail))) << detail;
    rd_kafka_t* raw = rd_kafka_new(RD_KAFKA_CONSUMER, config, detail, sizeof(detail));
    ASSERT_NE(nullptr, raw) << detail;
    Consumer consumer(raw);
    rd_kafka_topic_partition_list_t* topics = rd_kafka_topic_partition_list_new(1);
    rd_kafka_topic_partition_list_add(topics, topic.c_str(), 0);
    rd_kafka_topic_partition_list_set_offset(topics, topic.c_str(), 0, RD_KAFKA_OFFSET_BEGINNING);
    
    ASSERT_EQ(RD_KAFKA_RESP_ERR_NO_ERROR, rd_kafka_assign(raw, topics));
    rd_kafka_topic_partition_list_destroy(topics);

    FileLogEngine logEngine("kafka-sink-test.log");
    const auto library = string(XMQ_TEST_MESSAGES_WEBHOOK_LIBRARY);
    ExtensionHost::Configured extension{
        .m_name = "message_export",
        .m_library = library,
        .m_settings = {{"sink", "kafka_sink"},
                       {"bootstrap_servers", brokers},
                       {"kafka_topic", topic},
                       {"mqtt_filter", "SQL rules"},
                       {"sql_rule", R"(SELECT topic, client_id AS client FROM "sensors/#")"}}};
    ExtensionHost host(logEngine, "kafka-sink-test");
    host.start({extension});
    ASSERT_TRUE(host.tapping());

    for (int i = 0; i < expected; ++i)
        host.publishMessage("client", "user", "sensors/1", "payload", 7, 0, false);

    vector<string> bodies;
    const auto deadline = chrono::steady_clock::now() + chrono::seconds(15);
    while (static_cast<int>(bodies.size()) < expected && chrono::steady_clock::now() < deadline)
    {
        rd_kafka_message_t* message = rd_kafka_consumer_poll(raw, 250);
        if (!message)
        {
            continue;
        }
        if (message->err == RD_KAFKA_RESP_ERR_NO_ERROR)
            bodies.emplace_back(static_cast<const char*>(message->payload), message->len);
        rd_kafka_message_destroy(message);
    }
    host.stop();
    rd_kafka_consumer_close(raw);
    ASSERT_EQ(expected, static_cast<int>(bodies.size()));
    for (const auto& body: bodies)
        EXPECT_NE(string::npos, body.find(R"("topic":"sensors/1")"));
}

TEST(KafkaSinkTests, rejectsSaslCredentialsWithoutSaslTransport)
{
    const ExtensionHost::Configured config{
        .m_name = "message_export", .m_library = XMQ_TEST_MESSAGES_WEBHOOK_LIBRARY,
        .m_settings = {{"sink", "kafka_sink"}, {"bootstrap_servers", "127.0.0.1:9092"},
                       {"kafka_topic", "xmq-demo"}, {"kafka_sasl_username", "user"},
                       {"kafka_sasl_password", "secret"}}};
    FileLogEngine log("kafka-sink-sasl-test.log");
    ExtensionHost host(log, "kafka-sink-sasl-test", [](auto, auto, auto) { return true; });
    host.start({config});
    EXPECT_FALSE(host.tapping());
    host.stop();
}

TEST(KafkaSinkTests, rejectsCaFileWithoutSsl)
{
    const ExtensionHost::Configured config{
        .m_name = "message_export", .m_library = XMQ_TEST_MESSAGES_WEBHOOK_LIBRARY,
        .m_settings = {{"sink", "kafka_sink"}, {"bootstrap_servers", "127.0.0.1:9092"},
                       {"kafka_topic", "xmq-demo"}, {"kafka_ca_file", "/nonexistent/ca.pem"}}};
    FileLogEngine log("kafka-sink-ca-test.log");
    ExtensionHost host(log, "kafka-sink-ca-test", [](auto, auto, auto) { return true; });
    host.start({config});
    EXPECT_FALSE(host.tapping());
    host.stop();
}

#ifdef XMQ_TEST_PERSISTENT_DELIVERY_QUEUE
TEST(KafkaSinkTests, keepsPersistentRecordUntilKafkaAcknowledgesAfterOutage)
{
    using namespace std::chrono_literals;
    char error[256]{};
    const Consumer owner(rd_kafka_new(RD_KAFKA_PRODUCER, nullptr, error, sizeof error));
    ASSERT_TRUE(owner) << error;
    const unique_ptr<rd_kafka_mock_cluster_t, decltype(&rd_kafka_mock_cluster_destroy)> cluster(
        rd_kafka_mock_cluster_new(owner.get(), 1), rd_kafka_mock_cluster_destroy);
    ASSERT_TRUE(cluster);
    const string brokers = rd_kafka_mock_cluster_bootstraps(cluster.get());
    ASSERT_EQ(RD_KAFKA_RESP_ERR_NO_ERROR, rd_kafka_mock_topic_create(cluster.get(), "outage", 1, 1));
    ASSERT_EQ(RD_KAFKA_RESP_ERR_NO_ERROR, rd_kafka_mock_broker_set_down(cluster.get(), 1));

    auto directory = (filesystem::temp_directory_path() / "xmq-kafka-outage-XXXXXX").string();
    ASSERT_NE(nullptr, mkdtemp(directory.data()));
    auto cleanup = [](const string* path) { filesystem::remove_all(*path); };
    const unique_ptr<const string, decltype(cleanup)> guard(&directory, cleanup);
    const auto path = (filesystem::path(directory) / "queue.sqlite").string();
    auto pending = [&]()
    {
        sqlite3* raw{};
        const auto status = sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
        const unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
        sqlite3_stmt* statement{};
        if (status != SQLITE_OK || sqlite3_prepare_v2(raw, "SELECT count(*) FROM records", -1,
                                                      &statement, nullptr) != SQLITE_OK)
            return -1;
        const unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> query(statement, sqlite3_finalize);
        return sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int(statement, 0) : -1;
    };
    const auto waitFor = [](auto condition)
    {
        const auto deadline = chrono::steady_clock::now() + 10s;
        while (!condition() && chrono::steady_clock::now() < deadline)
            this_thread::sleep_for(20ms);
        return condition();
    };
    FileLogEngine log("kafka-sink-outage-test.log");
    const ExtensionHost::Configured config{
        .m_name = "message_export",
        .m_library = XMQ_TEST_MESSAGES_WEBHOOK_LIBRARY,
        .m_settings = {{"sink", "kafka_sink"}, {"bootstrap_servers", brokers}, {"kafka_topic", "outage"},
                       {"kafka_timeout_ms", "3000"}, {"persistent_delivery", "true"},
                       {"delivery_queue_path", path}, {"mqtt_filter", "SQL rules"},
                       {"sql_rule", R"(SELECT topic FROM "sensors/#")"}}};
    {
        ExtensionHost host(log, "kafka-outage-test");
        host.start({config});
        ASSERT_TRUE(host.tapping());
        host.publishMessage("client", "user", "sensors/1", "", 0, 0, false);
        ASSERT_TRUE(waitFor([&] { return pending() == 1; }));
        // Let one produce attempt through its full timeout with the broker down.
        this_thread::sleep_for(3300ms);
        host.stop();
        ASSERT_EQ(1, pending());
    }
    ASSERT_EQ(RD_KAFKA_RESP_ERR_NO_ERROR, rd_kafka_mock_broker_set_up(cluster.get(), 1));
    {
        ExtensionHost host(log, "kafka-outage-test");
        host.start({config});
        ASSERT_TRUE(host.tapping());
        ASSERT_TRUE(waitFor([&] { return pending() == 0; }));
    }

    auto* options = rd_kafka_conf_new();
    ASSERT_EQ(RD_KAFKA_CONF_OK, rd_kafka_conf_set(options, "bootstrap.servers", brokers.c_str(),
                                                 error, sizeof error));
    const Consumer reader(rd_kafka_new(RD_KAFKA_CONSUMER, options, error, sizeof error));
    ASSERT_TRUE(reader) << error;
    const unique_ptr<rd_kafka_topic_t, decltype(&rd_kafka_topic_destroy)> topic(
        rd_kafka_topic_new(reader.get(), "outage", nullptr), rd_kafka_topic_destroy);
    ASSERT_TRUE(topic);
    ASSERT_EQ(0, rd_kafka_consume_start(topic.get(), 0, RD_KAFKA_OFFSET_BEGINNING));
    const unique_ptr<rd_kafka_message_t, decltype(&rd_kafka_message_destroy)> message(
        rd_kafka_consume(topic.get(), 0, 5000), rd_kafka_message_destroy);
    rd_kafka_consume_stop(topic.get(), 0);
    ASSERT_TRUE(message);
    ASSERT_EQ(RD_KAFKA_RESP_ERR_NO_ERROR, message->err);
    EXPECT_EQ(R"({"topic":"sensors/1"})",
              string_view(static_cast<const char*>(message->payload), message->len));
}
#endif

#endif
