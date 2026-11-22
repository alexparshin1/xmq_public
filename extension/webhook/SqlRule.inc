// Included inside Webhook.cpp's private namespace, after the JSON reader.
using SqlKind = JsonValue::Kind;

bool validPayloadUtf8(std::string_view text)
{
    size_t at = 0;
    while (at < text.size())
    {
        const auto first = static_cast<unsigned char>(text[at++]);
        if (first < 0x80)
            continue;
        const auto count = first < 0xC2 ? 0u : first < 0xE0 ? 1u :
                           first < 0xF0 ? 2u : first <= 0xF4 ? 3u : 0u;
        if (!count || count > text.size() - at)
            return false;
        for (unsigned i = 0; i < count; ++i)
        {
            const auto byte = static_cast<unsigned char>(text[at++]);
            if (byte < 0x80 || byte > 0xBF || (i == 0 &&
                ((first == 0xE0 && byte < 0xA0) || (first == 0xED && byte >= 0xA0) ||
                 (first == 0xF0 && byte < 0x90) || (first == 0xF4 && byte >= 0x90))))
                return false;
        }
    }
    return true;
}

struct SqlValue
{
    SqlKind kind{SqlKind::Null};
    std::string text;
};

enum class SqlTruth { False, True, Unknown };

struct SqlRow
{
    std::string topic, clientId, username, payload;
    std::string timestamp, level, subject, message;
    bool log{};
    unsigned qos{};
    bool retain{};
    std::optional<JsonValue> json;
};

using SqlField = std::function<SqlValue(const SqlRow&)>;
using SqlPredicate = std::function<SqlTruth(const SqlRow&)>;

std::optional<double> sqlNumber(std::string_view text)
{
    if (text.empty())
        return {};
    double number{};
    const auto end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, number);
    if (ec != std::errc{} || ptr != end || !std::isfinite(number))
        return {};
    return number;
}

struct SqlRule
{
    std::vector<std::string> filters;
    bool needsJson{};
    bool needsPayload{};
    std::vector<std::pair<std::string, SqlField>> columns;
    SqlPredicate where = [](const SqlRow&) { return SqlTruth::True; };

    std::optional<std::string> render(const SqlRow& row) const
    {
        if (where(row) != SqlTruth::True)
            return {};
        std::string result = "{";
        for (const auto& [key, field]: columns)
        {
            if (result.size() > 1)
                result += ',';
            result += '"' + jsonEscape(key) + "\":";
            const auto value = field(row);
            switch (value.kind)
            {
                case SqlKind::String: result += '"' + jsonEscape(value.text) + '"'; break;
                case SqlKind::Boolean:
                case SqlKind::Number: result += value.text; break;
                default: result += "null"; break;
            }
        }
        result += '}';
        return result;
    }
};

class SqlParser
{
public:
    explicit SqlParser(std::string_view text, bool log = false) : m_text(text), m_log(log) {}

    SqlRule parse()
    {
        if (m_text.size() > 4096)
            fail("rule is longer than 4096 bytes");
        require("SELECT");
        const bool all = take("*");
        if (all)
        {
            static const char* mqttFields[] = {"topic", "qos", "retain", "client_id", "username", "timestamp", "payload"};
            static const char* logFields[] = {"timestamp", "level", "subject", "message"};
            for (const auto* name: m_log ? std::span<const char*>(logFields)
                                         : std::span<const char*>(mqttFields))
            {
                // These names are implicit, not tokens in the input after '*'.
                SqlParser fieldParser(name, m_log);
                std::string fieldName;
                m_rule.columns.emplace_back(name, fieldParser.operand(fieldName));
                m_rule.needsPayload |= fieldParser.m_rule.needsPayload;
            }
        }
        for (;;)
        {
            if (all)
            {
                space();
                if (m_at >= m_text.size() || m_text[m_at] != ',')
                    break;
                ++m_at;
            }
            std::string name;
            auto field = operand(name);
            if (take("AS"))
                name = identifier();
            if (name.empty())
                fail("expression needs an AS alias");
            if (std::ranges::any_of(m_rule.columns, [&](const auto& column) { return column.first == name; }))
                fail("duplicate output field");
            m_rule.columns.emplace_back(std::move(name), std::move(field));
            if (!all && !take(","))
                break;
        }
        require("FROM");
        if (m_log)
            require("LOGS");
        else
        {
            do
            {
                auto filter = quoted('"');
                validateFilter(filter);
                m_rule.filters.push_back(std::move(filter));
            } while (take(","));
        }
        if (take("WHERE"))
            m_rule.where = expression(0, false);
        space();
        if (m_at != m_text.size())
        {
            if (m_text.substr(m_at).starts_with("FROM"))
                fail("unexpected FROM: each rule has one FROM before WHERE");
            if (m_text[m_at] == ';')
                ++m_at;
            else
                fail("unexpected input between rules");
        }
        return std::move(m_rule);
    }

    size_t consumed() const { return m_at; }

private:
    void validateFilter(const std::string& filter)
    {
        if (filter.empty() || filter.find('\0') != std::string::npos)
            fail("expected an MQTT filter");
        if (filter.front() == '/')
            fail("an MQTT filter cannot start with /");
        if (filter.size() > 65535)
            fail("an MQTT filter cannot exceed 65535 bytes");
        size_t start = 0;
        for (;;)
        {
            const auto end = filter.find('/', start);
            const auto level = std::string_view(filter).substr(start, end - start);
            if (level.empty())
                fail("an MQTT filter cannot have an empty level");
            if ((level.find('+') != level.npos && level != "+") ||
                (level.find('#') != level.npos && (level != "#" || end != std::string::npos)))
                fail("invalid MQTT filter wildcard");
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
    }

    [[noreturn]] void fail(const std::string& reason) const
    {
        throw std::invalid_argument("sql_rule: " + reason + " at offset " + std::to_string(m_at));
    }

    static bool word(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_';
    }

    void space()
    {
        while (m_at < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_at])))
            ++m_at;
    }

    bool take(std::string_view token)
    {
        space();
        const auto tail = m_text.substr(m_at);
        if (tail.size() < token.size() || !std::ranges::equal(token, tail.substr(0, token.size()),
            [](char a, char b) { return a == std::toupper(static_cast<unsigned char>(b)); }))
            return false;
        if (word(token.back()) && tail.size() > token.size() && word(tail[token.size()]))
            return false;
        m_at += token.size();
        return true;
    }

    void require(std::string_view token)
    {
        if (!take(token))
            fail("expected " + std::string(token));
    }

    std::string identifier()
    {
        space();
        const auto start = m_at;
        if (start == m_text.size() || !word(m_text[start]) ||
            (m_text[start] >= '0' && m_text[start] <= '9'))
            fail("expected identifier");
        while (m_at < m_text.size() && word(m_text[m_at]))
            ++m_at;
        return std::string(m_text.substr(start, m_at - start));
    }

    std::string quoted(char quote)
    {
        space();
        if (m_at == m_text.size() || m_text[m_at++] != quote)
            fail("expected quoted text");
        std::string value;
        while (m_at < m_text.size())
        {
            const char c = m_text[m_at++];
            if (c == quote)
            {
                if (m_at == m_text.size() || m_text[m_at] != quote)
                    return value;
                ++m_at;
            }
            value += c;
        }
        fail("unterminated quoted text");
    }

    SqlField operand(std::string& name, unsigned depth = 0)
    {
        if (depth >= 32)
            fail("expression nesting exceeds 32 levels");
        space();
        if (take("CAST"))
        {
            require("(");
            auto field = operand(name, depth + 1);
            require("AS");
            require("DOUBLE");
            require(")");
            return [field = std::move(field)](const SqlRow& row) -> SqlValue
            {
                auto value = field(row);
                if (value.kind == SqlKind::Number)
                    return value;
                if (value.kind != SqlKind::String)
                    return {};
                const auto number = sqlNumber(trimmed(value.text));
                if (!number)
                    return {};
                char buffer[64];
                const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), *number);
                return error == std::errc{} ? SqlValue{SqlKind::Number, std::string(buffer, end)} : SqlValue{};
            };
        }
        if (m_at < m_text.size() && m_text[m_at] == '\'')
        {
            auto value = quoted('\'');
            return [value = std::move(value)](const SqlRow&) { return SqlValue{SqlKind::String, value}; };
        }
        if (take("TRUE"))
            return [](const SqlRow&) { return SqlValue{SqlKind::Boolean, "true"}; };
        if (take("FALSE"))
            return [](const SqlRow&) { return SqlValue{SqlKind::Boolean, "false"}; };
        if (take("NULL"))
            return [](const SqlRow&) { return SqlValue{}; };
        if (m_at < m_text.size() && (m_text[m_at] == '-' || std::isdigit(static_cast<unsigned char>(m_text[m_at]))))
        {
            const auto start = m_at;
            while (m_at < m_text.size() && std::string_view("0123456789.eE+-").find(m_text[m_at]) != std::string_view::npos)
                ++m_at;
            auto number = std::string(m_text.substr(start, m_at - start));
            JsonValue value;
            if (!JsonReader(number).read(value) || value.kind != SqlKind::Number || !sqlNumber(number))
                fail("invalid numeric literal");
            return [number = std::move(number)](const SqlRow&) { return SqlValue{SqlKind::Number, number}; };
        }
        name = identifier();
        if (name == "timestamp")
            return [](const SqlRow& row) { return SqlValue{SqlKind::String, row.timestamp}; };
        if (m_log)
        {
            for (const auto& [key, member]: {
                std::pair{"level", &SqlRow::level}, {"subject", &SqlRow::subject}, {"message", &SqlRow::message}})
                if (name == key)
                    return [member](const SqlRow& row) { return SqlValue{SqlKind::String, row.*member}; };
            fail("unknown log field " + name);
        }
        if (name == "topic")
            return [](const SqlRow& row) { return SqlValue{SqlKind::String, row.topic}; };
        if (name == "client_id")
            return [](const SqlRow& row) { return SqlValue{SqlKind::String, row.clientId}; };
        if (name == "username")
            return [](const SqlRow& row) { return SqlValue{SqlKind::String, row.username}; };
        if (name == "qos")
            return [](const SqlRow& row) { return SqlValue{SqlKind::Number, std::to_string(row.qos)}; };
        if (name == "retain")
            return [](const SqlRow& row) { return SqlValue{SqlKind::Boolean, row.retain ? "true" : "false"}; };
        if (name != "payload")
            fail("unknown field " + name);
        std::vector<std::string> path;
        while (take("."))
        {
            name = identifier();
            path.push_back(name);
        }
        if (path.empty())
        {
            m_rule.needsPayload = true;
            return [](const SqlRow& row) -> SqlValue
            {
                return validPayloadUtf8(row.payload) ? SqlValue{SqlKind::String, row.payload} : SqlValue{};
            };
        }
        m_rule.needsJson = true;
        return [path = std::move(path)](const SqlRow& row) -> SqlValue
        {
            const auto* value = row.json ? &*row.json : nullptr;
            for (const auto& key: path)
            {
                if (!value || value->kind != SqlKind::Object)
                    return {};
                const auto found = std::ranges::find(value->object, key, &std::pair<std::string, JsonValue>::first);
                if (found == value->object.end())
                    return {};
                value = &found->second;
            }
            if (value->kind == SqlKind::Object || value->kind == SqlKind::Array ||
                (value->kind == SqlKind::Number && !sqlNumber(value->text)))
                return {};
            return {value->kind, value->text};
        };
    }

    SqlPredicate atom(unsigned depth)
    {
        if (depth >= 32)
            fail("expression nesting exceeds 32 levels");
        if (take("NOT"))
            return [child = atom(depth + 1)](const SqlRow& row)
            {
                const auto value = child(row);
                return value == SqlTruth::Unknown ? value :
                       value == SqlTruth::True ? SqlTruth::False : SqlTruth::True;
            };
        if (take("("))
        {
            auto result = expression(depth + 1, false);
            require(")");
            return result;
        }
        std::string name;
        auto lhs = operand(name);
        if (take("IS"))
        {
            const bool negate = take("NOT");
            require("NULL");
            return [lhs = std::move(lhs), negate](const SqlRow& row)
            {
                return ((lhs(row).kind == SqlKind::Null) != negate) ? SqlTruth::True : SqlTruth::False;
            };
        }
        for (const std::string_view op: {"<=", ">=", "!=", "<>", "=", "<", ">"})
        {
            if (!take(op))
                continue;
            auto rhs = operand(name);
            return [lhs = std::move(lhs), rhs = std::move(rhs), op](const SqlRow& row)
            {
                const auto a = lhs(row);
                const auto b = rhs(row);
                if (a.kind != b.kind || a.kind == SqlKind::Null)
                    return SqlTruth::Unknown;
                int order{};
                if (a.kind == SqlKind::Number)
                {
                    const auto x = sqlNumber(a.text);
                    const auto y = sqlNumber(b.text);
                    if (!x || !y)
                        return SqlTruth::Unknown;
                    order = (*x > *y) - (*x < *y);
                }
                else
                    order = a.text.compare(b.text);
                const bool result = op == "=" ? order == 0 : op == "!=" || op == "<>" ? order != 0 :
                    op == "<" ? order < 0 : op == ">" ? order > 0 : op == "<=" ? order <= 0 : order >= 0;
                return result ? SqlTruth::True : SqlTruth::False;
            };
        }
        return [lhs = std::move(lhs)](const SqlRow& row)
        {
            const auto value = lhs(row);
            return value.kind != SqlKind::Boolean ? SqlTruth::Unknown :
                   value.text == "true" ? SqlTruth::True : SqlTruth::False;
        };
    }

    SqlPredicate expression(unsigned depth, bool conjunction)
    {
        std::vector<SqlPredicate> terms;
        do
        {
            terms.push_back(conjunction ? atom(depth) : expression(depth, true));
        } while (take(conjunction ? "AND" : "OR"));
        if (terms.size() == 1)
            return std::move(terms.front());
        return [terms = std::move(terms), conjunction](const SqlRow& row)
        {
            const auto terminal = conjunction ? SqlTruth::False : SqlTruth::True;
            bool unknown = false;
            for (const auto& term: terms)
            {
                const auto value = term(row);
                if (value == terminal)
                    return terminal;
                unknown |= value == SqlTruth::Unknown;
            }
            return unknown ? SqlTruth::Unknown : conjunction ? SqlTruth::True : SqlTruth::False;
        };
    }

    std::string_view m_text;
    bool m_log;
    size_t m_at{};
    SqlRule m_rule;
};
