#include "server/Extensions/ExtensionHost.h"

#include <gtest/gtest.h>
#include <sptk5/FileLogEngine.h>

#include <arpa/inet.h>
#include <openssl/hmac.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

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
        m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_fd < 0)
        {
            return false;
        }
        int one = 1;
        setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(m_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(m_fd, 4) != 0)
        {
            return false;
        }
        socklen_t length = sizeof(address);
        getsockname(m_fd, reinterpret_cast<sockaddr*>(&address), &length);
        m_port = ntohs(address.sin_port);
        m_thread = std::thread([this] { loop(); });
        return true;
    }

    void stop()
    {
        m_stopping.store(true);
        if (m_fd >= 0)
        {
            ::shutdown(m_fd, SHUT_RDWR);
            ::close(m_fd);
            m_fd = -1;
        }
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }

    ~Receiver() { stop(); }

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

private:
    void loop()
    {
        while (!m_stopping.load())
        {
            const int client = ::accept(m_fd, nullptr, nullptr);
            if (client < 0)
            {
                break;
            }
            std::string request;
            char        buffer[4096];
            size_t      headerEnd = std::string::npos;
            size_t      contentLength = 0;
            while (true)
            {
                const auto received = ::recv(client, buffer, sizeof(buffer), 0);
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
            }
            const bool fail = m_served.fetch_add(1) == 0;
            const std::string response = fail
                ? "HTTP/1.1 500 Oops\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                : "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            ::send(client, response.data(), response.size(), MSG_NOSIGNAL);
            ::close(client);
        }
    }

    int                 m_fd {-1};
    uint16_t            m_port {0};
    std::atomic<int>    m_served {0};
    std::atomic<bool>   m_stopping {false};
    std::thread         m_thread;
    mutable std::mutex  m_mutex;
    std::vector<std::string> m_bodies;
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
    EXPECT_NE(string::npos, bodies[1].find("\"payload_base64\":\"aGVsbG8=\""));
    EXPECT_NE(string::npos, bodies[1].find("\"qos\":1"));

    const auto signatures = receiver.signatures();
    ASSERT_EQ(2U, signatures.size());
    EXPECT_EQ("sha256=" + hmacHex(bodies[1], "topsecret"), signatures[1]);

    host.stop();
    receiver.stop();
}
