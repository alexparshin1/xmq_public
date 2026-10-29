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

#include "ReconnectScheduler.h"

#include <sptk5/net/TCPSocket.h>
#include <sptk5/net/SocketEvents.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <ranges>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace std;
using namespace sptk;

namespace xmq::client {

namespace {

#ifdef _WIN32
#define CLOSE_SOCKET ::closesocket
using socklen_type = int;
#else
#define CLOSE_SOCKET ::close
using socklen_type = socklen_t;
#endif

struct Attempt
{
    SocketType         m_fd {INVALID_SOCKET};
    shared_ptr<TCPSocket> m_socket;
    ReconnectScheduler::ConnectedCallback m_onConnected;
    ReconnectScheduler::FailedCallback    m_onFailed;
};

struct Pending
{
    Pending(chrono::steady_clock::time_point due, const Host& host, string bindAddress,
            ReconnectScheduler::ConnectedCallback onConnected, ReconnectScheduler::FailedCallback onFailed)
        : m_due(due)
        , m_host(host)
        , m_bindAddress(std::move(bindAddress))
        , m_onConnected(std::move(onConnected))
        , m_onFailed(std::move(onFailed))
    {
    }

    chrono::steady_clock::time_point      m_due;
    Host                                  m_host;
    string                                m_bindAddress;
    ReconnectScheduler::ConnectedCallback m_onConnected;
    ReconnectScheduler::FailedCallback    m_onFailed;
};

SocketType createNonBlockingSocket(const sockaddr& address)
{
    const auto fd = ::socket(address.sa_family, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET)
    {
        return INVALID_SOCKET;
    }

#ifdef _WIN32
    u_long nonBlocking = 1;
    ioctlsocket(fd, FIONBIO, &nonBlocking);
#else
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
    return fd;
}

bool bindToAddress(const SocketType fd, const string& bindAddress)
{
    if (bindAddress.empty())
    {
        return true;
    }

    sockaddr_storage storage {};
    socklen_type     length = 0;
    if (inet_pton(AF_INET, bindAddress.c_str(), &reinterpret_cast<sockaddr_in&>(storage).sin_addr) == 1)
    {
        reinterpret_cast<sockaddr_in&>(storage).sin_family = AF_INET;
        length = sizeof(sockaddr_in);
    }
    else if (inet_pton(AF_INET6, bindAddress.c_str(), &reinterpret_cast<sockaddr_in6&>(storage).sin6_addr) == 1)
    {
        reinterpret_cast<sockaddr_in6&>(storage).sin6_family = AF_INET6;
        length = sizeof(sockaddr_in6);
    }
    else
    {
        return false;
    }

    return ::bind(fd, reinterpret_cast<const sockaddr*>(&storage), length) == 0;
}

} // namespace

struct ReconnectScheduler::Private
{
    // Deadlines, in order of due time. A multimap rather than a heap of Pending: Host is
    // copy-constructible but not assignable, and a heap would need both.
    mutex                                     m_timerMutex;
    condition_variable                        m_timerCondition;
    multimap<chrono::steady_clock::time_point, shared_ptr<Pending>> m_pending;
    thread                                    m_timerThread;

    // Attempts whose TCP connection is in progress, looked up by the reactor's token.
    mutex                                                   m_attemptMutex;
    unordered_map<uint64_t, shared_ptr<Attempt>>            m_attempts;
    atomic_uint64_t                                         m_nextAttempt {1};
    unique_ptr<SocketEvents<Attempt>>                       m_reactor;

    // Work that still blocks: the TLS handshake, CONNECT, and the wait for its answer.
    mutex                          m_workMutex;
    condition_variable             m_workCondition;
    deque<function<void()>>        m_work;
    vector<thread>                 m_workers;
    atomic_bool                    m_stop {false};

    void startTimer();
    void startAttempt(const shared_ptr<Pending>& pending);
    void finishAttempt(const shared_ptr<Attempt>& attempt, bool connected);
    void finishFailed(const shared_ptr<Attempt>& attempt);
    void post(function<void()> job);
};

void ReconnectScheduler::Private::post(function<void()> job)
{
    {
        const unique_lock lock(m_workMutex);
        if (m_stop)
        {
            return;
        }
        m_work.push_back(std::move(job));
    }
    m_workCondition.notify_one();
}

void ReconnectScheduler::Private::startAttempt(const shared_ptr<Pending>& pending)
{
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* addresses = nullptr;
    if (getaddrinfo(pending->m_host.hostname().c_str(), to_string(pending->m_host.port()).c_str(),
                    &hints, &addresses) != 0 || addresses == nullptr)
    {
        if (pending->m_onFailed)
        {
            pending->m_onFailed();
        }
        return;
    }

    SocketType fd = INVALID_SOCKET;
    bool       inProgress = false;
    for (auto* address = addresses; address != nullptr; address = address->ai_next)
    {
        fd = createNonBlockingSocket(*address->ai_addr);
        if (fd == INVALID_SOCKET)
        {
            continue;
        }
        if (!bindToAddress(fd, pending->m_bindAddress))
        {
            CLOSE_SOCKET(fd);
            fd = INVALID_SOCKET;
            continue;
        }

        const auto result = ::connect(fd, address->ai_addr, static_cast<socklen_type>(address->ai_addrlen));
        if (result == 0)
        {
            break;
        }

#ifdef _WIN32
        const auto error = WSAGetLastError();
        inProgress = error == WSAEWOULDBLOCK;
#else
        const auto error = errno;
        inProgress = error == EINPROGRESS || error == EWOULDBLOCK;
#endif
        if (inProgress)
        {
            break;
        }

        CLOSE_SOCKET(fd);
        fd = INVALID_SOCKET;
    }
    freeaddrinfo(addresses);

    if (fd == INVALID_SOCKET)
    {
        if (pending->m_onFailed)
        {
            pending->m_onFailed();
        }
        return;
    }

    if (!inProgress)
    {
        // Connected without waiting (loopback, or the handshake completed within connect()).
        post([onConnected = pending->m_onConnected, fd]
             {
                 onConnected(fd);
             });
        return;
    }

    // Watch for writability: a non-blocking connect completes silently, and the peer sends
    // nothing until CONNECT, so readability alone can never report success.
    auto socket = make_shared<TCPSocket>();
    socket->attach(fd, false);
    socket->blockingMode(false);

    auto attempt = make_shared<Attempt>();
    attempt->m_fd = fd;
    attempt->m_socket = socket;
    attempt->m_onConnected = pending->m_onConnected;
    attempt->m_onFailed = pending->m_onFailed;

    const auto id = m_nextAttempt.fetch_add(1);
    {
        const unique_lock lock(m_attemptMutex);
        m_attempts[id] = attempt;
    }

    try
    {
        m_reactor->add(socket, attempt, false, true);
    }
    catch (...)
    {
        const unique_lock lock(m_attemptMutex);
        m_attempts.erase(id);
        CLOSE_SOCKET(fd);
        if (attempt->m_onFailed)
        {
            attempt->m_onFailed();
        }
    }
}

void ReconnectScheduler::Private::finishAttempt(const shared_ptr<Attempt>& attempt, const bool connected)
{
    // Detach the descriptor before the Attempt dies: on success it belongs to the worker (and
    // through it to the session), on failure it is closed here.
    if (attempt->m_socket && attempt->m_socket->fd() != INVALID_SOCKET)
    {
        attempt->m_socket->detach();
    }

    if (connected)
    {
        post([onConnected = attempt->m_onConnected, fd = attempt->m_fd]
             {
                 onConnected(fd);
             });
    }
    else
    {
        if (attempt->m_fd != INVALID_SOCKET)
        {
            CLOSE_SOCKET(attempt->m_fd);
        }
        if (attempt->m_onFailed)
        {
            attempt->m_onFailed();
        }
    }
}

void ReconnectScheduler::Private::startTimer()
{
    unique_lock lock(m_timerMutex);
    while (!m_stop)
    {
        if (m_pending.empty())
        {
            m_timerCondition.wait(lock);
            continue;
        }

        const auto due = m_pending.begin()->first;
        if (chrono::steady_clock::now() < due)
        {
            m_timerCondition.wait_until(lock, due);
            continue;
        }

        auto pending = m_pending.begin()->second;
        m_pending.erase(m_pending.begin());

        // Starting a connect is a few syscalls and never waits for the peer: doing it under the
        // timer lock would serialise the whole queue behind one getaddrinfo-less connect, but it
        // is short enough that the lock can be released first anyway.
        lock.unlock();
        startAttempt(pending);
        lock.lock();
    }
}

ReconnectScheduler::ReconnectScheduler()
    : m_private(make_unique<Private>())
{
    m_private->m_reactor = make_unique<SocketEvents<Attempt>>(
        "reconnect",
        [this](const weak_ptr<Attempt>& weakAttempt, SocketEventType eventType)
        {
            const auto attempt = weakAttempt.lock();
            if (!attempt)
            {
                return;
            }

            bool connected = false;
            if (eventType.m_writable)
            {
                int         socketError = 0;
                socklen_type length = sizeof(socketError);
                getsockopt(attempt->m_fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketError), &length);
                connected = socketError == 0;
            }

            if (!eventType.m_writable && !eventType.m_error && !eventType.m_hangup && !eventType.m_data)
            {
                return;
            }

            {
                const unique_lock lock(m_private->m_attemptMutex);
                for (auto iterator = m_private->m_attempts.begin(); iterator != m_private->m_attempts.end(); ++iterator)
                {
                    if (iterator->second == attempt)
                    {
                        m_private->m_attempts.erase(iterator);
                        break;
                    }
                }
            }
            if (attempt->m_socket)
            {
                m_private->m_reactor->remove(attempt->m_socket);
            }

            m_private->finishAttempt(attempt, connected);
        },
        chrono::milliseconds(100), SocketPoolTriggerMode::OneShot);

    m_private->m_timerThread = thread([this]
                                      {
                                          m_private->startTimer();
                                      });

    const auto workerCount = max(4u, thread::hardware_concurrency() * 2);
    for (unsigned i = 0; i < workerCount; ++i)
    {
        m_private->m_workers.emplace_back([this]
                                          {
                                              for (;;)
                                              {
                                                  function<void()> job;
                                                  {
                                                      unique_lock lock(m_private->m_workMutex);
                                                      m_private->m_workCondition.wait(lock, [this]
                                                                                      {
                                                                                          return m_private->m_stop ||
                                                                                                 !m_private->m_work.empty();
                                                                                      });
                                                      if (m_private->m_stop)
                                                      {
                                                          return;
                                                      }
                                                      job = std::move(m_private->m_work.front());
                                                      m_private->m_work.pop_front();
                                                  }
                                                  job();
                                              }
                                          });
    }
}

ReconnectScheduler::~ReconnectScheduler()
{
    stop();
}

ReconnectScheduler& ReconnectScheduler::instance()
{
    static ReconnectScheduler scheduler;
    return scheduler;
}

void ReconnectScheduler::schedule(const chrono::milliseconds                         delay,
                                  const Host&                                       host,
                                  const string&                                     bindAddress,
                                  ConnectedCallback                                 onConnected,
                                  FailedCallback                                    onFailed)
{
    auto pending = make_shared<Pending>(chrono::steady_clock::now() + delay, host, bindAddress,
                                        std::move(onConnected), std::move(onFailed));

    {
        const unique_lock lock(m_private->m_timerMutex);
        if (m_private->m_stop)
        {
            return;
        }
        m_private->m_pending.emplace(pending->m_due, pending);
    }
    m_private->m_timerCondition.notify_one();
}

void ReconnectScheduler::stop()
{
    if (m_private->m_stop.exchange(true))
    {
        return;
    }

    m_private->m_timerCondition.notify_all();
    if (m_private->m_timerThread.joinable())
    {
        m_private->m_timerThread.join();
    }

    {
        const unique_lock lock(m_private->m_timerMutex);
        m_private->m_pending.clear();
    }

    if (m_private->m_reactor)
    {
        m_private->m_reactor->stop();
    }

    {
        const unique_lock lock(m_private->m_attemptMutex);
        for (const auto& attempt: m_private->m_attempts | views::values)
        {
            if (attempt->m_socket)
            {
                attempt->m_socket->close();
            }
        }
        m_private->m_attempts.clear();
    }

    m_private->m_workCondition.notify_all();
    for (auto& worker: m_private->m_workers)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
}

} // namespace xmq::client
