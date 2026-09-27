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

#include "base/Message.h"
#include "base/ProtocolVersion.h"
#include "common/ConnectCredentials.h"
#include "test/ServerTests_Suite.h"

#include <cstdint>
#include <future>
#include <sptk5/OsProcess.h>
#include <sptk5/cnet>

namespace xmq {

/**
 * @brief Base class for an external MQTT client.
 * Currently supports Mosquitto MQTT clients (mosquitto_pub and mosquitto_sub)
 * and XMQ test MQTT clients (xmq_pub and xmq_sub).
 */
class ExternalClient final
{
public:
    using StringProperties = std::map<std::string, std::string, std::less<>>;

    /**
     * @brief MQTT client kind.
     */
    enum class ClientKind : uint8_t
    {
        XMQ,
        Mosquitto
    };

    /**
     * @brief Client connection encryption mode.
     */
    enum class EncryptionMode : uint8_t
    {
        None,
        Tls
    };

    /**
     * @brief MQTT session mode.
     */
    enum class SessionMode : uint8_t
    {
        Clean,
        Continue
    };

    /**
     * @brief Console output mode.
     */
    enum class OutputMode : uint8_t
    {
        Quiet,
        Debug
    };

    /**
     * @brief Constructor.
     * @param clientKind        Client kind.
     * @param clientId          Client id.
     * @param protocolVersion   MQTT protocol version.
     * @param host              Server host.
     * @param encryptionMode    Connection encryption mode.
     */
    ExternalClient(ClientKind             clientKind,
                   std::string_view       clientId,
                   const ProtocolVersion& protocolVersion = ProtocolVersion::MqttV5,
                   sptk::Host             host = sptk::Host("localhost", ServerTests_Suite::TestTcpPortNumber),
                   const EncryptionMode&  encryptionMode = EncryptionMode::None);

    /**
     * @brief Start subscriber.
     * @param topic             Topic to subscribe to.
     * @param qos               QOS.
     * @param exitAfterNumberOfMessages Exit after receiving N messages.
     * @param sessionMode       Session mode.
     * @param properties        Connect message properties.
     * @param outputMode        Console output mode.
     * @param messageTimeout    Exit after message receive timeout.
     * @return process handler.
     */
    sptk::SOsProcess startSubscriber(const sptk::String& topic, Qos qos, uint32_t exitAfterNumberOfMessages,
                                     SessionMode sessionMode, const StringProperties& properties,
                                     OutputMode outputMode, std::chrono::seconds messageTimeout) const;

    /**
     * @brief Start publisher.
     * @param topic             Topic to subscribe to.
     * @param qos               QOS.
     * @param sendMessageCount  Exit after sending N messages.
     * @param properties        Message properties.
     * @param outputMode        Console output mode.
     * @param messageSize       Message payload size.
     * @return process handler.
     */
    sptk::SOsProcess startPublisher(const sptk::String& topic, Qos qos = Qos::Qos1, size_t sendMessageCount = 10,
                                    const StringProperties& properties = {}, OutputMode outputMode = OutputMode::Quiet,
                                    size_t messageSize = 128) const;

private:
    sptk::Host                          m_host;
    std::unique_ptr<ConnectCredentials> m_credentials;
    ProtocolVersion                     m_protocolVersion;
    EncryptionMode                      m_encryptionMode;
    ClientKind                          m_clientKind;

    static sptk::String publisherExecutable(ClientKind externalClientKind);
    static sptk::String subscriberExecutable(ClientKind externalClientKind);

    sptk::Strings initBasicOptions(
        const sptk::String& topic, const Qos& qos, const SessionMode& sessionMode,
        const StringProperties& properties, const OutputMode& outputMode) const;

    static sptk::SOsProcess executeOsCommandAsync(
        const sptk::String& command, const sptk::Strings& arguments,
        const sptk::String& extraOptions = "", OutputMode outputMode = {});

    static std::string  protocolVersionToString(ProtocolVersion protocolVersion);
    static sptk::String propertiesToString(std::string_view mqttCommand, const std::map<std::string, std::string, std::less<>>& properties);
};

} // namespace xmq
