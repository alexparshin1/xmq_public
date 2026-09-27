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

#include "ConnectMessage.h"

using namespace std;
using namespace sptk;
using namespace xmq;

ConnectMessageParameters::ConnectMessageParameters(const ConnectCredentials& credentials, const SLastWillInfo& lastWill, const bool cleanSession,
                                                   const ProtocolVersion protocolVersion, const uint16_t keepAliveSeconds)
    : ConnectCredentials(credentials)
    , m_protocolVersion(protocolVersion)
    , m_cleanSession(cleanSession ? 1 : 0)
    , m_keepAliveSec(keepAliveSeconds)
    , m_lastWill(lastWill)
{
}

ConnectMessage::ConnectMessage()
    : Message(Type::Connect)
    , m_parameters(make_shared<ConnectMessageParameters>())
{
}

ConnectMessage::ConnectMessage(const ConnectCredentials& credentials, const SLastWillInfo& lastWill, const bool cleanSession,
                               const ProtocolVersion protocolVersion, const uint16_t keepAliveSeconds)
    : Message(Type::Connect)
    , m_parameters(make_shared<ConnectMessageParameters>(credentials, lastWill, cleanSession, protocolVersion, keepAliveSeconds))
{
}

const string& ConnectMessage::getUsername() const
{
    return m_parameters->getUsername();
}

void ConnectMessage::setUsername(const std::string_view username) const
{
    m_parameters->setUsername(username);
}

const string& ConnectMessage::getPassword() const
{
    return m_parameters->getPassword();
}

void ConnectMessage::setPassword(const std::string_view password) const
{
    m_parameters->setPassword(password);
}

std::string ConnectMessage::toString() const
{
    std::stringstream str;
    str << name() << " client=" << getClientId()
        << " user=" << getUsername()
        << " session=" << (getParameters()->m_cleanSession == 1 ? "clean" : "persistent");
    if (getProperties())
    {
        str << *getProperties();
    }

    return str.str();
}
