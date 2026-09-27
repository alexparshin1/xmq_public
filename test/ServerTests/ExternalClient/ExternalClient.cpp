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

#include "ExternalClient.h"

#include "common/DirectoryNames.h"

#ifndef _WIN32
#include <sstream>
#endif

using namespace std;
using namespace sptk;

namespace {

/// The build tree's own xmq_pub and xmq_sub, quoted: these are absolute paths handed to a shell,
/// and a build directory is as entitled to a space in its name as any other. Mosquitto's clients
/// stay bare names looked up on PATH - they belong to the machine, not to this build.
String builtExecutable(const char* path)
{
    return String("\"") + path + "\"";
}

} // namespace

namespace xmq {

ExternalClient::ExternalClient(ClientKind clientKind, std::string_view clientId, const ProtocolVersion& protocolVersion,
                               Host host, const EncryptionMode& encryptionMode)
    : m_host(std::move(host))
    , m_protocolVersion(protocolVersion)
    , m_encryptionMode(encryptionMode)
    , m_clientKind(clientKind)
{
    m_credentials = make_unique<ConnectCredentials>(clientId, "user", "secret");
}

SOsProcess
ExternalClient::startSubscriber(const String& topic, const Qos qos, const uint32_t exitAfterNumberOfMessages,
                                const SessionMode sessionMode, const StringProperties& properties,
                                const OutputMode outputMode, const chrono::seconds messageTimeout) const
{
    Strings subscribeOptions = initBasicOptions(topic, qos, sessionMode, properties, outputMode);

    if (exitAfterNumberOfMessages > 0)
    {
        subscribeOptions.push_back("-C " + to_string(exitAfterNumberOfMessages));
    }
    else if (exitAfterNumberOfMessages == 0)
    {
        // Exit as soon as subscriptions acknowledged by broker
        subscribeOptions.push_back("-E");
    }

    if (messageTimeout.count() > 0)
    {
        subscribeOptions.push_back("-W " + to_string(messageTimeout.count()));
    }

    const auto executable = subscriberExecutable(m_clientKind) + " ";
    return executeOsCommandAsync(executable, subscribeOptions, "", outputMode);
}

SOsProcess ExternalClient::startPublisher(const String& topic, const Qos qos, const size_t sendMessageCount,
                                          const StringProperties& properties,
                                          const OutputMode outputMode, const size_t messageSize) const
{
    Strings publishOptions = initBasicOptions(topic, qos, SessionMode::Continue, properties, outputMode);

    Buffer buffer;
    while (buffer.size() < messageSize)
    {
        buffer.append("This is a test message ");
    }
    buffer.bytes(messageSize);

    if (constexpr auto smallMessageSize = 1024;
        messageSize <= smallMessageSize)
    {
        publishOptions.push_back("-m \"" + String(buffer) + "\"");
    }
    else
    {
        buffer.saveToFile("test.message");
        publishOptions.push_back("-f test.message");
    }
    publishOptions.push_back("--repeat " + to_string(sendMessageCount));

    const auto executable = publisherExecutable(m_clientKind) + " ";
    return executeOsCommandAsync(executable, publishOptions, "", outputMode);
}

Strings ExternalClient::initBasicOptions(
    const String& topic, const Qos& qos, const SessionMode& sessionMode,
    const StringProperties& properties, const OutputMode& outputMode) const
{
    stringstream stream;
    stream << "-L mqtt://user:secret@" + m_host.hostname() + ":" << m_host.port() << "/" << topic;
    Strings options {
        stream.str(),
        "-q " + to_string(static_cast<int>(qos)),
        "-i " + string(m_credentials->getClientId()),
        protocolVersionToString(m_protocolVersion),
        propertiesToString("CONNECT", properties),
    };

    if (m_encryptionMode == EncryptionMode::Tls)
    {
        options.push_back("--insecure");
        // The certificate this server issued itself, which is its own authority: self-signed, so
        // handing it to the client as the CA is exactly what makes the connection verifiable.
        options.push_back("--cafile " + DirectoryNames::certsDirectory().string() + "/node.crt");
    }

    if (sessionMode == SessionMode::Continue)
    {
        options.push_back("-c");
    }

    if (outputMode == OutputMode::Debug)
    {
        options.push_back("-d");
    }

    return options;
}

SOsProcess ExternalClient::executeOsCommandAsync(const String& command, const Strings& arguments,
                                                 const String&    extraOptions,
                                                 const OutputMode outputMode)
{
    stringstream cmd;
#ifndef _WIN32
    cmd << "nice ";
#endif
    cmd << command << " " << arguments.join(" ") << " " << extraOptions;
    auto fullCommand = cmd.str();

    if (outputMode == OutputMode::Debug)
    {
        COUT("Executing: " << fullCommand);
    }

    auto echoStdOut = outputMode != OutputMode::Quiet;
    auto process = make_shared<OsProcess>(fullCommand, [echoStdOut](const String& line)
                                          {
                                              if (echoStdOut)
                                              {
                                                  COUT(line << '\n');
                                              }
                                          });
    process->start();
    return process;
}

string ExternalClient::protocolVersionToString(const ProtocolVersion protocolVersion)
{
    switch (protocolVersion)
    {
        using enum ProtocolVersion;
        case MqttV31:
            return "-V 31";
        case MqttV311:
            return "-V 311";
        case MqttV5:
            return "-V 5";
    }

    return {};
}

String ExternalClient::propertiesToString(const string_view mqttCommand, const map<string, string, less<>>& properties)
{
    Strings options;
    for (const auto& [property, value]: properties)
    {
        if (property == "user-property")
        {
            auto nameAndValue = String(value).split("=");
            options.push_back("-D " + string(mqttCommand) + " " + property + " " + nameAndValue[0] + " " + nameAndValue[1]);
        }
        else
        {
            stringstream stream;
            stream << "-D " << mqttCommand << " " << property << " " << value;
            options.push_back(stream.str());
        }
    }
    return options.join(" ");
}

String ExternalClient::publisherExecutable(const ClientKind externalClientKind)
{
    return externalClientKind == ClientKind::Mosquitto ? String("mosquitto_pub") : builtExecutable(XMQ_PUB_EXECUTABLE);
}

String ExternalClient::subscriberExecutable(const ClientKind externalClientKind)
{
    return externalClientKind == ClientKind::Mosquitto ? String("mosquitto_sub") : builtExecutable(XMQ_SUB_EXECUTABLE);
}

} // namespace xmq
