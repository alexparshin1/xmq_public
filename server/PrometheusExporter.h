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

#include "base/xmq.h"
#include "HostMetrics.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <sptk5/cutils>
#include <sptk5/net/HttpReader.h>
#include <sptk5/net/TCPSocket.h>

namespace xmq {

/**
 * @brief A copy of the broker counters a scrape reports.
 *
 * Copied rather than read through a pointer to the server: the exporter answers on its own
 * thread, and stopping the broker destroys the object the pointer would name. The controller
 * fills this while it holds the lock that guards that object.
 */
struct BrokerMetrics
{
    uint64_t clientsConnected {0};
    uint64_t clientsDisconnected {0};
    uint64_t clientsMaximum {0};
    uint64_t clientsTotal {0};

    uint64_t messagesReceived {0};
    uint64_t messagesSent {0};
    uint64_t publishReceived {0};
    uint64_t publishSent {0};
    uint64_t publishDropped {0};

    uint64_t messagesRetained {0};
    uint64_t messagesStored {0};
    uint64_t bytesReceived {0};
    uint64_t bytesSent {0};
    uint64_t subscriptions {0};
    uint64_t clusterMessagesReceived {0};

    uint64_t queueReceive {0};
    uint64_t queueSend {0};
    uint64_t queueDelivery {0};

    uint64_t uptimeSeconds {0};
};

/**
 * @brief The Prometheus scrape endpoint.
 *
 * A listener of its own, on its own port, rather than a path on the configuration interface:
 * a scrape carries no credentials, and the scraper is often a process that has no business
 * reaching the administrator's interface at all. It answers GET /metrics with the Prometheus
 * text exposition format and nothing else.
 *
 * The figures come from the same SystemStatistics and HostMetrics the dashboard uses, so the
 * exporter adds no work to the delivery path. HostMetrics is kept here because its CPU
 * percentages are differences between two readings: one shared by the dashboard would report
 * whatever interval the page happened to ask over.
 *
 * Disabled unless the configuration enables it. The listener is owned for the lifetime of the
 * process, like the configuration interface, so metrics keep describing a broker that is stopped
 * rather than disappearing with it.
 */
class XMQ_EXPORT PrometheusExporter final
{
public:
    /// Returns the broker counters, or nothing while MQTT is stopped.
    using BrokerMetricsProvider = std::function<std::optional<BrokerMetrics>()>;

    /**
     * @brief Constructor.
     * @param brokerMetrics     Returns the counters of the running broker, when there is one.
     * @param nodeName          This node's name, reported as a label.
     * @param logEngine         Where the listener reports failures.
     * @param bindAddress       Address to accept scrapes on.
     * @param port              Port to accept scrapes on.
     * @param username          HTTP Basic username, or empty for no authentication.
     * @param passwordHash      The username's stored verifier, empty when there is no username.
     */
    PrometheusExporter(BrokerMetricsProvider               brokerMetrics,
                       std::string                         nodeName,
                       std::shared_ptr<sptk::LogEngine>    logEngine,
                       std::string                         bindAddress,
                       uint16_t                            port,
                       std::string                         username = {},
                       std::string                         passwordHash = {});

    ~PrometheusExporter();

    PrometheusExporter(const PrometheusExporter&) = delete;
    PrometheusExporter& operator=(const PrometheusExporter&) = delete;

    /**
     * @brief Bind the port and start answering scrapes.
     * @throws sptk::Exception when the address and port cannot be listened on.
     */
    void start();

    /**
     * @brief Stop answering scrapes and release the port. Safe to call more than once.
     */
    void stop();

    /**
     * @brief Render the current values in the Prometheus text format.
     *
     * Separate from the socket so it can be checked without one, and so the body a scrape gets
     * is the same string a test reads.
     *
     * @return the exposition body.
     */
    [[nodiscard]] std::string render();

private:
    BrokerMetricsProvider            m_brokerMetrics;
    std::string                      m_nodeName;
    std::shared_ptr<sptk::LogEngine> m_logEngine;
    std::string                      m_bindAddress;
    uint16_t                         m_port;
    std::string                      m_username;     ///< Empty means the endpoint is open.
    std::string                      m_passwordHash; ///< Verifier of the password, never the password.

    sptk::TCPSocket   m_listener;   ///< Accepts scrapes.
    std::thread       m_thread;     ///< The accept loop.
    std::atomic<bool> m_running {false};

    HostMetrics m_hostMetrics;      ///< Kept for the interval between two scrapes.

    void serve();
    void serveClient(SocketType socket);

    /**
     * @brief Whether the request carries acceptable HTTP Basic credentials.
     *
     * Always true when no username is configured. The password is checked against the stored
     * verifier, so what this process holds cannot be replayed as a password.
     *
     * @param headers       Request headers.
     * @return true when the request may be answered.
     */
    [[nodiscard]] bool authorized(const sptk::HttpHeaders& headers) const;
};

} // namespace xmq
