#ifdef XMQ_TEST_PERSISTENT_DELIVERY_QUEUE

#include "extension/webhook/DeliveryQueue.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using xmq::exporter::DeliveryQueue;

class DeliveryQueueTests: public testing::Test
{
protected:
    void SetUp() override
    {
        auto pattern = (std::filesystem::temp_directory_path() / "xmq-delivery-XXXXXX").string();
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(nullptr, created);
        directory = created;
        path = (directory / "queue.sqlite").string();
    }

    void TearDown() override
    {
        if (!directory.empty())
            std::filesystem::remove_all(directory);
    }

    std::filesystem::path directory;
    std::string path;
};

TEST_F(DeliveryQueueTests, KeepsOldestRecordsWhenAtLimit)
{
    DeliveryQueue queue(path, "sink-A");
    ASSERT_TRUE(queue.append("abc", 6));
    ASSERT_TRUE(queue.append("def", 6));
    EXPECT_FALSE(queue.append("g", 6));
    EXPECT_FALSE(queue.acknowledge(queue.front()->id + 1));
    ASSERT_TRUE(queue.acknowledge(queue.front()->id));
    EXPECT_TRUE(queue.append("ghi", 6));
    EXPECT_EQ("def", queue.front()->data);
}

TEST_F(DeliveryQueueTests, DoesNotReplayToAnotherDestination)
{
    {
        DeliveryQueue queue(path, "sink-A");
        ASSERT_TRUE(queue.append("body", 100));
    }
    EXPECT_THROW((DeliveryQueue(path, "sink-B")), std::runtime_error);
    DeliveryQueue queue(path, "sink-A");
    ASSERT_TRUE(queue.front());
    EXPECT_EQ("body", queue.front()->data);
}

TEST_F(DeliveryQueueTests, RejectsConcurrentOwnership)
{
    DeliveryQueue queue(path, "sink-A");
    EXPECT_THROW((DeliveryQueue(path, "sink-A")), std::runtime_error);
    EXPECT_TRUE(queue.append("body", 100));
}

TEST_F(DeliveryQueueTests, ReopensCommittedWalAfterWriterExitsWithoutCleanup)
{
    const std::string contents("body\0bindings", 13);
    const auto child = fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        try
        {
            DeliveryQueue queue(path, "sink-A");
            _exit(queue.append(contents, 100) ? 0 : 1);
        }
        catch (...)
        {
            _exit(2);
        }
    }
    int status{};
    ASSERT_EQ(child, waitpid(child, &status, 0));
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(0, WEXITSTATUS(status));
    {
        DeliveryQueue queue(path, "sink-A");
        const auto record = queue.front();
        ASSERT_TRUE(record);
        EXPECT_EQ(contents, record->data);
        ASSERT_TRUE(queue.acknowledge(record->id));
    }
    DeliveryQueue queue(path, "sink-A");
    EXPECT_FALSE(queue.front());
}

} // namespace

#endif
