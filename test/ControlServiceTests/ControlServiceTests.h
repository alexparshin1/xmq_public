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

#include "ServerTests/ServerTests.h"
#include "server/ControlService.h"
#include "server/ControlServiceListener.h"
#include "server/ServerController.h"

namespace xmq {

/**
 * @brief Controller standing in for the real one, around a server the fixture already started.
 *
 * ControlService reaches the server through a controller rather than holding one directly, so
 * that a stopped server leaves the interface intact. The tests need the same shape without
 * taking over the fixture's server lifecycle, so starting and stopping here only move the flag
 * the service reads.
 */
class XMQ_EXPORT TestServerController final : public IServerController
{
public:
    TestServerController(Server* server, std::shared_ptr<Settings> settings)
        : m_server(server)
        , m_settings(std::move(settings))
    {
    }

    [[nodiscard]] const std::shared_ptr<sptk::LogEngine>& getLogEngine() const override { return m_logEngine; }
    [[nodiscard]] sptk::String getServiceName() const override { return "XMQ Test Service"; }
    [[nodiscard]] sptk::String getServiceDescription() const override { return "XMQ Test Service"; }

    void startService() override {}
    void stopService() override {}
    bool isStopped(std::chrono::milliseconds) override { return m_server == nullptr; }

    bool startServing(std::string&) override
    {
        m_serving = true;
        return true;
    }

    /// The real one reads the configuration from disk; these tests are about the control service,
    /// not about what is on disk beside it.
    ExtensionHost::Report reloadExtensions() override
    {
        return {};
    }

    /// The real one rotates the log engine's file; this double has no log engine of its own to
    /// rotate, and no test here asks it to - only that SIGHUP's handling can reach the interface.
    std::filesystem::path rotateLog() override
    {
        return {};
    }

    /// These tests are about the control service, not about which extensions a broker happens to
    /// have loaded - and this double has no broker behind it to ask.
    std::vector<ExtensionHost::Description> describeExtensions() override
    {
        return {};
    }

    ExtensionHost::Report switchExtension(const std::string& name, bool) override
    {
        return ExtensionHost::Report::note(name + ": no broker behind this controller");
    }

    ExtensionHost::Report setExtensionSettings(const std::string& name,
                                               const std::map<std::string, std::string>&) override
    {
        return ExtensionHost::Report::note(name + ": no broker behind this controller");
    }

    void stopServing() override
    {
        m_serving = false;
    }

    [[nodiscard]] bool isServing() const override { return m_serving && m_server != nullptr; }

    // The fixture serves no interface, so there is nothing to move; the port is recorded instead,
    // so a test can see what was asked for.
    bool moveControlService(uint16_t port, std::string&) override
    {
        m_controlServicePort = port;
        return true;
    }

    [[nodiscard]] uint16_t controlServicePort() const { return m_controlServicePort; }

    // The interface a test serves, when it serves one at all. Set by the fixture that starts a
    // listener, so that installing a certificate reaches it the way it reaches the real one.
    void setControlServiceListener(SControlServiceListener listener)
    {
        m_controlServiceListener = std::move(listener);
    }

    bool updateControlServiceKeys(const std::filesystem::path& certificateFile,
                                  const std::filesystem::path& privateKeyFile,
                                  std::string&                 reason) override
    {
        if (!m_controlServiceListener)
        {
            reason = "The configuration interface is not running.";
            return false;
        }

        try
        {
            m_controlServiceListener->useKeys(std::make_shared<sptk::SSLKeys>(privateKeyFile, certificateFile));
            return true;
        }
        catch (const sptk::Exception& exception)
        {
            reason = exception.what();
            return false;
        }
    }

    bool setControlServiceEncrypted(const bool encrypted, std::string& reason) override
    {
        if (!m_controlServiceListener)
        {
            reason = "The configuration interface is not running.";
            return false;
        }

        try
        {
            // The keys the fixture built the listener with, rather than a certificate prepared
            // from settings: a test must not write into the machine's certificates directory just
            // by turning encryption back on.
            m_controlServiceListener->serveEncrypted(encrypted ? m_webServiceKeys : nullptr);
            return true;
        }
        catch (const sptk::Exception& exception)
        {
            reason = exception.what();
            return false;
        }
    }

    void setWebServiceKeys(std::shared_ptr<sptk::SSLKeys> sslKeys) { m_webServiceKeys = std::move(sslKeys); }

    [[nodiscard]] Server* getServer() const override { return m_serving ? m_server : nullptr; }

    [[nodiscard]] const std::shared_ptr<Settings>& getSettings() const override { return m_settings; }

private:
    Server*                          m_server;
    std::shared_ptr<Settings>        m_settings;
    std::shared_ptr<sptk::LogEngine> m_logEngine;
    bool                             m_serving {true};
    uint16_t                         m_controlServicePort {0};
    SControlServiceListener          m_controlServiceListener;
    std::shared_ptr<sptk::SSLKeys>   m_webServiceKeys;
};

class XMQ_EXPORT XMQ_ControlServiceTests : public XMQ_ServerTests
{
protected:
    void        SetUp() override;
    std::string Login(const std::string& username, const std::string& password) const;

    std::shared_ptr<TestServerController> m_controller;
    std::shared_ptr<ControlService>       m_controlService;
};

} // namespace xmq
