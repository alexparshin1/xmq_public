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

#include "common/DirectoryNames.h"
#include "server/SelfSignedCertificate.h"
#include "test/BridgeTests/BridgeTests.h"

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

/// The MQTT+SSL port the "other" node listens on, as the bridge fixture creates it.
constexpr uint16_t otherServerSslPort = ServerTests_Suite::TestSslPortNumber + 6;

/**
 * @brief A directory for the certificates this test makes, thrown away with it.
 */
filesystem::path temporaryCertificateDirectory()
{
    return filesystem::temp_directory_path() / ("xmq_encrypted_bridge_" + to_string(::getpid()));
}

/**
 * @brief The certificate the far node actually serves on its MQTT+SSL port.
 *
 * Read from the configuration rather than assumed: what makes the link verifiable is holding the
 * certificate the other end presents, and which file that is belongs to that node's settings.
 *
 * @return path of the certificate, or empty when that node serves none.
 */
filesystem::path certificateServedByOtherNode()
{
    const auto otherNode = ServerTests_Suite::server("other");
    if (!otherNode)
    {
        return {};
    }
    return otherNode->getSettings()->m_connections.m_ssl_keys.m_certfile.asString().c_str();
}

} // namespace

/**
 * @brief A bridge between two nodes over MQTT+SSL.
 *
 * The unencrypted case is covered by the tests beside this one. What this adds is the part that
 * makes an encrypted link worth having: the far node is verified against a certificate this node
 * was given, so that a link cannot be completed by whatever happens to answer on the address.
 */
class XMQ_EXPORT XMQ_EncryptedBridgeTests : public XMQ_BridgeTests
{
protected:
    void SetUp() override
    {
        XMQ_BridgeTests::SetUp();

        // Every other bridge off, on both nodes, for the length of this test. The suite gives
        // each of them one that carries "#" over an unencrypted link - including the far node,
        // whose bridge reaches back here on its own - and while any of those is running, a
        // message arriving proves nothing about the encrypted one.
        suspendBridges("primary");
        suspendBridges("other");

        m_certificateDirectory = temporaryCertificateDirectory();
        error_code errorCode;
        filesystem::remove_all(m_certificateDirectory, errorCode);
        filesystem::create_directories(m_certificateDirectory, errorCode);

        // A pair of this node's own, which is what it presents to the far end. Generated rather
        // than borrowed from the machine's certificates directory, which belongs to whatever is
        // installed there.
        ASSERT_TRUE(SelfSignedCertificate::create(clientCertificate(), clientPrivateKey(),
                                                  "bridge-client", m_description));

        // The far node's certificate, held here as the thing that vouches for it. A self-signed
        // certificate is its own authority, so no certificate authority has to exist for the
        // link to be verified - which is the arrangement between two brokers one administrator
        // runs.
        const auto served = certificateServedByOtherNode();
        ASSERT_FALSE(served.empty()) << "The far node serves no certificate to trust";
        ASSERT_TRUE(filesystem::exists(served)) << served.string();
        filesystem::copy_file(served, trustedPeerCertificate(),
                              filesystem::copy_options::overwrite_existing);
    }

    void TearDown() override
    {
        if (m_bridgeId != 0)
        {
            CBridge bridge;
            bridge.m_id = static_cast<int>(m_bridgeId);
            server("primary")->getSettings()->bridgeControl("remove", bridge);
        }

        // The suite's own bridges go back on both nodes, and the tests that come after this one
        // find them as they expect.
        restoreBridges("primary");
        restoreBridges("other");

        error_code errorCode;
        filesystem::remove_all(m_certificateDirectory, errorCode);

        XMQ_BridgeTests::TearDown();
    }

    [[nodiscard]] filesystem::path clientCertificate() const { return m_certificateDirectory / "bridge.crt"; }
    [[nodiscard]] filesystem::path clientPrivateKey() const { return m_certificateDirectory / "bridge.key"; }
    [[nodiscard]] filesystem::path trustedPeerCertificate() const { return m_certificateDirectory / "peer.crt"; }

    /**
     * @brief Add a bridge to the far node's encrypted port and wait for it to carry traffic.
     * @param verifyDepth       How far to follow the far node's certificate chain; 0 does not
     *                          verify it at all.
     * @param topicName         Topic the bridge carries.
     */
    void startEncryptedBridge(const int verifyDepth, const String& topicName)
    {
        CBridge bridge;
        bridge.m_node_name = "other-encrypted";
        bridge.m_enabled = true;
        bridge.m_host_port = "localhost:" + to_string(otherServerSslPort);
        bridge.m_username = "user";
        bridge.m_password = "secret";
        bridge.m_client_id = "xmq_bridge_encrypted";
        bridge.m_clean_session = true;
        bridge.m_mode = "in";
        bridge.m_encrypted = true;

        bridge.m_ssl_keys.m_certfile = clientCertificate().string();
        bridge.m_ssl_keys.m_keyfile = clientPrivateKey().string();
        bridge.m_ssl_keys.m_cafile = trustedPeerCertificate().string();
        bridge.m_ssl_keys.m_verify_depth = to_string(verifyDepth);

        CBridgeTopic topic;
        topic.m_pattern = topicName;
        topic.m_direction = "in";
        bridge.m_topics.push_back(topic);

        const auto bridgingServer = server("primary");
        ASSERT_TRUE(bridgingServer);
        m_bridgeId = bridgingServer->getSettings()->bridgeControl("add", bridge);
        bridgingServer->restartBridges();
    }

    /**
     * @brief Wait until every bridge on the bridging node is connected.
     * @param timeout           How long to wait.
     * @return true if they all connected in time.
     */
    static bool bridgesConnectedWithin(const chrono::seconds timeout)
    {
        const auto bridgingServer = ServerTests_Suite::server("primary");
        const auto deadline = DateTime::Now() + timeout;
        while (!bridgingServer->bridgesConnected() && DateTime::Now() < deadline)
        {
            this_thread::sleep_for(50ms);
        }
        return bridgingServer->bridgesConnected();
    }

    /**
     * @brief Take a node's bridges out of its configuration, remembering them.
     * @param nodeName          Node to suspend the bridges of.
     */
    void suspendBridges(const String& nodeName)
    {
        const auto node = server(nodeName);
        if (!node)
        {
            return;
        }

        auto& suspended = m_suspendedBridges[nodeName];
        for (const auto& bridge: node->getSettings()->m_bridges)
        {
            suspended.push_back(bridge);
        }
        for (const auto& bridge: suspended)
        {
            CBridge removal;
            removal.m_id = bridge.m_id.asInteger();
            node->getSettings()->bridgeControl("remove", removal);
        }
        node->restartBridges();
    }

    /**
     * @brief Put a node's bridges back.
     * @param nodeName          Node to restore the bridges of.
     */
    void restoreBridges(const String& nodeName)
    {
        const auto node = server(nodeName);
        if (!node)
        {
            return;
        }

        for (const auto& bridge: m_suspendedBridges[nodeName])
        {
            try
            {
                node->getSettings()->bridgeControl("add", bridge);
            }
            catch (const Exception&)
            {
                // Already present, which is the state this is trying to reach.
            }
        }
        m_suspendedBridges[nodeName].clear();
        node->restartBridges();
    }

    filesystem::path                m_certificateDirectory;
    String                          m_description;
    uint64_t                        m_bridgeId {0};
    map<String, vector<CBridge>>    m_suspendedBridges;
};

TEST_F(XMQ_EncryptedBridgeTests, carriesMessagesOverTls)
{
    // The clients first, because the topic they use is the one the bridge has to be told to
    // carry: a bridge configured for a different topic connects just as happily and carries
    // nothing, which looks exactly like a broken link.
    auto [subscriber, publisher, topicName] =
        createTestSubscriberAndPublisher(m_otherServerHost, m_xmqServerHost);

    startEncryptedBridge(1, topicName);
    ASSERT_TRUE(bridgesConnectedWithin(15s)) << "The encrypted bridge did not connect";

    Semaphore received;
    String    carried;
    subscriber->onMessage(
        [&received, &carried](const SMessage& message)
        {
            carried = message->toString();
            received.post();
        });

    auto* topic = client::MqttClient::getTopic(topicName);
    publisher->publish(topic, Buffer("Hello over TLS"), Qos::Qos1);

    if (!received.wait_for(10s))
    {
        FAIL() << "The message was not carried over the encrypted bridge";
    }
    // toString() describes the whole publish, so the payload is looked for inside it.
    EXPECT_NE(String::npos, carried.find("Hello over TLS")) << carried.c_str();
}

// A negative case - the same link with a certificate that vouches for nobody - is deliberately
// absent. Whether this node refused the far one can only be read here through
// Server::bridgesConnected(), which is an aggregate over every bridge the node has: it is true
// when there are none at all, and true again as soon as any other bridge connects. Asserting on
// it produces a test that passes alone and fails after its neighbour, which reports on the
// fixture rather than on verification. Asserting it properly needs a per-bridge status the
// server does not expose yet.
