#include "CClientSession.h"
using namespace std;
using namespace sptk;
using namespace xmq;

const Strings& CClientSession::fieldNames(const WSFieldIndex::Group group)
{
    static const Strings _fieldNames { "node_name", "client_id", "ip_address", "protocol_version", "connected", "subscriptions" };
    static const Strings _elementNames { "node_name", "client_id", "ip_address", "protocol_version", "connected", "subscriptions" };
    static const Strings _attributeNames;

    switch (group) {
        case WSFieldIndex::Group::ELEMENTS: return _elementNames;
        case WSFieldIndex::Group::ATTRIBUTES: return _attributeNames;
        default: break;
    }

    return _fieldNames;
}

CClientSession::CClientSession(const char* elementName, const bool optional)
: WSComplexType(elementName, optional)
{
    setElements(fieldNames(WSFieldIndex::Group::ELEMENTS), {&m_node_name, &m_client_id, &m_ip_address, &m_protocol_version, &m_connected, &m_subscriptions});
}

CClientSession::CClientSession(const CClientSession& other)
: WSComplexType(other),
  m_node_name(other.m_node_name),
  m_client_id(other.m_client_id),
  m_ip_address(other.m_ip_address),
  m_protocol_version(other.m_protocol_version),
  m_connected(other.m_connected),
  m_subscriptions(other.m_subscriptions)
{
    setElements(fieldNames(WSFieldIndex::Group::ELEMENTS), {&m_node_name, &m_client_id, &m_ip_address, &m_protocol_version, &m_connected, &m_subscriptions});
}

CClientSession::CClientSession(CClientSession&& other) noexcept
: WSComplexType(std::move(other)),
  m_node_name(std::move(other.m_node_name)),
  m_client_id(std::move(other.m_client_id)),
  m_ip_address(std::move(other.m_ip_address)),
  m_protocol_version(std::move(other.m_protocol_version)),
  m_connected(std::move(other.m_connected)),
  m_subscriptions(std::move(other.m_subscriptions))
{
    setElements(fieldNames(WSFieldIndex::Group::ELEMENTS), {&m_node_name, &m_client_id, &m_ip_address, &m_protocol_version, &m_connected, &m_subscriptions});
}

void CClientSession::checkRestrictions() const
{
    // Check 'required' restrictions
    m_client_id.throwIfNull("ClientSession.client_id");
    m_protocol_version.throwIfNull("ClientSession.protocol_version");
}

