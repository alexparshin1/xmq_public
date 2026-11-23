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

#pragma once

#include "DeliveryQueue.h"

#include "extension/xmq_extension.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace xmq::exporter {

/**
 * @brief The durable delivery worker shared by every sink.
 *
 * An exporter renders a record, hands it to this class, and the class owns it from then on: one
 * writer stores it in SQLite, one delivery worker takes the head, asks the sink to deliver it and
 * removes it only after success. Retry, backoff and dead-lettering are the same for a webhook, a
 * database INSERT and a Kafka topic - a sink only has to report 0 (delivered), 1 (permanent
 * rejection) or 2 (reconnect and retry) through the Deliver callback.
 *
 * Nothing here runs on the message path. Next() is the one blocking callback: it waits for a
 * rendered record and is called only from the writer thread. The MQTT receive thread merely puts
 * a record into the extension's bounded in-memory queue.
 *
 * The queue file belongs to one sink configuration. The extension supplies an identity key so a
 * file cannot be reused after the sink, its settings or the extension change.
 */
class PersistentDelivery
{
public:
    /// Length-prefix a part of a record or of a destination identity key.
    static void appendPart(std::string& out, std::string_view part)
    {
        for (unsigned shift = 0; shift < 64; shift += 8)
            out.push_back(static_cast<char>((static_cast<uint64_t>(part.size()) >> shift) & 0xff));
        out.append(part);
    }


    struct Record
    {
        std::string body;
        std::vector<std::pair<std::string, std::string>> fields;
    };

    /// Waits for the next prepared record; returns nullopt when the destination is stopping and
    /// drained. Called only from the writer thread.
    using Next = std::function<std::optional<Record>()>;
    /// 0: delivered, 1: permanent failure, 2: reconnect and retry.
    using Deliver = std::function<int(const Record&)>;
    /// Publish a rejected or overflowing record to the dead-letter topic; false means it was not
    /// accepted, in which case the caller decides how to report the loss.
    using DeadLetter = std::function<bool(const Record&)>;
    using Report = std::function<void(uint32_t outcome)>;
    /// True once the destination has been asked to stop.
    using Stopping = std::function<bool()>;
    /// Wakes the extension's own waiters so a stop does not wait for their timeout.
    using Wake = std::function<void()>;
    /// A record that could not be queued and was not dead-lettered.
    using Dropped = std::function<void()>;

    explicit PersistentDelivery(std::shared_ptr<DeliveryQueue> queue, std::int64_t limit,
                                Next next, Deliver deliver, DeadLetter deadLetter, Report report,
                                Stopping stopping, Wake wake, Dropped dropped)
        : m_queue(std::move(queue))
        , m_limit(limit)
        , m_next(std::move(next))
        , m_deliver(std::move(deliver))
        , m_deadLetter(std::move(deadLetter))
        , m_report(std::move(report))
        , m_stopping(std::move(stopping))
        , m_wake(std::move(wake))
        , m_dropped(std::move(dropped))
    {
        m_threadsStorage[0] = std::thread([this] { write(); });
        m_threadsStorage[1] = std::thread([this] { runDelivery(); });
    }

    PersistentDelivery(const PersistentDelivery&) = delete;
    PersistentDelivery& operator=(const PersistentDelivery&) = delete;

    ~PersistentDelivery()
    {
        stop();
    }

    /// Join both workers. Safe to call more than once and from the destructor. Idempotent because
    /// the threads are joined only while joinable.
    void stop()
    {
        {
            const std::scoped_lock lock(m_stopLock);
            m_stop = true;
        }
        m_backoff.notify_all();
        if (m_wake)
            m_wake();
        for (auto& thread: m_threadsStorage)
        {
            if (thread.joinable())
                thread.join();
        }
    }

private:
    static std::string readPart(std::string_view& data)
    {
        if (data.size() < 8)
            throw std::runtime_error("invalid delivery queue record");
        uint64_t length{};
        for (unsigned index = 0; index < 8; ++index)
            length |= static_cast<uint64_t>(static_cast<unsigned char>(data[index])) << (index * 8);
        data.remove_prefix(8);
        if (length > data.size())
            throw std::runtime_error("invalid delivery queue record length");
        std::string result(data.substr(0, length));
        data.remove_prefix(length);
        return result;
    }

    static std::string encode(const Record& record)
    {
        std::string encoded;
        appendPart(encoded, record.body);
        for (const auto& [name, value]: record.fields)
        {
            appendPart(encoded, name);
            appendPart(encoded, value);
        }
        return encoded;
    }

    static Record decode(const std::string& data)
    {
        std::string_view view(data);
        Record record;
        record.body = readPart(view);
        while (!view.empty())
            record.fields.emplace_back(readPart(view), readPart(view));
        return record;
    }

    /// The writer is the only thread that appends. The delivery worker never writes a record, so
    /// the head cannot change under it and acknowledgement stays a single DELETE by id.
    void write()
    {
        for (;;)
        {
            const auto record = m_next();
            if (!record)
                return;
            bool appended = false;
            try
            {
                appended = m_queue->append(encode(*record), m_limit);
            }
            catch (const std::exception&)
            {
                if (m_report)
                    m_report(XMQ_EXPORT_QUEUE_FAILED);
            }
            if (!appended)
            {
                if (m_report)
                    m_report(XMQ_EXPORT_QUEUE_FAILED);
                if (!m_deadLetter || !m_deadLetter(*record))
                    if (m_dropped)
                        m_dropped();
            }
        }
    }

    void runDelivery()
    {
        unsigned            delay = 1;
        std::optional<int64_t> deliveredId;
        bool                deadLetterPending = false;
        for (;;)
        {
            if (!deliveredId && m_stopping && m_stopping())
                return;
            try
            {
                if (deliveredId)
                {
                    if (!m_queue->acknowledge(*deliveredId))
                        throw std::runtime_error("delivery queue head changed before acknowledgement");
                    deliveredId.reset();
                    delay = 1;
                    continue;
                }
                const auto stored = m_queue->front();
                if (!stored)
                {
                    std::unique_lock lock(m_stopLock);
                    if (!m_stop)
                        m_backoff.wait_for(lock, std::chrono::milliseconds(250));
                    continue;
                }
                const Record record = decode(stored->data);
                if (!deadLetterPending)
                {
                    const int status = m_deliver ? m_deliver(record) : 1;
                    if (status == 0)
                    {
                        deliveredId = stored->id;
                        continue;
                    }
                    if (m_report)
                        m_report(XMQ_EXPORT_SINK_FAILED);
                    deadLetterPending = status == 1 && m_deadLetter != nullptr;
                }
                if (deadLetterPending && m_deadLetter(record))
                {
                    deadLetterPending = false;
                    deliveredId = stored->id;
                    continue;
                }
            }
            catch (const std::exception&)
            {
                if (m_report)
                    m_report(XMQ_EXPORT_QUEUE_FAILED);
            }
            std::unique_lock lock(m_stopLock);
            if (m_backoff.wait_for(lock, std::chrono::seconds(delay), [this] { return m_stop; }))
                return;
            delay = std::min(60u, delay * 2);
        }
    }

    std::shared_ptr<DeliveryQueue> m_queue;
    std::int64_t                   m_limit;
    Next                           m_next;
    Deliver                        m_deliver;
    DeadLetter                     m_deadLetter;
    Report                         m_report;
    Stopping                       m_stopping;
    Wake                           m_wake;
    Dropped                        m_dropped;

    std::mutex                     m_stopLock;
    std::condition_variable        m_backoff;
    bool                           m_stop{false};
    std::array<std::thread, 2>     m_threadsStorage;
};

} // namespace xmq::exporter
