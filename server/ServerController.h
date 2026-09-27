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

#include "Server.h"

#include <sptk5/cutils>

namespace xmq {
/**
 * Server that is controlled by OSService.
 */
class XMQ_EXPORT IServerController
{
public:
    virtual ~IServerController() = default;

    /**
     * @return Server's log engine.
     */
    [[nodiscard]] virtual const std::shared_ptr<sptk::LogEngine>& getLogEngine() const = 0;

    /**
     * @return Service name.
     */
    [[nodiscard]] virtual sptk::String getServiceName() const = 0;

    /**
     * @return Service name.
     */
    [[nodiscard]] virtual sptk::String getServiceDescription() const = 0;

    /**
     * @brief Start server execution.
     */
    virtual void startService() = 0;

    /**
     * Stop server execution.
     */
    virtual void stopService() = 0;

    /**
     * @brief Wait until server is stopped or timeout occurs.
     * @param sleepInterval     Sleep interval.
     * @return True if the server was stopped.
     */
    virtual bool isStopped(std::chrono::milliseconds sleepInterval) = 0;

    /**
     * @brief Start serving MQTT, if it is not being served already.
     *
     * Distinct from startService(), which is the process lifetime: this brings up an MQTT server
     * within a process that is already running, and leaves the configuration interface untouched.
     *
     * @param reason        Receives why the server could not be started, when it could not.
     * @return true if MQTT is being served when this returns.
     */
    /**
     * @brief Re-read the extension configuration on a running broker.
     *
     * @return One line per extension that was affected. Empty when nothing had changed, which is
     *         itself worth saying back: an operator who edited the wrong file needs to know that
     *         the broker found nothing to do.
     */
    virtual ExtensionHost::Report reloadExtensions() = 0;

    /// What the interface shows on the Extensions screen. Empty while the MQTT server is stopped:
    /// extensions live inside it, so there is nothing loaded to describe.
    virtual std::vector<ExtensionHost::Description> describeExtensions() = 0;

    /// Switches one on or off on a running broker. Enable also starts it, disable also stops it.
    virtual ExtensionHost::Report switchExtension(const std::string& name, bool on) = 0;

    /// Writes an extension's settings back to the file its entry came from, then applies them
    /// without stopping the broker. Written first: a value that cannot be written is never applied.
    virtual ExtensionHost::Report setExtensionSettings(const std::string&                        name,
                                               const std::map<std::string, std::string>& settings) = 0;

    virtual bool startServing(std::string& reason) = 0;

    /**
     * @brief Stop serving MQTT, keeping the process and the configuration interface alive.
     *
     * The server object is destroyed rather than quiesced, so its listeners, sessions, bridges,
     * delivery threads and storage connections all go with it.
     */
    virtual void stopServing() = 0;

    /**
     * @return true if an MQTT server is currently running.
     */
    [[nodiscard]] virtual bool isServing() const = 0;

    /**
     * @brief Serve the configuration interface on another port, without restarting the process.
     *
     * So that a page which has just changed the interface port can send the browser to the new
     * one, rather than telling someone to restart the service and find their way back by hand.
     *
     * The address is decided here rather than passed in: it follows whether the administrator
     * has a password yet, so a setup that has just set one moves the interface off the loopback
     * address by calling this with the port it is already on.
     *
     * @param port          Port to serve the interface on.
     * @param reason        Receives why the interface could not move, when it could not.
     * @return true if the interface is now served on that port.
     */
    virtual bool moveControlService(uint16_t port, std::string& reason) = 0;

    /**
     * @brief Serve the configuration interface with a newly installed certificate.
     *
     * So that replacing the certificate does not require a restart, and does not disconnect the
     * session that replaced it. Connections already open keep the previous certificate.
     *
     * @param certificateFile   Certificate to serve.
     * @param privateKeyFile    Its private key.
     * @param reason            Receives why the interface kept the old certificate, when it did.
     * @return true if the interface is now serving the new certificate.
     */
    virtual bool updateControlServiceKeys(const std::filesystem::path& certificateFile,
                                          const std::filesystem::path& privateKeyFile,
                                          std::string&                 reason) = 0;

    /**
     * @brief Serve the configuration interface over the other scheme, without restarting.
     *
     * Turning encryption on prepares a certificate the same way starting up does, so an interface
     * that has never been encrypted does not need one installed first.
     *
     * @param encrypted     True to serve HTTPS, false to serve plain HTTP.
     * @param reason        Receives why the scheme did not change, when it did not.
     * @return true if the interface is now served over the requested scheme.
     */
    virtual bool setControlServiceEncrypted(bool encrypted, std::string& reason) = 0;

    /**
     * @return The running server, or nullptr while MQTT is stopped.
     */
    [[nodiscard]] virtual Server* getServer() const = 0;

    /**
     * @return Server settings, which outlive any individual server.
     */
    [[nodiscard]] virtual const std::shared_ptr<Settings>& getSettings() const = 0;
};

/**
 * Server that is controlled by OSService.
 */
class XMQ_EXPORT ServerController final : public IServerController
{
public:
    /**
     * @brief Constructor.
     * @param settings          Server settings.
     */
    explicit ServerController(const std::shared_ptr<Settings>& settings);

    ~ServerController() override = default;

    /**
     * @return Server's log engine.
     */
    [[nodiscard]] const std::shared_ptr<sptk::LogEngine>& getLogEngine() const override;

    /**
     * @return Service name.
     */
    [[nodiscard]] sptk::String getServiceName() const override;

    /**
     * @return Service description.
     */
    [[nodiscard]] sptk::String getServiceDescription() const override;

    /**
     * Start server execution.
     */
    void startService() override;

    /**
     * Stop server execution.
     */
    void stopService() override;

    /**
     *
     * @param sleepInterval     Sleep interval.
     * @return True if the server was stopped.
     */
    bool isStopped(std::chrono::milliseconds sleepInterval) override;

    ExtensionHost::Report            reloadExtensions() override;
    std::vector<ExtensionHost::Description> describeExtensions() override;
    ExtensionHost::Report            switchExtension(const std::string& name, bool on) override;
    ExtensionHost::Report            setExtensionSettings(
        const std::string& name, const std::map<std::string, std::string>& settings) override;
    bool                             startServing(std::string& reason) override;
    void                             stopServing() override;
    [[nodiscard]] bool               isServing() const override;
    bool                             moveControlService(uint16_t port, std::string& reason) override;
    bool                             updateControlServiceKeys(const std::filesystem::path& certificateFile,
                                                              const std::filesystem::path& privateKeyFile,
                                                              std::string&                 reason) override;
    bool                             setControlServiceEncrypted(bool encrypted, std::string& reason) override;
    [[nodiscard]] Server*            getServer() const override;
    [[nodiscard]] const std::shared_ptr<Settings>& getSettings() const override;

    /**
     * @brief The file the log engine will write to, from what the configuration asked for.
     *
     * A configured name with no directory in it is resolved against the logs directory, keeping
     * the name. An absolute or relative path with a directory is taken as given.
     *
     * @param settings          Server settings.
     * @return path of the log file.
     */
    static std::filesystem::path logFilePath(const Settings& settings);

private:
    mutable std::mutex               m_mutex;     ///< Guards the server instance against concurrent start/stop.
    std::shared_ptr<Settings>        m_settings;  ///< Server settings.
    std::shared_ptr<sptk::LogEngine> m_logEngine; ///< Server log engine.

    /**
     * @brief Fires the daily log rotation.
     *
     * Its own timer rather than the server's: the log exists whether or not the broker managed to
     * start serving, and a server that failed to start is exactly the one whose log matters.
     */
    sptk::Timer       m_logRotationTimer;
    sptk::STimerEvent m_logRotationEvent; ///< The next rotation, kept so that it can be cancelled.

    /**
     * @brief Arranges for the log to be set aside at the next midnight, and again every midnight.
     *
     * Each firing schedules the one after it rather than repeating on a 24-hour interval, so the
     * rotation stays on midnight instead of drifting to whatever time the server happened to start,
     * and follows the clock through a change of daylight saving.
     */
    void scheduleLogRotation();

    /**
     * @brief How many rotated logs the configuration asks to keep.
     * @return the number, or LogEngine::keepAllArchives when none are to be deleted.
     */
    [[nodiscard]] size_t logsToKeep() const;
    std::unique_ptr<Server>          m_xmqServer; ///< Server instance, absent while MQTT is stopped.

    /**
     * @brief Configuration interface listener.
     *
     * Owned here rather than by the server, so that stopping MQTT can destroy the server outright
     * while the interface carries on. It is what the user is pressing Start in, so it must outlive
     * every server it starts - including one that fails to start at all.
     */
    SControlServiceListener m_controlServiceListener;

    /**
     * @brief Bring up the configuration interface, once, for the lifetime of the process.
     */
    void startControlService();

    /**
     * @brief The address the configuration interface accepts connections on.
     *
     * The loopback address until the administrator has a password, and every address afterwards.
     * A server that has just been installed has no password on the administrator account and
     * admits it without one, which is safe only while nothing off this machine can reach the
     * interface; setting the password through the setup page is what opens it up.
     *
     * @return the address to bind the interface to.
     */
    [[nodiscard]] sptk::String controlServiceAddress() const;

    /**
     * @brief Keys the interface is served with, generating a self-signed pair if there is none.
     * @param logger            Where what happened is reported.
     * @return the keys, or empty when the interface is to be served over plain HTTP.
     */
    [[nodiscard]] std::shared_ptr<sptk::SSLKeys> webServiceKeys(const sptk::Logger& logger) const;

    /**
     * @brief Create a log engine based on the server settings.
     * @param settings          Server settings.
     * @return Log engine.
     */
    static std::shared_ptr<sptk::LogEngine> makeLogEngine(const std::shared_ptr<Settings>& settings);

};

} // namespace xmq
