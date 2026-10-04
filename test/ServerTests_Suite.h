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

#include "server/Server.h"
#include <gtest/gtest.h>
#include <sptk5/cutils>

namespace xmq {

/**
 * @brief Base class for server tests.
 */
class XMQ_EXPORT ServerTests_Suite
    : public testing::Test
{
public:
    static constexpr uint16_t MqttTcpPortNumber = 1884;
    static constexpr uint16_t TestTcpPortNumber = 1880;
    static constexpr uint16_t TestSslPortNumber = 8880;
    static constexpr uint16_t TestServicePortNumber = 9880;
    static constexpr uint16_t TestBridgeTcpPortNumber = 1886;

    /**
     * @brief Execute before each test.
     */
    void SetUp() override;

    /**
     * @brief Whether this suite needs the bridges in the test configuration started.
     *
     * False for everything but the suites that test bridging. Two of the three configured bridges
     * point at a port that only exists while the cluster tests run, so anywhere else they retry for
     * the whole run and their attempts are counted by the broker alongside real clients.
     */
    [[nodiscard]] virtual bool bridgesNeeded() const
    {
        return false;
    }

    /**
     * @brief Execute after each test.
     */
    void TearDown() override;


    /**
     * @brief Execute after all tests.
     */
    static void TearDownTestSuite();

    /**
     * @brief Get the server instance.
     * @return.
     */
    [[nodiscard]] static std::shared_ptr<Server> server(const std::string& nodeName = "");

    /**
     * @brief Get the test log engine.
     * @return test log engine.
     */
    [[nodiscard]] static std::shared_ptr<sptk::LogEngine> logEngine();

    /**
     * @brief Set debug log mode.
     * @param debugMode         Debug mode.
     * @param nodeName          Optional node name.
     * @return local logger.
     */
    static std::shared_ptr<sptk::Logger> debugLog(bool debugMode = true, const std::string& nodeName = "");

    [[nodiscard]] static std::string testName();

    static void printTitle(std::string_view title);

protected:
    /**
     * @brief Create test MQTT server.
     * @param listenerPortTcp   TCP (not encrypted) listener port.
     * @param listenerPortSsl   SSL (encrypted) listener port.
     * @param servicePortTcp    WebService port number.
     * @param cleanStart        Clean start as reset database storage.
     * @param nodeName          Server node name.
     * @param userName          Username for all nodes.
     * @param password          Password for all nodes.
     * @param minLogLevel       Optional minimum log level.
     * @param persistence       Enable persistent storage. Pass false to run the server entirely
     *                          in memory, with no Redis - the configuration used by deployments
     *                          that don't need session recovery.
     */
    static SServer createServer(uint16_t listenerPortTcp = TestTcpPortNumber,
                                uint16_t listenerPortSsl = TestSslPortNumber,
                                uint16_t           servicePortTcp = TestServicePortNumber,
                                bool               cleanStart = true,
                                const std::string& nodeName = "primary",
                                const std::string& userName = "user",
                                const std::string& password = "secret",
                                sptk::LogPriority  minLogLevel = sptk::LogPriority::Info,
                                bool               persistence = true,
                                bool               enableBridges = false);

    /**
     * @brief Stop and destroy the XMQ server instance.
     */
    static void stopServers();

    /**
     * @brief Stop and destroy one server instance, leaving the others running.
     * @param nodeName          Node name the server was created with.
     */
    static void stopServer(const std::string& nodeName);

    /**
     * @brief Verify that a test listener port is not already in use.
     * Throws an exception if another process (usually a leftover xmq_unit_tests instance)
     * is already listening on the port. Because the server listeners use SO_REUSEPORT,
     * a second instance would otherwise bind successfully and silently share the port,
     * producing confusing test failures instead of a clear error.
     * @param port              Port number to check; 0 (disabled listener) is skipped.
     */
    static void checkPortIsFree(uint16_t port);

private:
    static std::map<std::string, SServer, std::less<>> m_servers; ///< MQTT server instance.
    static std::string                                 m_testSuiteName; ///< Test suite name.
    static std::shared_ptr<sptk::FileLogEngine>        m_logEngine; ///< Log engine.
};

} // namespace xmq
