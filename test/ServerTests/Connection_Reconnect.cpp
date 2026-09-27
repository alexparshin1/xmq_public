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

// Delivery to a client that reconnects using the client id it had before.
//
// Reduced from a bridge failure: a bridge is an ordinary MQTT client that reconnects with a
// stable client id, and after the reconnect its SUBSCRIBE appeared to be lost - the remote
// logged the CONNECT and nothing after it, and the bridge then carried no traffic while
// reporting itself connected. It only worked against a broker that had no session for that
// client id yet, which pointed at the server's handling of a second connection for an id it
// already knows rather than at anything bridge-specific.
//
// The sequence below is that situation with no bridge involved: subscribe, receive, disconnect,
// connect again with the same id, and publish once more.

#include "client/MqttClient.h"
#include "server/ClientSession/ClientSessionManager.h"
#include "server/Subscription/SubscriptionManager.h"
#include "test/ServerTests_Suite.h"
#include <sptk5/threads/JoiningThread.h>

#include <gtest/gtest.h>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

class XMQ_ReconnectTests
    : public ServerTests_Suite
{
};

shared_ptr<client::MqttClient> connectClient(const string&                 clientId,
                                             const bool                    cleanSession,
                                             const PublishMessageCallback& messageCallback = nullptr,
                                             const ProtocolVersion         protocolVersion = ProtocolVersion::MqttV31,
                                             const string&                 originNode = {})
{
    auto client = make_shared<client::MqttClient>(ServerTests_Suite::logEngine());

    client::ConnectParameters connectParameters;
    connectParameters.m_cleanSession = cleanSession;

    if (messageCallback)
    {
        client->onMessage(messageCallback);
    }

    // A bridge identifies itself with this property, and the server then marks the session as a
    // bridge origin - which changes how Subscription::deliverTo() treats it.
    SMessageProperties messageProperties;
    if (!originNode.empty())
    {
        auto properties = make_shared<MessageProperties>();
        properties->setUserProperty("origin-node", originNode);
        messageProperties = properties;
    }

    EXPECT_EQ(ReasonCode::Success,
              client->connect(Host("localhost", ServerTests_Suite::TestTcpPortNumber),
                              ConnectCredentials(clientId, "user", "secret"),
                              connectParameters, protocolVersion, messageProperties));

    return client;
}

// Publishes one message to the topic from a throwaway publisher, and reports whether the
// subscriber's callback ran within the timeout.
bool publishAndWait(const string& topicName, Semaphore& received)
{
    const auto publisher = connectClient("reconnect-publisher-" + topicName, true);
    publisher->publish(client::MqttClient::getTopic(topicName), Buffer("payload"), Qos::Qos1);

    constexpr auto deliveryTimeout = 2000ms;
    const auto     delivered = received.wait_for(deliveryTimeout);

    publisher->disconnect();
    return delivered;
}

// What the broker holds for this client, asked only once a delivery has already failed.
//
// Three quite different faults look identical from the client's side - the message simply does not
// arrive - and the message a failing test prints should say which it was: the subscription was
// taken away, the session was, or neither and the message was lost on its way. Asking afterwards
// costs nothing and cannot disturb the race being described, which is why it is done here rather
// than by logging inside the server: a log on that path would change the timing that produces it.
string brokerStateFor(const string& clientId, const string& topicName)
{
    const auto server = ServerTests_Suite::server();
    if (!server)
    {
        return " [no server to ask]";
    }

    stringstream state;

    const auto sessionManager = server->getClientSessionManager();
    if (!sessionManager)
    {
        return " [no session manager]";
    }

    if (const auto session = sessionManager->find(clientId))
    {
        // Not connected has two quite different causes and they point at different code: the
        // socket was taken off the session (clearConnection), or it is still attached and has
        // been closed. Reported apart, because "connected=no" alone does not say which.
        const auto socket = session->getSocket();
        state << " [session " << static_cast<const void*>(session.get())
              << ": connected=" << (session->isConnected() ? "yes" : "no")
              << ", socket=" << (socket ? (socket->active() ? "attached and active" : "attached but closed") : "none")
              << ", clean=" << (session->isCleanSession() ? "yes" : "no")
              << ", subscriptions=" << session->getSubscribedTo().size()
              << ", socket last removed by a close that "
              << (session->lastCloseNamedItsConnection() ? "named its connection" : "NAMED NO CONNECTION")
              << ", from site " << session->lastCloseSite()
              << " (1 read failure, 2 keep-alive, 3 end of DISCONNECT, 4 no CONNECT in time, 5 batched write)]";
    }
    else
    {
        state << " [no session for '" << clientId << "' in the manager]";
    }

    if (const auto subscriptionManager = server->getSubscriptionManager())
    {
        state << " [subscriptions matching '" << topicName
              << "': " << subscriptionManager->getSubscriptions(topicName).size() << "]";
    }

    return state.str();
}

// Which session object the manager holds for this client id, as a bare address.
//
// State alone cannot tell two quite different faults apart. If the session that lost its socket is
// the same object that took the connection over, the teardown paths are at fault. If it is a
// different object, then something registered another session under this client id - the manager
// keys by client id and add() overwrites silently - and the socket that went missing was never
// this connection's to begin with. Recorded after each connect and compared on failure.
const void* sessionIdentity(const string& clientId)
{
    const auto server = ServerTests_Suite::server();
    if (!server)
    {
        return nullptr;
    }
    const auto sessionManager = server->getClientSessionManager();
    if (!sessionManager)
    {
        return nullptr;
    }
    return sessionManager->find(clientId).get();
}

// Subscribe, and wait until the broker has actually registered it.
//
// MqttClient::subscribe() only sends the SUBSCRIBE - it does not wait for the SUBACK - so a test
// that publishes straight afterwards is racing the broker's processing of its own subscription.
// The publish then legitimately matches nothing and is never delivered, which looks exactly like
// a lost message. Rare (roughly one cycle in two thousand) and entirely an artefact of the test.
bool subscribeAndWait(const shared_ptr<client::MqttClient>& client, const Destination& destination,
                      const chrono::milliseconds timeout = 2000ms)
{
    Semaphore subscribed;
    client->onAck([&subscribed](const SMessage& ack)
                  {
                      if (ack && ack->is(Message::Type::SubscribeAck))
                      {
                          subscribed.post();
                      }
                  });

    client->subscribe(destination);
    const auto acknowledged = subscribed.wait_for(timeout);

    client->onAck({});
    return acknowledged;
}

// Whether a publish arrived within the normal window, only after it, or not at all.
//
// "Not delivered" from publishAndWait() conflates two very different failures: a message the
// broker genuinely lost, and one that was merely slower than the timeout because the server was
// busy. They call for opposite investigations, so the reproducer distinguishes them by waiting
// again, much longer, before giving up.
enum class DeliveryOutcome
{
    OnTime,
    Late,
    Lost
};

DeliveryOutcome publishAndClassify(const string& topicName, Semaphore& received,
                                   const chrono::milliseconds graceTimeout = 15000ms)
{
    const auto publisher = connectClient("reconnect-publisher-" + topicName, true);
    publisher->publish(client::MqttClient::getTopic(topicName), Buffer("payload"), Qos::Qos1);

    constexpr auto deliveryTimeout = 2000ms;
    auto           outcome = DeliveryOutcome::OnTime;
    if (!received.wait_for(deliveryTimeout))
    {
        // The semaphore counts, so consuming it here keeps the tally balanced for the next cycle
        // whichever way this goes.
        outcome = received.wait_for(graceTimeout) ? DeliveryOutcome::Late : DeliveryOutcome::Lost;
    }

    publisher->disconnect();
    return outcome;
}

// The whole sequence, for one combination of clean session and re-subscribing. Both parameters
// matter: a session that isn't clean is supposed to still hold its subscription after the
// reconnect, while a bridge re-subscribes every time regardless.
void expectDeliveryAfterReconnect(const bool cleanSession, const bool resubscribeAfterReconnect)
{
    const string topicName = format("reconnect/{}/{}",
                                    cleanSession ? "clean" : "persistent",
                                    resubscribeAfterReconnect ? "resubscribe" : "resume");
    const string clientId = "reconnect-subscriber-" + topicName;

    Semaphore  received;
    const auto onMessage = [&received](const SPublishMessage&)
    {
        received.post();
    };

    // 1. Subscribe, and confirm delivery works before any reconnect. Without this the test
    //    could pass a broken second half simply because it was never working.
    auto subscriber = connectClient(clientId, cleanSession, onMessage);
    ASSERT_TRUE(subscribeAndWait(subscriber, Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(Qos::Qos1))))
        << "The broker did not acknowledge the subscription, so what follows would"
           " measure the race with its own SUBSCRIBE rather than the reconnect";

    ASSERT_TRUE(publishAndWait(topicName, received))
        << "No delivery before the reconnect, so the reconnect itself is not what this measures";

    // 2. Disconnect and connect again as the same client.
    subscriber->disconnect();
    subscriber.reset();

    subscriber = connectClient(clientId, cleanSession, onMessage);
    if (resubscribeAfterReconnect)
    {
        ASSERT_TRUE(subscribeAndWait(subscriber, Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(Qos::Qos1))))
            << "The broker did not acknowledge the subscription, so what follows would"
               " measure the race with its own SUBSCRIBE rather than the reconnect"
            << brokerStateFor(clientId, topicName);
    }

    // 3. The same publish must arrive again.
    EXPECT_TRUE(publishAndWait(topicName, received))
        << "No delivery after reconnecting as '" << clientId << "' ("
        << (cleanSession ? "clean" : "persistent") << " session, "
        << (resubscribeAfterReconnect ? "re-subscribed" : "relying on session resumption") << ")"
        << brokerStateFor(clientId, topicName);

    subscriber->disconnect();
}

// The takeover case: a second connection arrives for a client id whose session is still active,
// so the server has to hand the session over rather than start a fresh one. Nothing disconnects
// the first client - that is the whole point, and it is what separates this from the reconnect
// above, where the session was cleanly gone before the second connect.
void expectDeliveryAfterTakeover(const bool cleanSession, const bool resubscribeAfterTakeover,
                                 const ProtocolVersion protocolVersion, const string& originNode = {})
{
    const string topicName = format("takeover/{}/{}/v{}{}",
                                    cleanSession ? "clean" : "persistent",
                                    resubscribeAfterTakeover ? "resubscribe" : "resume",
                                    static_cast<int>(protocolVersion),
                                    originNode.empty() ? "" : "/bridge");
    const string clientId = "takeover-subscriber-" + topicName;

    Semaphore receivedByFirst;
    Semaphore receivedBySecond;

    // 1. First client subscribes, and delivery is confirmed working.
    const auto first = connectClient(clientId, cleanSession, [&receivedByFirst](const SPublishMessage&)
                                     {
                                         receivedByFirst.post();
                                     },
                                     protocolVersion, originNode);
    ASSERT_TRUE(subscribeAndWait(first, Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(Qos::Qos1))))
        << "The broker did not acknowledge the subscription, so what follows would"
           " measure the race with its own SUBSCRIBE rather than the reconnect";

    ASSERT_TRUE(publishAndWait(topicName, receivedByFirst))
        << "No delivery to the first client, so the takeover is not what this measures";

    // 2. Second client connects with the same id while the first is still connected.
    auto second = connectClient(clientId, cleanSession, [&receivedBySecond](const SPublishMessage&)
                                {
                                    receivedBySecond.post();
                                },
                                protocolVersion, originNode);
    if (resubscribeAfterTakeover)
    {
        ASSERT_TRUE(subscribeAndWait(second, Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(Qos::Qos1))))
            << "The broker did not acknowledge the subscription, so what follows would"
               " measure the race with its own SUBSCRIBE rather than the reconnect";
    }

    // 3. The subscription belongs to the session, and the session is now the second client's,
    //    so the message has to reach it.
    EXPECT_TRUE(publishAndWait(topicName, receivedBySecond))
        << "No delivery after taking over '" << clientId << "' ("
        << (cleanSession ? "clean" : "persistent") << " session, "
        << (resubscribeAfterTakeover ? "re-subscribed" : "relying on the taken-over subscription")
        << ", MQTT " << static_cast<int>(protocolVersion) << ")";

    second->disconnect();
}

// The bridge's actual failure shape: disconnect and connect again on the same client id with
// nothing in between, so the new CONNECT races the server's teardown of the old session. One
// cycle is not enough - on loopback the teardown usually wins - so the cycle is repeated, and
// the iteration that loses is reported. A bridge does exactly this once per rebuild, against a
// server across a network, where the two arrive further apart.
void expectDeliveryAcrossRapidReconnects(const bool cleanSession, const bool resubscribe,
                                         const ProtocolVersion protocolVersion, const size_t cycles)
{
    const string topicName = format("rapid/{}/{}/v{}",
                                    cleanSession ? "clean" : "persistent",
                                    resubscribe ? "resubscribe" : "resume",
                                    static_cast<int>(protocolVersion));
    const string clientId = "rapid-subscriber-" + topicName;

    Semaphore  received;
    const auto onMessage = [&received](const SPublishMessage&)
    {
        received.post();
    };

    auto subscriber = connectClient(clientId, cleanSession, onMessage, protocolVersion);
    ASSERT_TRUE(subscribeAndWait(subscriber, Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(Qos::Qos1))))
        << "The broker did not acknowledge the subscription, so what follows would"
           " measure the race with its own SUBSCRIBE rather than the reconnect";
    ASSERT_TRUE(publishAndWait(topicName, received)) << "No delivery before any reconnect";

    for (size_t cycle = 1; cycle <= cycles; ++cycle)
    {
        // No wait anywhere in here: the point is to give the server's teardown of the old
        // session as little time as possible before the new one arrives.
        subscriber->disconnect();
        subscriber = connectClient(clientId, cleanSession, onMessage, protocolVersion);
        if (resubscribe)
        {
            ASSERT_TRUE(subscribeAndWait(subscriber, Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(Qos::Qos1))))
                << "The broker did not acknowledge the subscription, so what follows would"
                   " measure the race with its own SUBSCRIBE rather than the reconnect"
                << brokerStateFor(clientId, topicName);
        }

        ASSERT_TRUE(publishAndWait(topicName, received))
            << "No delivery on reconnect cycle " << cycle << " of " << cycles << " for '" << clientId
            << "' (" << (cleanSession ? "clean" : "persistent") << " session, "
            << (resubscribe ? "re-subscribed" : "relying on session resumption")
            << ", MQTT " << static_cast<int>(protocolVersion) << ")"
            << brokerStateFor(clientId, topicName);
    }

    subscriber->disconnect();
}

// Abrupt connection loss - the socket is dropped with no DISCONNECT packet - immediately followed
// by a reconnect on the same client id. This is the one shape the other reconnect tests do not
// stage: they all disconnect gracefully, which lets the server finish tearing the old session down
// before the new CONNECT arrives. Dropping the socket instead leaves the old connection's hangup
// to be processed while the new one is already authenticating, which is what a bridge does when
// its link is rebuilt and where its SUBSCRIBE was observed to go missing.
void expectDeliveryAfterAbruptReconnect(const bool cleanSession, const bool resubscribe,
                                        const ProtocolVersion protocolVersion, const size_t cycles)
{
    const string topicName = format("abrupt/{}/{}/v{}",
                                    cleanSession ? "clean" : "persistent",
                                    resubscribe ? "resubscribe" : "resume",
                                    static_cast<int>(protocolVersion));
    const string clientId = "abrupt-subscriber-" + topicName;

    Semaphore  received;
    const auto onMessage = [&received](const SPublishMessage&)
    {
        received.post();
    };

    auto subscriber = connectClient(clientId, cleanSession, onMessage, protocolVersion);
    ASSERT_TRUE(subscribeAndWait(subscriber, Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(Qos::Qos1))))
        << "The broker did not acknowledge the subscription, so what follows would"
           " measure the race with its own SUBSCRIBE rather than the reconnect";
    ASSERT_TRUE(publishAndWait(topicName, received)) << "No delivery before any reconnect";

    for (size_t cycle = 1; cycle <= cycles; ++cycle)
    {
        // hangup() drops the socket without sending DISCONNECT, so the server sees the connection
        // die rather than being told about it - and has had no warning to start cleaning up before
        // the replacement arrives.
        subscriber->hangup();
        subscriber = connectClient(clientId, cleanSession, onMessage, protocolVersion);
        if (resubscribe)
        {
            ASSERT_TRUE(subscribeAndWait(subscriber, Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(Qos::Qos1))))
                << "The broker did not acknowledge the subscription, so what follows would"
                   " measure the race with its own SUBSCRIBE rather than the reconnect"
                << brokerStateFor(clientId, topicName);
        }

        ASSERT_TRUE(publishAndWait(topicName, received))
            << "No delivery on abrupt reconnect cycle " << cycle << " of " << cycles << " for '" << clientId
            << "' (" << (cleanSession ? "clean" : "persistent") << " session, "
            << (resubscribe ? "re-subscribed" : "relying on session resumption")
            << ", MQTT " << static_cast<int>(protocolVersion) << ")"
            << brokerStateFor(clientId, topicName);
    }

    subscriber->disconnect();
}

// The same abrupt loss, but with the old connection dropped from another thread so that its
// teardown genuinely overlaps the new connection's CONNECT and SUBSCRIBE rather than merely
// preceding them. The sequential variant above cannot stage that: by the time it calls connect(),
// the drop has already been handed to the kernel.
void expectDeliveryAfterOverlappingReconnect(const bool cleanSession, const ProtocolVersion protocolVersion,
                                             const size_t cycles)
{
    const string topicName = format("overlap/{}/v{}", cleanSession ? "clean" : "persistent", static_cast<int>(protocolVersion));
    const string clientId = "overlap-subscriber-" + topicName;

    Semaphore  received;
    const auto onMessage = [&received](const SPublishMessage&)
    {
        received.post();
    };

    auto subscriber = connectClient(clientId, cleanSession, onMessage, protocolVersion);
    ASSERT_TRUE(subscribeAndWait(subscriber, Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(Qos::Qos1))))
        << "The broker did not acknowledge the subscription, so what follows would"
           " measure the race with its own SUBSCRIBE rather than the reconnect";
    ASSERT_TRUE(publishAndWait(topicName, received)) << "No delivery before any reconnect";

    for (size_t cycle = 1; cycle <= cycles; ++cycle)
    {
        const auto doomed = subscriber;

        // Dropped concurrently with the reconnect below, so the server is tearing the old
        // connection down while it authenticates the new one for the same client id.
        JoiningThread dropper([doomed]
                              {
                                  doomed->hangup();
                              });

        subscriber = connectClient(clientId, cleanSession, onMessage, protocolVersion);
        ASSERT_TRUE(subscribeAndWait(subscriber, Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(Qos::Qos1))))
            << "The broker did not acknowledge the subscription, so what follows would"
               " measure the race with its own SUBSCRIBE rather than the reconnect"
            << brokerStateFor(clientId, topicName);
        dropper.join();

        ASSERT_TRUE(publishAndWait(topicName, received))
            << "No delivery on overlapping reconnect cycle " << cycle << " of " << cycles << " for '" << clientId
            << "' (" << (cleanSession ? "clean" : "persistent") << " session, MQTT "
            << static_cast<int>(protocolVersion) << ")"
            << brokerStateFor(clientId, topicName);
    }

    subscriber->disconnect();
}

// The full bridge rebuild sequence, and the only one of these that can actually observe a lost
// SUBSCRIBE. The others cannot: a persistent session still holds the subscription from the previous
// cycle, so it keeps delivering whether or not the new SUBSCRIBE was processed. Unsubscribing on
// the old connection first - exactly what Bridge::stop() does - removes that safety net, so
// delivery afterwards depends solely on the SUBSCRIBE that follows the new CONNECT.
void expectDeliveryAfterUnsubscribeAndOverlappingReconnect(const ProtocolVersion protocolVersion, const size_t cycles)
{
    const string topicName = format("rebuild/v{}", static_cast<int>(protocolVersion));
    const string clientId = "rebuild-subscriber-" + topicName;
    const string originNode = "bridge-origin-node";

    Semaphore  received;
    const auto onMessage = [&received](const SPublishMessage&)
    {
        received.post();
    };

    size_t lateCount = 0;
    size_t lostCount = 0;
    string lateCycles;
    string lostCycles;

    auto subscriber = connectClient(clientId, false, onMessage, protocolVersion, originNode);
    ASSERT_TRUE(subscribeAndWait(subscriber, Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(Qos::Qos1))))
        << "No SUBACK for the initial subscribe";
    ASSERT_TRUE(publishAndWait(topicName, received)) << "No delivery before any rebuild";

    for (size_t cycle = 1; cycle <= cycles; ++cycle)
    {
        // Caught rather than allowed to escape. "Not connected" thrown from here means the broker
        // closed the socket of the connection that had just taken the session over - the shape the
        // overtaken-close guards exist to prevent - and an exception leaving the test body says
        // only "unknown file: Failure", with nothing about which cycle or what the broker held.
        try
        {
            // Bridge::stop(): unsubscribe on the established connection, then drop it.
            subscriber->unsubscribe(Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(Qos::Qos1)));

            const auto    doomed = subscriber;
            JoiningThread dropper([doomed]
                                  {
                                      doomed->hangup();
                                  });

            subscriber = connectClient(clientId, false, onMessage, protocolVersion, originNode);
            // Taken the moment the CONNECT has been answered, before the SUBSCRIBE: this is the session
            // the broker had for this client id at the point the new connection was accepted.
            const auto sessionAtConnect = sessionIdentity(clientId);
            const auto acknowledged = subscribeAndWait(subscriber, Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(Qos::Qos1)));
            dropper.join();

            ASSERT_TRUE(acknowledged) << "No SUBACK on bridge rebuild cycle " << cycle << " of " << cycles
                                      << " - the SUBSCRIBE following the new CONNECT was never answered"
                                      << brokerStateFor(clientId, topicName)
                                      << " [session at connect: " << sessionAtConnect
                                      << ", now: " << sessionIdentity(clientId)
                                      << (sessionAtConnect == sessionIdentity(clientId) ? " - the same object" : " - A DIFFERENT OBJECT") << "]";

            switch (publishAndClassify(topicName, received))
            {
                case DeliveryOutcome::OnTime:
                    break;
                case DeliveryOutcome::Late:
                    ++lateCount;
                    lateCycles += to_string(cycle) + " ";
                    break;
                case DeliveryOutcome::Lost:
                    ++lostCount;
                    lostCycles += to_string(cycle) + " ";
                    break;
            }
        }
        catch (const Exception& e)
        {
            FAIL() << "Threw on bridge rebuild cycle " << cycle << " of " << cycles << ": " << e.what()
                   << brokerStateFor(clientId, topicName);
        }
    }

    subscriber->disconnect();

    // Reported together, because which of the two is non-zero decides what to investigate: lost
    // means the broker dropped a message, late means it was only slower than the window.
    EXPECT_EQ(0U, lostCount) << lostCount << " of " << cycles << " publishes were never delivered"
                             << " (cycles: " << lostCycles << ")";
    EXPECT_EQ(0U, lateCount) << lateCount << " of " << cycles << " publishes arrived only after the"
                             << " 2s window, so they were delayed rather than lost (cycles: " << lateCycles << ")";
}

// Reconnect churn and nothing else: no subscribe, no publish, no delivery check. Used to find the
// smallest shape that still reproduces the heap corruption seen under the full bridge-rebuild
// reproducer, so the slower memory tools have as little to wade through as possible.
void churnReconnects(const size_t cycles, const bool cleanSession, const bool concurrentDrop,
                     const bool withSubscriptions = false, const bool withPublish = false)
{
    const string clientId = format("churn-{}-{}-{}{}", cleanSession ? "clean" : "persistent",
                                   concurrentDrop ? "concurrent" : "sequential",
                                   withSubscriptions ? "sub" : "nosub", withPublish ? "-pub" : "");
    const string topicName = "churn/" + clientId;
    const auto   destination = Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(Qos::Qos1));

    Semaphore                    received;
    const PublishMessageCallback onMessage =
        withPublish ? PublishMessageCallback([&received](const SPublishMessage&)
                                             {
                                                 received.post();
                                             })
                    : PublishMessageCallback();

    auto client = connectClient(clientId, cleanSession, onMessage);
    if (withSubscriptions)
    {
        (void) subscribeAndWait(client, destination);
    }

    for (size_t cycle = 1; cycle <= cycles; ++cycle)
    {
        if (withSubscriptions)
        {
            client->unsubscribe(destination);
        }

        const auto doomed = client;

        if (concurrentDrop)
        {
            // The drop overlaps the reconnect, so the server tears one connection down while
            // authenticating its replacement for the same client id.
            JoiningThread dropper([doomed]
                                  {
                                      doomed->hangup();
                                  });
            client = connectClient(clientId, cleanSession, onMessage);
            if (withSubscriptions)
            {
                (void) subscribeAndWait(client, destination);
            }
        }
        else
        {
            doomed->hangup();
            client = connectClient(clientId, cleanSession, onMessage);
            if (withSubscriptions)
            {
                (void) subscribeAndWait(client, destination);
            }
        }

        if (withPublish)
        {
            const auto publisher = connectClient("churn-publisher-" + clientId, true);
            publisher->publish(client::MqttClient::getTopic(topicName), Buffer("payload"), Qos::Qos1);
            (void) received.wait_for(2000ms);
            publisher->disconnect();
        }
    }

    client->disconnect();
}

} // namespace

// FIXED. Raise the cycle counts to 200 for a sensitive measurement - at 25 they run fast enough
// for the normal suite, but rare races need the longer run to show up.
//
// The bug: teardown paths identify their victim by *session* when what failed was a *connection*.
// A session that is not clean outlives its connections, so once a reconnect has handed the session
// a new connection, a failure belonging to the old one closes the replacement's socket instead.
// The client keeps a connection the broker never reads again and its SUBSCRIBE is never dispatched.
//
// Two such paths were found by tracing connection ownership across threads, and fixed, each
// by comparing against the connection the work was for:
//   - ClientSessionThread::threadFunction - a failed send closed the session after a takeover had
//     already moved it to a new connection. isConnected() cannot catch this: it is true of the
//     replacement.
//   - ClientSession::disconnect - a failed DISCONNECT write did the same.
// Together these took the reproducer from failing every run within ~40 cycles to surviving 200
// cycles on roughly 70-90% of runs.
//
// What is left: at least one more mechanism. A captured trace of a residual failure shows a clean
// takeover - no stray teardown of the new connection - so the remaining loss happens somewhere
// else. Rare aborts under this stress are also unexplained and may predate these fixes.
// Narrowing candidates for the crash. Disabled: they assert nothing, they exist to be run under
// valgrind or a crash-rate loop.
//     ./xmq_unit_tests --gtest_also_run_disabled_tests --gtest_filter="*Churn*"
TEST_F(XMQ_ReconnectTests, DISABLED_Churn_Persistent_Concurrent)
{
    churnReconnects(500, false, true);
}

TEST_F(XMQ_ReconnectTests, DISABLED_Churn_Persistent_Sequential)
{
    churnReconnects(500, false, false);
}

TEST_F(XMQ_ReconnectTests, DISABLED_Churn_Clean_Concurrent)
{
    churnReconnects(500, true, true);
}

TEST_F(XMQ_ReconnectTests, DISABLED_Churn_Persistent_Concurrent_Subs)
{
    churnReconnects(500, false, true, true);
}

TEST_F(XMQ_ReconnectTests, DISABLED_Churn_Persistent_Sequential_Subs)
{
    churnReconnects(500, false, false, true);
}

TEST_F(XMQ_ReconnectTests, DISABLED_Churn_Clean_Concurrent_Subs)
{
    churnReconnects(500, true, true, true);
}

TEST_F(XMQ_ReconnectTests, DISABLED_Churn_Persistent_Concurrent_Subs_Publish)
{
    churnReconnects(200, false, true, true, true);
}

TEST_F(XMQ_ReconnectTests, Delivery_AfterBridgeRebuild_Mqtt5)
{
    expectDeliveryAfterUnsubscribeAndOverlappingReconnect(ProtocolVersion::MqttV5, 25);
}

TEST_F(XMQ_ReconnectTests, Delivery_AfterBridgeRebuild_Mqtt31)
{
    expectDeliveryAfterUnsubscribeAndOverlappingReconnect(ProtocolVersion::MqttV31, 25);
}

TEST_F(XMQ_ReconnectTests, Delivery_AfterOverlappingReconnect_BridgeShape)
{
    expectDeliveryAfterOverlappingReconnect(false, ProtocolVersion::MqttV5, 25);
}

TEST_F(XMQ_ReconnectTests, Delivery_AfterOverlappingReconnect_CleanSession)
{
    expectDeliveryAfterOverlappingReconnect(true, ProtocolVersion::MqttV31, 25);
}

// The bridge shape: abrupt loss, immediate reconnect, immediate re-subscribe, repeated.
TEST_F(XMQ_ReconnectTests, Delivery_AfterAbruptReconnect_BridgeShape)
{
    expectDeliveryAfterAbruptReconnect(false, true, ProtocolVersion::MqttV5, 10);
}

TEST_F(XMQ_ReconnectTests, Delivery_AfterAbruptReconnect_PersistentSession_ResumesSubscription)
{
    expectDeliveryAfterAbruptReconnect(false, false, ProtocolVersion::MqttV31, 10);
}

TEST_F(XMQ_ReconnectTests, Delivery_AfterAbruptReconnect_CleanSession)
{
    expectDeliveryAfterAbruptReconnect(true, true, ProtocolVersion::MqttV31, 10);
}

// A client that reconnects and subscribes again - what a bridge does on every reconnect.
TEST_F(XMQ_ReconnectTests, Delivery_AfterReconnect_PersistentSession_Resubscribes)
{
    expectDeliveryAfterReconnect(false, true);
}

TEST_F(XMQ_ReconnectTests, Delivery_AfterReconnect_CleanSession_Resubscribes)
{
    expectDeliveryAfterReconnect(true, true);
}

// A session that isn't clean keeps its subscriptions across the reconnect, so delivery must
// resume without subscribing again.
TEST_F(XMQ_ReconnectTests, Delivery_AfterReconnect_PersistentSession_ResumesSubscription)
{
    expectDeliveryAfterReconnect(false, false);
}

// Session takeover: the second connection arrives before the first has gone away. This is what a
// bridge does when it is rebuilt - it disconnects and reconnects in the same instant, so the
// server can still be holding the old session when the new CONNECT lands.
TEST_F(XMQ_ReconnectTests, Delivery_AfterTakeover_PersistentSession_Resubscribes)
{
    expectDeliveryAfterTakeover(false, true, ProtocolVersion::MqttV31);
}

TEST_F(XMQ_ReconnectTests, Delivery_AfterTakeover_PersistentSession_ResumesSubscription)
{
    expectDeliveryAfterTakeover(false, false, ProtocolVersion::MqttV31);
}

TEST_F(XMQ_ReconnectTests, Delivery_AfterTakeover_CleanSession_Resubscribes)
{
    expectDeliveryAfterTakeover(true, true, ProtocolVersion::MqttV31);
}

// A bridge connects as MQTT 5, so the same takeover is checked on that path too.
TEST_F(XMQ_ReconnectTests, Delivery_AfterTakeover_PersistentSession_Resubscribes_Mqtt5)
{
    expectDeliveryAfterTakeover(false, true, ProtocolVersion::MqttV5);
}

TEST_F(XMQ_ReconnectTests, Delivery_AfterTakeover_PersistentSession_ResumesSubscription_Mqtt5)
{
    expectDeliveryAfterTakeover(false, false, ProtocolVersion::MqttV5);
}

// The closest stand-in for a bridge: MQTT 5, a session that isn't clean, and the origin-node
// property that has the server treat the session as a bridge origin.
TEST_F(XMQ_ReconnectTests, Delivery_AfterTakeover_BridgeOriginSession_Resubscribes)
{
    expectDeliveryAfterTakeover(false, true, ProtocolVersion::MqttV5, "remote-node");
}

TEST_F(XMQ_ReconnectTests, Delivery_AfterTakeover_BridgeOriginSession_ResumesSubscription)
{
    expectDeliveryAfterTakeover(false, false, ProtocolVersion::MqttV5, "remote-node");
}

// Repeated disconnect-and-immediately-reconnect, which is the shape a bridge rebuild takes.
// A bridge re-subscribes every time and connects as MQTT 5 on a session that is not clean, so
// that combination is the one to watch; the others bound where the problem is.
TEST_F(XMQ_ReconnectTests, Delivery_AcrossRapidReconnects_BridgeShape)
{
    expectDeliveryAcrossRapidReconnects(false, true, ProtocolVersion::MqttV5, 25);
}

TEST_F(XMQ_ReconnectTests, Delivery_AcrossRapidReconnects_PersistentSession_ResumesSubscription)
{
    expectDeliveryAcrossRapidReconnects(false, false, ProtocolVersion::MqttV5, 25);
}

TEST_F(XMQ_ReconnectTests, Delivery_AcrossRapidReconnects_CleanSession)
{
    expectDeliveryAcrossRapidReconnects(true, true, ProtocolVersion::MqttV31, 25);
}
