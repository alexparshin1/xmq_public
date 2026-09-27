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

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>

namespace xmq {

/**
 * @brief A vector of trivially copyable values that keeps the first Capacity of them inside itself.
 *
 * It exists for one reason: a message's properties must cost no allocation at all when they fit,
 * and they nearly always fit. std::vector allocates on the first element and sptk::Buffer allocates
 * sixteen bytes in its default constructor, so either of them would put an allocation on the path
 * of every message that carries a property - which is the cost this whole representation is meant
 * to remove.
 *
 * Only trivially copyable T: growth and erasure move the elements with memmove and no element is
 * ever destroyed individually, which is what keeps the operations to a single call each.
 *
 * @tparam T          Element type, trivially copyable.
 * @tparam Capacity   How many elements live inside the object before it reaches for the heap.
 */
template<typename T, size_t Capacity>
class InlineVector
{
    static_assert(std::is_trivially_copyable_v<T>, "InlineVector stores trivially copyable values only");
    static_assert(Capacity > 0, "An inline capacity of zero defeats the purpose");

public:
    InlineVector() = default;

    InlineVector(const InlineVector& other)
    {
        assign(other.data(), other.m_size);
    }

    InlineVector(InlineVector&& other) noexcept
    {
        adopt(other);
    }

    InlineVector& operator=(const InlineVector& other)
    {
        if (this != &other)
        {
            assign(other.data(), other.m_size);
        }
        return *this;
    }

    InlineVector& operator=(InlineVector&& other) noexcept
    {
        if (this != &other)
        {
            m_heap.reset();
            m_capacity = Capacity;
            m_size = 0;
            adopt(other);
        }
        return *this;
    }

    ~InlineVector() = default;

    [[nodiscard]] T* data() noexcept
    {
        return m_heap ? m_heap.get() : inlineData();
    }

    [[nodiscard]] const T* data() const noexcept
    {
        return m_heap ? m_heap.get() : inlineData();
    }

    [[nodiscard]] size_t size() const noexcept
    {
        return m_size;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return m_size == 0;
    }

    [[nodiscard]] bool onHeap() const noexcept
    {
        return m_heap != nullptr;
    }

    T& operator[](const size_t index) noexcept
    {
        return data()[index];
    }

    const T& operator[](const size_t index) const noexcept
    {
        return data()[index];
    }

    [[nodiscard]] T* begin() noexcept { return data(); }
    [[nodiscard]] T* end() noexcept { return data() + m_size; }
    [[nodiscard]] const T* begin() const noexcept { return data(); }
    [[nodiscard]] const T* end() const noexcept { return data() + m_size; }

    /**
     * @brief Drop every element, keeping whatever storage was already reserved.
     */
    void clear() noexcept
    {
        m_size = 0;
    }

    /**
     * @brief Make room for at least this many elements in total.
     * @param capacity Number of elements the storage must hold.
     */
    void reserve(const size_t capacity)
    {
        if (capacity <= m_capacity)
        {
            return;
        }
        // Doubling, so that appending property by property stays linear overall.
        const auto grown = std::max(capacity, m_capacity * 2);
        auto       replacement = std::make_unique<T[]>(grown);
        if (m_size != 0)
        {
            std::memcpy(replacement.get(), data(), m_size * sizeof(T));
        }
        m_heap = std::move(replacement);
        m_capacity = grown;
    }

    /**
     * @brief Append one element.
     * @param value Element to append.
     */
    void push_back(const T& value)
    {
        reserve(m_size + 1);
        data()[m_size] = value;
        ++m_size;
    }

    /**
     * @brief Append a run of elements.
     * @param values Where to copy from.
     * @param count  How many elements.
     */
    void append(const T* values, const size_t count)
    {
        if (count == 0)
        {
            return;
        }
        reserve(m_size + count);
        std::memcpy(data() + m_size, values, count * sizeof(T));
        m_size += count;
    }

    /**
     * @brief Replace the contents with a run of elements.
     * @param values Where to copy from.
     * @param count  How many elements.
     */
    void assign(const T* values, const size_t count)
    {
        m_size = 0;
        append(values, count);
    }

    /**
     * @brief Grow by this many elements, leaving them uninitialised, and return where they start.
     * @param count How many elements to add.
     * @return Pointer to the first added element.
     */
    T* extend(const size_t count)
    {
        reserve(m_size + count);
        auto* added = data() + m_size;
        m_size += count;
        return added;
    }

    /**
     * @brief Remove a run of elements, closing the gap.
     * @param offset Where the run starts.
     * @param count  How many elements to remove.
     */
    void erase(const size_t offset, const size_t count)
    {
        if (count == 0 || offset >= m_size)
        {
            return;
        }
        const auto removed = std::min(count, m_size - offset);
        const auto tail = m_size - offset - removed;
        if (tail != 0)
        {
            std::memmove(data() + offset, data() + offset + removed, tail * sizeof(T));
        }
        m_size -= removed;
    }

private:
    void adopt(InlineVector& other) noexcept
    {
        if (other.m_heap)
        {
            m_heap = std::move(other.m_heap);
            m_capacity = other.m_capacity;
        }
        else if (other.m_size != 0)
        {
            std::memcpy(inlineData(), other.inlineData(), other.m_size * sizeof(T));
        }
        m_size = other.m_size;
        other.m_size = 0;
        other.m_capacity = Capacity;
    }

    [[nodiscard]] T* inlineData() noexcept
    {
        return std::launder(reinterpret_cast<T*>(m_inline));
    }

    [[nodiscard]] const T* inlineData() const noexcept
    {
        return std::launder(reinterpret_cast<const T*>(m_inline));
    }

    alignas(T) unsigned char m_inline[Capacity * sizeof(T)] {};
    std::unique_ptr<T[]>     m_heap;
    size_t                   m_size {0};
    size_t                   m_capacity {Capacity};
};

} // namespace xmq
