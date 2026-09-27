/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#pragma once

#include "server/Server.h"

namespace xmq {

class XMQ_EXPORT XMQ_ServerTestsLink
{
public:
    static void linkTests()
    {
        linkBridgeTests();
        linkClusterTests();
        linkMessagePropertiesTests();
        linkPersitenceTests();
        linkPublishPropertiesTests();
        linkSubscriptionTests();
        linkSysTopicCounterTests();
        linkTopicAliasTests();
    }

private:
    static void linkBridgeTests();
    static void linkClusterTests();
    static void linkMessagePropertiesTests();
    static void linkPersitenceTests();
    static void linkPublishPropertiesTests();
    static void linkSubscriptionTests();
    static void linkSysTopicCounterTests();
    static void linkTopicAliasTests();
};

} // namespace xmq
