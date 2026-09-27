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

#include "Reporter.h"
#include "base/ProtocolVersion.h"
#include "client/MqttClient.h"
#include "common/ConnectCredentials.h"
#include <sptk5/CommandLine.h>
#include <sptk5/cutils>
#include <sptk5/net/Host.h>

namespace xmq {

class RunDefinition final
{
public:
    /**
     * @brief Constructor.
     */
    RunDefinition() = default;

    /**
     * @brief Destructor.
     */
    ~RunDefinition() = default;

    void parseUri(sptk::CommandLine& commandLine);
    /**
     * @brief Read parsed command line arguments into run definition fields.
     * @param commandLine       Parsed command line.
     */
    void load(sptk::CommandLine& commandLine);

    [[nodiscard]] size_t getSendCount() const
    {
        return m_sendCount;
    }

    [[nodiscard]] size_t getReceiveCount() const
    {
        return m_receiveCount;
    }

    std::unique_ptr<sptk::Host>         m_serverHost;                                          ///< Server host
    sptk::Strings                       m_bindAddresses;                                       ///< Bind to local interface IP address
    std::shared_ptr<ConnectCredentials> m_credentials;                                         ///< User credentials
    client::ConnectParameters           m_connectParameters;                                   //< Connect parameters
    ProtocolVersion                     m_protocolVersion = ProtocolVersion::MqttV31;          ///< MQTT protocol version
    std::chrono::seconds                m_disconnectAfterSeconds {0};                          ///< Disconnect after, seconds
    SMessageProperties                  m_connectProperties;                                   ///< Connect message properties
    SMessageProperties                  m_publishProperties;                                   ///< Publish message properties
    std::shared_ptr<MessageProperties>  m_commandConnectProperties;                            ///< Connect properties from -D, or null
    std::shared_ptr<MessageProperties>  m_commandPublishProperties;                            ///< Publish properties from -D, or null
    size_t                              m_sendCount = 1;                                       ///< Message send count
    size_t                              m_receiveCount = 1;                                    ///< Message receive count
    sptk::Buffer                        m_message;                                             ///< Message to send
    sptk::Strings                       m_topics;                                              ///< Topics to send/receive messages to
    Qos                                 m_qos = Qos::Qos1;                                     ///< QoS
    size_t                              m_connectRate = 0;                                     ///< Connect rate
    size_t                              m_messageRate = 0;                                     ///< Message send rate
    bool                                m_messageReadFromStdin = false;                        ///< Read messages from stdin, one per line
    bool                                m_messageWriteToStdout = true;                         ///< Write received messages to stdout
    sptk::LogPriority                   m_logPriority = sptk::LogPriority::Info;               ///< Log priority
    size_t                              m_sessionCount = 1;                                    ///< Session count
    std::shared_ptr<sptk::SSLKeys>      m_sslKeys;                                             ///< SSL keys
    Reporter::CountersFormat            m_showCounters {Reporter::CountersFormat::NoCounters}; ///< Show progress counters
    bool                                m_sendTimestamp = false;                               ///< Inject a send timestamp at the start of each message, xmq_pub only.
    bool                                m_retain = false;                                      ///< Publish as retained, xmq_pub only.
};

} // namespace xmq
