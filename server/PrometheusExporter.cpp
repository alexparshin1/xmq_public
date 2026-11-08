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

#include "PrometheusExporter.h"

#include "Settings/PasswordHash.h"
#include "base/xmq-config.h"

#include <sptk5/Base64.h>
#include <sptk5/net/Host.h>
#include <sptk5/net/HttpReader.h>
#include <sptk5/net/URL.h>

#include <chrono>
#include <sstream>

using namespace std;
using namespace sptk;

namespace xmq {

namespace {

/// Prometheus label values are quoted, with exactly three characters needing an escape.
string escapeLabel(const string& value)
{
    string escaped;
    escaped.reserve(value.size());
    for (const auto character: value)
    {
        switch (character)
        {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            default: escaped += character; break;
        }
    }
    return escaped;
}

/**
 * @brief One metric family with a single unlabelled sample.
 *
 * HELP and TYPE are emitted before every sample because Prometheus requires them before the
 * first sample of a family, and repeating them is harmless in the text format.
 */
void appendMetric(string& body, const char* name, const char* help, const char* type, const uint64_t value)
{
    body += "# HELP ";
    body += name;
    body += ' ';
    body += help;
    body += "\n# TYPE ";
    body += name;
    body += ' ';
    body += type;
    body += '\n';
    body += name;
    body += ' ';
    body += to_string(value);
    body += '\n';
}

void appendMetric(string& body, const char* name, const char* help, const char* type, const double value)
{
    body += "# HELP ";
    body += name;
    body += ' ';
    body += help;
    body += "\n# TYPE ";
    body += name;
    body += ' ';
    body += type;
    body += '\n';
    body += name;
    body += ' ';
    body += to_string(value);
    body += '\n';
}

} // namespace

PrometheusExporter::PrometheusExporter(BrokerMetricsProvider            brokerMetrics,
                                       string                           nodeName,
                                       shared_ptr<LogEngine>            logEngine,
                                       string                           bindAddress,
                                       const uint16_t                   port,
                                       string                           username,
                                       string                           passwordHash)
    : m_brokerMetrics(std::move(brokerMetrics))
    , m_nodeName(std::move(nodeName))
    , m_logEngine(std::move(logEngine))
    , m_bindAddress(std::move(bindAddress))
    , m_port(port)
    , m_username(std::move(username))
    , m_passwordHash(std::move(passwordHash))
{
}

PrometheusExporter::~PrometheusExporter()
{
    stop();
}

void PrometheusExporter::start()
{
    if (m_thread.joinable())
    {
        return;
    }

    m_listener.host(Host(m_bindAddress, m_port));

    // Not SO_REUSEPORT: a metrics port another process already serves must fail here rather than
    // be shared, for the same reason a broker listener does - a scrape answered by two different
    // processes is a series with two meanings.
    m_listener.listen(m_port, false);

    m_running = true;
    m_thread = thread([this]
                      {
                          serve();
                      });

    if (m_logEngine)
    {
        const Logger logger(*m_logEngine, "[Prometheus] ");
        logger.info("Listening on " + m_bindAddress + ":" + to_string(m_port) + ".");
    }
}

void PrometheusExporter::stop()
{
    m_running = false;

    // Closing wakes the accept loop out of its poll. It is safe from here: Socket::close takes
    // the descriptor out of the socket under its own lock, and accept() holds that lock only
    // while it is not waiting.
    if (m_listener.active())
    {
        m_listener.close();
    }

    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

void PrometheusExporter::serve()
{
    while (m_running)
    {
        SocketType socket = INVALID_SOCKET;
        sockaddr_storage peer {};
        socklen_t        peerLength = sizeof(peer);
        try
        {
            if (!m_listener.accept(socket, peer, peerLength, chrono::milliseconds(250)))
            {
                continue;
            }
        }
        catch (const Exception& exception)
        {
            if (m_running.load() && m_logEngine)
            {
                const Logger logger(*m_logEngine, "[Prometheus] ");
                logger.error("Accept failed: " + string(exception.what()));
            }
            continue;
        }

        try
        {
            serveClient(socket);
        }
        catch (const exception& exception)
        {
            if (m_logEngine)
            {
                const Logger logger(*m_logEngine, "[Prometheus] ");
                logger.debug("A scrape could not be answered: " + string(exception.what()));
            }
        }
    }
}

void PrometheusExporter::serveClient(const SocketType socket)
{
    auto client = make_shared<TCPSocket>();
    client->attach(socket, true);

    // A scrape sends one short request and reads one reply. Nothing else is offered, so a
    // connection that goes quiet is dropped rather than held: this is the only thread that
    // answers metrics, and one stalled client must not stop the next scrape.
    if (!client->readyToRead(chrono::seconds(5)))
    {
        return;
    }

    Buffer     content;
    HttpReader reader(client, content, HttpReader::ReadMode::REQUEST);
    reader.readHttpHeaders();

    if (!authorized(reader.getHttpHeaders()))
    {
        client->write(String("HTTP/1.1 401 Unauthorized\r\n"
                             "WWW-Authenticate: Basic realm=\"XMQ metrics\", charset=\"UTF-8\"\r\n"
                             "Content-Length: 0\r\n"
                             "Connection: close\r\n"
                             "\r\n"));
        return;
    }

    const auto method = reader.getRequestType();
    const auto url = URL(reader.getRequestURL());

    String response;
    if ((method == "GET" || method == "HEAD") && url.path() == "/metrics")
    {
        const auto body = render();
        response = "HTTP/1.1 200 OK\r\n"
                   "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
                   "Content-Length: " + to_string(body.size()) + "\r\n"
                   "Connection: close\r\n"
                   "\r\n";
        if (method == "GET")
        {
            response += body.c_str();
        }
    }
    else
    {
        const String body = "Not Found\n";
        response = "HTTP/1.1 404 Not Found\r\n"
                   "Content-Type: text/plain; charset=utf-8\r\n"
                   "Content-Length: " + to_string(body.length()) + "\r\n"
                   "Connection: close\r\n"
                   "\r\n" + body;
    }

    client->write(response);
}

bool PrometheusExporter::authorized(const HttpHeaders& headers) const
{
    // No username is the documented way to leave the endpoint open, and it is what an
    // installation upgrading from before authentication existed has.
    if (m_username.empty())
    {
        return true;
    }

    // A username with no verifier is a configuration that promises authentication it cannot
    // perform. Refuse rather than answer everyone.
    if (m_passwordHash.empty())
    {
        return false;
    }

    const auto header = headers.find("authorization");
    if (header == headers.end())
    {
        return false;
    }

    const auto& value = header->second;
    const auto  space = value.find(' ');
    if (space == String::npos || String(value.substr(0, space)).toLowerCase() != "basic")
    {
        return false;
    }

    try
    {
        Buffer decoded;
        Base64::decode(decoded, String(String(value.substr(space + 1)).trim()));
        const string credentials(reinterpret_cast<const char*>(decoded.data()), decoded.bytes());
        const auto   colon = credentials.find(':');
        if (colon == string::npos || credentials.substr(0, colon) != m_username)
        {
            return false;
        }
        return PasswordHash::verify(credentials.substr(colon + 1), m_passwordHash);
    }
    catch (const Exception&)
    {
        // A malformed Authorization value is a refusal, not a request to fail.
        return false;
    }
}

string PrometheusExporter::render()
{
    string body;

    // The build first, so a scrape of a starting or stopped broker still says which binary and
    // which node answered it.
    body += "# HELP xmq_build_info Build information for this broker.\n"
            "# TYPE xmq_build_info gauge\n"
            "xmq_build_info{version=\"" + escapeLabel(XMQ_VERSION_NUMBER) +
            "\",node=\"" + escapeLabel(m_nodeName) + "\"} 1\n";

    const auto broker = m_brokerMetrics ? m_brokerMetrics() : std::nullopt;
    appendMetric(body, "xmq_up", "Whether the MQTT broker is running.", "gauge",
                 static_cast<uint64_t>(broker.has_value() ? 1 : 0));

    if (broker)
    {
        appendMetric(body, "xmq_broker_clients_connected", "Clients currently connected.", "gauge",
                     broker->clientsConnected);
        appendMetric(body, "xmq_broker_clients_maximum", "Most clients connected at one time.", "gauge",
                     broker->clientsMaximum);
        appendMetric(body, "xmq_broker_clients_total",
                     "Clients that have connected since the broker started.", "counter", broker->clientsTotal);
        appendMetric(body, "xmq_broker_clients_disconnected_total",
                     "Clients that have disconnected since the broker started.", "counter",
                     broker->clientsDisconnected);
        appendMetric(body, "xmq_broker_messages_received_total", "MQTT messages received.", "counter",
                     broker->messagesReceived);
        appendMetric(body, "xmq_broker_messages_sent_total", "MQTT messages sent.", "counter",
                     broker->messagesSent);
        appendMetric(body, "xmq_broker_publish_received_total", "PUBLISH messages received.", "counter",
                     broker->publishReceived);
        appendMetric(body, "xmq_broker_publish_sent_total", "PUBLISH messages sent.", "counter",
                     broker->publishSent);
        appendMetric(body, "xmq_broker_publish_dropped_total",
                     "PUBLISH messages dropped because a session queue was full.", "counter",
                     broker->publishDropped);
        appendMetric(body, "xmq_broker_retained_messages", "Retained messages held.", "gauge",
                     broker->messagesRetained);
        appendMetric(body, "xmq_broker_stored_messages", "Messages held for later delivery.", "gauge",
                     broker->messagesStored);
        appendMetric(body, "xmq_broker_bytes_received_total", "Bytes received from clients.", "counter",
                     broker->bytesReceived);
        appendMetric(body, "xmq_broker_bytes_sent_total", "Bytes sent to clients.", "counter",
                     broker->bytesSent);
        appendMetric(body, "xmq_broker_subscriptions", "Subscriptions held.", "gauge", broker->subscriptions);
        appendMetric(body, "xmq_broker_cluster_messages_received_total",
                     "Publications received from other cluster nodes.", "counter",
                     broker->clusterMessagesReceived);
        appendMetric(body, "xmq_broker_queue_receive", "Messages waiting in the receive queues.", "gauge",
                     broker->queueReceive);
        appendMetric(body, "xmq_broker_queue_send", "Messages waiting in the send queues.", "gauge",
                     broker->queueSend);
        appendMetric(body, "xmq_broker_queue_delivery", "Messages waiting in the delivery queues.", "gauge",
                     broker->queueDelivery);
        appendMetric(body, "xmq_broker_uptime_seconds", "Seconds since the broker started serving.", "gauge",
                     broker->uptimeSeconds);
    }

    if (const auto host = m_hostMetrics.read(); host.m_available)
    {
        appendMetric(body, "xmq_host_cpu_percent", "Whole-machine CPU use, percent of all cores.", "gauge",
                     host.m_cpuPercentTotal);
        appendMetric(body, "xmq_host_cpu_cores", "Hardware threads on this machine.", "gauge",
                     static_cast<uint64_t>(host.m_cpuCores));
        appendMetric(body, "xmq_host_memory_used_bytes", "Memory in use, excluding reclaimable cache.", "gauge",
                     host.m_memoryUsed);
        appendMetric(body, "xmq_host_memory_available_bytes", "Memory available to start new work.", "gauge",
                     host.m_memoryAvailable);
        appendMetric(body, "xmq_host_memory_total_bytes", "Physical memory installed.", "gauge",
                     host.m_memoryTotal);
        appendMetric(body, "xmq_process_cpu_percent", "This process's CPU use, percent of one core.", "gauge",
                     host.m_cpuPercentProcess);
        appendMetric(body, "xmq_process_resident_memory_bytes", "Resident memory of this process.", "gauge",
                     host.m_memoryUsedByProcess);
        appendMetric(body, "xmq_process_uptime_seconds", "Seconds since this process started.", "gauge",
                     host.m_processUptimeSeconds);
    }

    return body;
}

} // namespace xmq
