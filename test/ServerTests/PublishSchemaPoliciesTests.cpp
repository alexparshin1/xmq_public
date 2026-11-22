#include "server/PublishSchemaPolicies.h"

#include <gtest/gtest.h>
#ifdef XMQ_SPTK_JSON_SCHEMA
#include <sptk5/xdoc/JsonSchema.h>
#endif

#include <chrono>
#include <fstream>

namespace xmq {
namespace {

class PublishSchemaPoliciesTests : public testing::Test
{
protected:
    void SetUp() override
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_directory = std::filesystem::temp_directory_path() / ("xmq-publish-schema-" + std::to_string(stamp));
        ASSERT_TRUE(std::filesystem::create_directory(m_directory));
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(m_directory, error);
    }

    void write(const char* file, std::string_view text) const
    {
        std::ofstream output(m_directory / file);
        output << text;
        output.close();
        ASSERT_TRUE(output.good());
    }

    auto load() const { return PublishSchemaPolicies::load(m_directory / "xmq_server.conf"); }

    std::filesystem::path m_directory;
};

TEST_F(PublishSchemaPoliciesTests, DisablesValidationForMissingAndEmptyPolicies)
{
    EXPECT_EQ(nullptr, load());
    write("xmq_publish_schemas.json", "{}");
    EXPECT_EQ(nullptr, load());
}

TEST_F(PublishSchemaPoliciesTests, InvalidConfigurationFailsClosed)
{
    write("xmq_publish_schemas.json", R"({"sensors/#/reading":"reading.json"})");
    EXPECT_THROW((void)load(), std::runtime_error);
    write("xmq_publish_schemas.json", R"({"sensors/+":"missing.json"})");
    EXPECT_THROW((void)load(), std::runtime_error);
}

#ifdef XMQ_SPTK_JSON_SCHEMA
TEST_F(PublishSchemaPoliciesTests, EnforcesAllMatchingSchemasAndSkipsUnmatchedPayloads)
{
    if (!sptk::xdoc::JsonSchema::available())
        GTEST_SKIP() << "SPTK schema validation is disabled";

    write("xmq_publish_schemas.json", R"({
        "sensors/+": "numeric.json",
        "sensors/temperature/#": "nonnegative.json"
    })");
    write("numeric.json", R"({"type":"number"})");
    write("nonnegative.json", R"({"minimum":0})");
    const auto policies = load();
    ASSERT_NE(nullptr, policies);
    const std::string_view temperature[]{"sensors", "temperature"};
    const std::string_view pressure[]{"sensors", "pressure"};
    const std::string_view unrelated[]{"status", "health"};
    EXPECT_TRUE(policies->accepts(temperature, "21.5"));
    EXPECT_FALSE(policies->accepts(temperature, "-1"));
    EXPECT_FALSE(policies->accepts(temperature, "\"21.5\""));
    EXPECT_TRUE(policies->accepts(pressure, "-1"));
    EXPECT_TRUE(policies->accepts(unrelated, "plain text"));
}
#endif

} // namespace
} // namespace xmq
