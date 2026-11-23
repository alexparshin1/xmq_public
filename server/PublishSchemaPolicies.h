#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace xmq {

/// An immutable snapshot of the MQTT ingress policies loaded at startup.
class PublishSchemaPolicies
{
public:
    static std::unique_ptr<const PublishSchemaPolicies> load(const std::filesystem::path& brokerConfig);
    ~PublishSchemaPolicies();

    [[nodiscard]] bool accepts(std::span<const std::string_view> topic, std::string_view payload) const;

private:
    struct Rules;
    explicit PublishSchemaPolicies(std::unique_ptr<const Rules> rules);
    std::unique_ptr<const Rules> m_rules;
};

} // namespace xmq
