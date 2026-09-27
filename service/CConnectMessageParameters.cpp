#include "CConnectMessageParameters.h"
using namespace std;
using namespace sptk;
using namespace xmq;

const Strings& CConnectMessageParameters::fieldNames(const WSFieldIndex::Group group)
{
    static const Strings _fieldNames { "client_id", "username", "protocol_version", "clean_session", "keep_alive_sec", "last_will" };
    static const Strings _elementNames { "client_id", "username", "protocol_version", "clean_session", "keep_alive_sec", "last_will" };
    static const Strings _attributeNames;

    switch (group) {
        case WSFieldIndex::Group::ELEMENTS: return _elementNames;
        case WSFieldIndex::Group::ATTRIBUTES: return _attributeNames;
        default: break;
    }

    return _fieldNames;
}

CConnectMessageParameters::CConnectMessageParameters(const char* elementName, const bool optional)
: WSComplexType(elementName, optional)
{
    setElements(fieldNames(WSFieldIndex::Group::ELEMENTS), {&m_client_id, &m_username, &m_protocol_version, &m_clean_session, &m_keep_alive_sec, &m_last_will});
}

CConnectMessageParameters::CConnectMessageParameters(const CConnectMessageParameters& other)
: WSComplexType(other),
  m_client_id(other.m_client_id),
  m_username(other.m_username),
  m_protocol_version(other.m_protocol_version),
  m_clean_session(other.m_clean_session),
  m_keep_alive_sec(other.m_keep_alive_sec),
  m_last_will(other.m_last_will)
{
    setElements(fieldNames(WSFieldIndex::Group::ELEMENTS), {&m_client_id, &m_username, &m_protocol_version, &m_clean_session, &m_keep_alive_sec, &m_last_will});
}

CConnectMessageParameters::CConnectMessageParameters(CConnectMessageParameters&& other) noexcept
: WSComplexType(std::move(other)),
  m_client_id(std::move(other.m_client_id)),
  m_username(std::move(other.m_username)),
  m_protocol_version(std::move(other.m_protocol_version)),
  m_clean_session(std::move(other.m_clean_session)),
  m_keep_alive_sec(std::move(other.m_keep_alive_sec)),
  m_last_will(std::move(other.m_last_will))
{
    setElements(fieldNames(WSFieldIndex::Group::ELEMENTS), {&m_client_id, &m_username, &m_protocol_version, &m_clean_session, &m_keep_alive_sec, &m_last_will});
}

void CConnectMessageParameters::checkRestrictions() const
{
    // Check 'required' restrictions
    m_client_id.throwIfNull("ConnectMessageParameters.client_id");
    m_username.throwIfNull("ConnectMessageParameters.username");
}

