#include "PublishSchemaPolicies.h"

#include <sptk5/Buffer.h>
#include <sptk5/xdoc/Document.h>
#ifdef XMQ_SPTK_JSON_SCHEMA
#include <sptk5/xdoc/JsonSchema.h>
#endif

#include <stdexcept>
#include <string>
#include <vector>

namespace xmq {
namespace {

#ifdef XMQ_SPTK_JSON_SCHEMA
std::vector<std::string> compileFilter(std::string_view filter)
{
    if (filter.empty() || filter.size() > 65535 || filter.find('\0') != filter.npos ||
        filter.starts_with("$share/"))
        throw std::runtime_error("expected an MQTT topic filter");

    std::vector<std::string> levels;
    for (;;)
    {
        const auto slash = filter.find('/');
        const auto level = filter.substr(0, slash);
        if (level.find_first_of("+#") != level.npos &&
            level != "+" && !(level == "#" && slash == filter.npos))
            throw std::runtime_error("invalid MQTT wildcard placement");
        levels.emplace_back(level);
        if (slash == filter.npos)
            return levels;
        filter.remove_prefix(slash + 1);
    }
}

bool matches(const std::vector<std::string>& filter, std::span<const std::string_view> topic)
{
    if (!topic.empty() && topic.front().starts_with('$') &&
        (filter.front() == "+" || filter.front() == "#"))
        return false;

    for (size_t i = 0; i < filter.size(); ++i)
    {
        const auto& level = filter[i];
        if (level == "#")
            return true;
        if (i >= topic.size() || (level != "+" && level != topic[i]))
            return false;
    }
    return filter.size() == topic.size();
}
#endif

std::string_view bytes(const sptk::Buffer& buffer)
{
    return {reinterpret_cast<const char*>(buffer.data()), buffer.size()};
}

} // namespace

struct PublishSchemaPolicies::Rules
{
#ifdef XMQ_SPTK_JSON_SCHEMA
    struct Rule
    {
        std::vector<std::string> filter;
        sptk::xdoc::JsonSchema schema;
    };
    std::vector<Rule> policies;
#endif
};

PublishSchemaPolicies::PublishSchemaPolicies(std::unique_ptr<const Rules> rules)
    : m_rules(std::move(rules))
{
}

PublishSchemaPolicies::~PublishSchemaPolicies() = default;

std::unique_ptr<const PublishSchemaPolicies> PublishSchemaPolicies::load(const std::filesystem::path& brokerConfig)
{
    if (brokerConfig.empty())
        return nullptr;

    const auto config = brokerConfig.parent_path() / "xmq_publish_schemas.json";
    if (!std::filesystem::exists(config))
        return nullptr;

    try
    {
        sptk::Buffer settings;
        settings.loadFromFile(config);
        const auto text = bytes(settings);
        const auto first = text.find_first_not_of(" \r\n\t");
        if (first == text.npos || text[first] != '{')
            throw std::runtime_error("expected a JSON object mapping topic filters to schema files");

        sptk::xdoc::Document document;
        document.load(settings);
        const auto& entries = document.root()->nodes();
        if (entries.empty())
            return nullptr;

#ifdef XMQ_SPTK_JSON_SCHEMA
        auto rules = std::make_unique<Rules>();
        for (const auto& entry: entries)
        {
            const auto filter = std::string(entry->getName());
            try
            {
                if (entry->type() != sptk::xdoc::Node::Type::Text || entry->getString().empty())
                    throw std::runtime_error("expected a schema file path");
                auto levels = compileFilter(filter);
                const auto schemaFile = config.parent_path() / entry->getString().c_str();
                sptk::Buffer schema;
                schema.loadFromFile(schemaFile);
                rules->policies.push_back({std::move(levels), sptk::xdoc::JsonSchema::compile(bytes(schema))});
            }
            catch (const std::exception& error)
            {
                throw std::runtime_error("filter '" + filter + "': " + error.what());
            }
        }
        return std::unique_ptr<const PublishSchemaPolicies>(new PublishSchemaPolicies(std::move(rules)));
#else
        throw std::runtime_error("SPTK JSON Schema support is unavailable");
#endif
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error(config.string() + ": " + error.what());
    }
}

bool PublishSchemaPolicies::accepts(std::span<const std::string_view> topic, std::string_view payload) const
{
#ifdef XMQ_SPTK_JSON_SCHEMA
    for (const auto& policy: m_rules->policies)
    {
        if (matches(policy.filter, topic) && !policy.schema.validate(payload))
            return false;
    }
#else
    (void)topic;
    (void)payload;
#endif
    return true;
}

} // namespace xmq
