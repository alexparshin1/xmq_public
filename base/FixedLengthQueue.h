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

#include "xmq.h"
#include <queue>

namespace xmq {

/**
 * @brief Fixed-length queue with bounded capacity and thread-safe operations.
 * @remarks Thread-safe.
 * @tparam T Type of elements stored in the queue.
 */
template<typename T>
class XMQ_EXPORT FixedLengthQueue
{
public:
    /**
     * @brief Construct a new FixedLengthQueue object with the specified maximum size.
     * @param maxSize Maximum number of elements the queue can hold.
     */
    explicit FixedLengthQueue(const size_t maxSize)
        : m_pushSemaphore(static_cast<ptrdiff_t>(maxSize))
    {
    }

    /**
     * @brief Push an element into the queue with a timeout.
     * @param value Element to be pushed.
     * @param timeout Maximum time to wait for the push operation.
     * @return True if the element was successfully pushed, false otherwise.
     */
    bool push(const T& value, const std::chrono::milliseconds& timeout)
    {
        if (!m_pushSemaphore.try_acquire_for(timeout))
        {
            return false;
        }

        try
        {
            std::lock_guard lock(m_mutex);
            m_queue.push(value);
        }
        catch (...)
        {
            m_pushSemaphore.release();
            throw;
        }

        m_popSemaphore.release();
        return true;
    }

    /**
     * @brief Push an element into the queue with a timeout.
     * @param value Element to be pushed.
     * @param timeout Maximum time to wait for the push operation.
     * @return True if the element was successfully pushed, false otherwise.
     */
    bool push(T&& value, const std::chrono::milliseconds& timeout)
    {
        if (!m_pushSemaphore.try_acquire_for(timeout))
        {
            return false;
        }

        try
        {
            std::lock_guard lock(m_mutex);
            m_queue.push(std::move(value));
        }
        catch (...)
        {
            m_pushSemaphore.release();
            throw;
        }

        m_popSemaphore.release();

        return true;
    }

    /**
     * @brief Pop an element from the queue with a timeout.
     * @param value Item to pop.
     * @param timeout Pop timeout.
     * @return true if the item popped successfully, or false if timed out.
     */
    bool pop(T& value, const std::chrono::milliseconds& timeout)
    {
        if (!m_popSemaphore.try_acquire_for(timeout))
        {
            return false;
        }

        {
            std::lock_guard lock(m_mutex);
            value = std::move(m_queue.front());
            m_queue.pop();
        }

        m_pushSemaphore.release();

        return true;
    }

    /**
     * @brief Pop an element from the queue with a timeout.
     * @param values Items to pop.
     * @param count Maximum number of items to pop.
     * @param timeout Pop timeout.
     * @return true if the item popped successfully, or false if timed out.
     */
    bool pop(std::vector<T>& values, const size_t count, const std::chrono::milliseconds& timeout)
    {
        values.clear();
        values.reserve(count);
        if (!m_popSemaphore.try_acquire_for(timeout))
        {
            return false;
        }

        size_t poppedCount = 0;
        {
            std::lock_guard lock(m_mutex);

            for (size_t i = 0; i < count && !m_queue.empty(); ++i)
            {
                values.push_back(std::move(m_queue.front()));
                m_queue.pop();
                ++poppedCount;
            }
        }

        m_pushSemaphore.release(static_cast<ptrdiff_t>(poppedCount));

        return poppedCount > 0;
    }

    /**
     * @brief Check if the queue is empty.
     * @return true if the queue is empty.
     */
    auto empty() const
    {
        std::lock_guard lock(m_mutex);
        return m_queue.empty();
    }

    /**
     * @brief Get the current size of the queue.
     * @return queue size.
     */
    auto size() const
    {
        std::lock_guard lock(m_mutex);
        return m_queue.size();
    }

private:
    mutable std::mutex        m_mutex;            ///< Mutex for thread-safe operations.
    std::queue<T>             m_queue;            ///< Queue to store elements.
    std::counting_semaphore<> m_pushSemaphore;    ///< Push semaphore for bounded capacity.
    std::counting_semaphore<> m_popSemaphore {0}; ///< Push semaphore for bounded capacity.
};

} // namespace xmq
