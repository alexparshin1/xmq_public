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

#include "HostMetrics.h"
#include "JWTManager.h"
#include "server/Server.h"
#include "service/CXmqServiceBase.h"

namespace xmq {

class ControlService final : public CXmqServiceBase
{
public:
    /**
     * @brief Constructor.
     */
    explicit ControlService(IServerController* controller);

    /**
     * @brief Destructor.
     */
    ~ControlService() override = default;

    void GetClientSessions(const CGetClientSessions& input, CGetClientSessionsResponse& output, sptk::HttpAuthentication* authentication) override;
    void GetSubscriptions(const CGetSubscriptions& input, CGetSubscriptionsResponse& output, sptk::HttpAuthentication* authentication) override;
    void SSLKeysControl(const CSSLKeysControl& input, CSSLKeysControlResponse& output, sptk::HttpAuthentication* auth) override;
    void LimitsControl(const CLimitsControl& input, CLimitsControlResponse& output, sptk::HttpAuthentication* authentication) override;
    void ListenerControl(const CListenerControl& input, CListenerControlResponse& output, sptk::HttpAuthentication* authentication) override;
    void Login(const CLogin& input, CLoginResponse& output, sptk::HttpAuthentication*) override;
    void LoggingControl(const CLoggingControl& input, CLoggingControlResponse& output, sptk::HttpAuthentication* authentication) override;
    void PersistenceControl(const CPersistenceControl& input, CPersistenceControlResponse& output, sptk::HttpAuthentication* authentication) override;
    void ServiceControl(const CServiceControl& input, CServiceControlResponse& output, sptk::HttpAuthentication* auth) override;
    void InitialSetupControl(const CInitialSetupControl& input, CInitialSetupControlResponse& output, sptk::HttpAuthentication* auth) override;
    void ServerControl(const CServerControl& input, CServerControlResponse& output, sptk::HttpAuthentication* auth) override;
    void GetStatistics(const CGetStatistics& input, CGetStatisticsResponse& output, sptk::HttpAuthentication* auth) override;
    sptk::String findBridgePassword(int bridgeId) const;
    void UserControl(const CUserControl& input, CUserControlResponse& output, sptk::HttpAuthentication* authentication) override;

    void ExtensionControl(const CExtensionControl& input, CExtensionControlResponse& output,
                          sptk::HttpAuthentication* authentication) override;

    void UserGroupControl(const CUserGroupControl& input, CUserGroupControlResponse& output,
                          sptk::HttpAuthentication* authentication) override;

    void SetupState(const CSetupState& input, CSetupStateResponse& output,
                    sptk::HttpAuthentication* authentication) override;

    /// Applies the membership an account arrived with, when it arrived with one.
    void applyGroups(const CUser& user) const;
    void BridgeControl(const CBridgeControl& input, CBridgeControlResponse& output, sptk::HttpAuthentication* authentication) override;

    /**
     * @brief The running server.
     * @return The server, or nullptr while MQTT is stopped - every caller must check.
     */
    [[nodiscard]] Server* server() const;

    /**
     * @brief Server settings, which outlive any individual server and stay editable while
     * MQTT is stopped.
     */
    [[nodiscard]] const std::shared_ptr<Settings>& getSettings() const;

    /**
     * @brief The running server, for operations that cannot mean anything without one.
     * @throws sptk::Exception when MQTT is stopped.
     */
    [[nodiscard]] Server& requireServer() const;

private:
    /**
     * @brief Install an uploaded certificate and key for the configuration interface.
     * @param keys              Uploaded file content.
     * @param output            Receives a note when the files were installed but not put to use.
     * @throws sptk::Exception when only one of the two was sent, or the files cannot be written.
     */
    void installWebServiceCertificate(const CWebServiceKeys& keys, CSSLKeysControlResponse& output) const;

    /**
     * @brief Issue a fresh self-signed certificate for the configuration interface.
     *
     * Replaces whatever is installed, which is the difference between this and what happens at
     * startup. The way back from a certificate that has expired, or that names a host this
     * machine no longer answers to.
     *
     * @param output            Receives a note when the pair was issued but not put to use.
     * @throws sptk::Exception when the pair cannot be created.
     */
    void reissueWebServiceCertificate(CSSLKeysControlResponse& output) const;

    /**
     * @brief Record a newly installed pair and put the running interface on it.
     * @param certificateFile   Installed certificate.
     * @param privateKeyFile    Installed private key.
     * @param output            Receives a note when the interface kept the previous certificate.
     */
    void applyWebServiceCertificate(const std::filesystem::path& certificateFile,
                                    const std::filesystem::path& privateKeyFile,
                                    CSSLKeysControlResponse&     output) const;

    /**
     * @brief Trust another node by its certificate.
     *
     * Broker-to-broker links are between servers one administrator runs, so the peer's own
     * certificate is what vouches for it: a self-signed certificate is its own authority, and no
     * certificate authority has to exist for the link to be verified.
     *
     * @param peer              Name to keep it under, and the certificate in PEM form.
     * @throws sptk::Exception when the name or the certificate is unusable.
     */
    static void trustPeerCertificate(const CPeerCertificate& peer);

    /**
     * @brief Stop trusting a node.
     * @param peer              Name it was kept under.
     * @throws sptk::Exception when no peer is kept under that name.
     */
    static void distrustPeerCertificate(const CPeerCertificate& peer);

    /**
     * @brief Report the trusted peers and this node's own certificate.
     * @param output            Receives both.
     */
    static void reportPeerCertificates(CSSLKeysControlResponse& output);

    IServerController* m_controller;
    JWTManager m_jwtManager;

    /**
     * @brief Host and process figures for the dashboard.
     *
     * Held by the service rather than by the server, for the same reason the interface is: they
     * stay meaningful, and keep their CPU baseline, while the server is stopped.
     */
    HostMetrics m_hostMetrics;

    void authenticate(sptk::HttpAuthentication* authentication, CUser& user);

    /**
     * @brief Whether these credentials are the administrator of a server nobody has set up yet.
     *
     * A freshly installed server has no password on the administrator account, and no password
     * can be matched against one that does not exist - so the user manager refuses it, as it
     * must: the MQTT listeners ask the same question of the same accounts. The interface admits
     * it here instead, and only while it is bound to the loopback address, which is what
     * ServerController::controlServiceAddress() ties to the same condition.
     *
     * @param username          Account name offered.
     * @param password          Password offered, which must be empty for this to be true.
     * @return true if this is the administrator of a server with no password set.
     */
    [[nodiscard]] bool isUnconfiguredAdministrator(const sptk::String& username, const sptk::String& password) const;

    /**
     * @brief Refuse a setup naming a Redis that cannot be reached, before anything is written.
     *
     * On the setup page an address is what switches persistence on, and a broker pointed at a
     * Redis that is not there still starts: it falls back to keeping everything in memory and
     * says so in the log alone. Checked by connecting and speaking the protocol, so that
     * something else listening on the port is caught as well.
     *
     * @param setup             Settings the setup page sent.
     * @throws sptk::Exception when an address is given and nothing answers as Redis.
     */
    static void requireReachableRedis(const CInitialSetup& setup);
};

} // namespace xmq
