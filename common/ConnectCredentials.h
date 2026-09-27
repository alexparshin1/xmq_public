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

#include <sptk5/Buffer.h>
#include <string>

namespace xmq {

/**
 * @brief Represents the credentials required for establishing a connection.
 */
class ConnectCredentials
{
public:
    ConnectCredentials() = default;
    ConnectCredentials(const ConnectCredentials&) = default;

    ConnectCredentials(std::string_view clientId, std::string_view username, std::string_view password)
        : m_clientId(clientId)
        , m_username(username)
        , m_password(password)
    {
    }

    [[nodiscard]] const std::string& getClientId() const
    {
        return m_clientId;
    }

    void setClientId(const std::string_view clientId)
    {
        m_clientId = clientId;
    }

    [[nodiscard]] const std::string& getUsername() const
    {
        return m_username;
    }

    void setUsername(const std::string_view username)
    {
        m_username = username;
    }

    [[nodiscard]] const std::string& getPassword() const
    {
        return m_password;
    }

    void setPassword(const std::string_view password)
    {
        m_password = password;
    }

private:
    std::string m_clientId;
    std::string m_username;
    std::string m_password;
};

} // namespace xmq
