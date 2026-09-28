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
#include "ServerController.h"

#include "SelfSignedCertificate.h"
#include "common/DirectoryNames.h"

using namespace std;
using namespace sptk;
using namespace xmq;

filesystem::path ServerController::logFilePath(const Settings& settings)
{
    const auto configured = filesystem::path(settings.m_logging.m_log_to.asString().c_str());

    // A name with no directory in it is a name, not a place: it belongs in the logs directory,
    // under the name that was asked for. This used to return <logs>/xmq_server.log whatever the
    // name was, so "log_to": "mybroker.log" quietly wrote to xmq_server.log instead - and the
    // same wrong rule was written out twice, here and in main.cpp, which is how they agreed.
    if (!configured.has_parent_path())
    {
        const auto name = configured.filename();
        return DirectoryNames::logsDirectory() / (name.empty() ? filesystem::path("xmq_server.log") : name);
    }

    return configured;
}

shared_ptr<LogEngine> ServerController::makeLogEngine(const std::shared_ptr<Settings>& settings)
{
    using enum LogEngine::Option;
    const auto serverLogPath = logFilePath(*settings);
    const auto serverLogPathStr = String(serverLogPath.string());

    filesystem::create_directories(serverLogPath.parent_path());

    // Appending, which is not the default: FileLogEngine overwrites unless told otherwise, so
    // every start of the service threw away the log of the run before it. That is exactly the
    // log worth having - a server is restarted because something went wrong, and restarting it
    // was destroying the record of what. It cost us the evidence once already.
    constexpr auto appendToLog = true;
    const auto     logEngine = make_shared<FileLogEngine>(serverLogPathStr.c_str(), appendToLog);
    logEngine->option(STDOUT, true);
    logEngine->option(DATE, true);
    logEngine->option(TIME, true);
    logEngine->option(MILLISECONDS, true);

    return logEngine;
}

ServerController::ServerController(const shared_ptr<Settings>& settings)
    : m_settings(settings)
    , m_logEngine(makeLogEngine(settings))
{
}

const std::shared_ptr<LogEngine>& ServerController::getLogEngine() const
{
    return m_logEngine;
}

String ServerController::getServiceName() const
{
    return "XMQ Service";
}

String ServerController::getServiceDescription() const
{
    return "XMQ MQTT Server";
}

// Midnight after this one, as the calendar has it: today's date at 00:00, moved on a day and then
// rebuilt from the date that lands on. Rebuilding is what makes it midnight rather than "24 hours
// later" - across a daylight saving change those are an hour apart, and it is midnight that a log
// named for a date should be cut at.
namespace {
DateTime nextMidnight()
{
    short year = 0;
    short month = 0;
    short day = 0;
    short weekDay = 0;
    short yearDay = 0;

    DateTime::Now().decodeDate(&year, &month, &day, &weekDay, &yearDay);
    const auto tomorrow = DateTime(year, month, day) + chrono::hours(24);
    tomorrow.decodeDate(&year, &month, &day, &weekDay, &yearDay);
    return {year, month, day};
}
} // namespace

size_t ServerController::logsToKeep() const
{
    // A week of daily logs when the configuration does not say. Nothing said before, and the logs
    // simply accumulated; a default that keeps a week is what an operator would have set anyway.
    static constexpr size_t defaultKeptLogs = 7;

    const auto& keepLogs = m_settings->m_logging.m_keep_logs;
    if (keepLogs.isNull())
    {
        return defaultKeptLogs;
    }

    // Zero means no limit, as it does elsewhere in this configuration - max_queued_writes reads the
    // same way. Someone who wants every log kept says so, and nothing is deleted.
    const auto value = keepLogs.asInteger();
    return value > 0 ? static_cast<size_t>(value) : LogEngine::keepAllArchives;
}

std::filesystem::path ServerController::rotateLog()
{
    // Logged after the rotation, so the line lands in the new file and says where the old one
    // went. An engine with nothing to set aside - a log that is not a file of ours - returns an
    // empty path and is not worth a line.
    const auto archived = m_logEngine->rotate(logsToKeep());
    if (const Logger logger(*m_logEngine, "[Log] ");
        !archived.empty())
    {
        logger.info("Log rotated, yesterday's is " + String(archived.string()));
    }
    return archived;
}

void ServerController::scheduleLogRotation()
{
    m_logRotationEvent = m_logRotationTimer.fireAt(
        nextMidnight().timePoint(),
        [this]
        {
            rotateLog();
            scheduleLogRotation();
        });
}

void ServerController::startService()
{
    // Nothing else trims the log: it is opened for appending, so that restarting the broker does
    // not destroy the record of why it was restarted, and without this it would grow for as long
    // as the service runs.
    scheduleLogRotation();

    // The engine the constructor built, not another one over the same file. Building a second
    // was how the first one's output got discarded.

    // The interface comes up first and stays up: it is how a server that will not start gets
    // diagnosed and corrected, so it must not depend on one having started.
    startControlService();

    if (string reason;
        !startServing(reason))
    {
        const Logger logger(*m_logEngine, "[Startup] ");
        logger.error("MQTT server did not start: " + reason);
        logger.info("The configuration interface is running - correct the configuration there and start the server.");
    }
}

shared_ptr<SSLKeys> ServerController::webServiceKeys(const Logger& logger) const
{
    if (!m_settings->m_web_service.m_encrypted.asBool())
    {
        logger.warning("The configuration interface is set to plain HTTP. It carries the "
                       "administrator's password and everything the server is configured with.");
        return {};
    }

    const filesystem::path certificateFile = m_settings->m_web_service.m_certfile.asString().c_str();
    const filesystem::path privateKeyFile = m_settings->m_web_service.m_keyfile.asString().c_str();

    try
    {
        // Made out to the address other machines reach this node at, not to the name this machine
        // calls itself: they are the same thing only on a server nobody connects to from outside.
        if (String description;
            SelfSignedCertificate::create(certificateFile, privateKeyFile,
                                          m_settings->nodeHostName(), description))
        {
            logger.info("Created a " + description);
        }

        // Logged every start, not only when it was just created: a self-signed certificate is
        // worth accepting in a browser only if its fingerprint can be checked against something
        // that is not the browser.
        if (const auto description = SelfSignedCertificate::describe(certificateFile);
            !description.empty())
        {
            logger.info("Web GUI certificate: " + description);
        }

        return make_shared<SSLKeys>(privateKeyFile, certificateFile);
    }
    catch (const Exception& exception)
    {
        // Served without encryption rather than not served at all. The interface is how a broken
        // installation is repaired - including the file permissions that usually cause this - and
        // an interface nobody can reach cannot repair anything. It is not quiet about it.
        logger.error("The configuration interface could not be encrypted: " + string(exception.what()));
        logger.error("Falling back to plain HTTP. Correct the certificate at " +
                     certificateFile.string() + " and restart to serve HTTPS again.");
        return {};
    }
}

String ServerController::controlServiceAddress() const
{
    constexpr auto everyAddress = "0.0.0.0";
    constexpr auto loopbackAddress = "127.0.0.1";
    return m_settings->administratorPasswordSet() ? everyAddress : loopbackAddress;
}

void ServerController::startControlService()
{
    if (m_controlServiceListener)
    {
        return;
    }

    const auto servicePort = m_settings->m_web_service.m_listener_port.asInteger();
    if (servicePort <= 0)
    {
        return;
    }

    const Logger logger(*m_logEngine, "[Startup] ");
    try
    {
        const auto sslKeys = webServiceKeys(logger);
        const auto bindAddress = controlServiceAddress();
        m_controlServiceListener = ControlServiceListener::factory(this, sslKeys, m_logEngine, bindAddress,
                                                                   static_cast<uint16_t>(servicePort));
        // The scheme is named, not just the port: it is now the thing most likely to be wrong in
        // a browser's address bar.
        logger.info(format("Listening Web GUI on {}:{} ({}).", bindAddress.c_str(), servicePort,
                           sslKeys ? "https" : "http"));

        // Said at every start until a password is set, because it explains both halves of what
        // an installation looks like before then: an interface that answers on this machine and
        // nowhere else, and an administrator account that lets anyone in who reaches it.
        if (!m_settings->administratorPasswordSet())
        {
            logger.warning("No password is set for the '" + String(Settings::administratorUsername()) +
                           "' account. Until one is, the Web GUI answers on this machine only, at " +
                           // The address, not the name: the interface listens on IPv4 only, and
                           // "localhost" resolves to ::1 first on a current Windows - so the name
                           // sends the first connection attempt where nothing is listening.
                           String(sslKeys ? "https" : "http") + "://127.0.0.1:" + to_string(servicePort) +
                           " - open it and set the password.");
        }
    }
    catch (const Exception& exception)
    {
        logger.error("Configuration interface did not start: " + string(exception.what()));
    }
}

ExtensionHost::Report ServerController::reloadExtensions()
{
    const scoped_lock lock(m_mutex);

    if (!m_xmqServer)
    {
        // The extensions live in the server, so there are none to reconfigure while it is stopped -
        // and starting it reads the configuration afresh anyway.
        throw Exception("The MQTT server is not running, so there are no extensions loaded");
    }
    return m_xmqServer->reloadExtensions();
}

vector<ExtensionHost::Description> ServerController::describeExtensions()
{
    const scoped_lock lock(m_mutex);

    // Empty rather than an error: extensions live inside the server, so a stopped broker has none
    // loaded, and the screen showing nothing is the truth rather than a failure.
    return m_xmqServer ? m_xmqServer->describeExtensions() : vector<ExtensionHost::Description> {};
}

ExtensionHost::Report ServerController::switchExtension(const string& name, const bool on)
{
    const scoped_lock lock(m_mutex);

    if (!m_xmqServer)
    {
        throw Exception("The MQTT server is not running, so there are no extensions to switch");
    }
    return m_xmqServer->switchExtension(name, on);
}

ExtensionHost::Report ServerController::setExtensionSettings(const string& name, const map<string, string>& settings)
{
    const scoped_lock lock(m_mutex);

    if (!m_xmqServer)
    {
        throw Exception("The MQTT server is not running, so there are no extensions to configure");
    }
    return m_xmqServer->setExtensionSettings(name, settings);
}

bool ServerController::startServing(string& reason)
{
    const scoped_lock lock(m_mutex);

    if (m_xmqServer)
    {
        return true;
    }

    try
    {
        m_xmqServer = make_unique<Server>(m_settings, m_logEngine);
        return true;
    }
    catch (const exception& exception)
    {
        reason = exception.what();
    }

    // A server that threw partway through construction owns nothing that should linger.
    m_xmqServer.reset();
    return false;
}

void ServerController::stopServing()
{
    unique_ptr<Server> stopping;

    {
        const scoped_lock lock(m_mutex);
        stopping.swap(m_xmqServer);
    }

    if (stopping)
    {
        stopping->stop();
        // Destroyed outside the lock: teardown joins the server's threads, and one of them may be
        // in a control-API call that wants the very lock held here.
        stopping.reset();
    }
}

bool ServerController::moveControlService(const uint16_t port, string& reason)
{
    if (!m_controlServiceListener)
    {
        reason = "The configuration interface is not running.";
        return false;
    }

    try
    {
        // Only when the socket actually moved. This is asked for after every change to the
        // accounts, most of which move nothing, and a line saying so each time would bury the
        // one time it matters.
        if (const auto bindAddress = controlServiceAddress();
            m_controlServiceListener->bindTo(bindAddress, port))
        {
            const Logger logger(*m_logEngine, "[Service] ");
            logger.info(format("Web GUI is now listening on {}:{}.", bindAddress.c_str(), port));
        }
        return true;
    }
    catch (const Exception& exception)
    {
        reason = exception.what();
        const Logger logger(*m_logEngine, "[Service] ");
        logger.error(format("Web GUI could not move to port {}: {}", port, reason));
        return false;
    }
}

bool ServerController::updateControlServiceKeys(const filesystem::path& certificateFile,
                                                const filesystem::path& privateKeyFile, string& reason)
{
    if (!m_controlServiceListener)
    {
        reason = "The configuration interface is not running.";
        return false;
    }

    const Logger logger(*m_logEngine, "[Service] ");
    try
    {
        m_controlServiceListener->useKeys(make_shared<SSLKeys>(privateKeyFile, certificateFile));

        // Logged with its fingerprint for the same reason it is logged at startup: it is what an
        // administrator checks the browser's warning against.
        logger.info("Web GUI certificate replaced: " + SelfSignedCertificate::describe(certificateFile));
        return true;
    }
    catch (const Exception& exception)
    {
        reason = exception.what();
        logger.error("Web GUI kept its previous certificate: " + reason);
        return false;
    }
}

bool ServerController::setControlServiceEncrypted(const bool encrypted, string& reason)
{
    if (!m_controlServiceListener)
    {
        reason = "The configuration interface is not running.";
        return false;
    }

    const Logger logger(*m_logEngine, "[Service] ");
    try
    {
        shared_ptr<SSLKeys> sslKeys;
        if (encrypted)
        {
            // The same preparation the process does at startup, so an interface that has never
            // been encrypted does not need a certificate installed first: one is issued to itself.
            sslKeys = webServiceKeys(logger);
            if (!sslKeys)
            {
                // webServiceKeys() falls back to plain HTTP and says why in the log. That is the
                // right answer while starting up, where being unreachable is worse; here it is
                // not, because the interface is already up and serving.
                throw Exception("A certificate for the interface could not be prepared. The log "
                                "says what went wrong with it.");
            }
        }

        m_controlServiceListener->serveEncrypted(sslKeys);
        logger.info("Web GUI is now served over " + String(encrypted ? "https" : "http") + ".");
        return true;
    }
    catch (const Exception& exception)
    {
        reason = exception.what();
        logger.error("Web GUI kept its previous scheme: " + reason);
        return false;
    }
}

bool ServerController::isServing() const
{
    const scoped_lock lock(m_mutex);
    return m_xmqServer != nullptr;
}

Server* ServerController::getServer() const
{
    const scoped_lock lock(m_mutex);
    return m_xmqServer.get();
}

const shared_ptr<Settings>& ServerController::getSettings() const
{
    return m_settings;
}

bool ServerController::isStopped(const std::chrono::milliseconds sleepInterval)
{
    // This decides whether the process keeps running, which is not the same question as whether
    // MQTT is being served. With the interface listening, a stopped server is a state the process
    // sits in - waiting for someone to press Start - rather than a reason to exit.
    if (m_controlServiceListener)
    {
        if (const auto server = getServer())
        {
            if (!server->isStopped(sleepInterval))
            {
                return false;
            }
            // It stopped on its own. Reap it so the state is honestly "stopped" and Start can be
            // pressed again, rather than taking the whole process down with it.
            stopServing();
        }
        else
        {
            this_thread::sleep_for(sleepInterval);
        }
        return false;
    }

    // No interface to keep the process alive for.
    const auto server = getServer();
    if (!server)
    {
        return true;
    }

    return server->isStopped(sleepInterval);
}

void ServerController::stopService()
{
    // Before the log engine is replaced below, since the rotation uses it.
    if (m_logRotationEvent)
    {
        m_logRotationEvent->cancel();
        m_logRotationEvent.reset();
    }

    // The process is going away, so the interface goes too - unlike stopServing(), which keeps it.
    stopServing();
    m_controlServiceListener.reset();

    m_logEngine = makeLogEngine(m_settings);
}
