#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <functional>
#include <optional>
#include <ranges>
#include <regex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xmq::webhook_sql {
namespace {

std::string jsonEscape(const std::string_view text)
{
    std::string out;
    for (const char character: text)
    {
        switch (character)
        {
            case '"':
                out += R"(\")";
                break;
            case '\\':
                out += R"(\\)";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(character) < 0x20)
                {
                    char buffer[7];
                    snprintf(buffer, sizeof(buffer), "\\u%04x", character);
                    out += buffer;
                }
                else
                {
                    out += character;
                }
        }
    }
    return out;
}

struct JsonValue
{
    enum class Kind
    {
        Null,
        String,
        Number,
        Boolean,
        Object,
        Array
    };

    Kind                                           kind{Kind::Null};
    std::string                                    text;
    std::vector<std::pair<std::string, JsonValue>> object;
    std::vector<JsonValue>                         array;
};

class JsonReader
{
public:
    explicit JsonReader(const std::string_view text)
        : m_text(text)
    {
    }

    bool read(JsonValue& value)
    {
        skipSpace();
        return readValue(value, 0) && (skipSpace(), m_at == m_text.size());
    }

    [[nodiscard]] const std::string& error() const
    {
        return m_error;
    }

private:
    void skipSpace()
    {
        while (m_at < m_text.size() &&
               (m_text[m_at] == ' ' || m_text[m_at] == '\t' || m_text[m_at] == '\n' || m_text[m_at] == '\r'))
        {
            ++m_at;
        }
    }

    bool fail(const std::string& what)
    {
        m_error = what + " at offset " + std::to_string(m_at);
        return false;
    }

    bool readValue(JsonValue& value, unsigned depth)
    {
        if (depth >= 64)
        {
            return fail("JSON nesting exceeds 64 levels");
        }
        if (m_at >= m_text.size())
        {
            return fail("the value ended early");
        }
        switch (m_text[m_at])
        {
            case '{':
                return readObject(value, depth);
            case '[':
                return readArray(value, depth);
            case '"': {
                value.kind = JsonValue::Kind::String;
                return readString(value.text);
            }
            case 't':
            case 'f': {
                const auto word = m_text.substr(m_at, 4);
                const bool isTrue = word.starts_with("true");
                if (!isTrue && !word.starts_with("false"))
                {
                    return fail("not a JSON value");
                }
                value.kind = JsonValue::Kind::Boolean;
                value.text = isTrue ? "true" : "false";
                m_at += isTrue ? 4 : 5;
                return true;
            }
            case 'n': {
                if (!m_text.substr(m_at).starts_with("null"))
                {
                    return fail("not a JSON value");
                }
                value.kind = JsonValue::Kind::Null;
                m_at += 4;
                return true;
            }
            default: {
                const auto start = m_at;
                while (m_at < m_text.size() &&
                       (std::isdigit(static_cast<unsigned char>(m_text[m_at])) || m_text[m_at] == '-' ||
                        m_text[m_at] == '+' || m_text[m_at] == '.' || m_text[m_at] == 'e' ||
                        m_text[m_at] == 'E'))
                {
                    ++m_at;
                }
                const auto number = m_text.substr(start, m_at - start);
                static const std::regex syntax(R"(-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?)");
                if (number.size() > 128 || !std::regex_match(number.begin(), number.end(), syntax))
                {
                    return fail("not a JSON value");
                }
                value.kind = JsonValue::Kind::Number;
                value.text = std::string(m_text.substr(start, m_at - start));
                return true;
            }
        }
    }

    bool readObject(JsonValue& value, unsigned depth)
    {
        value.kind = JsonValue::Kind::Object;
        ++m_at; // '{'
        skipSpace();
        if (m_at < m_text.size() && m_text[m_at] == '}')
        {
            ++m_at;
            return true;
        }
        while (true)
        {
            skipSpace();
            std::string name;
            if (!readString(name))
            {
                return false;
            }
            skipSpace();
            if (m_at >= m_text.size() || m_text[m_at] != ':')
            {
                return fail("expected ':'");
            }
            ++m_at;
            skipSpace();
            JsonValue member;
            if (!readValue(member, depth + 1))
            {
                return false;
            }
            value.object.emplace_back(std::move(name), std::move(member));
            skipSpace();
            if (m_at < m_text.size() && m_text[m_at] == ',')
            {
                ++m_at;
                continue;
            }
            if (m_at < m_text.size() && m_text[m_at] == '}')
            {
                ++m_at;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool readArray(JsonValue& value, unsigned depth)
    {
        value.kind = JsonValue::Kind::Array;
        ++m_at; // '['
        skipSpace();
        if (m_at < m_text.size() && m_text[m_at] == ']')
        {
            ++m_at;
            return true;
        }
        while (true)
        {
            skipSpace();
            JsonValue element;
            if (!readValue(element, depth + 1))
            {
                return false;
            }
            value.array.push_back(std::move(element));
            skipSpace();
            if (m_at < m_text.size() && m_text[m_at] == ',')
            {
                ++m_at;
                continue;
            }
            if (m_at < m_text.size() && m_text[m_at] == ']')
            {
                ++m_at;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool readString(std::string& out)
    {
        if (m_at >= m_text.size() || m_text[m_at] != '"')
        {
            return fail("expected a string");
        }
        ++m_at;
        out.clear();
        while (m_at < m_text.size())
        {
            const char character = m_text[m_at++];
            if (character == '"')
            {
                return true;
            }
            if (character != '\\')
            {
                if (static_cast<unsigned char>(character) < 0x20)
                {
                    return fail("unescaped control character");
                }
                out += character;
                continue;
            }
            if (m_at >= m_text.size())
            {
                return fail("a string ended in the middle of an escape");
            }
            switch (m_text[m_at++])
            {
                case '"':
                    out += '"';
                    break;
                case '\\':
                    out += '\\';
                    break;
                case '/':
                    out += '/';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u': {
                    unsigned code = 0;
                    const auto readHex = [&](unsigned& value)
                    {
                        if (m_text.size() - m_at < 4)
                            return false;
                        const auto* begin = m_text.data() + m_at;
                        const auto [end, ec] = std::from_chars(begin, begin + 4, value, 16);
                        m_at += 4;
                        return ec == std::errc{} && end == begin + 4;
                    };
                    if (!readHex(code))
                        return fail("invalid Unicode escape");
                    if (code >= 0xD800 && code <= 0xDBFF)
                    {
                        unsigned low{};
                        if (!m_text.substr(m_at).starts_with("\\u"))
                            return fail("missing low surrogate");
                        m_at += 2;
                        if (!readHex(low) || low < 0xDC00 || low > 0xDFFF)
                            return fail("invalid low surrogate");
                        code = 0x10000 + ((code - 0xD800) << 10U) + (low - 0xDC00);
                    }
                    else if (code >= 0xDC00 && code <= 0xDFFF)
                        return fail("unexpected low surrogate");
                    if (code < 0x80)
                    {
                        out += static_cast<char>(code);
                    }
                    else if (code < 0x800)
                    {
                        out += static_cast<char>(0xC0 | (code >> 6U));
                        out += static_cast<char>(0x80 | (code & 0x3FU));
                    }
                    else if (code < 0x10000)
                    {
                        out += static_cast<char>(0xE0 | (code >> 12U));
                        out += static_cast<char>(0x80 | ((code >> 6U) & 0x3FU));
                        out += static_cast<char>(0x80 | (code & 0x3FU));
                    }
                    else
                    {
                        out += static_cast<char>(0xF0 | (code >> 18U));
                        out += static_cast<char>(0x80 | ((code >> 12U) & 0x3FU));
                        out += static_cast<char>(0x80 | ((code >> 6U) & 0x3FU));
                        out += static_cast<char>(0x80 | (code & 0x3FU));
                    }
                    break;
                }
                default:
                    return fail("unknown escape");
            }
        }
        return fail("a string was not closed");
    }

    const std::string_view m_text;
    size_t                 m_at{0};
    std::string            m_error;
};

/// MQTT topic matching, because the host is given one filter for the whole extension while each
/// destination has its own: the host filter is "#" and the narrowing happens here.
bool topicMatches(const std::string_view filter, const std::string_view topic)
{
    size_t at = 0;
    size_t topicAt = 0;
    while (true)
    {
        const auto filterEnd = filter.find('/', at);
        const auto topicEnd = topic.find('/', topicAt);
        const auto filterLevel = filter.substr(at, filterEnd == std::string_view::npos
                                                       ? std::string_view::npos
                                                       : filterEnd - at);
        const auto topicLevel = topic.substr(topicAt, topicEnd == std::string_view::npos
                                                          ? std::string_view::npos
                                                          : topicEnd - topicAt);

        if (filterLevel == "#")
        {
            return true;
        }
        if (filterLevel != "+" && filterLevel != topicLevel)
        {
            return false;
        }

        const bool filterLast = filterEnd == std::string_view::npos;
        const bool topicLast = topicEnd == std::string_view::npos;
        if (filterLast && topicLast)
        {
            return true;
        }

        if (filterLast)
        {
            return false;
        }

        if (topicLast)
        {
            // Mosquitto's "sport/#" covers "sport" itself.
            return filter.ends_with("#");
        }

        at = filterEnd + 1;
        topicAt = topicEnd + 1;
    }
}

/// Whitespace around a setting is an artefact of how the file was written, not part of its value.
std::string trimmed(const std::string_view text)
{
    size_t first = 0;
    size_t last = text.size();
    while (first < last && std::isspace(static_cast<unsigned char>(text[first])))
    {
        ++first;
    }
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])))
    {
        --last;
    }
    return std::string(text.substr(first, last - first));
}

#include "SqlRule.inc"

bool sqlTopicMatches(const SqlRule& rule, std::string_view topic)
{
    return std::ranges::any_of(rule.filters, [topic](const auto& filter) { return topicMatches(filter, topic); });
}

[[maybe_unused]] std::vector<SqlRule> sqlRules(std::string text, bool log = false)
{
    if (text.size() > 4096)
        throw std::invalid_argument("SQL rules exceed 4096 bytes");
    std::vector<SqlRule> rules;
    while (!text.empty())
    {
        SqlParser parser(text, log);
        rules.push_back(parser.parse());
        text = trimmed(std::string_view(text).substr(parser.consumed()));
    }
    if (rules.empty())
        throw std::invalid_argument("SQL rules mode needs at least one rule");
    return rules;
}

struct SqlMatch
{
    std::optional<std::string> body;
    size_t rule{};
    bool invalidJson{};
};

[[maybe_unused]] SqlMatch evaluateSql(const std::vector<SqlRule>& rules, SqlRow& row)
{
    SqlMatch result;
    bool parsed = false;
    for (size_t i = 0; i < rules.size(); ++i)
    {
        const auto& rule = rules[i];
        if (!row.log && !sqlTopicMatches(rule, row.topic))
            continue;
        if (rule.needsJson && !parsed)
        {
            parsed = true;
            JsonValue json;
            if (JsonReader(row.payload).read(json))
                row.json.emplace(std::move(json));
            else
                result.invalidJson = true;
        }
        if (rule.needsJson && !row.json)
            continue;
        result.body = rule.render(row);
        if (result.body)
        {
            result.rule = i + 1;
            break;
        }
    }
    return result;
}

} // namespace
} // namespace xmq::webhook_sql
