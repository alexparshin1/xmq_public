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

#include "ScenarioEngine.h"
#include "base/MessageProperties.h"
#include "client/MqttClient.h"
#include <sptk5/threads/JoiningThread.h>

#include <cstdlib>

#ifndef _WIN32
#include <sys/resource.h>
#endif

#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

#include <fstream>
#include <optional>

using namespace std;
using namespace chrono;
using namespace sptk;
using namespace xmq;

void ScenarioEngine::load(const std::filesystem::path& fpath)
{
    Buffer buffer;
    buffer.loadFromFile(fpath);

    xdoc::Document doc;
    doc.load(buffer);

    m_scenario.load(doc.root());
    verifyScenario();
}

string ScenarioEngine::name() const
{
    return m_scenario.m_name.asString().c_str();
}

std::string ScenarioEngine::type() const
{
    return m_scenario.m_type.asString().c_str();
}

void ScenarioEngine::printOperationTiming(size_t counter, const std::string& operationName, const double milliseconds)
{
    COUT(format("{} {} for {:0.1f}s ({:0.1f}K/sec)", counter, operationName, milliseconds / 1000.0, counter / milliseconds));
}

ScenarioEngine::Type ScenarioEngine::typeFromString(const std::string& str)
{
    using enum Type;
    static const map<string, Type, less<>> types = {
        {"Fan-In", FanIn},
        {"Fan-Out", FanOut},
        {"Point-To-Point", PointToPoint},
        {"Connections", Connections},
    };
    const auto it = types.find(str);
    if (it == types.end())
    {
        throw std::invalid_argument("Invalid scenario type");
    }
    return it->second;
}

std::string ScenarioEngine::typeToString(const Type type)
{
    using enum Type;
    static const map<Type, string> types = {
        {FanIn, "Fan-In"},
        {FanOut, "Fan-Out"},
        {PointToPoint, "Point-To-Point"},
        {Connections, "Connections"},
    };
    const auto it = types.find(type);
    if (it == types.end())
    {
        throw std::invalid_argument("Invalid scenario type");
    }
    return it->second;
}

void ScenarioEngine::verifyScenario() const
{
    switch (typeFromString(m_scenario.m_type))
    {
        using enum Type;
        case FanIn:
            if (m_scenario.m_subscribers.m_client_count.asInteger() >= m_scenario.m_publishers.m_client_count.asInteger())
            {
                throw std::invalid_argument(m_scenario.m_name.asString() + ": Should be less subscribers than publishers");
            }
            break;
        case FanOut:
            if (m_scenario.m_subscribers.m_client_count.asInteger() <= m_scenario.m_publishers.m_client_count.asInteger())
            {
                throw std::invalid_argument(m_scenario.m_name.asString() + ": Should be less publishers than subscribers");
            }
            break;
        case PointToPoint:
            if (m_scenario.m_publishers.m_client_count.asInteger() != m_scenario.m_subscribers.m_client_count.asInteger())
            {
                throw std::invalid_argument(m_scenario.m_name.asString() + ": Should be same number of publishers and subscribers");
            }
            break;
        case Connections:
            if (m_scenario.m_subscribers.m_client_count.asInteger() != 0)
            {
                throw std::invalid_argument(m_scenario.m_name.asString() + ": Should be 0 subscribers");
            }
            if (m_scenario.m_parameters.m_message_count.asInteger() != 0)
            {
                throw std::invalid_argument(m_scenario.m_name.asString() + ": Message count should be 0");
            }
            break;
    }
}

void ScenarioEngine::probeConnection(const Host& serverHost, const ConnectCredentials& serverCredentials,
                                     const client::ConnectParameters& connectParameters, const ProtocolVersion protocolVersion) const
{
    const ConnectCredentials probeCredentials("xmq_scn-connect-probe", serverCredentials.getUsername(), serverCredentials.getPassword());

    client::MqttClient probeClient(nullptr, "", "");

    ReasonCode reasonCode;
    try
    {
        reasonCode = probeClient.connect(serverHost, probeCredentials, connectParameters, protocolVersion, {}, m_sslKeys);
    }
    catch (const std::exception& e)
    {
        throw Exception(format("Can't connect to {}: {}", serverHost.toString(), e.what()));
    }

    if (reasonCode != ReasonCode::Success)
    {
        throw Exception(format("Can't connect to {}: {}", serverHost.toString(), toString(reasonCode)));
    }

    probeClient.disconnect();
}

client::SMqttClient ScenarioEngine::connectClientAsync(const Host& serverHost, const client::ConnectParameters& connectParameters,
                                                       const ProtocolVersion protocolVersion,
                                                       const size_t clientIndex, const size_t clientCount, atomic_size_t& connectedCount,
                                                       Semaphore& allConnected, RoundTripLatency& roundTripLatency,
                                                       const ConnectCredentials& connectCredentials,
                                                       const SMessageProperties& connectProperties) const
{
    String bindAddress;
    if (!m_bindAddresses.empty())
    {
        bindAddress = m_bindAddresses[clientIndex % m_bindAddresses.size()];
    }

    auto client = make_shared<client::MqttClient>(m_logEngine, "", bindAddress);
    roundTripLatency.sent();
    const auto reasonCode = client->connectAsync(serverHost, connectCredentials, connectParameters, protocolVersion, connectProperties, m_sslKeys,
                                                 [this, &connectedCount, clientCount, &allConnected, &roundTripLatency](const SMessage&)
                                                 {
                                                     if (m_paceMaker)
                                                     {
                                                         roundTripLatency.received();
                                                     }

                                                     if (const auto count = ++connectedCount; count == clientCount)
                                                     {
                                                         allConnected.post();
                                                     }
                                                 });
    if (reasonCode != ReasonCode::Success)
    {
        // A synchronous failure (e.g. socket/DNS error) never calls the completion callback
        // above, so without this check the client would neither count as connected nor as
        // failed - it would just silently stall until the caller's 60s watchdog times out.
        throw Exception(toString(reasonCode));
    }
    return client;
}

void ScenarioEngine::validateProperties() const
{
    const auto publisherProtocolVersion =
        static_cast<ProtocolVersion>(m_scenario.m_publishers.m_protocol_version.asInteger());

    (void) groupProperties(m_scenario.m_publishers.m_connect_properties, m_connectProperties,
                           publisherProtocolVersion, "connect");
    (void) groupProperties(m_scenario.m_publishers.m_publish_properties, m_publishProperties,
                           publisherProtocolVersion, "publish");

    // A Connections scenario never creates the subscriber group, so its version must not refuse a
    // run that would never have used it.
    if (typeFromString(m_scenario.m_type) != Type::Connections)
    {
        (void) groupProperties(m_scenario.m_subscribers.m_connect_properties, m_connectProperties,
                               static_cast<ProtocolVersion>(m_scenario.m_subscribers.m_protocol_version.asInteger()),
                               "connect");
    }
}

shared_ptr<MessageProperties> ScenarioEngine::groupProperties(const WSArray<CMessageProperty>&     declared,
                                                              const shared_ptr<MessageProperties>& fromCommandLine,
                                                              const ProtocolVersion                protocolVersion,
                                                              const string_view                    what)
{
    if (declared.empty() && !fromCommandLine)
    {
        return {}; // Nothing asked for: send no property block at all, not an empty one.
    }

    if (protocolVersion != ProtocolVersion::MqttV5)
    {
        throw Exception("This scenario asks for " + string(what) + " properties, which need MQTT 5; "
                                                                   "protocol_version is " +
                        to_string(static_cast<int>(protocolVersion)));
    }

    auto properties = make_shared<MessageProperties>();
    for (const auto& declaration: declared)
    {
        setPropertyFromText(*properties, declaration.m_name.asString().c_str(),
                            declaration.m_value.asString().c_str());
    }

    // The command line last, so it overrides the file rather than the other way round.
    if (fromCommandLine)
    {
        properties->merge(*fromCommandLine);
    }
    return properties;
}

vector<client::SMqttClient> ScenarioEngine::connectClientGroup(const Host&                      serverHost,
                                                               const ConnectCredentials&        serverCredentials,
                                                               const client::ConnectParameters& connectParameters,
                                                               const ProtocolVersion            protocolVersion,
                                                               const size_t                     clientCount,
                                                               const string&                    clientIdPrefix,
                                                               const string&                    scenarioTitle,
                                                               const string_view                progressLabel,
                                                               const SMessageProperties&        connectProperties) const
{
    mutex                       amutex;
    vector<client::SMqttClient> clients;
    atomic_size_t               connectedCount = 0;
    atomic_size_t               failedCount = 0;
    Semaphore                   allConnected;

    SynchronizedQueue<size_t> clientIndices;
    vector<microseconds>      connectionLatency(clientCount);

    for (size_t i = 0; i < clientCount; ++i)
    {
        clientIndices.push_back(i);
    }

    JoiningThreads threads;

    const auto                         scenarioType = typeFromString(m_scenario.m_type);
    optional<steady_clock::time_point> testEndTime;
    if (const auto durationSec = m_scenario.m_parameters.m_duration_sec.asInteger();
        scenarioType == Type::Connections && durationSec > 0)
    {
        testEndTime = steady_clock::now() + seconds(durationSec);
    }

    constexpr size_t threadCount = 128;

    milliseconds statsInterval {0};
    if (m_paceMaker)
    {
        statsInterval = chrono::duration_cast<milliseconds>(
            clientCount * m_paceMaker->interval() / m_connectIntervals);
    }

    RoundTripLatency roundTripLatency;

    // Polls connectedCount while the worker threads below run, redrawing the bar in place. Runs on
    // a thread of its own so it doesn't perturb the connect loop's timing.
    //
    // A plain std::thread and a flag, where this used to be a jthread and a stop_token: the token
    // needs -fexperimental-library on libc++, and this was the only stop_token in the project.
    // What jthread also gave was stopping and joining on the way out, and that is not decoration -
    // the connect timeout below throws, and a joinable std::thread destroyed while unwinding calls
    // std::terminate. Hence the destructor here. stopAndJoin() is idempotent so that the ordinary
    // path can call it early, before the bar is redrawn one last time.
    ProgressBar progressBar(string(progressLabel), m_showProgress);
    struct ProgressThread
    {
        std::atomic_bool stop {false};
        std::thread      thread;

        void stopAndJoin()
        {
            stop = true;
            if (thread.joinable())
            {
                thread.join();
            }
        }

        ~ProgressThread()
        {
            stopAndJoin();
        }
    } progress;

    if (m_showProgress)
    {
        progress.thread = thread([&connectedCount, clientCount, &progressBar, &progress]
                                 {
                                     while (!progress.stop && connectedCount.load() < clientCount)
                                     {
                                         progressBar.update(connectedCount.load(), clientCount);
                                         this_thread::sleep_for(50ms);
                                     }
                                 });
    }

    roundTripLatency.start(statsInterval);
    for (size_t i = 0; i < threadCount; ++i)
    {
        threads.emplace_back([this, &amutex, &clientIndices, &serverCredentials, &clientIdPrefix, &serverHost, &connectParameters, protocolVersion, &connectedCount, &failedCount, &allConnected, &connectProperties,
                              clientCount, &clients, testEndTime, &roundTripLatency]
                             {
                                 size_t clientIndex = 0;
                                 while (!clientIndices.empty() && clientIndices.pop_front(clientIndex, 100ms))
                                 {
                                     if (m_paceMaker)
                                     {
                                         m_paceMaker->next();
                                     }

                                     if (testEndTime && steady_clock::now() > *testEndTime)
                                     {
                                         allConnected.post();
                                         break;
                                     }

                                     ConnectCredentials connectCredentials(clientIdPrefix + to_string(clientIndex),
                                                                           serverCredentials.getUsername(), serverCredentials.getPassword());

                                     // A failed connect (bad hostname, refused connection, ...) must not escape this
                                     // thread: an uncaught exception here would call std::terminate and kill the whole
                                     // process instead of just failing this one client. Account for the failure the
                                     // same way a successful connect would, so allConnected.wait() below still unblocks.
                                     try
                                     {
                                         auto client = connectClientAsync(serverHost, connectParameters, protocolVersion, clientIndex, clientCount,
                                                                          connectedCount, allConnected, roundTripLatency, connectCredentials,
                                                                          connectProperties);

                                         const scoped_lock lock(amutex);
                                         clients.push_back(std::move(client));
                                     }
                                     catch (const std::exception& e)
                                     {
                                         ++failedCount;
                                         CERR(format("Client '{}' connect to {} failed: {}", connectCredentials.getClientId(), serverHost.toString(), e.what()));
                                         if (const auto count = ++connectedCount; count == clientCount)
                                         {
                                             allConnected.post();
                                         }
                                     }
                                 }
                             });
    }
    // Join first: each launcher thread always terminates on its own (queue drained, or
    // testEndTime elapsed for Connections scenarios) independently of whether every dispatched
    // connect has actually completed - completion is signaled separately, from the async I/O
    // callback in connectClientAsync().
    threads.clear();

    // By now every connect attempt has been dispatched (or abandoned via testEndTime). Any
    // client that hasn't reported success or failure at this point is a stalled handshake -
    // e.g. a dropped SYN/ACK or a ConnectAck that never arrived - that will never resolve on
    // its own. Bound the wait so one straggler out of a huge client count can't hang the run
    // forever (see incident: 100K-connection run that never exited).
    if (constexpr seconds allConnectedTimeout {60};
        !allConnected.wait_for(allConnectedTimeout))
    {
        const auto connected = connectedCount.load();
        throw Exception(format("Only {} of {} client connection(s) completed within {}s; {} connection(s) appear stalled (no response from server).",
                               connected, clientCount, allConnectedTimeout.count(), clientCount - connected));
    }

    progress.stopAndJoin();
    progressBar.update(connectedCount.load(), clientCount);
    progressBar.finish();

    if (const auto failed = failedCount.load(); failed > 0)
    {
        throw Exception(format("{} of {} client connection(s) failed. See errors above for details.", failed, clientCount));
    }

    if (m_paceMaker)
    {
        roundTripLatency.stop();
        roundTripLatency.print(scenarioTitle);
    }

    return clients;
}

void ScenarioEngine::subscribeClients(size_t subscriberCount, const size_t durationSeconds, const size_t totalMessageCount, vector<RoundTripLatency>& clientPublishLatency)
{
    const auto testEndTime = steady_clock::now() + seconds(durationSeconds);

    m_latencyTraceTotal.clear();

    Stopwatch stopwatch;
    stopwatch.start();
    SMessageProperties properties {};
    m_receivedMessageCount = 0;
    const auto              subscriberTopicTemplate = m_scenario.m_subscribers.m_topics.asString();
    const RegularExpression matchClientId(R"(\$clientid)");

    const auto subscribedCount = make_shared<atomic_size_t>(0);
    const auto allSubscribed = make_shared<Semaphore>();

    // Fan-In uses a shared subscription: each message is delivered to exactly one subscriber,
    // and its send is recorded once in clientPublishLatency[0], so every subscriber must pop
    // from that same shared queue.
    const auto scenarioType = typeFromString(m_scenario.m_type);

    for (size_t clientIndex = 0; clientIndex < subscriberCount; clientIndex++)
    {
        auto index = to_string(clientIndex);
        auto clientId = m_scenario.m_subscribers.m_id_prefix.asString() + index;

        const auto latencyIndex = scenarioType == Type::FanIn ? 0 : clientIndex;

        auto subscriber = m_subscribers[clientIndex];
        subscriber->onMessage([this, totalMessageCount, durationSeconds, testEndTime, &clientPublishLatency, latencyIndex](const auto& message)
                              {
                                  if (m_scenarioCompleted.load())
                                  {
                                      return;
                                  }
                                  if (message->is(Message::Type::Publish))
                                  {
                                      clientPublishLatency[latencyIndex].received();
                                      // ClientReceive was already snapped in Session::receiveMessages(),
                                      // right before this callback ran; only present when the publisher
                                      // attached a trace (see publish()'s latencyTraceEnabled gate).
                                      if (const auto properties = message->getProperties())
                                      {
                                          if (const auto* latencyTrace = properties->getLatencyTrace())
                                          {
                                              m_latencyTraceTotal.add(latencyTrace);
                                          }
                                      }
                                  }
                                  const auto count = ++m_receivedMessageCount;
                                  if ((totalMessageCount > 0 && count == totalMessageCount) ||
                                      (durationSeconds > 0 && steady_clock::now() > testEndTime))
                                  {
                                      m_scenarioCompleted = true;
                                      m_allMessagesReceived.post();
                                  }
                              });

        subscriber->onAck([subscribedCount, allSubscribed, subscriberCount](const SMessage& message)
                          {
                              if (message->is(Message::Type::SubscribeAck) &&
                                  subscribedCount->fetch_add(1, std::memory_order_acq_rel) + 1 == subscriberCount)
                              {
                                  allSubscribed->post();
                              }
                          });

        const auto* topic = substituteTopic(subscriberTopicTemplate, clientIndex, clientId);

        Destination destination(topic, SubscriptionOptions(Qos::Qos1));
        subscriber->subscribe(destination);

        // What a subscriber must take with it when its node goes away and it comes back on another
        // one: the session it left behind does not carry the subscription unless the scenario asked
        // for a persistent session, and taking it again costs one packet. The topic comes from the
        // client's own pool, so it outlives the client; the client itself is held weakly, because
        // the callback lives in the client it refers to and a shared pointer there would keep it
        // alive for as long as it lives.
        const Topic*                       subscriptionTopic = topic;
        const weak_ptr<client::MqttClient> weakSubscriber = subscriber;
        subscriber->onReconnect(
            [weakSubscriber, subscriptionTopic]
            {
                if (const auto client = weakSubscriber.lock())
                {
                    client->subscribe(Destination(subscriptionTopic, SubscriptionOptions(Qos::Qos1)));
                }
            });
    }

    // Wait until every subscription is acknowledged before returning, so the subsequent publish()
    // cannot outrun subscription registration.
    if (!allSubscribed->wait_for(seconds(30)))
    {
        throw Exception(format("Only {} of {} subscriptions were acknowledged within the timeout",
                               subscribedCount->load(), subscriberCount));
    }
    stopwatch.stop();
}

const CHost& ScenarioEngine::groupServer(const CTestClientGroup& group) const
{
    // An element that is present but empty is treated as absent: a scenario file that carries
    // "server": {} would otherwise send the group to port 0 on no host at all.
    if (group.m_server.isNull() || group.m_server.m_hostname.asString().empty())
    {
        return m_scenario.m_server;
    }
    return group.m_server;
}

namespace {

/// Descriptors this process needs beyond one per connection: the reactor, the log, the scenario
/// file, and whatever the runtime keeps open. Measured at 17 for the broker; doubled here and
/// rounded, because being wrong in this direction only costs a warning.
constexpr size_t descriptorHeadroom = 64;

/**
 * @brief Widen this process's own descriptor limit towards what the run needs.
 *
 * The soft limit is the process's to raise, up to the hard one, and a load generator that does not
 * bother is the reason a run stops a few connections short of a round number and looks like the
 * broker refusing them - 499,983 of 500,000, once, for a whole afternoon.
 *
 * @return the soft limit in force afterwards, or nothing where it cannot be read.
 */
optional<size_t> raiseDescriptorLimit(const size_t needed)
{
#ifdef _WIN32
    // No per-process descriptor limit to raise: sockets are not descriptors here.
    (void) needed;
    return {};
#else
    rlimit limit = {};
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0)
    {
        return {};
    }

    if (limit.rlim_cur < static_cast<rlim_t>(needed))
    {
        auto wanted = limit;
        wanted.rlim_cur = limit.rlim_max == RLIM_INFINITY
                              ? static_cast<rlim_t>(needed)
                              : min(static_cast<rlim_t>(needed), limit.rlim_max);
        if (setrlimit(RLIMIT_NOFILE, &wanted) == 0)
        {
            limit = wanted;
        }
    }

    return static_cast<size_t>(limit.rlim_cur);
#endif
}

/// How many ephemeral source ports the kernel will hand out, per local address.
optional<size_t> ephemeralPortCount()
{
#if defined(_WIN32)
    return {};
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
    const auto readPort = [](const char* name) -> optional<int>
    {
        int    value = 0;
        size_t length = sizeof(value);
        if (sysctlbyname(name, &value, &length, nullptr, 0) != 0)
        {
            return {};
        }
        return value;
    };

    const auto first = readPort("net.inet.ip.portrange.first");
    const auto last = readPort("net.inet.ip.portrange.last");
    if (!first || !last || *last < *first)
    {
        return {};
    }
    return static_cast<size_t>(*last - *first + 1);
#else
    ifstream range("/proc/sys/net/ipv4/ip_local_port_range");
    int      first = 0;
    int      last = 0;
    if (!range || !(range >> first >> last) || last < first)
    {
        return {};
    }
    return static_cast<size_t>(last - first + 1);
#endif
}

/// Where an administrator widens that range, named for the platform this was built for.
constexpr string_view ephemeralPortSetting()
{
#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
    return "net.inet.ip.portrange.first / .last";
#else
    return "net.ipv4.ip_local_port_range";
#endif
}

} // namespace

void ScenarioEngine::checkSystemLimits(const size_t clientCount) const
{
    // Before the first connection rather than after the run: every limit below fails by refusing
    // connections, and a refused connection is reported as the broker being unavailable. The whole
    // point of saying this here is that the message names the setting instead of the broker.
    if (clientCount == 0)
    {
        return;
    }

    const auto needed = clientCount + descriptorHeadroom;
    if (const auto descriptors = raiseDescriptorLimit(needed);
        descriptors.has_value() && *descriptors < needed)
    {
        CERR(format("WARNING: this process may open {} descriptors ({} clients plus {} for its own "
                    "use) but is limited to {}. Connections past that limit fail, and the failure "
                    "is reported as the server being unavailable. Raise the hard limit for this "
                    "user - 'ulimit -n' shows the soft one, which has already been raised as far as "
                    "the hard one allows.",
                    needed, clientCount, descriptorHeadroom, *descriptors));
    }

    // One source port per connection, per local address: the kernel cannot reuse a port for a
    // second connection to the same destination. Extra addresses multiply the supply, which is
    // what --bind-address is for.
    const auto localAddresses = max<size_t>(1, m_bindAddresses.size());
    if (const auto ports = ephemeralPortCount();
        ports.has_value() && *ports * localAddresses < clientCount)
    {
        CERR(format("WARNING: {} clients need more source ports than this host offers - {} per "
                    "local address across {} address(es), {} in all. The connections beyond that "
                    "cannot be given a port. Widen {}, or give the run more local addresses.",
                    clientCount, *ports, localAddresses, *ports * localAddresses,
                    ephemeralPortSetting()));
    }
}

void ScenarioEngine::connectClients(vector<RoundTripLatency>& clientPublishLatencies, const string& scenarioTitle)
{
    const auto sessionCount = static_cast<size_t>(m_scenario.m_publishers.m_client_count.asInteger()) +
                              static_cast<size_t>(m_scenario.m_subscribers.m_client_count.asInteger());
    checkSystemLimits(sessionCount);

    // The scenario knows exactly how many sessions it is about to open, and the pool that watches
    // their sockets would otherwise rehash its registration map on the way - at 2500 connections a
    // second that lands 140 seconds into the run, and it shows in every Connections result as a
    // step in that interval. Sized once here, it never grows.
    client::Session::reserveSessionPool(sessionCount);

    // A client group may name a broker of its own. Pointing publishers at one server and
    // subscribers at another is how a bridge is exercised: what is published on one has to
    // arrive at subscribers on the other, and the measured latency is then the bridge's.
    const auto& publisherServer = groupServer(m_scenario.m_publishers);
    const auto& subscriberServer = groupServer(m_scenario.m_subscribers);

    Host serverHost(publisherServer.m_hostname.asString().c_str(), static_cast<uint16_t>(publisherServer.m_port.asInteger()));
    Host subscriberHost(subscriberServer.m_hostname.asString().c_str(), static_cast<uint16_t>(subscriberServer.m_port.asInteger()));

    auto connectionRate = m_scenario.m_parameters.m_connection_rate.asInteger();

    m_paceMaker = connectionRate > 0 ? make_shared<PaceMaker>(connectionRate) : nullptr;

    m_latencyTraceTotal.clear();

    ConnectCredentials connectCredentials("", publisherServer.m_username.asString(), publisherServer.m_password.asString());
    ConnectCredentials subscriberCredentials("", subscriberServer.m_username.asString(), subscriberServer.m_password.asString());

    client::ConnectParameters connectParameters {};
    if (!m_scenario.m_parameters.m_keep_alive_sec.isNull())
    {
        connectParameters.m_keepAliveInterval = seconds(m_scenario.m_parameters.m_keep_alive_sec.asInteger());
    }
    if (!m_scenario.m_parameters.m_max_inflight_messages.isNull())
    {
        // Bounds each client's own un-acked QoS1 backlog, so a publisher throttles against its
        // own ack round-trip time instead of sending at PaceMaker's fixed rate regardless of
        // whether the broker is keeping up (see incident: sustained 50K-connection runs against
        // an overloaded broker hitting "Not connected" once keep-alives fall behind).
        connectParameters.m_maxInflightMessages = static_cast<uint16_t>(m_scenario.m_parameters.m_max_inflight_messages.asInteger());
    }

    // Publishers and subscribers each carry their own clean_session in the scenario, so they
    // need their own parameters rather than one shared copy. Absent means true - that is what
    // every scenario got while this field was read from the file but never applied.
    const auto groupConnectParameters = [&connectParameters](const CTestClientGroup& group)
    {
        auto parameters = connectParameters;
        parameters.m_cleanSession = group.m_clean_session.isNull() || group.m_clean_session.asBool();
        // Absent or false means the client stays down, which is what a measurement wants by default:
        // a client that comes back on its own would hide a broker that threw it out. A test that
        // changes the cluster under its load asks for the other behaviour, and then a client whose
        // node goes away comes back and the load outlives the node it was connected to.
        parameters.m_autoReconnect = !group.m_auto_reconnect.isNull() && group.m_auto_reconnect.asBool();
        if (!group.m_reconnect_interval.isNull())
        {
            parameters.m_reconnectInterval = seconds(group.m_reconnect_interval.asInteger());
        }
        if (!group.m_reconnect_attempts.isNull())
        {
            parameters.m_reconnectAttempts = static_cast<int>(group.m_reconnect_attempts.asInteger());
        }
        return parameters;
    };

    Stopwatch stopwatch;

    const auto publisherProtocolVersion = static_cast<ProtocolVersion>(m_scenario.m_publishers.m_protocol_version.asInteger());

    // Every property set the scenario asks for is built before the first connection is attempted.
    // A set that MQTT 3 cannot carry has to refuse the run, and the refusal is worth nothing if an
    // unreachable broker reports itself first - which is precisely how the check was missed once.
    validateProperties();

    // Probe the server with a single synchronous connection before launching the (possibly huge) client
    // group, so an unreachable broker fails fast with a clear message instead of every one of thousands
    // of clients failing individually.
    // The probe keeps a clean session regardless of the scenario: it must not leave a
    // persistent session behind for its own client id.
    probeConnection(serverHost, connectCredentials, connectParameters, publisherProtocolVersion);
    if (subscriberHost != serverHost)
    {
        // Two brokers means two ways to be unreachable, and the second one would otherwise
        // only surface after every publisher had already connected.
        probeConnection(subscriberHost, subscriberCredentials, connectParameters,
                        static_cast<ProtocolVersion>(m_scenario.m_subscribers.m_protocol_version.asInteger()));
    }

    // Create publishers:
    stopwatch.start();
    size_t     publisherCount = m_scenario.m_publishers.m_client_count.asInteger();
    const auto publisherClientIdPrefix = m_scenario.m_publishers.m_id_prefix.asString();
    m_publishers = connectClientGroup(serverHost, connectCredentials, groupConnectParameters(m_scenario.m_publishers),
                                      publisherProtocolVersion, publisherCount, publisherClientIdPrefix,
                                      scenarioTitle + " (connect clients)", "Connecting publishers",
                                      groupProperties(m_scenario.m_publishers.m_connect_properties,
                                                      m_connectProperties, publisherProtocolVersion, "connect"));
    stopwatch.stop();

    if (typeFromString(m_scenario.m_type) == Type::Connections)
    {
        return;
    }

    const auto durationSeconds = static_cast<size_t>(m_scenario.m_parameters.m_duration_sec.asInteger());
    auto       totalMessageCount = static_cast<size_t>(m_scenario.m_parameters.m_message_count.asInteger());
    size_t     subscriberCount = m_scenario.m_subscribers.m_client_count.asInteger();
    if (typeFromString(m_scenario.m_type) == Type::FanOut)
    {
        totalMessageCount *= subscriberCount;
    }

    // Create subscribers:
    const auto subscriberClientIdPrefix = m_scenario.m_subscribers.m_id_prefix.asString();
    const auto subscriberProtocolVersion = static_cast<ProtocolVersion>(m_scenario.m_subscribers.m_protocol_version.asInteger());
    m_subscribers = connectClientGroup(subscriberHost, subscriberCredentials, groupConnectParameters(m_scenario.m_subscribers),
                                       subscriberProtocolVersion, subscriberCount, subscriberClientIdPrefix,
                                       scenarioTitle + " (connect clients)", "Connecting subscribers",
                                       groupProperties(m_scenario.m_subscribers.m_connect_properties,
                                                       m_connectProperties, subscriberProtocolVersion, "connect"));

    clientPublishLatencies.resize(subscriberCount);

    subscribeClients(subscriberCount, durationSeconds, totalMessageCount, clientPublishLatencies);
}

void ScenarioEngine::disconnectClients()
{
    // subscribeClients() registers onMessage callbacks that capture clientPublishLatency by
    // reference (owned by the caller, Scenario::executeScenario's clientPublishLatencies).
    // MqttClient::onMessage({}) drains any already-in-flight callback before returning, so by
    // the time this loop finishes, no callback can touch that vector after it's destroyed.
    for (const auto& subscriber: m_subscribers)
    {
        subscriber->onMessage({});
    }

    m_publishers.clear();
    m_subscribers.clear();
}

const Topic* ScenarioEngine::substituteTopic(const String& topicTemplate, const size_t clientIndex, const string_view clientId)
{
    static const RegularExpression matchClientId("\\$clientid", "i");
    static const RegularExpression matchClientIndex("\\$clientindex", "i");

    auto replaced = false;
    auto topicName = matchClientId.replaceAll(topicTemplate, string(clientId), replaced);
    topicName = matchClientIndex.replaceAll(topicName, to_string(clientIndex), replaced);

    return client::MqttClient::getTopic(topicName);
}

vector<const Topic*> ScenarioEngine::makeTopicList(const int publisherCount, size_t topicCount, const String& publisherTopicTemplate) const
{
    vector<const Topic*> topics;
    auto                 publisherIndex = 0;
    for (size_t i = 0; i < topicCount; i++)
    {
        const auto  publisher = m_publishers[publisherIndex];
        const auto* topic = substituteTopic(publisherTopicTemplate, publisherIndex, publisher->getClientId());
        topics.push_back(topic);
        publisherIndex = (publisherIndex + 1) % publisherCount;
    }
    return topics;
}

void ScenarioEngine::publish(vector<RoundTripLatency>& clientPublishLatencies, const string& scenarioTitle)
{
    m_latencyTraceTotal.clear();
    m_scenarioCompleted = false;
    m_receivedMessageCount = 0;

    const auto publishMessageCount = m_scenario.m_parameters.m_message_count.asInteger();
    const auto publisherCount = m_scenario.m_publishers.m_client_count.asInteger();

    // publish_rate is per publisher. The send loop below round-robins evenly across all
    // publishers (one send per publisher per full cycle), so pacing that single shared loop
    // at publishRate * publisherCount yields exactly publishRate msg/s from each publisher.
    const auto publishRate = m_scenario.m_parameters.m_publish_rate.asInteger();
    m_paceMaker = publishRate > 0 ? make_shared<PaceMaker>(publishRate * publisherCount) : nullptr;

    size_t     topicCount {0};
    const auto testType = typeFromString(m_scenario.m_type);
    switch (testType)
    {
        using enum Type;
        case FanIn:
            // Resolve the topic template for each publisher. A literal template still gives
            // one logical topic; $clientindex/$clientid must produce distinct publisher topics.
            topicCount = publisherCount;
            break;
        case FanOut:
        case PointToPoint:
            topicCount = m_scenario.m_subscribers.m_client_count.asInteger();
            break;
        case Connections:
            topicCount = 0;
            break;
    }

    if (topicCount == 0)
    {
        // Nowhere to send.
        return;
    }

    const auto              publisherTopicTemplate = m_scenario.m_publishers.m_topics.asString();
    const RegularExpression matchClientId("\\$clientid", "i");
    const RegularExpression matchClientIndex("\\$clientindex", "i");

    // topics[i] must resolve $clientindex to the same index the publish loop uses for
    // message i (sendMessageCount % publisherCount), so the subscriber with that index
    // receives it and pops the matching latency sample.
    vector<const Topic*> topics = makeTopicList(publisherCount, topicCount, publisherTopicTemplate);

    const auto payloadSize = static_cast<size_t>(m_scenario.m_parameters.m_payload_size.asInteger());
    Buffer     payload;
    payload.fill('#', payloadSize);

    // The per-hop phase breakdown (LatencyPhase::ServerWireIn..ClientReceive) travels as an MQTT5
    // user property, so it only carries any signal over an MQTT5 connection; on MQTT3/3.1.1 the
    // properties never reach the wire, and diff()/print() would be reading all-zero fields past
    // ClientWireOut. Gate trace creation on protocol version so MQTT3 runs (e.g. the primary
    // Mosquitto-comparable benchmarks) pay no allocation cost and report nothing misleading.
    // Off unless asked for: the latency trace is XMQ-only debugging instrumentation. It takes
    // per-hop measurements inside the server that no other broker implements, and travels as an
    // MQTT5 user property costing an allocation and extra wire bytes on every message. Leaving it
    // on for MQTT5 runs therefore charges XMQ for work its competitors never do, understating it
    // in any cross-broker comparison - and the published benchmarks are exactly that.
    //
    // To investigate XMQ's own latency breakdown, run an MQTT5 scenario (or any scenario with
    // -V 5) with XMQ_LATENCY_TRACE=1 in the environment. Nothing else needs changing; the receive
    // side already ignores messages that carry no trace.
    const auto latencyTraceEnabled =
        std::getenv("XMQ_LATENCY_TRACE") != nullptr &&
        static_cast<ProtocolVersion>(m_scenario.m_publishers.m_protocol_version.asInteger()) == ProtocolVersion::MqttV5;

    // Computed once, copied per message below: what the publishers' group declared, merged with
    // anything -D asked for.
    const auto publishProperties =
        groupProperties(m_scenario.m_publishers.m_publish_properties, m_publishProperties,
                        static_cast<ProtocolVersion>(m_scenario.m_publishers.m_protocol_version.asInteger()),
                        "publish");

    const auto qos = static_cast<Qos>(m_scenario.m_subscribers.m_qos.asInteger());

    auto sendMessageCount = 0;

    // Duration-based scenarios carry no message_count to stop on: completion is only ever observed
    // on the receiving side (onMessage flips m_scenarioCompleted once now > testEndTime). Bound the
    // sending loop on the wall clock too, so a delivery stall - which keeps that callback from ever
    // firing - cannot spin here forever.
    const auto durationSeconds = static_cast<size_t>(m_scenario.m_parameters.m_duration_sec.asInteger());
    const auto publishDeadline = durationSeconds > 0
                                     ? std::optional {steady_clock::now() + seconds(durationSeconds)}
                                     : std::nullopt;

    Stopwatch stopwatch;
    stopwatch.start();

    // Progress is tracked against whichever bound the loop below actually stops on: sent message
    // count when publishMessageCount is set, elapsed milliseconds against the duration otherwise.
    // A total of 0 (neither bound set) means progress can't be shown; ProgressBar::update() is a
    // no-op in that case, so the call sites below don't need to special-case it.
    const auto  publishStartTime = steady_clock::now();
    const auto  progressTotal = publishMessageCount > 0
                                    ? static_cast<size_t>(publishMessageCount)
                                    : (durationSeconds > 0 ? durationSeconds * 1000 : 0);
    ProgressBar progressBar("Publishing", m_showProgress);

    // How many messages to publish between asking the progress bar anything. The bar rate-limits
    // its own redraws to one per 100ms, but it can only do that after being called, and each call
    // reads the clock: at 100k messages a second that was a hundred thousand clock reads a second
    // spent deciding not to draw, on a client that is itself the bottleneck of the heaviest
    // scenarios. The step follows the run's own size - a thousand checks over the whole run, and
    // never coarser than every thousand messages - so a four-second burst still animates while a
    // half-hour run stops paying per message.
    const auto progressStep = max<size_t>(1, min<size_t>(progressTotal / 1000, 1000));

    // Group latency samples into ~10 buckets over the expected run. Without a pace maker, the
    // publish rate is unknown, so fall back to fixed 100ms buckets; clamp to at least 1ms so
    // a tiny message count cannot produce a zero-width bucket (which would make every interval
    // key collapse to 0 and overwrite the previous one).
    auto groupInterval = microseconds(100000);
    if (m_reportInterval > microseconds(0))
    {
        // Asked for explicitly (--result-interval): the resolution then no longer depends on how
        // long the run is, which is what watching a queue build up requires.
        groupInterval = m_reportInterval;
    }
    else if (publishMessageCount > 0 && m_paceMaker)
    {
        groupInterval = max(duration_cast<microseconds>(publishMessageCount / 10 * m_paceMaker->interval()),
                            microseconds(1000));
    }
    else if (durationSeconds > 0)
    {
        groupInterval = microseconds(durationSeconds * 100000);
    }

    for (auto& clientPublishLatency: clientPublishLatencies)
    {
        clientPublishLatency.start(groupInterval);
    }

    while (true)
    {
        if ((publishMessageCount && sendMessageCount == publishMessageCount) || m_scenarioCompleted.load())
        {
            break;
        }

        if (publishDeadline && steady_clock::now() > *publishDeadline)
        {
            break;
        }

        auto        publisherIndex = sendMessageCount % publisherCount;
        const auto  publisher = m_publishers[publisherIndex];
        const auto* topic = topics[sendMessageCount % topicCount];

        if (m_paceMaker)
        {
            m_paceMaker->next();
        }

        switch (testType)
        {
            using enum Type;
            case FanIn:
                // Shared subscription: each message is delivered to exactly one subscriber,
                // so record the send once in the shared latency object.
                clientPublishLatencies[0].sent();
                break;
            case FanOut:
                for (auto& clientPublishLatency: clientPublishLatencies)
                {
                    clientPublishLatency.sent();
                }
                break;
            case PointToPoint:
                clientPublishLatencies[publisherIndex].sent();
                break;
            default:
                throw Exception("Unknown test type");
        }

        if (latencyTraceEnabled || publishProperties)
        {
            // A fresh properties object per message, not one reused across the loop: MqttClient's
            // publish() stores the shared_ptr as-is (Message::setProperties() is an alias, not a
            // copy), so a shared instance here would let every in-flight message's send thread
            // mutate the same LatencyTrace concurrently. Whatever "-D publish ..." asked for is
            // copied into that fresh object, which is also what a real client would pay.
            auto messageProperties = publishProperties ? make_shared<MessageProperties>(*publishProperties)
                                                       : make_shared<MessageProperties>();
            if (latencyTraceEnabled)
            {
                messageProperties->getLatencyTrace(true)->snap(LatencyPhase::ClientSend);
            }
            publisher->publish(topic, payload, qos, messageProperties);
        }
        else
        {
            publisher->publish(topic, payload, qos);
        }
        ++sendMessageCount;

        if (m_showProgress && static_cast<size_t>(sendMessageCount) % progressStep == 0)
        {
            const auto progressCurrent = publishMessageCount > 0
                                             ? static_cast<size_t>(sendMessageCount)
                                             : static_cast<size_t>(duration_cast<milliseconds>(steady_clock::now() - publishStartTime).count());
            progressBar.update(progressCurrent, progressTotal);
        }
    }

    // Never block indefinitely waiting for the receiving side to signal completion. If delivery stalls
    // (or a duration-based run publishes its whole window without a message arriving past testEndTime),
    // fall back to a bounded grace period so the test returns and reports what it received instead of
    // hanging. A duration-based run has already spent its window publishing, so its grace is short.
    const auto deliveryGrace = publishDeadline ? seconds(5) : seconds(30);
    m_allMessagesReceived.wait_for(deliveryGrace);

    progressBar.update(progressTotal, progressTotal);
    progressBar.finish();

    stopwatch.stop();
    for (auto& clientPublishLatency: clientPublishLatencies)
    {
        clientPublishLatency.stop();
    }

    auto summaryPublishLatency = RoundTripLatency::combine(clientPublishLatencies);

    summaryPublishLatency.print(scenarioTitle);

    if (latencyTraceEnabled)
    {
        m_latencyTraceTotal.print(scenarioTitle);
    }
}
