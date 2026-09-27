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

#include "Subscriber.h"
#include "ExecuteParallel.h"
#include "SubscriberCommandLine.h"
#include "base/LatencyTrace.h"
#include "common/DisconnectMessage.h"

using namespace std;
using namespace sptk;
using namespace xmq;

Subscriber::Subscriber(const vector<std::string>& args)
    : Utility(make_shared<SubscriberCommandLine>(args))
{
}

int Subscriber::run()
{
    if (commandLine().hasOption("help"))
    {
        constexpr auto defaultScreenWidth = 80;
        const auto*    colsEnv = getenv("COLS");
        const size_t   screenCols = colsEnv == nullptr ? defaultScreenWidth : string2int(colsEnv);
        commandLine().printHelp(screenCols);
        return 1;
    }

    Logger logger(*logEngine());

    createClients();
    const auto clientsTotal = clients().size();

    auto connectedClientCounter = connectClients();
    if (connectedClientCounter < clientsTotal)
    {
        CERR("Not all clients connected.");
        return 1;
    }

    for (auto& client: clients())
    {
        client->setTopics(runDefinition().m_topics);
    }

    Semaphore         allMessagesReceived;
    atomic_size_t     receivedMessageCount = 0;
    auto              messageBatchSize = runDefinition().m_showCounters != Reporter::CountersFormat::NoCounters && runDefinition().m_receiveCount > 0 ? runDefinition().m_receiveCount / 20 : 0;
    DateTime          messageBatchStarted;
    LatencyTraceTotal latencyTraceBatch;
    LatencyTraceTotal latencyTraceTotal;

    if (messageBatchSize == 0)
    {
        messageBatchSize = 1000;
    }

    Reporter subscribeReporter("Subscribe " + to_string(clientsTotal) + " clients",
                               {"total clients", "ms/batch", "clients K/sec"},
                               runDefinition().m_showCounters);

    stringstream receiveTitle;
    receiveTitle << "Receive " << runDefinition().m_receiveCount << " messages";
    Reporter receiveReporter(receiveTitle.str(), {"total messages", "ms/batch", "msgs K/sec", "latency, ms"}, runDefinition().m_showCounters);

    for (const auto& client: clients())
    {
        const auto& clientId = client->getClientId();
        client->onDisconnect(
            [clientId, &connectedClientCounter, &allMessagesReceived](const SMessage&)
            {
                --connectedClientCounter;
                COUT("Client " + clientId + " disconnected.");
                if (connectedClientCounter == 0)
                {
                    COUT("Exiting: All clients disconnected.");
                    allMessagesReceived.post();
                }
            });

        const auto receiveCount = runDefinition().m_receiveCount;
        client->onMessage(
            [this, &receivedMessageCount, &allMessagesReceived, receiveCount,
             messageBatchSize, &messageBatchStarted, &latencyTraceTotal, &latencyTraceBatch, &receiveReporter](const SPublishMessage& publishMessage)
            {
                if (runDefinition().m_messageWriteToStdout)
                {
                    COUT(publishMessage->toString());
                }

                // If message is a trace, update trace.
                if (auto* latencyTrace = publishMessage->asTrace())
                {
                    latencyTrace->snap(LatencyPhase::ClientReceive);
                    latencyTraceBatch.add(latencyTrace);
                    latencyTraceTotal.add(latencyTrace);
                }

                const auto count = ++receivedMessageCount;

                if (count == 1)
                {
                    receiveReporter.printHeader();
                    messageBatchStarted = DateTime::Now();
                }

                if (messageBatchSize && count % messageBatchSize == 0)
                {
                    static mutex aMutex;
                    lock_guard   lock(aMutex);
                    receiveReporter.printRow(count, messageBatchSize,
                                             latencyTraceBatch.diff(LatencyPhase::ClientSend, LatencyPhase::ClientWireOut) / 1000.0);
                    latencyTraceBatch = {};
                    messageBatchStarted = DateTime::Now();
                }

                if (receiveCount > 0 && count >= receiveCount)
                {
                    allMessagesReceived.post();
                }
            });
    }

    ExecuteParallel parallel(
        subscribeReporter, clients(),
        [this](const SUtilityClient& client)
        {
            if (client->isConnected())
            {
                Destinations destinations;
                for (const auto& topic: client->getTopics())
                {
                    Destination destination(client::MqttClient::getTopic(topic), SubscriptionOptions {runDefinition().m_qos});
                    destinations.push_back(destination);
                }

                client->subscribe(destinations);
                return true;
            }
            CERR("Client " << client->getClientId() << " not connected");
            return false;
        });

    parallel.execute(runDefinition().m_showCounters, 100);

    if (runDefinition().m_sendCount == 0)
    {
        allMessagesReceived.post();
    }

    if (runDefinition().m_disconnectAfterSeconds.count() > 0)
    {
        allMessagesReceived.wait_for(runDefinition().m_disconnectAfterSeconds);
    }
    else
    {
        allMessagesReceived.wait();
    }
    receiveReporter.printFooter(false);

    disconnectClients();

    this_thread::sleep_for(100ms);

    return 0;
}

int main(const int argc, const char* argv[])
{
    const vector<string> args(argv, argv + argc);
    try
    {
        // Inside the try, not before it. The constructor reads the command line, and everything it
        // refuses - an unknown property, a number given a word, properties asked for on a protocol
        // version that has none - used to leave the process on an unhandled exception instead of a
        // message.
        Subscriber           subscriber(args);
        return subscriber.run();
    }
    catch (const Exception& e)
    {
        CERR(e.message());
    }
    return 1;
}
