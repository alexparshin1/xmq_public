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

#include "ServerPerformanceTests.h"
#include "test/ServerTests/ExternalClient/ExternalClient.h"

/*
  To enable SSL performance tests, define SSL_PERFORMANCE_TESTS.
  To enable Mosquitto performance tests, define MOSQUITTO_PERFORMANCE_TESTS
*/

using namespace std;
using namespace sptk;
using namespace xmq;

INSTANTIATE_TEST_SUITE_P(
    ProtocolVersions, XMQ_ServerPerformanceTests,
    testing::Values(ProtocolVersion::MqttV31, ProtocolVersion::MqttV311, ProtocolVersion::MqttV5));

TEST_P(XMQ_ServerPerformanceTests, SharedSubscription_Scaling_QOS1)
{
    const auto protocolVersion = GetParam();

    for (const size_t subscriberCount: {1U, 2U, 4U, 8U})
    {
        testSharedSubscriptionScaling(TestTcpPortNumber, TestMessageCount, protocolVersion, subscriberCount);
    }
}

TEST_P(XMQ_ServerPerformanceTests, PublishBulk_QOS0)
{
    const auto protocolVersion = GetParam();

    testPublishPerformance("xmq", TestTcpPortNumber, TestMessageCount, Qos::Qos0,
                           protocolVersion, ExternalClient::EncryptionMode::None, false);

#ifdef MOSQUITTO_PERFORMANCE_TESTS
    testPublishPerformance("mosquitto", MqttTcpPortNumber, TestMessageCount, Qos::Qos0, TestSenderCount,
                           protocolVersion, ExternalClient::EncryptionMode::None);
#endif

#ifdef SSL_PERFORMANCE_TESTS
    testPerformance("xmq", TestSslPortNumber, messageCount, Qos::Qos0, senderCount, protocolVersion, ExternalClient::EncryptionMode::Tls);
    testPerformance("mosquitto", 8884, messageCount, Qos::Qos0, senderCount, protocolVersion, ExternalClient::EncryptionMode::Tls);
#endif
}

TEST_P(XMQ_ServerPerformanceTests, PublishBulk_QOS1)
{
    const auto protocolVersion = GetParam();

    testPublishPerformance("xmq", TestTcpPortNumber, TestMessageCount, Qos::Qos1,
                           protocolVersion, ExternalClient::EncryptionMode::None, false);

#ifdef MOSQUITTO_PERFORMANCE_TESTS
    testPublishPerformance("mosquitto", MqttTcpPortNumber, TestMessageCount, Qos::Qos1, TestSenderCount,
                           protocolVersion, ExternalClient::EncryptionMode::None);
#endif

#ifdef SSL_PERFORMANCE_TESTS
    testPerformance("mosquitto", 8884, messageCount, Qos::Qos1, senderCount, protocolVersion, ExternalClient::EncryptionMode::Tls);
    testPerformance("xmq", TestSslPortNumber, messageCount, Qos::Qos1, senderCount, protocolVersion, ExternalClient::EncryptionMode::Tls);
#endif
}

TEST_P(XMQ_ServerPerformanceTests, PublishBulk_QOS2)
{
    const auto protocolVersion = GetParam();

    testPublishPerformance("xmq", TestTcpPortNumber, TestMessageCount, Qos::Qos2,
                           protocolVersion, ExternalClient::EncryptionMode::None, false);

#ifdef MOSQUITTO_PERFORMANCE_TESTS
    testPublishPerformance("mosquitto", MqttTcpPortNumber, TestMessageCount, Qos::Qos2, TestSenderCount,
                           protocolVersion, ExternalClient::EncryptionMode::None);
#endif

#ifdef SSL_PERFORMANCE_TESTS
    testPerformance("mosquitto", 8884, messageCount, Qos::Qos2, senderCount, protocolVersion, ExternalClient::EncryptionMode::Tls);
    testPerformance("xmq", TestSslPortNumber, messageCount, Qos::Qos2, senderCount, protocolVersion, ExternalClient::EncryptionMode::Tls);
#endif
}
