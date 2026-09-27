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

#include "ServerTests.h"

using namespace std;
using namespace sptk;
using namespace xmq;

INSTANTIATE_TEST_SUITE_P(
    ProtocolVersions, XMQ_ServerTests,
    testing::Values(ProtocolVersion::MqttV31, ProtocolVersion::MqttV311, ProtocolVersion::MqttV5));

XMQ_ServerTests::TestNames XMQ_ServerTests::makeTestNames()
{
    static size_t testIndex = 1;

    TestNames testNames;

    testNames.m_publisherClientId = "publisher-" + to_string(testIndex);
    testNames.m_subscriberClientId = "subscriber-" + to_string(testIndex);
    testNames.m_topicName = "topic/" + to_string(testIndex);

    ++testIndex;

    return testNames;
}

void XMQ_ServerTests::printTiming(const std::string_view operationName, const size_t messageCount, const double seconds)
{
    constexpr auto digits = 2;
    stringstream   out;
    out << operationName << " " << messageCount
        << " messages for " << setprecision(digits) << seconds << "s (";
    if (constexpr auto limit = 5000;
        messageCount < limit)
    {
        out << fixed << static_cast<double>(messageCount) / seconds << " msg/s)";
    }
    else
    {
        out << fixed << static_cast<double>(messageCount) / seconds / 1000. << "K msg/s)";
    }
    COUT(out.str());
}
