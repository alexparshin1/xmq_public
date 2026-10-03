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
#include "ScenarioTests.h"

using namespace std;
using namespace sptk;
using namespace xmq;
using namespace filesystem;

XMQ_ScenarioTests::ScenarioMap XMQ_ScenarioTests::m_basicTestScenarios;
XMQ_ScenarioTests::ScenarioMap XMQ_ScenarioTests::m_encryptedTestScenarios;

void XMQ_ScenarioTests::loadScenarioGroup(const path& scenarioDirectory, map<ScenarioEngine::Type, shared_ptr<ScenarioEngine>>& scenarios)
{
    for (auto const& dir_entry: directory_iterator {scenarioDirectory})
    {
        try
        {
            if (dir_entry.path().extension() == ".json")
            {
                const auto testScenario = make_shared<ScenarioEngine>();
                testScenario->load(dir_entry.path());
                auto scenario = ScenarioEngine::typeFromString(testScenario->type());
                scenarios[scenario] = testScenario;
            }
        }
        catch (const exception& e)
        {
            CERR("Can't load scenario " << dir_entry.path().string() << ": " << e.what());
        }
    }
}

void XMQ_ScenarioTests::loadScenarios()
{
    // XMQ_TEST_SCENARIO_DIRECTORY is the source directory, passed in by test/CMakeLists.txt. The
    // path is absolute on purpose: a relative one is resolved against the current directory, and
    // the suite is started from a different one on Windows than on the other platforms.
    const path basicScenarios = path(XMQ_TEST_SCENARIO_DIRECTORY) / "Basic";
    // generic_string(), not string(): CMake hands the directory over with forward slashes, and
    // appending "Basic" adds a native separator, so on Windows the printed path would mix the two.
    COUT("Loading scenarios from " << basicScenarios.generic_string() << ".");
    if (m_basicTestScenarios.empty())
    {
        loadScenarioGroup(basicScenarios, m_basicTestScenarios);
    }

    // The same scenarios again, for the encrypted listener. Loaded rather than copied so the
    // plain engines stay exactly as the files describe them, and so a TLS run that leaves a
    // client group behind cannot disturb the plain one.
    if (m_encryptedTestScenarios.empty())
    {
        loadScenarioGroup(basicScenarios, m_encryptedTestScenarios);
        for (const auto& [type, engine]: m_encryptedTestScenarios)
        {
            engine->scenario().m_server.m_port = TestSslPortNumber;
            // Empty keys still mean "encrypt": the listener asks for no client certificate.
            engine->sslKeys(std::make_shared<sptk::SSLKeys>());
        }
    }
}

shared_ptr<ScenarioEngine> XMQ_ScenarioTests::getScenario(const ScenarioEngine::Type scenario)
{
    const auto it = m_basicTestScenarios.find(scenario);
    if (it == m_basicTestScenarios.end())
    {
        throw Exception(format("Scenario {} not found", ScenarioEngine::typeToString(scenario)));
    }
    return it->second;
}

shared_ptr<ScenarioEngine> XMQ_ScenarioTests::getEncryptedScenario(const ScenarioEngine::Type scenario)
{
    const auto it = m_encryptedTestScenarios.find(scenario);
    if (it == m_encryptedTestScenarios.end())
    {
        throw Exception(format("Scenario {} not found", ScenarioEngine::typeToString(scenario)));
    }
    return it->second;
}

TEST_F(XMQ_ScenarioTests, BasicConnectionsScenario)
{
    try
    {
        const auto scenario = getScenario(ScenarioEngine::Type::Connections);

        vector<RoundTripLatency> clientPublishLatencies;
        scenario->connectClients(clientPublishLatencies, scenario->name());
        scenario->disconnectClients();
    }
    catch (const Exception& e)
    {
        FAIL() << e.what();
    }
}

TEST_F(XMQ_ScenarioTests, BasicFanInScenario)
{
    try
    {
        const auto scenario = getScenario(ScenarioEngine::Type::FanIn);

        vector<RoundTripLatency> clientPublishLatencies;
        scenario->connectClients(clientPublishLatencies, scenario->name());
        scenario->publish(clientPublishLatencies, scenario->name());
        scenario->disconnectClients();
    }
    catch (const Exception& e)
    {
        FAIL() << e.what();
    }
}

/**
 * Verify that Fan-In resolves publisher topic placeholders separately for each publisher.
 * Create three publishers and one shared subscriber, plus an independent wildcard observer.
 * Send six QoS1 messages and check that the observer receives all three indexed topic names.
 * The old single-topic send loop delivered every message to index zero and fails this check.
 */
TEST_F(XMQ_ScenarioTests, FanInPublishesToEachPublisherTopic)
{
    ScenarioEngine engine;
    engine.load(path(XMQ_TEST_SCENARIO_DIRECTORY) / "Basic" / "Fan-In-1K-5-1K-1K.json");
    auto& scenario = engine.scenario();
    const auto [publisherId, observerId, topicRoot] = makeTestNames();
    scenario.m_publishers.m_client_count = 3;
    scenario.m_publishers.m_id_prefix = publisherId;
    scenario.m_publishers.m_topics = topicRoot + "/$clientindex";
    scenario.m_subscribers.m_client_count = 1;
    scenario.m_subscribers.m_topics = "$share/fanin-topic-test/" + topicRoot + "/#";
    scenario.m_parameters.m_duration_sec = 0;
    scenario.m_parameters.m_message_count = 6;
    scenario.m_parameters.m_publish_rate = 10;

    mutex receivedMutex;
    set<string> receivedTopics;
    size_t receivedCount = 0;
    Semaphore allReceived;
    client::MqttClient observer;
    observer.onMessage([&](const SPublishMessage& message)
                       {
                           const scoped_lock lock(receivedMutex);
                           receivedTopics.emplace(message->destination()->toString());
                           if (++receivedCount == 6)
                           {
                               allReceived.post();
                           }
                       });
    ASSERT_EQ(ReasonCode::Success,
              observer.connect(Host(scenario.m_server.m_hostname.asString(), scenario.m_server.m_port.asInteger()),
                               ConnectCredentials(observerId, "user", "secret"),
                               {.m_cleanSession = true}, ProtocolVersion::MqttV5));
    observer.subscribe(topicRoot + "/#");

    vector<RoundTripLatency> latencies;
    engine.connectClients(latencies, engine.name());
    engine.publish(latencies, engine.name());
    EXPECT_TRUE(allReceived.wait_for(1s));
    engine.disconnectClients();
    observer.disconnect();

    const scoped_lock lock(receivedMutex);
    EXPECT_EQ((set<string> {topicRoot + "/0", topicRoot + "/1", topicRoot + "/2"}), receivedTopics);
}

TEST_F(XMQ_ScenarioTests, BasicFanOutScenario)
{
    try
    {
        const auto scenario = getScenario(ScenarioEngine::Type::FanOut);

        vector<RoundTripLatency> clientPublishLatencies;
        scenario->connectClients(clientPublishLatencies, scenario->name());
        scenario->publish(clientPublishLatencies, scenario->name());
        scenario->disconnectClients();
    }
    catch (const Exception& e)
    {
        FAIL() << e.what();
    }
}

TEST_F(XMQ_ScenarioTests, BasicPointToPointScenario)
{
    try
    {
        const auto scenario = getScenario(ScenarioEngine::Type::PointToPoint);

        vector<RoundTripLatency> clientPublishLatencies;
        scenario->connectClients(clientPublishLatencies, scenario->name());
        scenario->publish(clientPublishLatencies, scenario->name());
        scenario->disconnectClients();
    }
    catch (const Exception& e)
    {
        FAIL() << e.what();
    }
}

// The same four scenarios over TLS. The encrypted listener used to carry a connect test and a
// single external-client publish and nothing else, which is how a broker that stopped reading a
// TLS socket while data was still in it - see Connection_SSL.cpp - went unnoticed until
// 2026-09-08. Traffic, not a handshake, is what finds those.
TEST_F(XMQ_ScenarioTests, EncryptedConnectionsScenario)
{
    try
    {
        const auto scenario = getEncryptedScenario(ScenarioEngine::Type::Connections);

        vector<RoundTripLatency> clientPublishLatencies;
        scenario->connectClients(clientPublishLatencies, scenario->name());
        scenario->disconnectClients();
    }
    catch (const Exception& e)
    {
        FAIL() << e.what();
    }
}

TEST_F(XMQ_ScenarioTests, EncryptedFanInScenario)
{
    try
    {
        const auto scenario = getEncryptedScenario(ScenarioEngine::Type::FanIn);

        vector<RoundTripLatency> clientPublishLatencies;
        scenario->connectClients(clientPublishLatencies, scenario->name());
        scenario->publish(clientPublishLatencies, scenario->name());
        scenario->disconnectClients();
    }
    catch (const Exception& e)
    {
        FAIL() << e.what();
    }
}

TEST_F(XMQ_ScenarioTests, EncryptedFanOutScenario)
{
    try
    {
        const auto scenario = getEncryptedScenario(ScenarioEngine::Type::FanOut);

        vector<RoundTripLatency> clientPublishLatencies;
        scenario->connectClients(clientPublishLatencies, scenario->name());
        scenario->publish(clientPublishLatencies, scenario->name());
        scenario->disconnectClients();
    }
    catch (const Exception& e)
    {
        FAIL() << e.what();
    }
}

TEST_F(XMQ_ScenarioTests, EncryptedPointToPointScenario)
{
    try
    {
        const auto scenario = getEncryptedScenario(ScenarioEngine::Type::PointToPoint);

        vector<RoundTripLatency> clientPublishLatencies;
        scenario->connectClients(clientPublishLatencies, scenario->name());
        scenario->publish(clientPublishLatencies, scenario->name());
        scenario->disconnectClients();
    }
    catch (const Exception& e)
    {
        FAIL() << e.what();
    }
}
