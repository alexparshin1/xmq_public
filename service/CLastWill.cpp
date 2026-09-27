#include "CLastWill.h"
using namespace std;
using namespace sptk;
using namespace xmq;

const Strings& CLastWill::fieldNames(const WSFieldIndex::Group group)
{
    static const Strings _fieldNames { "topic", "message", "retain", "qos", "will_delay", "message_properties" };
    static const Strings _elementNames { "topic", "message", "retain", "qos", "will_delay", "message_properties" };
    static const Strings _attributeNames;

    switch (group) {
        case WSFieldIndex::Group::ELEMENTS: return _elementNames;
        case WSFieldIndex::Group::ATTRIBUTES: return _attributeNames;
        default: break;
    }

    return _fieldNames;
}

CLastWill::CLastWill(const char* elementName, const bool optional)
: WSComplexType(elementName, optional)
{
    setElements(fieldNames(WSFieldIndex::Group::ELEMENTS), {&m_topic, &m_message, &m_retain, &m_qos, &m_will_delay, &m_message_properties});
}

CLastWill::CLastWill(const CLastWill& other)
: WSComplexType(other),
  m_topic(other.m_topic),
  m_message(other.m_message),
  m_retain(other.m_retain),
  m_qos(other.m_qos),
  m_will_delay(other.m_will_delay),
  m_message_properties(other.m_message_properties)
{
    setElements(fieldNames(WSFieldIndex::Group::ELEMENTS), {&m_topic, &m_message, &m_retain, &m_qos, &m_will_delay, &m_message_properties});
}

CLastWill::CLastWill(CLastWill&& other) noexcept
: WSComplexType(std::move(other)),
  m_topic(std::move(other.m_topic)),
  m_message(std::move(other.m_message)),
  m_retain(std::move(other.m_retain)),
  m_qos(std::move(other.m_qos)),
  m_will_delay(std::move(other.m_will_delay)),
  m_message_properties(std::move(other.m_message_properties))
{
    setElements(fieldNames(WSFieldIndex::Group::ELEMENTS), {&m_topic, &m_message, &m_retain, &m_qos, &m_will_delay, &m_message_properties});
}

void CLastWill::checkRestrictions() const
{


    // Check 'required' restrictions
    m_topic.throwIfNull("LastWill.topic");
    m_message.throwIfNull("LastWill.message");
}

