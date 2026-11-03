#include "PasswordImport.h"

#include "PasswordHash.h"

#include <algorithm>
#include <sstream>

using namespace std;
using namespace sptk;

namespace xmq {

namespace {

string trim(const string& text)
{
    const auto first = text.find_first_not_of(" \t\r");
    if (first == string::npos)
    {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

string jsonQuote(const string& text)
{
    string result = "\"";
    for (const auto character: text)
    {
        switch (character)
        {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(character) < 0x20)
                {
                    char buffer[7];
                    snprintf(buffer, sizeof(buffer), "\\u%04x", character);
                    result += buffer;
                }
                else
                {
                    result += character;
                }
        }
    }
    result += '"';
    return result;
}

string statusName(const PasswordImport::Status status)
{
    switch (status)
    {
        case PasswordImport::Status::New: return "new";
        case PasswordImport::Status::Existing: return "existing";
        case PasswordImport::Status::Unsupported: return "unsupported";
        case PasswordImport::Status::Invalid: return "invalid";
    }
    return "invalid";
}

string arrayJson(const vector<PasswordImport::Entry>& entries)
{
    string result = "[";
    bool   first = true;
    for (const auto& entry: entries)
    {
        if (!first)
        {
            result += ",";
        }
        first = false;
        result += "{\"username\":" + jsonQuote(entry.m_username) +
                  ",\"status\":" + jsonQuote(statusName(entry.m_status)) +
                  ",\"reason\":" + jsonQuote(entry.m_reason) + "}";
    }
    return result + "]";
}

} // namespace

vector<PasswordImport::Entry> PasswordImport::parse(const string_view content)
{
    vector<Entry> entries;
    set<string>   seen;
    istringstream stream{string(content)};
    string        raw;
    bool          firstLine = true;
    size_t        lineNumber = 0;

    while (getline(stream, raw))
    {
        ++lineNumber;
        // A file edited on Windows starts with a byte-order mark, and every line ends with \r.
        if (firstLine)
        {
            firstLine = false;
            if (raw.starts_with("\xEF\xBB\xBF"))
            {
                raw.erase(0, 3);
            }
        }
        const auto line = trim(raw);
        if (line.empty() || line.front() == '#')
        {
            continue;
        }

        const auto colon = line.find(':');
        if (colon == string::npos || colon == 0)
        {
            entries.push_back({{}, {}, Status::Invalid,
                               "line " + to_string(lineNumber) + ": expected username:password", false});
            continue;
        }

        Entry entry;
        entry.m_username = trim(line.substr(0, colon));
        const auto stored = trim(line.substr(colon + 1));
        if (entry.m_username.empty())
        {
            entries.push_back({{}, {}, Status::Invalid,
                               "line " + to_string(lineNumber) + ": empty username", false});
            continue;
        }
        if (!seen.insert(entry.m_username).second)
        {
            entries.push_back({entry.m_username, {}, Status::Invalid,
                               "line " + to_string(lineNumber) + ": duplicate username", false});
            continue;
        }
        if (stored.empty())
        {
            entries.push_back({entry.m_username, {}, Status::Invalid,
                               "line " + to_string(lineNumber) + ": no password", false});
            continue;
        }

        entry.m_verifier = stored;
        if (stored.starts_with("$7$"))
        {
            if (PasswordHash::isHashed(stored))
            {
                entry.m_status = Status::New;
            }
            else
            {
                entry.m_status = Status::Invalid;
                entry.m_reason = "line " + to_string(lineNumber) + ": broken $7$ verifier";
            }
        }
        else if (stored.starts_with("$6$"))
        {
            entry.m_status = Status::Unsupported;
            entry.m_reason = "line " + to_string(lineNumber) +
                ": SHA512-crypt (Mosquitto 1.6) is not supported; the password has to be set again";
        }
        else if (stored.starts_with("$argon2id$"))
        {
            entry.m_status = Status::Unsupported;
            entry.m_reason = "line " + to_string(lineNumber) +
                ": Argon2id (Mosquitto 2.1) is not supported; the password has to be set again";
        }
        else if (stored.starts_with("$"))
        {
            entry.m_status = Status::Unsupported;
            entry.m_reason = "line " + to_string(lineNumber) + ": unknown verifier format";
        }
        else
        {
            // Mosquitto also accepts a clear-text password in this file. It is hashed with this
            // broker's verifier when the import is applied, never stored as it arrived.
            entry.m_plaintext = true;
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

PasswordImport::Plan PasswordImport::plan(const vector<Entry>& entries,
                                          const set<string>&  existingUsernames)
{
    Plan result;
    for (const auto& entry: entries)
    {
        if (entry.m_status == Status::Invalid)
        {
            result.m_invalid.push_back(entry);
        }
        else if (entry.m_status == Status::Unsupported)
        {
            result.m_unsupported.push_back(entry);
        }
        else if (existingUsernames.contains(entry.m_username))
        {
            auto existing = entry;
            existing.m_status = Status::Existing;
            existing.m_reason = "the account already exists and is not changed";
            result.m_existing.push_back(std::move(existing));
        }
        else
        {
            auto added = entry;
            added.m_status = Status::New;
            result.m_new.push_back(std::move(added));
        }
    }
    return result;
}

string PasswordImport::toJson(const Plan& plan)
{
    return "{\"new\":" + arrayJson(plan.m_new) +
           ",\"existing\":" + arrayJson(plan.m_existing) +
           ",\"unsupported\":" + arrayJson(plan.m_unsupported) +
           ",\"invalid\":" + arrayJson(plan.m_invalid) +
           ",\"counts\":{\"new\":" + to_string(plan.m_new.size()) +
           ",\"existing\":" + to_string(plan.m_existing.size()) +
           ",\"unsupported\":" + to_string(plan.m_unsupported.size()) +
           ",\"invalid\":" + to_string(plan.m_invalid.size()) + "}}";
}

} // namespace xmq
