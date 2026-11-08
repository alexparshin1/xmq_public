/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "JsonLogEngine.h"

#include <sptk5/DateTime.h>
#include <sptk5/cutils>
#include <sptk5/Logger.h>

#include <iomanip>
#include <sstream>

using namespace std;
using namespace sptk;

namespace xmq {

namespace {

/// JSON string escaping, including the control characters a message can carry.
string escape(const string& value)
{
    string escaped;
    escaped.reserve(value.size() + 8);
    for (const auto character: value)
    {
        switch (character)
        {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (static_cast<unsigned char>(character) < 0x20)
                {
                    ostringstream code;
                    code << "\\u" << hex << setw(4) << setfill('0')
                         << static_cast<int>(static_cast<unsigned char>(character));
                    escaped += code.str();
                }
                else
                {
                    escaped += character;
                }
                break;
        }
    }
    return escaped;
}

/**
 * @brief Take one prefix of the form "&lt;open&gt;value&lt;close&gt; " off the front.
 *
 * Only when it is there and non-empty: a message that begins with a '(' for its own reasons is
 * left alone rather than mistaken for the node prefix.
 */
bool takePrefix(string& text, const char open, const char close, string& value)
{
    if (text.empty() || text.front() != open)
    {
        return false;
    }

    const auto end = text.find(close, 1);
    if (end == string::npos || end == 1 || end + 1 >= text.size() || text[end + 1] != ' ')
    {
        return false;
    }

    value.assign(text, 1, end - 1);
    text.erase(0, end + 2);
    return true;
}

} // namespace

JsonLogEngine::JsonLogEngine(shared_ptr<LogEngine> inner, string nodeName)
    : LogEngine("JsonLogEngine")
    , m_inner(std::move(inner))
    , m_nodeName(std::move(nodeName))
{
}

JsonLogEngine::~JsonLogEngine()
{
    // Stop the save thread while m_inner is still alive. ~LogEngine() would do it, but by then
    // the members of this class have been destroyed and a queued message would reach a dead inner
    // engine - the hazard the LogEngine documentation warns about.
    shutdown();
}

bool JsonLogEngine::saveMessage(const Logger::Message& message)
{
    // The file engine adds DATE/TIME/PRIORITY itself; a line of JSON must not carry them. It was
    // given ENABLE only, and the timestamp is in the object.
    Logger::Message formatted(message.priority, format(message));
    formatted.timestamp = message.timestamp;
    const bool saved = m_inner->saveMessage(formatted);

    if (m_stdout)
    {
        if (message.priority <= LogPriority::Error)
        {
            CERR(formatted.message << '\n');
        }
        else
        {
            COUT(formatted.message << '\n');
        }
    }

    return saved;
}

void JsonLogEngine::flush()
{
    m_inner->flush();
}

void JsonLogEngine::reset()
{
    LogEngine::reset();
    m_inner->reset();
}

filesystem::path JsonLogEngine::rotate(const size_t keepArchives)
{
    return m_inner->rotate(keepArchives);
}

string JsonLogEngine::format(const Logger::Message& message) const
{
    string text = message.message;

    string prefix;
    string subject;
    string scope;
    // The broker's own logger prefixes "(node)"; kube-style controller loggers use names such as
    // "(xmq service)", which are components rather than nodes. Only the former is the node this
    // engine was built for, so anything else is reported as a component.
    const bool hasComponentPrefix = takePrefix(text, '(', ')', prefix) && prefix != m_nodeName;
    takePrefix(text, '<', '>', subject);
    takePrefix(text, '[', ']', scope);

    ostringstream line;
    line << "{\"timestamp\":\"" << escape(message.timestamp.isoDateTimeString(DateTime::PrintAccuracy::MILLISECONDS))
         << "\",\"level\":\"" << escape(LogEngine::priorityName(message.priority).toLowerCase().c_str())
         << "\",\"node\":\"" << escape(m_nodeName) << '"';

    if (hasComponentPrefix)
    {
        line << ",\"component\":\"" << escape(prefix) << '"';
    }
    if (!subject.empty())
    {
        line << ",\"subject\":\"" << escape(subject) << '"';
    }
    if (!scope.empty())
    {
        line << ",\"scope\":\"" << escape(scope) << '"';
    }

    line << ",\"message\":\"" << escape(text) << "\"}";
    return line.str();
}

} // namespace xmq
