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

#include "Publisher.h"
#include "PublisherCommandLine.h"
#include "Reporter.h"
#include "base/LatencyTrace.h"
#include "common/PaceMaker.h"

using namespace std;
using namespace sptk;
using namespace xmq;

Publisher::Publisher(const vector<std::string>& args)
    : Utility(make_shared<PublisherCommandLine>(args))
{
}

int Publisher::run()
{
    if (commandLine().hasOption("help"))
    {
        constexpr int defaultScreenWidth = 80;
        const auto*   colsEnv = getenv("COLS");
        const size_t  screenCols =
            colsEnv == nullptr ? defaultScreenWidth : string2int(colsEnv);
        commandLine().printHelp(screenCols);
        return 1;
    }

    createClients();
    (void) connectClients();

    for (auto& client: clients())
    {
        client->setTopics(runDefinition().m_topics);
    }

    Semaphore allMessagesReceived;

    if (runDefinition().m_sendCount == 0)
    {
        allMessagesReceived.post();
    }

    sendMessages(allMessagesReceived);

    disconnectClients();
    this_thread::sleep_for(100ms);

    return 0;
}

int Publisher::sendMessages(Semaphore& allMessagesDelivered) const
{
    mutex stdinMutex;

    atomic_int sendCount = static_cast<int>(runDefinition().m_sendCount);
    atomic_int totalDelivered = 0;

    auto showCounters =
        runDefinition().m_showCounters != Reporter::CountersFormat::NoCounters;
    // const auto messageBatchSize = showCounters && runDefinition().m_sendCount >
    // 0 ? runDefinition().m_sendCount / 20 : 0;
    const auto messageBatchSize =
        runDefinition().m_sendCount > 0 ? runDefinition().m_sendCount / 20 : 0;

    Reporter reporter(format("Publish {} messages", sendCount.load()),
                      {"total messages", "ms/batch", "msgs K/sec"});

    if (runDefinition().m_qos != Qos::Qos0)
    {
        for (const auto& client: clients())
        {
            client->onAck([&totalDelivered, &sendCount, &allMessagesDelivered,
                           showCounters, messageBatchSize,
                           &reporter](const SMessage& ack)
                          {
                              if (ack->isOneOf({Message::Type::PublishAck, Message::Type::PublishComplete}))
                              {
                                  ++totalDelivered;
                                  if (showCounters && totalDelivered == 1) reporter.printHeader();

                                  if (messageBatchSize && totalDelivered % messageBatchSize == 0)
                                  {
                                      if (showCounters)
                                      {
                                          reporter.printRow(totalDelivered, messageBatchSize);
                                      }
                                      else
                                      {
                                          reporter.countRow(messageBatchSize);
                                      }
                                  }

                                  if (totalDelivered == sendCount)
                                  {
                                      allMessagesDelivered.post();
                                  }
                              }
                          });
        }
    }

    atomic_int totalMessagesSent = 0;

    const auto readFromStdin = runDefinition().m_messageReadFromStdin;
    const auto qos = runDefinition().m_qos;

    Buffer message(runDefinition().m_message);
    if (runDefinition().m_sendTimestamp)
    {
        message.bytes(0);
        message.fill(0, sizeof(LatencyTrace));
    }

    const DateTime started("now");

    shared_ptr<PaceMaker> paceMaker;
    if (runDefinition().m_messageRate > 0)
    {
        paceMaker = make_shared<PaceMaker>(runDefinition().m_messageRate);
    }

    while (totalMessagesSent < sendCount)
    {
        if (totalMessagesSent >= sendCount)
        {
            break;
        }

        for (const auto& client: clients())
        {
            if (readFromStdin)
            {
                lock_guard lock(stdinMutex);
                string     line;
                if (!getline(cin, line) || line.empty())
                {
                    allMessagesDelivered.post();
                    break;
                }
                message = line;
            }

            for (const auto& topic: client->getTopics())
            {
                if (runDefinition().m_sendTimestamp)
                {
                    auto* latencyTrace = reinterpret_cast<LatencyTrace*>(message.data());
                    latencyTrace->m_signature = 0x5115;
                    latencyTrace->snap(LatencyPhase::ClientSend);
                }

                if (paceMaker)
                {
                    paceMaker->next();
                }

                client->publish(client::MqttClient::getTopic(topic), message,
                                runDefinition().m_qos, runDefinition().m_publishProperties,
                                runDefinition().m_retain, false);

                ++totalMessagesSent;

                if (qos == Qos::Qos0 && showCounters && totalMessagesSent == 1)
                {
                    reporter.printHeader();
                }

                if (qos == Qos::Qos0 && messageBatchSize &&
                    totalMessagesSent % messageBatchSize == 0)
                {
                    if (showCounters)
                    {
                        reporter.printRow(totalMessagesSent, messageBatchSize);
                    }
                    else
                    {
                        reporter.countRow(messageBatchSize);
                    }
                }
            }

            if (totalMessagesSent >= sendCount)
            {
                break;
            }
        }
    }

    if (runDefinition().m_qos == Qos::Qos0)
    {
        for (const auto& client: clients())
        {
            client->flush();
        }
    }
    else
    {
        allMessagesDelivered.wait_for(chrono::milliseconds(sendCount * 100));
    }

    reporter.printFooter(false);

    return totalMessagesSent;
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
        Publisher            publisher(args);
        return publisher.run();
    }
    catch (const Exception& e)
    {
        CERR(e.message());
    }
    return 1;
}
