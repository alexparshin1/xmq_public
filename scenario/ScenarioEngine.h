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

#include "ProgressBar.h"
#include "RoundTripInterval.h"
#include "client/MqttClient.h"
#include "common/PaceMaker.h"
#include "service/CTestScenario.h"

#include <atomic>
#include <ranges>
#include <utility>

namespace xmq {

class ScenarioEngine
{
public:
    enum class Type : uint8_t
    {
        FanIn,
        FanOut,
        PointToPoint,
        Connections
    };

    explicit ScenarioEngine() = default;

    explicit ScenarioEngine(CTestScenario scenario)
        : m_scenario(std::move(scenario))
    {
    }

    void load(const std::filesystem::path& fpath);

    std::string name() const;
    std::string type() const;

    /**
     * @brief Access the loaded scenario, e.g. to override its parameters before execution.
     */
    CTestScenario& scenario()
    {
        return m_scenario;
    }

    /**
     * @brief Set local interface IP addresses to bind the clients to, using round-robin.
     * When empty (the default), clients don't bind to a particular local interface.
     * @param addresses Local interface IP addresses.
     */
    void bindAddresses(sptk::Strings addresses)
    {
        m_bindAddresses = std::move(addresses);
    }

    /**
     * @brief Connect every client of the scenario over TLS, using these keys.
     * An empty (default-constructed) SSLKeys still means "encrypt": it is what a listener that
     * asks for no client certificate needs. A null pointer, the default, is a plain connection.
     * @param sslKeys SSL keys, or null for plain TCP.
     */
    void sslKeys(std::shared_ptr<sptk::SSLKeys> sslKeys)
    {
        m_sslKeys = std::move(sslKeys);
    }

    /**
     * @brief Enable or disable a live, in-place progress bar for the connect and publish phases.
     * Disabled by default; connectClients() and publish() are silent unless this is called with true.
     * @param show Whether to show the progress bar.
     */
    void showProgress(const bool show)
    {
        m_showProgress = show;
    }

    /**
     * @brief Sets how often the publish phase reports a row of results.
     *
     * By default the run is divided into ten intervals, which makes the resolution depend on the
     * run's length: five minutes gives 30-second rows, half an hour gives three-minute ones. That
     * is the wrong way round for watching when a queue starts to grow - the longer the run, the
     * coarser the view of it. A fixed interval decouples the two.
     *
     * @param interval How long each reported interval covers; zero keeps the ten-interval default.
     */
    /**
     * @brief How many rows the connect phase reports, rather than the ten it always had.
     *
     * The publish phase takes a width (--result-interval); the connect phase cannot, because it
     * ends when the last client is connected and nobody knows in advance how long that takes. It
     * takes a count instead, and divides the run it turns out to have made.
     * @param count Number of intervals, at least one.
     */
    /**
     * @brief The MQTT5 properties the command line asked for, by the packet that carries them.
     *
     * Kept as concrete objects because the publish set is copied into a fresh object for every
     * message: MqttClient stores what it is given by alias rather than by copy, and a set shared
     * across messages would be mutated by every send thread at once - the same reason the latency
     * trace already gets one of its own.
     *
     * @param connectProperties Properties for the CONNECT, or null.
     * @param publishProperties Properties for every PUBLISH, or null.
     */
    void commandProperties(std::shared_ptr<MessageProperties> connectProperties,
                           std::shared_ptr<MessageProperties> publishProperties)
    {
        m_connectProperties = std::move(connectProperties);
        m_publishProperties = std::move(publishProperties);
    }

    void connectIntervals(const size_t count)
    {
        m_connectIntervals = count > 0 ? count : 1;
    }

    void reportInterval(const std::chrono::microseconds interval)
    {
        m_reportInterval = interval;
    }

    /**
     * @brief Attach a log engine so client-side connect failures (caught and reduced to a
     * ReasonCode inside MqttClient) get their real error message logged instead of discarded.
     */
    void logEngine(std::shared_ptr<sptk::LogEngine> logEngine)
    {
        m_logEngine = std::move(logEngine);
    }

    void                      connectClients(std::vector<RoundTripLatency>& clientPublishLatencies, const std::string& scenarioTitle);
    void                      disconnectClients();
    static const Topic*       substituteTopic(const sptk::String& topicTemplate, size_t clientIndex, std::string_view clientId);
    std::vector<const Topic*> makeTopicList(int publisherCount, size_t topicCount, const sptk::String& publisherTopicTemplate) const;
    void                      publish(std::vector<RoundTripLatency>& clientPublishLatencies, const std::string& scenarioTitle);

    const LatencyTraceTotal& getLatencyTraceTotal() const
    {
        return m_latencyTraceTotal;
    }

    static void printOperationTiming(size_t counter, const std::string& operationName, double milliseconds);

    static Type        typeFromString(const std::string& str);
    static std::string typeToString(Type type);

private:
    CTestScenario                    m_scenario;
    sptk::Strings                    m_bindAddresses;
    std::vector<client::SMqttClient> m_publishers;
    std::vector<client::SMqttClient> m_subscribers;
    LatencyTraceTotal                m_latencyTraceTotal;
    std::atomic_bool                 m_scenarioCompleted;
    std::atomic_size_t               m_receivedMessageCount {0};
    sptk::Semaphore                  m_allMessagesReceived;
    std::shared_ptr<PaceMaker>       m_paceMaker;
    bool                             m_showProgress {false};
    std::chrono::microseconds        m_reportInterval {0};   ///< 0 = ten intervals per run.
    size_t                           m_connectIntervals {10}; ///< Rows the connect phase reports.
    std::shared_ptr<sptk::LogEngine> m_logEngine;
    std::shared_ptr<sptk::SSLKeys>   m_sslKeys; ///< Non-null connects every client over TLS.
    std::shared_ptr<MessageProperties> m_connectProperties; ///< Properties for the CONNECT, from the command line.
    std::shared_ptr<MessageProperties> m_publishProperties; ///< Properties for every PUBLISH, from the command line.

    void                             verifyScenario() const;

    /**
     * @brief The broker a client group connects to.
     *
     * A group may name its own server, which is what lets one scenario publish to one broker
     * and subscribe on another - the arrangement that exercises a bridge. A group without one
     * uses the scenario's server, so scenarios written before this existed are unaffected.
     * @param group             Publisher or subscriber group.
     * @return the group's server settings, or the scenario's.
     */
    [[nodiscard]] const CHost&       groupServer(const CTestClientGroup& group) const;

    void                             probeConnection(const sptk::Host& serverHost, const ConnectCredentials& serverCredentials, const client::ConnectParameters& connectParameters, ProtocolVersion protocolVersion) const;
    client::SMqttClient              connectClientAsync(const sptk::Host& serverHost, const client::ConnectParameters& connectParameters, ProtocolVersion protocolVersion, size_t clientIndex, size_t clientCount, std::atomic_size_t& connectedCount, sptk::Semaphore& allConnected, RoundTripLatency& roundTripLatency, const ConnectCredentials& connectCredentials, const SMessageProperties& connectProperties) const;
    std::vector<client::SMqttClient> connectClientGroup(const sptk::Host& serverHost, const ConnectCredentials& serverCredentials, const client::ConnectParameters& connectParameters, ProtocolVersion protocolVersion, size_t clientCount, const std::string& clientIdPrefix, const std::string& scenarioTitle, std::string_view progressLabel, const SMessageProperties& connectProperties) const;

    /**
     * @brief The properties a group sends, from the scenario file and the command line together.
     *
     * The command line wins where both name the same property, which is how a standing scenario is
     * varied for one run without editing it.
     *
     * @param declared          The scenario's list for this packet.
     * @param fromCommandLine   What -D asked for, or null.
     * @param protocolVersion   Refused unless MQTT5, which is the only version with properties.
     * @param what              "connect" or "publish", for the message when it is refused.
     */
    [[nodiscard]] static std::shared_ptr<MessageProperties> groupProperties(
        const sptk::WSArray<CMessageProperty>& declared,
        const std::shared_ptr<MessageProperties>& fromCommandLine,
        ProtocolVersion protocolVersion, std::string_view what);
    /**
     * @brief Build every property set the scenario asks for, and throw if any of them is refused.
     *
     * Called before the first connection is attempted. A scenario asking for properties on MQTT 3
     * must be refused by the refusal itself, not by whatever the broker happens to answer first.
     */
    void validateProperties() const;
    /**
     * @brief Warn about host limits that would refuse connections this run intends to open.
     *
     * Checked once, before the first connection: each of these limits fails by refusing a
     * connection, which reaches the caller as the server being unavailable and says nothing about
     * the setting behind it. Raises this process's own descriptor limit where it can.
     *
     * @param clientCount       Connections this run will open in total.
     */
    void                             checkSystemLimits(size_t clientCount) const;

    void                             subscribeClients(size_t subscriberCount, size_t durationSeconds, size_t totalMessageCount, std::vector<RoundTripLatency>& clientPublishLatency);
};

} // namespace xmq
