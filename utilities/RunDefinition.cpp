/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
║  code review                                                                 ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#include "RunDefinition.h"

#include "utilities/CommandProperties.h"
#include "PublisherCommandLine.h"
#include "base/MessageProperties.h"

#include <sptk5/net/URL.h>

using namespace std;
using namespace sptk;

namespace xmq {

void RunDefinition::parseUri(CommandLine& commandLine)
{
    if (const auto url = commandLine.getOptionValue("url");
        !url.empty())
    {
        const URL fullUrl(url);
        if (const auto user = fullUrl.username();
            !user.empty())
        {
            commandLine.setOptionValue("username", user);
        }

        if (const auto pass = fullUrl.password();
            !pass.empty())
        {
            commandLine.setOptionValue("password", pass);
        }

        auto [host, port] = fullUrl.hostAndPort();
        if (port == 0)
        {
            if (commandLine.hasOption("cafile"))
            {
                port = 8883;
            }
            else
            {
                port = 1883;
            }
        }

        if (const auto topic = fullUrl.path();
            !topic.empty())
        {
            m_topics = Strings(topic.substr(1), ",");
        }

        commandLine.setOptionValue("host", host);
        commandLine.setOptionValue("port", to_string(port));
    }
}

void RunDefinition::load(CommandLine& commandLine)
{
    parseUri(commandLine);

    m_serverHost = make_unique<Host>(commandLine.getOptionValue("host"), static_cast<uint16_t>(commandLine.getOptionValue("port").toInt()));
    m_bindAddresses = Strings(commandLine.getOptionValue("bind-address"), ",");
    m_credentials = make_shared<ConnectCredentials>(
        commandLine.getOptionValue("client-id").c_str(),
        commandLine.getOptionValue("username").c_str(),
        commandLine.getOptionValue("password").c_str());

    parseUri(commandLine);

    auto qos = commandLine.getOptionValue("qos").toInt();
    if (qos < 0 || qos > 2)
    {
        throw Exception("Invalid QoS: " + to_string(qos));
    }
    m_qos = static_cast<Qos>(qos);

    m_connectParameters.m_cleanSession = !commandLine.hasOption("disable-clean-session");
    // "keep-alive", the name the option is defined under. It was read as "keep-alive-seconds" from
    // the first import onwards - a name nothing defines - so the value was always empty and every
    // utility connected with keep-alive 0, whatever -k said. A broker does not arm its keep-alive
    // timer for 0, so the option silently did nothing at all.
    m_connectParameters.m_keepAliveInterval = chrono::seconds(commandLine.getOptionValue("keep-alive").toInt());
    m_connectParameters.m_maxInflightMessages = static_cast<uint16_t>(commandLine.getOptionValue("max-inflight").toInt());

    m_protocolVersion = static_cast<ProtocolVersion>(commandLine.getOptionValue("protocol-version").toInt());
    m_disconnectAfterSeconds = chrono::seconds(commandLine.getOptionValue("disconnect-after").toInt());

    LastWillInfo lastWillInfo;

    // What "-D connect ..." and "-D publish ..." asked for. The CONNECT set stays non-null even
    // when empty, because the connect path has always been handed one; the PUBLISH set is null
    // unless something was asked for, so an ordinary run still sends messages with no property
    // block at all.
    const CommandProperties commandProperties(Strings(commandLine.getOptionValue("property"), ";"));
    m_commandConnectProperties = commandProperties.forCommand("connect");
    m_commandPublishProperties = commandProperties.forCommand("publish");

    // Refused, not ignored. MQTT 3 has no properties at all, so a run that asked for them and got
    // version 3 would send none and report a number for something else entirely - the quiet kind of
    // wrong measurement that is worth more than a day when it is finally noticed.
    if ((m_commandConnectProperties || m_commandPublishProperties) &&
        m_protocolVersion != ProtocolVersion::MqttV5)
    {
        throw Exception("Message properties need --protocol-version 5; MQTT " +
                        to_string(static_cast<int>(m_protocolVersion)) + " has none");
    }
    m_connectProperties = m_commandConnectProperties ? m_commandConnectProperties
                                                     : make_shared<MessageProperties>();
    m_publishProperties = m_commandPublishProperties;

    m_sessionCount = static_cast<size_t>(commandLine.getOptionValue("sessions").toInt());
    if (m_sessionCount < 1)
    {
        m_sessionCount = 1;
    }

    const auto topics = commandLine.getOptionValue("topic");
    if (!topics.empty())
    {
        m_topics = Strings(topics, ",");
    }
    if (m_topics.empty())
    {
        m_topics = {"test/topic"};
    }

    const auto messageText = commandLine.getOptionValue("message");
    const auto messageFile = commandLine.getOptionValue("message-file");
    m_messageReadFromStdin = commandLine.hasOption("stdin");
    if (!m_messageReadFromStdin)
    {
        if (!messageFile.empty())
        {
            m_message.loadFromFile(messageFile.c_str());
        }
        else
        {
            m_message = messageText;
        }
    }

    m_sendCount = commandLine.getOptionValue("repeat").toInt();
    if (m_sendCount < 1)
    {
        m_sendCount = 1;
    }

    m_connectRate = commandLine.getOptionValue("connect-rate").toInt();
    m_messageRate = commandLine.getOptionValue("message-rate").toInt();

    m_receiveCount = commandLine.getOptionValue("receive-count").toInt();
    m_messageWriteToStdout = commandLine.hasOption("print-messages");
    m_sendTimestamp = commandLine.hasOption("send-timestamp");
    m_retain = commandLine.hasOption("retain");

    auto caFile = filesystem::path(commandLine.getOptionValue("cafile").c_str());
    auto keyFile = filesystem::path(commandLine.getOptionValue("key").c_str());
    auto certFile = filesystem::path(commandLine.getOptionValue("cert").c_str());
    if (!caFile.empty())
    {
        if (keyFile.empty())
        {
            keyFile = certFile;
        }
        m_sslKeys = make_shared<SSLKeys>(keyFile, certFile, "", caFile);
    }
    else
    {
        if (keyFile.empty() != certFile.empty())
        {
            throw Exception("Need both options --key and --cert defined");
        }
        if (!keyFile.empty())
        {
            m_sslKeys = make_shared<SSLKeys>(keyFile, certFile);
        }
    }

    // A listener that asks for no client certificate still encrypts, and there is nothing to pass
    // it: an empty SSLKeys is what says "encrypt, but I have no certificate of my own".
    if (!m_sslKeys && commandLine.hasOption("encrypted"))
    {
        m_sslKeys = make_shared<SSLKeys>();
    }

    if (commandLine.hasOption("debug"))
    {
        m_logPriority = LogPriority::Debug;
    }

    if (commandLine.hasOption("quiet"))
    {
        m_logPriority = LogPriority::Info;
    }

    if (commandLine.hasOption("show-counters-csv"))
    {
        m_showCounters = Reporter::CountersFormat::CsvCounters;
    }
    else if (commandLine.hasOption("show-counters"))
    {
        m_showCounters = Reporter::CountersFormat::TableCounters;
    }
    else
    {
        m_showCounters = Reporter::CountersFormat::NoCounters;
    }
}

} // namespace xmq
