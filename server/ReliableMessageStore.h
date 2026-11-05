/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <sptk5/net/RedisConnect.h>
#include <functional>
#include <atomic>
#include <mutex>
#include <vector>

namespace xmq {

/** A standalone broker's serialized publication transactions. No success ACK precedes fsync. */
class ReliableMessageStore
{
public:
    explicit ReliableMessageStore(const sptk::URL& url);
    static void sync(sptk::RedisConnect& redis);
    void invalidate() { m_indeterminate = true; }

    class Transaction
    {
    public:
        explicit Transaction(ReliableMessageStore& store);
        ~Transaction();
        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;

        // Each operation is validated by the script before any write is attempted.
        void hashSet(const std::string& key, const std::string& field, const sptk::Buffer& value);
        void addMember(const std::string& key, const std::string& member);
        void hashDelete(const std::string& key, const std::string& field);
        void onCommit(std::function<void()> action);
        void onRollback(std::function<void()> action);
        void commit();
        static Transaction* current() { return s_current; }

    private:
        struct Operation
        {
            std::string kind, key, field, value;
        };
        ReliableMessageStore& m_store;
        std::unique_lock<std::mutex> m_lock;
        std::vector<Operation> m_operations;
        std::vector<std::function<void()>> m_commit;
        std::vector<std::function<void()>> m_rollback;
        bool m_applied {false};
        static thread_local Transaction* s_current;
    };

private:
    sptk::RedisConnect m_redis;
    std::mutex m_mutex;
    std::atomic_bool m_indeterminate {false};
};
} // namespace xmq
