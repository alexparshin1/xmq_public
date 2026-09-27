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

#include "ConnectCredentials.h"
#include "base/Message.h"
#include "base/MessageProperties.h"
#include "base/ProtocolVersion.h"
#include "base/ReasonCode.h"

namespace xmq {

/**
 * @brief Last Will information.
 */
struct LastWillInfo
{
    std::string        m_topic;           ///< Topic to send last will message
    std::string        m_message;         ///< Last will message
    bool               m_retain {false};  ///< Message isRetain flag
    Qos                m_qos {Qos::Qos0}; ///< Message QoS
    uint32_t           m_willDelay {0};   ///< Message delay, seconds (MQTT5 only)
    SMessageProperties m_properties {};   ///< Message properties (MQTT5 only)
};

using SLastWillInfo = std::shared_ptr<LastWillInfo>;

/**
 * @brief Connect message parameters.
 */
struct ConnectMessageParameters : ConnectCredentials
{
    ConnectMessageParameters() = default;
    explicit ConnectMessageParameters(const ConnectCredentials& credentials,
                                      const SLastWillInfo&      lastWill = nullptr,
                                      bool                      cleanSession = true,
                                      ProtocolVersion           protocolVersion = ProtocolVersion::MqttV31,
                                      uint16_t                  keepAliveSeconds = TenMinutes);

    constexpr static auto TenMinutes = 600;
    ProtocolVersion       m_protocolVersion {ProtocolVersion::MqttV31}; ///< MQTT protocol version
    uint8_t               m_cleanSession {0};                           ///< Clean session flag
    uint16_t              m_keepAliveSec {0};                           ///< Session keepalive seconds
    SLastWillInfo         m_lastWill;                                   ///< Optional last will information.
};

using SConnectMessageParameters = std::shared_ptr<ConnectMessageParameters>;

/**
 * @brief The Connect message.
 */
class XMQ_EXPORT ConnectMessage final
    : public Message
{
public:
    /**
     * @brief Default constructor.
     */
    ConnectMessage();

    /**
     * @brief Constructor
     * @param credentials       Connect credentials.
     * @param lastWill          Last will information.
     * @param cleanSession      Clean session flag.
     * @param protocolVersion   Protocol version.
     * @param keepAliveSeconds  Session keepalive seconds.
     */
    explicit ConnectMessage(const ConnectCredentials& credentials,
                            const SLastWillInfo&      lastWill = nullptr,
                            bool                      cleanSession = true,
                            ProtocolVersion           protocolVersion = ProtocolVersion::MqttV31,
                            uint16_t                  keepAliveSeconds = TenMinutes);

    ConnectMessage(const ConnectMessage&) = delete;
    ConnectMessage(ConnectMessage&&) = default;
    ConnectMessage& operator=(const ConnectMessage&) = delete;
    ConnectMessage& operator=(ConnectMessage&&) = default;

    /**
     * @brief Destructor.
     */
    ~ConnectMessage() override = default;

    /**
     * @brief Get client id.
     * @return Client id.
     */
    [[nodiscard]] const std::string& getClientId() const
    {
        return m_parameters->getClientId();
    }

    /**
     * @brief Get username.
     * @return Username.
     */
    [[nodiscard]] const std::string& getUsername() const;

    /**
     * @brief Set username.
     * @param username          Username.
     */
    void setUsername(std::string_view username) const;

    /**
     * @brief Get password.
     * @return Password.
     */
    [[nodiscard]] const std::string& getPassword() const;

    /**
     * @brief Set password.
     * @return Password.
     */
    void setPassword(std::string_view password) const;

    /**
     * @brief Get string representation.
     * @return
     */
    [[nodiscard]] std::string toString() const override;

    ReasonCode getReasonCode() const
    {
        return m_responseCode;
    }

    void setReasonCode(const ReasonCode responseCode)
    {
        m_responseCode = responseCode;
    }

    bool getSessionPresent() const
    {
        return m_sessionPresent;
    }

    SConnectMessageParameters getParameters() const
    {
        return m_parameters;
    }

private:
    constexpr static auto     TenMinutes = 600;
    SConnectMessageParameters m_parameters;                         ///< Connect message parameters
    ReasonCode                m_responseCode {ReasonCode::Success}; ///< Response code, filled in by server
    bool                      m_sessionPresent {false};             ///< Session present flag, filled in by server
};

/**
 * @brief Shared pointer to ConnectMessage.
 */
using SConnectMessage = std::shared_ptr<ConnectMessage>;

} // namespace xmq
