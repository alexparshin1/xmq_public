/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "server/Cluster/Coordinator.h"
#include "server/Cluster/NodeIdentity.h"
#include <gtest/gtest.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <thread>

using namespace std;
using namespace chrono_literals;
using namespace xmq::cluster;

namespace {
// A small RESP peer: no external Redis, cluster membership or network changes.
// The second status reply fails once; recovery must open another TCP connection.
class InterruptedRedis
{
public:
    explicit InterruptedRedis(bool partial) : m_partial(partial)
    {
        m_listener = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_listener < 0) throw runtime_error("socket failed");
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(m_listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(m_listener, 4) != 0)
        {
            ::close(m_listener);
            throw runtime_error("bind/listen failed");
        }
        socklen_t size = sizeof(address);
        if (::getsockname(m_listener, reinterpret_cast<sockaddr*>(&address), &size) != 0)
        {
            ::close(m_listener);
            throw runtime_error("getsockname failed");
        }
        port = ntohs(address.sin_port);
        m_worker = thread([this] { serve(); });
    }

    ~InterruptedRedis()
    {
        m_stop = true;
        m_worker.join(); // reads and accept polling are bounded by 50ms
        ::close(m_listener);
    }

    uint16_t port {};
    atomic<int> connections {0};
    atomic<int> statuses {0};

private:
    int m_listener {-1};
    bool m_partial;
    atomic<bool> m_stop {false};
    thread m_worker;

    bool readByte(int fd, char& byte)
    {
        while (!m_stop)
        {
            pollfd descriptor {fd, POLLIN, 0};
            if (::poll(&descriptor, 1, 50) > 0) return ::recv(fd, &byte, 1, 0) == 1;
        }
        return false;
    }

    bool line(int fd, string& text)
    {
        text.clear();
        char byte;
        while (readByte(fd, byte))
        {
            if (byte == '\n')
            {
                if (!text.empty() && text.back() == '\r') text.pop_back();
                return true;
            }
            text += byte;
        }
        return false;
    }

    vector<string> command(int fd)
    {
        string header;
        if (!line(fd, header) || header.empty() || header[0] != '*') return {};
        vector<string> arguments;
        const auto count = stoi(header.substr(1));
        for (int i = 0; i < count; ++i)
        {
            if (!line(fd, header) || header.empty() || header[0] != '$') return {};
            string value(stoul(header.substr(1)), '\0');
            for (auto& byte : value) if (!readByte(fd, byte)) return {};
            char byte;
            if (!readByte(fd, byte) || byte != '\r' || !readByte(fd, byte) || byte != '\n') return {};
            arguments.push_back(std::move(value));
        }
        return arguments;
    }

    bool sendReply(int fd, string_view reply)
    {
        while (!reply.empty())
        {
            const auto sent = ::send(fd, reply.data(), reply.size(), MSG_NOSIGNAL);
            if (sent <= 0) return false;
            reply.remove_prefix(static_cast<size_t>(sent));
        }
        return true;
    }

    void serve()
    {
        while (!m_stop)
        {
            pollfd descriptor {m_listener, POLLIN, 0};
            if (::poll(&descriptor, 1, 50) <= 0) continue;
            const auto fd = ::accept(m_listener, nullptr, nullptr);
            if (fd < 0) continue;
            ++connections;
            bool interrupted = false;
            while (!m_stop)
            {
                const auto request = command(fd);
                if (request.empty()) break;
                if (interrupted) continue; // incomplete reply must cause client-side timeout
                string reply = ":1\r\n";
                if (request[0] == "HELLO") reply = "%0\r\n";
                else if (request[0] == "SELECT") reply = "+OK\r\n";
                else if (request.size() > 1 && request[1].find("local rank =") != string::npos)
                {
                    if (++statuses == 2)
                    {
                        reply = m_partial ? "*5\r\n$0\r\n\r\n:0\r\n" : ":0\r\n";
                        interrupted = m_partial;
                    }
                    else reply = "*5\r\n$0\r\n\r\n:0\r\n:-1\r\n$0\r\n\r\n:1\r\n";
                }
                if (!sendReply(fd, reply)) break;
            }
            ::close(fd);
        }
    }
};

class CoordinatorRedisRecoveryTests : public testing::TestWithParam<bool> {};
}

TEST_P(CoordinatorRedisRecoveryTests, DiscardsBrokenStreamAndKeepsRenewingLease)
{
    InterruptedRedis redis(GetParam());
    atomic<int> failures {0};
    Coordinator coordinator(NodeIdentity::generate(), "recovery-test", "localhost:13801",
                            sptk::URL("redis://127.0.0.1:" + to_string(redis.port) + "/8"), 1s, {},
                            [&](const string& error) { if (!error.empty()) ++failures; });
    coordinator.start();
    ASSERT_TRUE(coordinator.isOnline());
    const auto deadline = chrono::steady_clock::now() + 8s;
    while ((redis.connections < 2 || !coordinator.isOnline() || failures == 0) && chrono::steady_clock::now() < deadline)
        this_thread::sleep_for(20ms);
    ASSERT_GE(redis.connections.load(), 2) << "The interrupted stream was reused";
    ASSERT_GT(failures.load(), 0) << "The original failure was not reported";
    ASSERT_TRUE(coordinator.isOnline());
    const auto statuses = redis.statuses.load();
    const auto observeUntil = chrono::steady_clock::now() + 2200ms;
    while (chrono::steady_clock::now() < observeUntil)
    {
        ASSERT_TRUE(coordinator.isOnline()) << "Lease expired again after reconnect";
        this_thread::sleep_for(50ms);
    }
    EXPECT_GT(redis.statuses.load(), statuses + 5);
    EXPECT_EQ(2, redis.connections.load());
    coordinator.leave();
}

INSTANTIATE_TEST_SUITE_P(PartialOrMalformedReply, CoordinatorRedisRecoveryTests, testing::Values(true, false));
#endif
