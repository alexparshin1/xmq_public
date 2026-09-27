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

class XMQ_EXPORT XMQ_MqttClientTests
    : public testing::Test
    , public testing::WithParamInterface<ProtocolVersion>
{
public:
    static constexpr auto              m_sixtySeconds = std::chrono::seconds(60);
    static constexpr auto              m_smallTimeout = std::chrono::milliseconds(100);
    static std::shared_ptr<sptk::Host> m_mqttHost;
    static std::shared_ptr<sptk::Host> m_mqttSslHost;
    static std::stringstream           m_logStream;

    /**
     * @brief Constructor.
     */
    XMQ_MqttClientTests() = default;

    XMQ_MqttClientTests(const XMQ_MqttClientTests&) = delete;
    XMQ_MqttClientTests(XMQ_MqttClientTests&&) = delete;
    XMQ_MqttClientTests& operator=(const XMQ_MqttClientTests&) = delete;
    XMQ_MqttClientTests& operator=(XMQ_MqttClientTests&&) = delete;

    /**
     * @brief Destructor.
     */
    ~XMQ_MqttClientTests() override = default;

    /**
     * @brief Execute once for all tests.
     */
    void SetUp() override;

    static std::shared_ptr<sptk::LogEngine> createLogEngine(sptk::LogPriority minPriority, const std::string& kind = "file");
};

} // namespace xmq
