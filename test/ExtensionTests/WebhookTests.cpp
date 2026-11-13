#include "server/Extensions/ExtensionHost.h"

#include <gtest/gtest.h>
#include <sptk5/FileLogEngine.h>

#include <openssl/hmac.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

using namespace std;
using namespace sptk;
using namespace xmq;
namespace ranges = std::ranges;

namespace {

// The receiver is a plain TCP server, and the two families spell that differently: a Windows
// socket is a SOCKET, closed with closesocket(), and its send() takes an int length. Kept here,
// in the test, because the extension's own layer is inside the DLL and not on this side.
#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle invalidSocket = INVALID_SOCKET;
using SocketLength = int;
constexpr int sendFlags = 0;
constexpr int shutdownBoth = SD_BOTH;

bool ensureSockets()
{
    static const bool started = []
    {
        WSADATA data {};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
}

void closeSocket(const SocketHandle socket)
{
    closesocket(socket);
}
#else
using SocketHandle = int;
constexpr SocketHandle invalidSocket = -1;
using SocketLength = socklen_t;
constexpr int sendFlags = MSG_NOSIGNAL;
constexpr int shutdownBoth = SHUT_RDWR;

constexpr bool ensureSockets()
{
    return true;
}

void closeSocket(const SocketHandle socket)
{
    ::close(socket);
}
#endif

filesystem::path webhookLibrary()
{
#ifdef XMQ_TEST_WEBHOOK_LIBRARY
    return {XMQ_TEST_WEBHOOK_LIBRARY};
#else
    return {};
#endif
}

/// A very small HTTP receiver: enough to read a POST body, answer once with 500 and then 200.
class Receiver
{
public:
    bool start()
    {
        if (!ensureSockets())
        {
            return false;
        }
        m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_fd == invalidSocket)
        {
            return false;
        }
        int one = 1;
        setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&one), static_cast<SocketLength>(sizeof(one)));

        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(m_fd, reinterpret_cast<sockaddr*>(&address), static_cast<SocketLength>(sizeof(address))) != 0 ||
            listen(m_fd, 4) != 0)
        {
            closeSocket(m_fd);
            m_fd = invalidSocket;
            return false;
        }
        SocketLength length = static_cast<SocketLength>(sizeof(address));
        if (getsockname(m_fd, reinterpret_cast<sockaddr*>(&address), &length) != 0)
        {
            closeSocket(m_fd);
            m_fd = invalidSocket;
            return false;
        }
        m_port = ntohs(address.sin_port);
        m_thread = std::thread([this] { loop(); });
        return true;
    }

    void stop()
    {
        m_stopping.store(true);
        if (m_fd != invalidSocket)
        {
            ::shutdown(m_fd, shutdownBoth);
            closeSocket(m_fd);
            m_fd = invalidSocket;
        }
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }

    ~Receiver() { stop(); }

    /// How many requests to answer with 500 before succeeding, so a retry can be observed without
    /// every test paying for one.
    void failFirst(const int count) { m_failFirst = count; }

    [[nodiscard]] uint16_t port() const { return m_port; }
    [[nodiscard]] size_t requests() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_bodies.size();
    }
    [[nodiscard]] std::vector<std::string> bodies() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_bodies;
    }
    [[nodiscard]] std::vector<std::string> signatures() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_signatures;
    }
    [[nodiscard]] std::vector<std::string> headerBlocks() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_headerBlocks;
    }

private:
    void loop()
    {
        while (!m_stopping.load())
        {
            const SocketHandle client = ::accept(m_fd, nullptr, nullptr);
            if (client == invalidSocket)
            {
                break;
            }
            std::string request;
            char        buffer[4096];
            size_t      headerEnd = std::string::npos;
            size_t      contentLength = 0;
            while (true)
            {
                const auto received = ::recv(client, buffer, static_cast<SocketLength>(sizeof(buffer)), 0);
                if (received <= 0)
                {
                    break;
                }
                request.append(buffer, static_cast<size_t>(received));
                if (headerEnd == std::string::npos)
                {
                    headerEnd = request.find("\r\n\r\n");
                    if (headerEnd != std::string::npos)
                    {
                        const auto header = request.substr(0, headerEnd);
                        const auto lengthAt = header.find("Content-Length: ");
                        if (lengthAt != std::string::npos)
                        {
                            contentLength = static_cast<size_t>(std::stoul(header.substr(lengthAt + 16)));
                        }
                        const auto signatureAt = header.find("X-XMQ-Signature: ");
                        if (signatureAt != std::string::npos)
                        {
                            const auto end = header.find("\r\n", signatureAt);
                            const std::scoped_lock lock(m_mutex);
                            m_signatures.push_back(
                                header.substr(signatureAt + 17, end - signatureAt - 17));
                        }
                    }
                }
                if (headerEnd != std::string::npos &&
                    request.size() >= headerEnd + 4 + contentLength)
                {
                    break;
                }
            }
            if (headerEnd != std::string::npos)
            {
                const std::scoped_lock lock(m_mutex);
                m_bodies.push_back(request.substr(headerEnd + 4, contentLength));
                m_headerBlocks.push_back(request.substr(0, headerEnd));
            }
            const bool fail = m_served.fetch_add(1) < m_failFirst;
            const std::string response = fail
                ? "HTTP/1.1 500 Oops\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                : "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            ::send(client, response.data(), static_cast<int>(response.size()), sendFlags);
            closeSocket(client);
        }
    }

    SocketHandle        m_fd {invalidSocket};
    uint16_t            m_port {0};
    std::atomic<int>    m_served {0};
    int                 m_failFirst {0};
    std::atomic<bool>   m_stopping {false};
    std::thread         m_thread;
    mutable std::mutex  m_mutex;
    std::vector<std::string> m_bodies;
    std::vector<std::string> m_headerBlocks;
    std::vector<std::string> m_signatures;
};

class WebhookTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (webhookLibrary().empty() || !filesystem::exists(webhookLibrary()))
        {
            GTEST_SKIP() << "the webhook extension was not built";
        }
    }

    ExtensionHost::Configured extension(const Receiver& receiver, const string& filter = "sensors/#",
                                        const string& retries = "3") const
    {
        return ExtensionHost::Configured {
            .m_name = "webhook",
            .m_library = webhookLibrary(),
            .m_settings = {{"url", "http://127.0.0.1:" + to_string(receiver.port()) + "/hook"},
                           {"secret", "topsecret"},
                           {"topic_filter", filter},
                           {"timeout_ms", "2000"},
                           {"retries", retries},
                           {"allow_http", "true"}}};
    }

    ExtensionHost::Configured logExtension(const Receiver& receiver, const string& minLevel,
                                           const string& subjects = "") const
    {
        return ExtensionHost::Configured {
            .m_name = "webhook",
            .m_library = webhookLibrary(),
            .m_settings = {{"url", "http://127.0.0.1:" + to_string(receiver.port()) + "/hook"},
                           {"secret", "topsecret"},
                           {"timeout_ms", "2000"},
                           {"retries", "0"},
                           {"allow_http", "true"},
                           {"sources", "Send log"},
                           {"log_min_level", minLevel},
                           {"log_subjects", subjects}}};
    }

    FileLogEngine m_logEngine {"webhook-tests.log"};
};

std::string hmacHex(const std::string& body, const std::string& secret)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  length = 0;
    HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(body.data()), body.size(), digest, &length);
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned at = 0; at < length; ++at)
    {
        out += digits[digest[at] >> 4U];
        out += digits[digest[at] & 0x0FU];
    }
    return out;
}

} // namespace

TEST_F(WebhookTests, postsJSONAndRetriesAServerError)
{
    Receiver receiver;
    receiver.failFirst(1);
    ASSERT_TRUE(receiver.start());

    ExtensionHost host(m_logEngine, "webhook");
    host.start({extension(receiver)});
    ASSERT_TRUE(host.tapping());

    const std::string payload = "hello";
    host.publishMessage("client-1", "user", "sensors/1", payload.data(), payload.size(), 1, false);
    host.publishMessage("client-1", "user", "other/1", payload.data(), payload.size(), 1, false);

    const auto deadline = chrono::steady_clock::now() + chrono::seconds(5);
    while (receiver.requests() < 2 && chrono::steady_clock::now() < deadline)
    {
        this_thread::sleep_for(chrono::milliseconds(20));
    }

    const auto bodies = receiver.bodies();
    ASSERT_EQ(2U, bodies.size()) << "the 500 was not retried";
    EXPECT_NE(string::npos, bodies[1].find("\"topic\":\"sensors/1\""));
    EXPECT_NE(string::npos, bodies[1].find("\"message\":\"aGVsbG8=\""));
    EXPECT_NE(string::npos, bodies[1].find("\"level\":\"message\""));

    const auto signatures = receiver.signatures();
    ASSERT_EQ(2U, signatures.size());
    EXPECT_EQ("sha256=" + hmacHex(bodies[1], "topsecret"), signatures[1]);

    host.stop();
    receiver.stop();
}


TEST_F(WebhookTests, postsLogRecordsFromTheSecondInput)
{
    Receiver receiver;
    ASSERT_TRUE(receiver.start());

    ExtensionHost host(m_logEngine, "webhook");
    host.start({logExtension(receiver, "error")});
    ASSERT_TRUE(host.logTapping());

    host.publishLog(LogPriority::Info, LogSubject::Connect, "not severe enough");
    host.publishLog(LogPriority::Error, LogSubject::ServerConnections,
                    "test-client: refused by an extension");

    const auto deadline = chrono::steady_clock::now() + chrono::seconds(5);
    while (receiver.requests() < 1 && chrono::steady_clock::now() < deadline)
    {
        this_thread::sleep_for(chrono::milliseconds(20));
    }

    const auto bodies = receiver.bodies();
    ASSERT_EQ(1U, bodies.size()) << "the info line should not have been sent";
    EXPECT_NE(string::npos, bodies[0].find("\"topic\":\"\""));
    EXPECT_NE(string::npos, bodies[0].find("\"level\":\"error\""));
    EXPECT_NE(string::npos, bodies[0].find("\"message\":\"test-client: refused by an extension\""));

    host.stop();
    receiver.stop();
}

TEST_F(WebhookTests, aLogSubjectFilterKeepsTheOtherSubjectsOut)
{
    Receiver receiver;
    ASSERT_TRUE(receiver.start());

    ExtensionHost host(m_logEngine, "webhook");
    host.start({logExtension(receiver, "debug", "connect")});
    ASSERT_TRUE(host.logTapping());

    host.publishLog(LogPriority::Error, LogSubject::StorageEvents, "storage is unhappy");
    host.publishLog(LogPriority::Error, LogSubject::Connect, "a connect went wrong");

    const auto deadline = chrono::steady_clock::now() + chrono::seconds(5);
    while (receiver.requests() < 1 && chrono::steady_clock::now() < deadline)
    {
        this_thread::sleep_for(chrono::milliseconds(20));
    }
    this_thread::sleep_for(chrono::milliseconds(100));

    const auto bodies = receiver.bodies();
    ASSERT_EQ(1U, bodies.size());
    EXPECT_NE(string::npos, bodies[0].find("a connect went wrong"));
    EXPECT_EQ(string::npos, bodies[0].find("storage is unhappy"));

    host.stop();
    receiver.stop();
}


TEST_F(WebhookTests, sendsTheConfiguredTemplateAndHeaders)
{
    Receiver receiver;
    ASSERT_TRUE(receiver.start());

    auto configuration = extension(receiver);
    configuration.m_settings["template"] = R"({"text":"{{topic}}"})";
    configuration.m_settings["headers"] = R"(["X-Api-Key: topsecret","X-Source: xmq"])";
    configuration.m_settings["topic_filter"] = "sensors/#";

    ExtensionHost host(m_logEngine, "webhook");
    host.start({configuration});
    ASSERT_TRUE(host.tapping());

    const std::string payload = "hello";
    host.publishMessage("client-1", "user", "sensors/1", payload.data(), payload.size(), 0, false);

    const auto deadline = chrono::steady_clock::now() + chrono::seconds(5);
    while (receiver.requests() < 1 && chrono::steady_clock::now() < deadline)
    {
        this_thread::sleep_for(chrono::milliseconds(20));
    }
    this_thread::sleep_for(chrono::milliseconds(100));

    const auto bodies = receiver.bodies();
    ASSERT_EQ(1U, bodies.size());
    EXPECT_EQ(R"({"text":"sensors/1"})", bodies[0]);

    const auto headers = receiver.headerBlocks();
    ASSERT_EQ(1U, headers.size());
    EXPECT_NE(string::npos, headers[0].find("X-Api-Key: topsecret"));
    EXPECT_NE(string::npos, headers[0].find("X-Source: xmq"));

    host.stop();
    receiver.stop();
}

TEST_F(WebhookTests, refusesASourceThatIsNotOneOfTheThree)
{
    Receiver receiver;
    ASSERT_TRUE(receiver.start());

    auto configuration = extension(receiver);
    configuration.m_settings["sources"] = "messages";

    ExtensionHost host(m_logEngine, "webhook");
    host.start({configuration});

    // The interface offers three, and a value written by hand is held to the same list.
    EXPECT_FALSE(host.tapping());
    host.stop();
    receiver.stop();
}

/// A field the screen showed empty is written back as "": the extension has to read that as the
/// declared default, not hand it to stoi.
TEST_F(WebhookTests, anEmptyNumberStartsAtItsDefault)
{
    Receiver receiver;
    ASSERT_TRUE(receiver.start());

    auto configuration = extension(receiver);
    configuration.m_settings["concurrency"] = "";
    configuration.m_settings["timeout_ms"] = "";
    configuration.m_settings["retries"] = "";
    configuration.m_settings["max_payload"] = "";
    configuration.m_settings["sources"] = "";

    ExtensionHost host(m_logEngine, "webhook");
    host.start({configuration});
    ASSERT_TRUE(host.tapping());

    // The same settings arriving again, as Apply sends them, must also be accepted.
    const auto report = host.reloadSettings({configuration});
    EXPECT_FALSE(report.failed()) << report.m_problems[0];

    const std::string payload = "hello";
    host.publishMessage("client-1", "user", "sensors/1", payload.data(), payload.size(), 0, false);

    const auto deadline = chrono::steady_clock::now() + chrono::seconds(5);
    while (receiver.requests() < 1 && chrono::steady_clock::now() < deadline)
    {
        this_thread::sleep_for(chrono::milliseconds(20));
    }
    EXPECT_EQ(1U, receiver.requests());

    host.stop();
    receiver.stop();
}

TEST_F(WebhookTests, refusesAHeaderThatIsNotNameValue)
{
    Receiver receiver;
    ASSERT_TRUE(receiver.start());

    auto configuration = extension(receiver);
    configuration.m_settings["headers"] = R"(["just-a-value"])";

    ExtensionHost host(m_logEngine, "webhook");
    host.start({configuration});

    // Refused at start, so the host unloads it rather than keeping a half-configured connector.
    EXPECT_FALSE(host.tapping());
    host.stop();
    receiver.stop();
}

/// A setting that declares a format is refused by the broker, not only by the screen: the file
/// keeps the value that was in force, and the answer names the setting and what it wanted.
TEST_F(WebhookTests, aValueThatDoesNotMatchItsDeclaredFormatIsRefused)
{
    const auto directory = filesystem::temp_directory_path() /
                           ("xmq_webhook_settings_" + to_string(getpid()));
    filesystem::create_directories(directory);
    const auto brokerConfiguration = directory / "xmq_server.conf";
    const auto extensionsFile = ExtensionHost::configurationPathFor(brokerConfiguration);
    {
        ofstream file(extensionsFile);
        file << R"({"extensions":[{"name":"webhook","library":")"
             << webhookLibrary().generic_string()
             << R"(","enabled":true,"settings":{"url":"https://collector.example/hook","timeout_ms":"5000"}}]})";
    }

    ExtensionHost host(m_logEngine, "webhook");
    host.start(ExtensionHost::readConfiguration(brokerConfiguration, m_logEngine));
    ASSERT_TRUE(host.tapping());

    // The format reaches the interface with the setting, so the field can refuse a wrong value
    // before the request is made.
    const auto described = host.describe();
    const auto webhook = ranges::find_if(described, [](const auto& entry)
                                         { return entry.m_name == "webhook"; });
    ASSERT_NE(described.end(), webhook);
    const auto timeout = ranges::find_if(webhook->m_settings, [](const auto& setting)
                                         { return setting.m_name == "timeout_ms"; });
    ASSERT_NE(webhook->m_settings.end(), timeout);
    EXPECT_EQ("[1-9][0-9]{0,5}", timeout->m_pattern);

    const auto sources = ranges::find_if(webhook->m_settings, [](const auto& setting)
                                         { return setting.m_name == "sources"; });
    ASSERT_NE(webhook->m_settings.end(), sources);
    EXPECT_EQ("Send log,Send messages,Send log and messages", sources->m_choices);
    EXPECT_EQ("Send messages", sources->m_defaultValue);

    auto refused = ExtensionHost::readConfiguration(brokerConfiguration, m_logEngine).front();
    refused.m_settings["timeout_ms"] = "five seconds";
    const auto report = host.reloadSettings({refused});
    ASSERT_TRUE(report.failed());
    EXPECT_NE(string::npos, report.m_problems[0].find("timeout_ms")) << report.m_problems[0];

    // Not written: what the extension is running with and what the file says must not disagree.
    const auto onDisk = ExtensionHost::readConfiguration(brokerConfiguration, m_logEngine);
    ASSERT_EQ(1U, onDisk.size());
    EXPECT_EQ("5000", onDisk[0].m_settings.at("timeout_ms"));

    host.stop();
    error_code errorCode;
    filesystem::remove_all(directory, errorCode);
}

/// Several filters are one tap: the broker is asked for the list, and a body is copied only when
/// it matches one of them, so a topic nobody wants costs a comparison and not a copy.
TEST_F(WebhookTests, severalTopicFiltersShareOneTap)
{
    Receiver receiver;
    ASSERT_TRUE(receiver.start());

    ExtensionHost host(m_logEngine, "webhook");
    host.start({extension(receiver, R"(["sensors/#","alarms/+"])")});
    ASSERT_TRUE(host.tapping());

    const std::string payload = "value";
    host.publishMessage("client-1", "user", "sensors/1", payload.data(), payload.size(), 1, false);
    host.publishMessage("client-1", "user", "alarms/fire", payload.data(), payload.size(), 1, false);
    host.publishMessage("client-1", "user", "other/1", payload.data(), payload.size(), 1, false);

    const auto deadline = chrono::steady_clock::now() + chrono::seconds(5);
    while (receiver.requests() < 2 && chrono::steady_clock::now() < deadline)
    {
        this_thread::sleep_for(chrono::milliseconds(20));
    }
    this_thread::sleep_for(chrono::milliseconds(100));

    const auto bodies = receiver.bodies();
    ASSERT_EQ(2U, bodies.size()) << "the topic nobody asked for must not be posted";

    // The destination has several worker threads, so the two requests may arrive in either order.
    string posted;
    for (const auto& body: bodies)
    {
        posted += body;
    }
    EXPECT_NE(string::npos, posted.find("\"topic\":\"sensors/1\""));
    EXPECT_NE(string::npos, posted.find("\"topic\":\"alarms/fire\""));

    host.stop();
    receiver.stop();
}

/// A list setting is checked item by item: the array is the screen's storage format, and each
/// item is the value the extension sees.
TEST_F(WebhookTests, aListSettingIsCheckedItemByItem)
{
    const auto directory = filesystem::temp_directory_path() /
                           ("xmq_webhook_list_" + to_string(getpid()));
    filesystem::create_directories(directory);
    const auto brokerConfiguration = directory / "xmq_server.conf";
    const auto extensionsFile = ExtensionHost::configurationPathFor(brokerConfiguration);
    {
        ofstream file(extensionsFile);
        file << R"({"extensions":[{"name":"webhook","library":")"
             << webhookLibrary().generic_string()
             << R"(","enabled":true,"settings":{"url":"https://collector.example/hook",)"
             << R"("topic_filter":"[\"sensors/#\"]"}}]})";
    }

    ExtensionHost host(m_logEngine, "webhook");
    host.start(ExtensionHost::readConfiguration(brokerConfiguration, m_logEngine));
    ASSERT_TRUE(host.tapping());

    const auto described = host.describe();
    const auto webhook = ranges::find_if(described, [](const auto& entry)
                                         { return entry.m_name == "webhook"; });
    ASSERT_NE(described.end(), webhook);
    const auto filter = ranges::find_if(webhook->m_settings, [](const auto& setting)
                                         { return setting.m_name == "topic_filter"; });
    ASSERT_NE(webhook->m_settings.end(), filter);
    EXPECT_EQ(XMQ_SETTING_LIST, filter->m_type);

    // The interface's path: writeSettings() is what the Extensions page reaches through
    // Server::setExtensionSettings(), and it is where a script's value is judged too.
    auto settings = ExtensionHost::readConfiguration(brokerConfiguration, m_logEngine).front().m_settings;

    settings["topic_filter"] = R"(["sensors/#","bad#"])";
    const auto refused = host.writeSettings("webhook", settings);
    ASSERT_TRUE(refused.failed());
    EXPECT_NE(string::npos, refused.m_problems[0].find("topic_filter")) << refused.m_problems[0];

    settings["topic_filter"] = R"(["sensors/#","alarms/+"])";
    EXPECT_FALSE(host.writeSettings("webhook", settings).failed());

    const auto onDisk = ExtensionHost::readConfiguration(brokerConfiguration, m_logEngine);
    ASSERT_EQ(1U, onDisk.size());
    EXPECT_EQ(R"(["sensors/#","alarms/+"])", onDisk[0].m_settings.at("topic_filter"));

    host.stop();
    error_code errorCode;
    filesystem::remove_all(directory, errorCode);
}
