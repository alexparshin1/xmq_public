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

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <version>

namespace xmq {

/**
 * @brief An atomically replaceable std::shared_ptr.
 *
 * C++20's std::atomic<std::shared_ptr<T>> where the standard library has it, and an equivalent
 * built from the older free functions where it does not. libc++ is the case that does not: as of
 * LLVM 19 it has not implemented P0718R2, so std::atomic<std::shared_ptr<T>> falls back to the
 * primary template and fails a static assertion about trivial copyability - which is what a build
 * of this broker against libc++ ran into on every one of these members at once.
 *
 * Only load, store and exchange are offered, because those are the three operations this code
 * uses. Adding compare_exchange would mean writing it correctly on both sides, and nothing here
 * needs it.
 *
 * Where the standard specialisation exists this is a plain alias, so the generated code on
 * libstdc++ is exactly what it was before - the substitution costs nothing on the platforms the
 * broker is measured on.
 */
// XMQ_FORCE_ATOMIC_SHARED_PTR_LOCK takes the replacement even where the specialisation exists.
// It is not for production use: it is there so the cost of the replacement can be measured against
// the specialisation on one compiler, instead of being confounded with everything else that
// differs between two.
#if defined(__cpp_lib_atomic_shared_ptr) && !defined(XMQ_FORCE_ATOMIC_SHARED_PTR_LOCK)

template<typename T>
using AtomicSharedPtr = std::atomic<std::shared_ptr<T>>;

#else

// A lock of its own, rather than the free std::atomic_load/store overloads. Those are what libc++
// deprecated in favour of the specialisation it never shipped, and it implements them over one
// global table of sixteen mutexes hashed by address - so every shared_ptr in the process contends
// for the same sixteen locks, and the broker touches these members on the path of every message.
// One mutex per object costs a few dozen bytes and contends only with itself.
template<typename T>
class AtomicSharedPtr
{
public:
    using value_type = std::shared_ptr<T>;

    AtomicSharedPtr() noexcept = default;

    // Not explicit, so that a member can be initialised with nullptr the way the specialisation
    // allows.
    // NOLINTNEXTLINE(google-explicit-constructor)
    AtomicSharedPtr(value_type value) noexcept
        : m_value(std::move(value))
    {
    }

    // An atomic is not copyable, and neither is this.
    AtomicSharedPtr(const AtomicSharedPtr&) = delete;
    AtomicSharedPtr& operator=(const AtomicSharedPtr&) = delete;
    AtomicSharedPtr(AtomicSharedPtr&&) = delete;
    AtomicSharedPtr& operator=(AtomicSharedPtr&&) = delete;
    ~AtomicSharedPtr() = default;

    /// The memory order is accepted and ignored: the lock orders these accesses, and it orders
    /// them more strongly than any argument could ask for.
    [[nodiscard]] value_type load(std::memory_order = std::memory_order_seq_cst) const noexcept
    {
        const std::lock_guard lock(m_mutex);
        return m_value;
    }

    void store(value_type value, std::memory_order = std::memory_order_seq_cst) noexcept
    {
        // The value being replaced is released after the lock is dropped, not under it: this may
        // be its last reference, and running somebody's destructor inside the critical section
        // would hold the lock for as long as that destructor takes.
        value_type previous;
        {
            const std::lock_guard lock(m_mutex);
            previous = std::move(m_value);
            m_value = std::move(value);
        }
    }

    value_type exchange(value_type value, std::memory_order = std::memory_order_seq_cst) noexcept
    {
        const std::lock_guard lock(m_mutex);
        m_value.swap(value);
        return value;
    }

private:
    mutable std::mutex m_mutex;
    value_type         m_value;
};

#endif

} // namespace xmq
