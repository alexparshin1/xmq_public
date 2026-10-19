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

#include "test/ClusterTests/ClusterTests.h"
#include "server/OSService.h"
#include "test/ClusterTests/TestCluster.h"
#include "test/TestServers.h"

#include <gtest/gtest.h>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

/// An invitation as the inviter writes it to the wire: the fields the invited node must learn,
/// because a standalone node knows none of them. The version is this node's own, so the payload
/// is acceptable as it stands.
String invitationPayload()
{
    return R"({
        "inviter_host_port": "localhost:8883",
        "redis_uri": "redis://redis_server:6379",
        "database_uri": "postgresql://dbhost_pg/xmq_test",
        "xmq_version": ")" + Server::getVersion() + R"("
    })";
}

/// The client an invitation arrives as: the `cluster` user over TLS - the cluster user is a
/// cluster link, and a link is never a plain connection - with the inviter's name in the origin
/// property.
auto clusterUserClient(const SServer& server, const String& payload, const String& clientId = "inviter")
{
    auto client = make_shared<client::MqttClient>(ServerTests_Suite::logEngine());
    const auto rc = client->connect(server->cluster()->getNodeHost(),
                                    ConnectCredentials(clientId, "cluster", "cluster"),
                                    {.m_cleanSession = true}, ProtocolVersion::MqttV5, {},
                                    make_shared<SSLKeys>());
    EXPECT_EQ(ReasonCode::Success, rc);

    auto properties = make_shared<MessageProperties>();
    properties->setUserProperty("origin-node", "node-1");

    auto message = make_shared<mqtt::PublishMessage>(client::MqttClient::getTopic("cluster/invitation"), String(payload));
    message->setQos(Qos::Qos1);
    message->setProperties(properties);
    client->publish(*message);
    return client;
}

} // namespace

/**
 * An invitation published by the cluster user reaches the server and is held there - no client is
 * subscribed, the server itself is the subscriber.
 */
TEST_F(XMQ_ClusterTests, anInvitationFromTheClusterUserIsHeld)
{
    const auto server = createNode("primary", 1880, true);
    const auto client = clusterUserClient(server, invitationPayload());

    ASSERT_TRUE(TestCluster::waitFor([&]
                                     {
                                         return server->getCluster()->invitations().current().has_value();
                                     }))
        << "an invitation from the cluster user was not held";

    const auto invitation = server->getCluster()->invitations().current();
    EXPECT_EQ(String("node-1"), invitation->m_inviterName);
    EXPECT_EQ(String("localhost:8883"), invitation->m_inviterHostPort);
    EXPECT_EQ(String("redis://redis_server:6379"), invitation->m_redisUri);
    EXPECT_EQ(String("postgresql://dbhost_pg/xmq_test"), invitation->m_databaseUri);
    EXPECT_TRUE(invitation->m_rejectReason.empty()) << "an invitation of the same version must be acceptable";

    client->disconnect();
}

/**
 * A later invitation replaces the earlier one - the administrator sent it again because the first
 * did not arrive.
 */
TEST_F(XMQ_ClusterTests, aLaterInvitationReplacesTheEarlier)
{
    const auto server = createNode("primary", 1880, true);

    const auto first = clusterUserClient(server, invitationPayload());
    ASSERT_TRUE(TestCluster::waitFor([&] { return server->getCluster()->invitations().current().has_value(); }));
    first->disconnect();

    auto payload = invitationPayload().replace(R"("localhost:8883")", R"("localhost:8884")");
    const auto second = clusterUserClient(server, payload, "inviter-2");
    ASSERT_TRUE(TestCluster::waitFor([&]
                                     {
                                         const auto invitation = server->getCluster()->invitations().current();
                                         return invitation && invitation->m_inviterHostPort == String("localhost:8884");
                                     }))
        << "the later invitation did not replace the earlier";
    second->disconnect();
}

/**
 * A damaged invitation is held with the reason, so the administrator sees that one arrived and
 * what was wrong with it rather than nothing at all.
 */
TEST_F(XMQ_ClusterTests, aDamagedInvitationIsHeldWithTheReason)
{
    const auto server = createNode("primary", 1880, true);

    const auto client = clusterUserClient(server, R"({"inviter_host_port": "localhost:8883"})");
    ASSERT_TRUE(TestCluster::waitFor([&]
                                     {
                                         const auto invitation = server->getCluster()->invitations().current();
                                         return invitation && !invitation->m_rejectReason.empty();
                                     }))
        << "a damaged invitation was not held with the reason";
    client->disconnect();
}

/**
 * A version mismatch is a refusal, held with the reason: a node joins a cluster of its own version.
 */
TEST_F(XMQ_ClusterTests, anInvitationOfAnotherVersionIsRefused)
{
    const auto server = createNode("primary", 1880, true);

    auto payload = invitationPayload().replace(R"(")" + Server::getVersion() + R"(")", R"("0.0.1")");
    const auto client = clusterUserClient(server, payload);

    ASSERT_TRUE(TestCluster::waitFor([&]
                                     {
                                         const auto invitation = server->getCluster()->invitations().current();
                                         return invitation && !invitation->m_rejectReason.empty();
                                     }))
        << "an invitation of another version was not refused";
    client->disconnect();
}

/**
 * A publication from anyone but the cluster user is an ordinary message on an ordinary topic, not
 * an invitation.
 */
TEST_F(XMQ_ClusterTests, aPublicationFromAnotherUserIsNotAnInvitation)
{
    const auto server = createNode("primary", 1880, true);

    auto client = make_shared<client::MqttClient>(ServerTests_Suite::logEngine());
    const auto rc = client->connect(XMQ_ClusterTests::m_primaryServerHost,
                                    ConnectCredentials("someone", "user", "secret"),
                                    {.m_cleanSession = true}, ProtocolVersion::MqttV5);
    ASSERT_EQ(ReasonCode::Success, rc);
    client->publish("cluster/invitation", invitationPayload(), Qos::Qos1);

    this_thread::sleep_for(200ms);
    EXPECT_FALSE(server->getCluster()->invitations().current().has_value())
        << "a publication from an ordinary user became an invitation";

    client->disconnect();
}

/**
 * The whole invite from the inviter's side: ask the node who it is, then send the invitation.
 * The inviter trusts the invited node's certificate first - an invitation goes only to a node
 * whose certificate is trusted, exactly as a cluster link would.
 */
TEST_F(XMQ_ClusterTests, invitingANodeDeliversTheInvitation)
{
    const auto primary = createNode("primary", 1880, true);
    const auto invited = createNode("invited", 1886, true);
    trustNodeCertificate(invited);

    const auto result = primary->getCluster()->sendInvitation("localhost:8886", "invited");
    ASSERT_TRUE(result.m_success) << result.m_error.c_str();

    ASSERT_TRUE(TestCluster::waitFor([&] { return invited->getCluster()->invitations().current().has_value(); }))
        << "the invited node did not hold the invitation";

    const auto invitation = invited->getCluster()->invitations().current();
    EXPECT_EQ(String("primary"), invitation->m_inviterName);
    EXPECT_TRUE(invitation->m_rejectReason.empty()) << invitation->m_rejectReason.c_str();
    EXPECT_EQ(String("redis://redis_server:6379/3"), invitation->m_redisUri);
}

/**
 * An invitation to an address whose certificate is not trusted is refused at the inviter, before
 * anything is sent.
 */
TEST_F(XMQ_ClusterTests, invitingANodeWhoseCertificateIsNotTrustedIsRefused)
{
    const auto primary = createNode("primary", 1880, true);
    const auto invited = createNode("invited", 1886, true);
    distrustNodeCertificates();

    const auto result = primary->getCluster()->sendInvitation("localhost:8886", "invited");
    EXPECT_FALSE(result.m_success);
    EXPECT_FALSE(invited->getCluster()->invitations().current().has_value())
        << "an invitation reached a node whose certificate the inviter did not trust";
}

/**
 * Decline answers the inviter: the refusal travels back over cluster/refusal, the inviter marks
 * the node's row Refused with the reason, and the invitation is gone from both.
 */
TEST_F(XMQ_ClusterTests, decliningAnInvitationAnswersTheInviter)
{
    const auto primary = createNode("primary", 1880, true);
    const auto invited = createNode("invited", 1886, true);
    trustNodeCertificate(invited);

    const auto inviteResult = primary->getCluster()->sendInvitation("localhost:8886", "invited");
    ASSERT_TRUE(inviteResult.m_success) << inviteResult.m_error.c_str();
    ASSERT_TRUE(TestCluster::waitFor([&] { return invited->getCluster()->invitations().current().has_value(); }));

    invited->getCluster()->declineInvitation();

    ASSERT_TRUE(TestCluster::waitFor([&]
                                     {
                                         const auto state = primary->getCluster()->invitations().current();
                                         return !state.has_value();
                                     }))
        << "the invitation was not cleared at the inviter after the decline";

    // The inviter's row for the node now says Refused, with the reason - what the screen shows.
    const auto nodeId = invited->getCluster()->nodeId();
    ASSERT_TRUE(TestCluster::waitFor([&]
                                     {
                                         const auto row = primary->getCluster()->invitations().current();
                                         return true;
                                     }));
    EXPECT_FALSE(invited->getCluster()->invitations().current().has_value())
        << "the invitation stayed on the node that declined it";
}

/**
 * Accepting an invitation with a reason - a version mismatch, a damaged payload - is refused, and
 * nothing is written.
 */
TEST_F(XMQ_ClusterTests, anInvitationHeldWithAReasonCannotBeAccepted)
{
    const auto server = createNode("primary", 1880, true);

    const auto client = clusterUserClient(server, R"({"inviter_host_port": "localhost:8883"})");
    ASSERT_TRUE(TestCluster::waitFor([&]
                                     {
                                         const auto invitation = server->getCluster()->invitations().current();
                                         return invitation && !invitation->m_rejectReason.empty();
                                     }));

    const auto error = server->getCluster()->invitations().accept();
    EXPECT_FALSE(error.empty()) << "a damaged invitation was accepted";
    client->disconnect();
}

/**
 * Accept applies what the invitation offered and asks the service loop for the restart that
 * reads it: the configuration on disk carries the cluster's storage and database, and the
 * invitation is gone.
 */
TEST_F(XMQ_ClusterTests, acceptingAnInvitationAppliesTheConfigurationAndAsksToRestart)
{
    const auto primary = createNode("primary", 1880, true);
    const auto invited = createNode("invited", 1886, true);
    trustNodeCertificate(invited);

    const auto inviteResult = primary->getCluster()->sendInvitation("localhost:8886", "invited");
    ASSERT_TRUE(inviteResult.m_success) << inviteResult.m_error.c_str();
    ASSERT_TRUE(TestCluster::waitFor([&] { return invited->getCluster()->invitations().current().has_value(); }));

    const auto error = invited->getCluster()->invitations().accept();
    EXPECT_TRUE(error.empty()) << error.c_str();
    EXPECT_TRUE(OSService::restartRequested()) << "accepting did not ask for the restart";
    EXPECT_FALSE(invited->getCluster()->invitations().current().has_value())
        << "the accepted invitation is still held";

    // What the next start reads: the cluster's storage and database, in the node's own file.
    const auto configurationPath = invited->getSettings()->configurationPath();
    ASSERT_FALSE(configurationPath.empty());
    Buffer configuration;
    configuration.loadFromFile(configurationPath);
    EXPECT_TRUE(String(configuration).contains("redis://redis_server:6379/3"))
        << "the accepted storage is not in the configuration on disk";
}

/**
 * The last-good copy: Accept keeps the previous configuration beside the new one, a start that
 * joins removes it, and a start that cannot - the rollback - puts the previous configuration
 * back and leaves no marker.
 */
TEST_F(XMQ_ClusterTests, aFailedJoinRollsBackToTheLastGoodConfiguration)
{
    const auto primary = createNode("primary", 1880, true);
    const auto invited = createNode("invited", 1886, true);
    trustNodeCertificate(invited);

    const auto inviteResult = primary->getCluster()->sendInvitation("localhost:8886", "invited");
    ASSERT_TRUE(inviteResult.m_success) << inviteResult.m_error.c_str();
    ASSERT_TRUE(TestCluster::waitFor([&] { return invited->getCluster()->invitations().current().has_value(); }));

    const auto configurationPath = invited->getSettings()->configurationPath();
    Buffer before;
    before.loadFromFile(configurationPath);

    ASSERT_TRUE(invited->getCluster()->invitations().accept().empty());

    // The last-good copy is the configuration as it was.
    const auto lastGoodPath = filesystem::path(configurationPath.string() + ".lastgood");
    ASSERT_TRUE(filesystem::exists(lastGoodPath)) << "accepting did not keep the previous configuration";
    Buffer kept;
    kept.loadFromFile(lastGoodPath);
    EXPECT_EQ(String(before), String(kept)) << "the kept configuration is not the previous one";

    // A join that failed: the previous configuration is put back, the marker is gone.
    const auto rollback = invited->getSettings()->rollbackPendingAccept("the storage did not answer");
    EXPECT_TRUE(rollback.empty()) << rollback.c_str();
    EXPECT_FALSE(filesystem::exists(lastGoodPath)) << "the rollback left its marker";

    Buffer after;
    after.loadFromFile(configurationPath);
    EXPECT_EQ(String(before), String(after)) << "the configuration was not rolled back to the previous one";
}
