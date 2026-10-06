/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
║  code review                                                                 ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#include "base/MessageProperties.h"
#include "base/PropertyValueTypes.h"
#include "common/mqtt/VariableLength.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <netinet/in.h>
#endif

using namespace std;
using namespace sptk;
using namespace xmq;
using namespace mqtt;

using enum Property;

namespace {

constexpr size_t identifierSize = 1;
constexpr size_t stringLengthSize = 2;

/// The name under which a latency trace rides as a user property.
constexpr string_view latencyPropertyName = "latency";

uint16_t readUint16(const uint8_t* data)
{
    uint16_t value = 0;
    memcpy(&value, data, sizeof(value));
    return ntohs(value);
}

void writeUint16(uint8_t* data, const uint16_t value)
{
    const uint16_t encoded = htons(value);
    memcpy(data, &encoded, sizeof(encoded));
}

uint32_t readUint32(const uint8_t* data)
{
    uint32_t value = 0;
    memcpy(&value, data, sizeof(value));
    return ntohl(value);
}

void writeUint32(uint8_t* data, const uint32_t value)
{
    const uint32_t encoded = htonl(value);
    memcpy(data, &encoded, sizeof(encoded));
}

/// A string or binary payload: two bytes of length, then that many bytes.
size_t sizedPayloadLength(const uint8_t* payload, const size_t available, const char* what)
{
    if (available < stringLengthSize)
    {
        throw Exception(string("Not enough bytes to read ") + what + " length");
    }
    const size_t length = readUint16(payload);
    if (available < stringLengthSize + length)
    {
        throw Exception(string("Not enough bytes to read ") + what);
    }
    return stringLengthSize + length;
}

} // namespace

size_t MessageProperties::payloadLength(const Property property, const uint8_t* payload, const size_t available)
{
    using enum PropertyValueType;
    switch (PropertyValueTypes::getValueType(property))
    {
        case Uint8:
            if (available < 1)
            {
                throw Exception("Not enough bytes to read byte property");
            }
            return identifierSize + 1;

        case Uint16:
            if (available < 2)
            {
                throw Exception("Not enough bytes to read short integer property");
            }
            return identifierSize + 2;

        case Uint32:
            if (available < 4)
            {
                throw Exception("Not enough bytes to read integer property");
            }
            return identifierSize + 4;

        case VarInt:
        {
            auto* cursor = const_cast<uint8_t*>(payload);
            (void) VariableLength::read(cursor, static_cast<int>(available));
            return identifierSize + static_cast<size_t>(cursor - payload);
        }

        case String:
            return identifierSize + sizedPayloadLength(payload, available, "string property");

        case Binary:
            return identifierSize + sizedPayloadLength(payload, available, "binary data property");

        case NameValue:
        {
            const auto nameLength = sizedPayloadLength(payload, available, "user property name");
            const auto valueLength = sizedPayloadLength(payload + nameLength, available - nameLength,
                                                        "user property value");
            return identifierSize + nameLength + valueLength;
        }
    }
    throw Exception("Unknown message property type");
}

size_t MessageProperties::encodedIntegerLength(const Property property, const int64_t value)
{
    using enum PropertyValueType;
    switch (PropertyValueTypes::getValueType(property))
    {
        case Uint8:
            return identifierSize + 1;
        case Uint16:
            return identifierSize + 2;
        case Uint32:
            return identifierSize + 4;
        case VarInt:
            return identifierSize + VariableLength::bytes(static_cast<uint64_t>(value));
        default:
            throw Exception("Property " + propertyToString(property) + " does not hold a number");
    }
}

const MessageProperties::Entry* MessageProperties::findEntry(const Property property) const
{
    for (const auto& entry: m_index)
    {
        if (entry.id == property)
        {
            return &entry;
        }
    }
    return nullptr;
}

size_t MessageProperties::findEntryPosition(const Property property) const
{
    for (size_t position = 0; position < m_index.size(); ++position)
    {
        if (m_index[position].id == property)
        {
            return position;
        }
    }
    return m_index.size();
}

int64_t MessageProperties::decodeInteger(const Entry& entry) const
{
    const auto* payload = m_data.data() + entry.offset + identifierSize;

    using enum PropertyValueType;
    switch (PropertyValueTypes::getValueType(entry.id))
    {
        case Uint8:
            return *payload;
        case Uint16:
            return readUint16(payload);
        case Uint32:
            return static_cast<int64_t>(readUint32(payload));
        case VarInt:
        {
            auto* cursor = const_cast<uint8_t*>(payload);
            return static_cast<int64_t>(
                VariableLength::read(cursor, static_cast<int>(entry.length - identifierSize)));
        }
        default:
            throw Exception("Property " + propertyToString(entry.id) + " does not hold a number");
    }
}

string_view MessageProperties::decodeString(const Entry& entry) const
{
    const auto* payload = m_data.data() + entry.offset + identifierSize;
    const auto  length = readUint16(payload);
    return {bit_cast<const char*>(payload + stringLengthSize), length};
}

void MessageProperties::decodeUserProperty(const Entry& entry, string_view& name, string_view& value) const
{
    const auto* payload = m_data.data() + entry.offset + identifierSize;
    const auto  nameLength = readUint16(payload);
    name = {bit_cast<const char*>(payload + stringLengthSize), nameLength};

    const auto* valueStart = payload + stringLengthSize + nameLength;
    const auto  valueLength = readUint16(valueStart);
    value = {bit_cast<const char*>(valueStart + stringLengthSize), valueLength};
}

void MessageProperties::appendInteger(const Property property, const int64_t value)
{
    const auto length = encodedIntegerLength(property, value);
    const auto offset = m_data.size();
    auto*      target = m_data.extend(length);

    *target = static_cast<uint8_t>(property);

    using enum PropertyValueType;
    switch (PropertyValueTypes::getValueType(property))
    {
        case Uint8:
            target[identifierSize] = static_cast<uint8_t>(value);
            break;
        case Uint16:
            writeUint16(target + identifierSize, static_cast<uint16_t>(value));
            break;
        case Uint32:
            writeUint32(target + identifierSize, static_cast<uint32_t>(value));
            break;
        case VarInt:
        {
            auto* cursor = target + identifierSize;
            VariableLength::write(static_cast<uint64_t>(value), cursor);
            break;
        }
        default:
            throw Exception("Property " + propertyToString(property) + " does not hold a number");
    }

    m_index.push_back({property, static_cast<uint32_t>(offset), static_cast<uint32_t>(length)});
}

void MessageProperties::appendString(const Property property, const string_view value)
{
    if (value.size() > 0xFFFF)
    {
        throw Exception("Property " + propertyToString(property) + " value too long");
    }
    const auto length = identifierSize + stringLengthSize + value.size();
    const auto offset = m_data.size();
    auto*      target = m_data.extend(length);

    *target = static_cast<uint8_t>(property);
    writeUint16(target + identifierSize, static_cast<uint16_t>(value.size()));
    memcpy(target + identifierSize + stringLengthSize, value.data(), value.size());

    m_index.push_back({property, static_cast<uint32_t>(offset), static_cast<uint32_t>(length)});
}

void MessageProperties::eraseEntry(const size_t position)
{
    const auto entry = m_index[position];
    m_data.erase(entry.offset, entry.length);
    m_index.erase(position, 1);

    // Everything that used to sit behind the hole has moved forward by its width. The index is the
    // only record of where a property is, so it is the only thing that has to be repaired - there
    // are no tombstones to skip and no dead bytes left behind.
    for (auto& later: m_index)
    {
        if (later.offset > entry.offset)
        {
            later.offset -= entry.length;
        }
    }
}

bool MessageProperties::getProperty(const Property property, int64_t& value) const
{
    const auto* entry = findEntry(property);
    if (entry == nullptr)
    {
        return false;
    }
    value = decodeInteger(*entry);
    return true;
}

bool MessageProperties::getProperty(const Property property, string_view& value) const
{
    const auto* entry = findEntry(property);
    if (entry == nullptr)
    {
        return false;
    }
    value = decodeString(*entry);
    return true;
}

bool MessageProperties::takeProperty(const Property property, int64_t& value)
{
    const auto position = findEntryPosition(property);
    if (position == m_index.size())
    {
        return false;
    }
    value = decodeInteger(m_index[position]);
    eraseEntry(position);
    return true;
}

void MessageProperties::setProperty(const Property property, const int64_t value)
{
    if (property == SubscriptionIdentifier)
    {
        // Repeatable, like a user property: a message delivered through two matching subscriptions
        // carries both identifiers. Replacing would keep only the last.
        (void) addSubscriptionId(static_cast<uint32_t>(value));
        return;
    }

    if (const auto position = findEntryPosition(property);
        position != m_index.size())
    {
        // The common case by far: a property is overwritten with a value of its own type, so the
        // encoding is the same width and the bytes can be replaced where they lie. Only a variable
        // length integer can change width, and then the old bytes have to go first.
        if (const auto entry = m_index[position];
            entry.length == encodedIntegerLength(property, value))
        {
            auto* target = m_data.data() + entry.offset;
            using enum PropertyValueType;
            switch (PropertyValueTypes::getValueType(property))
            {
                case Uint8:
                    target[identifierSize] = static_cast<uint8_t>(value);
                    break;
                case Uint16:
                    writeUint16(target + identifierSize, static_cast<uint16_t>(value));
                    break;
                case Uint32:
                    writeUint32(target + identifierSize, static_cast<uint32_t>(value));
                    break;
                case VarInt:
                {
                    auto* cursor = target + identifierSize;
                    VariableLength::write(static_cast<uint64_t>(value), cursor);
                    break;
                }
                default:
                    throw Exception("Property " + propertyToString(property) + " does not hold a number");
            }
            if (property == MessageExpiryInterval)
            {
                setExpirationTimeStamp(DateTime::Now() + chrono::seconds(value));
            }
            return;
        }
        eraseEntry(position);
    }

    appendInteger(property, value);
    if (property == MessageExpiryInterval)
    {
        setExpirationTimeStamp(DateTime::Now() + chrono::seconds(value));
    }
}

void MessageProperties::setProperty(const Property property, const string_view& value)
{
    using enum PropertyValueType;
    if (const auto valueType = PropertyValueTypes::getValueType(property);
        valueType != String && valueType != Binary)
    {
        // The maps took this silently, because a string went into the string map whatever the
        // property was declared to hold. Wire bytes cannot: a byte property is one byte, and a
        // string written in its place is read back as something else entirely.
        throw Exception(propertyToString(property) + " holds a number, not text");
    }

    if (const auto position = findEntryPosition(property);
        position != m_index.size())
    {
        eraseEntry(position);
    }
    appendString(property, value);
}

bool MessageProperties::addSubscriptionId(const uint32_t subscriptionId)
{
    // A set deduplicated these; the block has to be asked. There are never many, and a message
    // that carries none - which is nearly all of them - pays one comparison per property.
    for (const auto& entry: m_index)
    {
        if (entry.id == SubscriptionIdentifier &&
            decodeInteger(entry) == static_cast<int64_t>(subscriptionId))
        {
            return false;
        }
    }
    appendInteger(SubscriptionIdentifier, subscriptionId);
    return true;
}

void MessageProperties::setSubscriptionIds(const set<uint32_t>& subscriptionIds)
{
    removeProperty(SubscriptionIdentifier, PropertyType::Integer);
    for (const auto subscriptionId: subscriptionIds)
    {
        (void) addSubscriptionId(subscriptionId);
    }
}

set<uint32_t> MessageProperties::getSubscriptionIds() const
{
    set<uint32_t> subscriptionIds;
    for (const auto& entry: m_index)
    {
        if (entry.id == SubscriptionIdentifier)
        {
            subscriptionIds.insert(static_cast<uint32_t>(decodeInteger(entry)));
        }
    }
    return subscriptionIds;
}

void MessageProperties::setUserProperty(const string_view name, const string_view value)
{
    if (name.size() > 0xFFFF || value.size() > 0xFFFF)
    {
        throw Exception("Property name or value too long");
    }

    const auto length = identifierSize + stringLengthSize + name.size() + stringLengthSize + value.size();
    const auto offset = m_data.size();
    auto*      target = m_data.extend(length);

    *target = static_cast<uint8_t>(UserProperty);
    auto* cursor = target + identifierSize;
    writeUint16(cursor, static_cast<uint16_t>(name.size()));
    cursor += stringLengthSize;
    memcpy(cursor, name.data(), name.size());
    cursor += name.size();
    writeUint16(cursor, static_cast<uint16_t>(value.size()));
    cursor += stringLengthSize;
    memcpy(cursor, value.data(), value.size());

    m_index.push_back({UserProperty, static_cast<uint32_t>(offset), static_cast<uint32_t>(length)});
}

void MessageProperties::setUserProperties(const UserMessageProperties& userProperties)
{
    removeProperty(UserProperty, PropertyType::User);
    for (const auto& [name, value]: userProperties)
    {
        setUserProperty(name, value);
    }
}

UserMessageProperties MessageProperties::getUserProperties() const
{
    UserMessageProperties properties;
    forEachUserProperty(
        [&properties](const string_view name, const string_view value)
        {
            properties.emplace(name, value);
        });
    return properties;
}

void MessageProperties::forEachUserProperty(const UserPropertyVisitor& visitor) const
{
    for (const auto& entry: m_index)
    {
        if (entry.id != UserProperty)
        {
            continue;
        }
        string_view name;
        string_view value;
        decodeUserProperty(entry, name, value);
        visitor(name, value);
    }
}

string_view MessageProperties::getUserProperty(const string_view name) const
{
    for (const auto& entry: m_index)
    {
        if (entry.id != UserProperty)
        {
            continue;
        }
        string_view propertyName;
        string_view propertyValue;
        decodeUserProperty(entry, propertyName, propertyValue);
        if (propertyName == name)
        {
            return propertyValue;
        }
    }
    return {};
}

LatencyTrace* MessageProperties::getLatencyTrace(const bool autoCreate)
{
    for (const auto& entry: m_index)
    {
        if (entry.id != UserProperty)
        {
            continue;
        }
        string_view name;
        string_view value;
        decodeUserProperty(entry, name, value);
        if (name == latencyPropertyName && value.size() == sizeof(LatencyTrace))
        {
            // Into the block itself, as it used to be into the map's string: the caller stamps
            // phases through this pointer, so it has to be the stored bytes and not a copy. It
            // stops being valid at the next change to these properties.
            return bit_cast<LatencyTrace*>(const_cast<char*>(value.data()));
        }
    }

    if (!autoCreate)
    {
        return nullptr;
    }

    const LatencyTrace latencyTrace;
    setUserProperty(latencyPropertyName,
                    {bit_cast<const char*>(&latencyTrace), sizeof(latencyTrace)});
    return getLatencyTrace(false);
}

void MessageProperties::setLatencyTrace(const LatencyTrace* latencyTrace)
{
    if (getLatencyTrace(false) != nullptr)
    {
        return; // try_emplace kept the first one, and so does this.
    }
    setUserProperty(latencyPropertyName,
                    {bit_cast<const char*>(latencyTrace), sizeof(LatencyTrace)});
}

void MessageProperties::removeProperty(const Property property, const PropertyType propertyType)
{
    using enum PropertyType;
    const auto wanted = propertyType == User ? UserProperty : property;

    // Backwards, so that removing several does not disturb the positions still to be examined.
    for (size_t position = m_index.size(); position > 0; --position)
    {
        if (m_index[position - 1].id == wanted)
        {
            eraseEntry(position - 1);
        }
    }
}

void MessageProperties::removeUserProperty(const string_view name)
{
    for (size_t position = m_index.size(); position > 0; --position)
    {
        const auto& entry = m_index[position - 1];
        if (entry.id != UserProperty)
        {
            continue;
        }
        string_view propertyName;
        string_view propertyValue;
        decodeUserProperty(entry, propertyName, propertyValue);
        if (propertyName == name)
        {
            eraseEntry(position - 1);
        }
    }
}

void MessageProperties::clear()
{
    m_data.clear();
    m_index.clear();
    m_expirationTimeStamp = DateTime();
}

void MessageProperties::read(const uint8_t* data, const size_t length)
{
    clear();
    if (length == 0)
    {
        return;
    }

    // The whole block in one copy, and then one walk to say where everything is. Nothing is
    // decoded here: a message whose properties are never read costs exactly this memcpy.
    m_data.assign(data, length);

    // Bounded by the block as it stands, not by the length that came in: pulling a subscription
    // identifier out shortens it, and reading to the original length walked off the end.
    size_t offset = 0;
    while (offset < m_data.size())
    {
        const auto remaining = m_data.size() - offset;
        if (remaining < identifierSize + 1)
        {
            throw Exception("Incorrect message property decode");
        }
        const auto property = static_cast<Property>(m_data[offset]);
        const auto entryLength = payloadLength(property, m_data.data() + offset + identifierSize,
                                               remaining - identifierSize);
        if (offset + entryLength > m_data.size())
        {
            throw Exception("Incorrect message property decode");
        }

        m_index.push_back({property, static_cast<uint32_t>(offset), static_cast<uint32_t>(entryLength)});
        offset += entryLength;
    }

    if (const auto* entry = findEntry(MessageExpiryInterval);
        entry != nullptr)
    {
        setExpirationTimeStamp(DateTime::Now() + chrono::seconds(decodeInteger(*entry)));
    }
}

void MessageProperties::write(uint8_t*& tail, const SubscriptionIdSet& subscriptionIds) const
{
    auto* start = tail;
    if (!m_data.empty())
    {
        memcpy(tail, m_data.data(), m_data.size());
        tail += m_data.size();
    }

    // The one property that cannot be copied as it stands: what goes out is the time left, not the
    // interval that was asked for. Rounded up for the same reason it is read that way - truncating
    // costs the message up to a second on every hop, and a message that crosses a bridge makes
    // several.
    if (const auto* entry = findEntry(MessageExpiryInterval);
        entry != nullptr)
    {
        const auto remaining = getExpirationTimeStamp() - DateTime::Now();
        const auto seconds = remaining > DateTime::duration::zero()
                                 ? static_cast<uint32_t>(chrono::ceil<chrono::seconds>(remaining).count())
                                 : 0;
        writeUint32(start + entry->offset + identifierSize, seconds);
    }

    // The message's own subscription identifiers went out with the block. These are the delivery's,
    // which belong to this subscriber and not to the message.
    for (const auto& subscriptionId: subscriptionIds)
    {
        writeVariableLengthIntegerProperty(SubscriptionIdentifier, tail, subscriptionId);
    }
}

uint32_t MessageProperties::expectedSize() const
{
    // The block, whole. What the delivery adds is counted by the caller, which is the only place
    // that knows it.
    return static_cast<uint32_t>(m_data.size());
}

void MessageProperties::merge(const MessageProperties& other)
{
    for (const auto& entry: other.m_index)
    {
        // Copied as bytes, which is both the cheapest way and the exact one: a user property that
        // appears twice arrives twice, in its order, and nothing is re-encoded on the way. The
        // same for a subscription identifier, which a message can carry more than one of.
        if (entry.id != UserProperty && entry.id != SubscriptionIdentifier)
        {
            if (const auto position = findEntryPosition(entry.id);
                position != m_index.size())
            {
                eraseEntry(position);
            }
        }

        const auto offset = m_data.size();
        m_data.append(other.m_data.data() + entry.offset, entry.length);
        m_index.push_back({entry.id, static_cast<uint32_t>(offset), entry.length});
    }

    if (const auto* entry = findEntry(MessageExpiryInterval);
        entry != nullptr)
    {
        setExpirationTimeStamp(DateTime::Now() + chrono::seconds(decodeInteger(*entry)));
    }
}

ReasonCode MessageProperties::validate(const Property property, const string_view& value) const
{
    if (property == ResponseTopic && value.find_first_of("#+") != string::npos)
    {
        return ReasonCode::ProtocolError;
    }
    return ReasonCode::Success;
}

ReasonCode MessageProperties::validate() const
{
    for (const auto& entry: m_index)
    {
        using enum PropertyValueType;
        if (const auto valueType = PropertyValueTypes::getValueType(entry.id);
            valueType != String && valueType != Binary)
        {
            continue;
        }
        if (const auto result = validate(entry.id, decodeString(entry));
            result != ReasonCode::Success)
        {
            return result;
        }
    }
    return ReasonCode::Success;
}

string MessageProperties::toString() const
{
    ostringstream text;

    const auto shortened = [](const string_view value)
    {
        constexpr size_t longest = 20;
        constexpr size_t kept = 17;
        return value.length() > longest ? string(value.substr(0, kept)) + "..." : string(value);
    };

    for (const auto& entry: m_index)
    {
        using enum PropertyValueType;
        switch (PropertyValueTypes::getValueType(entry.id))
        {
            case String:
            case Binary:
                text << " " << propertyToString(entry.id) << "=[" << shortened(decodeString(entry)) << "]";
                break;

            case NameValue:
            {
                string_view name;
                string_view value;
                decodeUserProperty(entry, name, value);
                text << " " << name << "=[" << shortened(value) << "]";
                break;
            }

            default:
                // Listed together at the end instead, as they were when they lived in a set.
                if (entry.id != SubscriptionIdentifier)
                {
                    text << " " << propertyToString(entry.id) << "=" << decodeInteger(entry);
                }
                break;
        }
    }

    if (const auto subscriptionIds = getSubscriptionIds();
        !subscriptionIds.empty())
    {
        text << " SubscriptionIds=[";
        auto first = true;
        for (const auto& subscriptionId: subscriptionIds)
        {
            if (first)
            {
                first = false;
            }
            else
            {
                text << ",";
            }
            text << subscriptionId;
        }
        text << "]";
    }

    return text.str();
}

XMQ_EXPORT ostream& operator<<(ostream& outputStream, const IMessageProperties& properties)
{
    return outputStream << properties.toString();
}

string UserMessageProperties::toString() const
{
    stringstream ss;

    auto first = true;
    for (const auto& [key, value]: *this)
    {
        if (first)
        {
            first = false;
        }
        else
        {
            ss << ", ";
        }
        ss << key << "=" << value;
    }

    return ss.str();
}

XMQ_EXPORT void xmq::setPropertyFromText(MessageProperties& properties, const std::string_view name,
                                         const std::string_view value)
{
    using enum mqtt::PropertyValueType;

    const auto property = propertyFromName(name);
    if (!property)
    {
        throw Exception("\"" + std::string(name) + "\" is not an MQTT5 property");
    }

    switch (mqtt::PropertyValueTypes::getValueType(*property))
    {
        case NameValue:
        {
            const auto separator = value.find('=');
            if (separator == std::string_view::npos)
            {
                throw Exception("A user property needs a name and a value: user-property name=value");
            }
            properties.setUserProperty(value.substr(0, separator), value.substr(separator + 1));
            break;
        }

        case String:
        case Binary:
            properties.setProperty(*property, value);
            break;

        case Uint8:
        case Uint16:
        case Uint32:
        case VarInt:
        {
            const sptk::String text {std::string(value)};
            if (!text.matches("^\\d+$"))
            {
                throw Exception(propertyToOptionName(*property) + " takes a number, not \"" +
                                std::string(value) + "\"");
            }
            properties.setProperty(*property, static_cast<int64_t>(text.toInt()));
            break;
        }
    }
}
