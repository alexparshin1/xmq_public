/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "server/PrometheusExporter.h"
#include "server/Settings/PasswordHash.h"

#include <gtest/gtest.h>
#include <sptk5/net/TCPSocket.h>

#include <array>
#include <chrono>
#include <stdexcept>
#include <string>

using namespace std;
using namespace std::chrono_literals;
using namespace sptk;
using namespace xmq;

namespace {

/**
 * @brief An ephemeral port nothing is listening on at the moment the caller gets it.
 *
 * Bound and released rather than guessed from a fixed range: the suite runs beside other
 * processes, and a port taken here fails the test instead of the feature.
 */
uint16_t freePort()
{
    TCPSocket probe;
    probe.host(Host("127.0.0.1", 0));
    probe.listen(0, false);

    sockaddr_in address {};
    socklen_t   length = sizeof(address);
    if (::getsockname(probe.fd(), reinterpret_cast<sockaddr*>(&address), &length) != 0)
    {
        probe.close();
        throw runtime_error("getsockname failed");
    }
    const auto port = ntohs(address.sin_port);
    probe.close();
    return port;
}

/// Send one request and read the reply until the server closes the connection.
string request(const uint16_t port, const string& path, const string& extraHeaders = {})
{
    TCPSocket client;
    client.open(Host("127.0.0.1", port), Socket::OpenMode::CONNECT, true, 2s);
    client.write(String("GET " + path + " HTTP/1.1\r\nHost: localhost\r\n" + extraHeaders +
                        "Connection: close\r\n\r\n"));

    string response;
    while (client.readyToRead(2s))
    {
        array<uint8_t, 4096> buffer {};
        const auto received = client.read(buffer.data(), buffer.size());
        if (received == 0)
        {
            break;
        }
        response.append(reinterpret_cast<const char*>(buffer.data()), received);
    }
    return response;
}

} // namespace

TEST(PrometheusExporterTests, OffersBuiltInMetricsWhileTheBrokerIsStopped)
{
    PrometheusExporter exporter([] { return optional<BrokerMetrics> {}; }, "node-a", nullptr, "127.0.0.1",
                                freePort());

    const auto body = exporter.render();
    EXPECT_NE(string::npos, body.find("xmq_up 0")) << body;
    EXPECT_NE(string::npos, body.find("xmq_build_info{version=\"")) << body;
    EXPECT_NE(string::npos, body.find("node=\"node-a\"")) << body;
    // A stopped broker has no counters, and zeroes would describe an idle one.
    EXPECT_EQ(string::npos, body.find("xmq_broker_clients_connected")) << body;
}

TEST(PrometheusExporterTests, ReportsBrokerCountersAndLabels)
{
    BrokerMetrics metrics;
    metrics.clientsConnected = 7;
    metrics.publishReceived = 1234;
    metrics.queueDelivery = 3;
    metrics.uptimeSeconds = 42;

    PrometheusExporter exporter([metrics] { return optional<BrokerMetrics> {metrics}; }, "node-\"b\"", nullptr,
                                "127.0.0.1", freePort());

    const auto body = exporter.render();
    EXPECT_NE(string::npos, body.find("xmq_up 1")) << body;
    EXPECT_NE(string::npos, body.find("xmq_broker_clients_connected 7")) << body;
    EXPECT_NE(string::npos, body.find("xmq_broker_publish_received_total 1234")) << body;
    EXPECT_NE(string::npos, body.find("xmq_broker_queue_delivery 3")) << body;
    EXPECT_NE(string::npos, body.find("xmq_broker_uptime_seconds 42")) << body;
    // A quote in a label value must not end the label.
    EXPECT_NE(string::npos, body.find("node=\"node-\\\"b\\\"\"")) << body;
}

TEST(PrometheusExporterTests, AnswersScrapesAndRefusesEverythingElse)
{
    const auto port = freePort();
    PrometheusExporter exporter([] { return optional<BrokerMetrics> {}; }, "node-a", nullptr, "127.0.0.1", port);
    exporter.start();

    const auto scrape = request(port, "/metrics");
    EXPECT_NE(string::npos, scrape.find("HTTP/1.1 200 OK")) << scrape;
    EXPECT_NE(string::npos, scrape.find("Content-Type: text/plain; version=0.0.4")) << scrape;
    EXPECT_NE(string::npos, scrape.find("xmq_up 0")) << scrape;

    const auto missing = request(port, "/");
    EXPECT_NE(string::npos, missing.find("HTTP/1.1 404 Not Found")) << missing;

    exporter.stop();
}

TEST(PrometheusExporterTests, RefusesAPortAnotherEndpointAlreadyServes)
{
    const auto port = freePort();
    PrometheusExporter first([] { return optional<BrokerMetrics> {}; }, "node-a", nullptr, "127.0.0.1", port);
    first.start();

    // The second endpoint must fail rather than share the port with the first and answer a scrape
    // from whichever process the kernel chooses. The exporter sets SO_REUSEPORT, because SPTK
    // offers SO_REUSEADDR only with it, so the refusal is the availability check before the bind.
    PrometheusExporter second([] { return optional<BrokerMetrics> {}; }, "node-b", nullptr, "127.0.0.1", port);
    EXPECT_THROW(second.start(), Exception);

    first.stop();
}

TEST(PrometheusExporterTests, RequiresBasicAuthenticationWhenAUsernameIsSet)
{
    const auto port = freePort();
    PrometheusExporter exporter([] { return optional<BrokerMetrics> {}; }, "node-a", nullptr, "127.0.0.1", port,
                                "metrics", PasswordHash::hash("Secret1!"));
    exporter.start();

    const auto anonymous = request(port, "/metrics");
    EXPECT_NE(string::npos, anonymous.find("HTTP/1.1 401 Unauthorized")) << anonymous;
    EXPECT_NE(string::npos, anonymous.find("WWW-Authenticate: Basic")) << anonymous;

    const auto wrong = request(port, "/metrics",
                               "Authorization: Basic bWV0cmljczp3cm9uZw==\r\n");
    EXPECT_NE(string::npos, wrong.find("HTTP/1.1 401 Unauthorized")) << wrong;

    const auto correct = request(port, "/metrics",
                                 "Authorization: Basic bWV0cmljczpTZWNyZXQxIQ==\r\n");
    EXPECT_NE(string::npos, correct.find("HTTP/1.1 200 OK")) << correct;
    EXPECT_NE(string::npos, correct.find("xmq_up 0")) << correct;

    exporter.stop();
}

TEST(PrometheusExporterTests, HealthProbesIgnoreTheMetricsAuthenticationAndSayWhyNotReady)
{
    const auto port = freePort();
    bool       ready = false;
    PrometheusExporter exporter([] { return optional<BrokerMetrics> {}; }, "node-a", nullptr, "127.0.0.1", port,
                                "metrics", PasswordHash::hash("Secret1!"),
                                [&ready]
                                {
                                    return ready ? Readiness {true, {}} : Readiness {false, "MQTT is not running"};
                                });
    exporter.start();

    const auto health = request(port, "/healthz");
    EXPECT_NE(string::npos, health.find("HTTP/1.1 200 OK")) << health;
    EXPECT_NE(string::npos, health.find("{\"status\":\"ok\"}")) << health;

    const auto notReady = request(port, "/readyz");
    EXPECT_NE(string::npos, notReady.find("HTTP/1.1 503 Service Unavailable")) << notReady;
    EXPECT_NE(string::npos, notReady.find("MQTT is not running")) << notReady;

    // The probes do not need credentials, but the counters still do.
    const auto metrics = request(port, "/metrics");
    EXPECT_NE(string::npos, metrics.find("HTTP/1.1 401 Unauthorized")) << metrics;

    ready = true;
    const auto nowReady = request(port, "/readyz");
    EXPECT_NE(string::npos, nowReady.find("HTTP/1.1 200 OK")) << nowReady;
    EXPECT_NE(string::npos, nowReady.find("{\"status\":\"ready\"}")) << nowReady;

    exporter.stop();
}
