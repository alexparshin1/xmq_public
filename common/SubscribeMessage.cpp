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

#include "SubscribeMessage.h"

using namespace std;
using namespace xmq;

SubscribeMessage::SubscribeMessage()
    : Message(Type::Subscribe)
{
    setQos(Qos::Qos1);
}

SubscribeMessage::SubscribeMessage(Destinations destinations)
    : Message(Type::Subscribe)
    , m_destinations(std::move(destinations))
{
    setQos(Qos::Qos1);
}

string SubscribeMessage::toString() const
{
    std::stringstream str;
    const auto        messageId = static_cast<int>(getId());
    str << name();

    if (messageId != 0)
    {
        str << " id=" << static_cast<int>(getId());
    }

    str << " destinations=[";
    sptk::Strings destinationStrings;
    for (const auto& destination: m_destinations)
    {
        destinationStrings.push_back(destination.toString());
    }
    str << destinationStrings.join(", ") << "]";

    if (m_subscriptionId != 0)
    {
        str << " subscriptionRecordId=" << m_subscriptionId;
    }

    return str.str();
}
