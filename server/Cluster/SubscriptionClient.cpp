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

#include "SubscriptionClient.h"

#include "common/mqtt/PublishMessage.h"
#include "server/Server.h"

using namespace std;
using namespace sptk;
using namespace xmq;
using namespace cluster;

SubscriptionClient::SubscriptionClient(Server* server)
    : m_server(server)
    , m_requestAttachNode(m_server->getTopic("$CLUSTER/request/attach_node"))
{
}

const string& SubscriptionClient::getClientId() const
{
    static string clientId("cluster");
    return clientId;
}

string_view SubscriptionClient::getClientIdUnlocked() const
{
    static string clientId("cluster");
    return clientId;
}

string_view SubscriptionClient::getUsername() const
{
    static string username("cluster");
    return username;
}

int64_t SubscriptionClient::recordId() const
{
    return 0;
}

SStorage SubscriptionClient::storage() const
{
    return nullptr;
}

MessageId SubscriptionClient::postMessage(const SMessage& message, Qos, const SubscriptionIdSet&, bool)
{
    executeMessageCallback(message);

    if (const auto request = dynamic_pointer_cast<PublishMessage>(message);
        !request)
    {
        return message->getId();
    }

    return {};
}

void SubscriptionClient::onMessage(MessageCallback messageCallback)
{
    scoped_lock lock(m_mutex);
    m_messageCallback = std::move(messageCallback);
}

void SubscriptionClient::executeMessageCallback(const SMessage& message) const
{
    scoped_lock lock(m_mutex);
    if (m_messageCallback)
    {
        m_messageCallback(message);
    }
}

string SubscriptionClient::bridgeOrigin() const
{
    // Not a constexpr string: MSVC's debug build gives std::string a container proxy, which makes
    // the object non-constant and refuses to compile it - so the same line built in Release and
    // failed in Debug. An empty string needs no storage to name anyway.
    return {};
}
