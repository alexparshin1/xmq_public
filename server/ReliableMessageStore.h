/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <sptk5/net/RedisConnect.h>
#include <functional>
#include <atomic>
#include <mutex>
#include <vector>
#include <condition_variable>
#include <deque>
#include <future>
#include <thread>
#include <array>

namespace xmq {

/** A standalone broker's serialized publication transactions. No success ACK precedes fsync. */
class ReliableMessageStore
{
    struct Operation { std::string kind, key, field, value; };
    struct CommitAction { std::function<void()> run; bool durableOnly; };
    struct Pending;
public:
    explicit ReliableMessageStore(const sptk::URL& url);
    ~ReliableMessageStore();
    static void sync(sptk::RedisConnect& redis);
    void invalidate() { m_indeterminate = true; }
    void drain();
    void shutdown();
    // Delivery ACK cleanup is already ordered after its durable publication. It must not
    // take the staging lock: its caller can hold the recipient's queue/session lock.
    void eraseDelivery(std::string key, std::string field, std::function<void()> success,
                       std::function<void(std::exception_ptr)> failure);
    void writeDelivery(std::string key, std::string field, const sptk::Buffer& value,
                       std::function<void()> success, std::function<void(std::exception_ptr)> failure);

    struct Statistics
    {
        uint64_t transactions, batches, largestBatch;
        uint64_t profiledBatches, profiledRequests;
        // Serialization, EVAL, WAITAOF, installation, completion, recycling (nanoseconds).
        std::array<uint64_t, 6> phaseNs;
    };
    Statistics statistics() const;

    class Transaction
    {
    public:
        explicit Transaction(ReliableMessageStore& store, bool readsRetained = false);
        ~Transaction();
        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;

        // Each operation is validated by the script before any write is attempted.
        void set(const std::string& key, const sptk::Buffer& value);
        void hashSet(const std::string& key, const std::string& field, const sptk::Buffer& value);
        void addMember(const std::string& key, const std::string& member);
        void hashDelete(const std::string& key, const std::string& field);
        void onCommit(std::function<void()> action);
        void onDurable(std::function<void()> action);
        void onRollback(std::function<void()> action);
        void commit();
        // The callbacks own any session/connection state needed after the receive pass returns.
        void commitAsync(std::function<void()> success, std::function<void(std::exception_ptr)> failure);
        void changesRetained() { m_changesRetained = true; }
        static Transaction* current() { return s_current; }

    private:
        ReliableMessageStore& m_store;
        std::unique_lock<std::mutex> m_lock;
        std::vector<Operation> m_operations;
        std::vector<CommitAction> m_commit;
        std::vector<std::function<void()>> m_rollback;
        bool m_submitted {false};
        bool m_changesRetained {false};
        std::shared_ptr<Pending> submit(std::function<void()> success = {},
                                        std::function<void(std::exception_ptr)> failure = {});
        static thread_local Transaction* s_current;
    };

private:
    struct Pending
    {
        std::vector<Operation> operations;
        std::vector<CommitAction> commit;
        std::vector<std::function<void()>> rollback;
        std::function<void()> success;
        std::function<void(std::exception_ptr)> failure;
        std::promise<void> completed;
        size_t bytes {0};
        bool changesRetained {false};
    };
    void enqueue(const std::shared_ptr<Pending>& pending);
    void queueDeliveryOperation(Operation operation, std::function<void()> success,
                                std::function<void(std::exception_ptr)> failure);
    struct CommittedBatch
    {
        std::vector<std::shared_ptr<Pending>> requests;
        std::exception_ptr failure;
        bool applied {false}, durable {false}, profile {false};
        std::array<uint64_t, 6> phases {};
    };
    void run();
    void complete();
    sptk::RedisConnect m_redis;
    // Protects staging/visibility order, never held while awaiting disk I/O.
    std::mutex m_mutex;
    std::atomic_bool m_indeterminate {false};
    mutable std::mutex m_queueMutex;
    std::condition_variable m_changed, m_readyChanged;
    std::deque<std::shared_ptr<Pending>> m_pending;
    std::deque<CommittedBatch> m_ready;
    size_t m_outstanding {0}, m_pendingBytes {0}, m_pendingRetained {0};
    bool m_stopping {false};
    bool m_writerDone {false};
    std::thread m_worker, m_completer;
    uint64_t m_transactions {0}, m_batches {0}, m_largestBatch {0};
    uint64_t m_finished {0};
    uint64_t m_profiledBatches {0}, m_profiledRequests {0};
    std::array<uint64_t, 6> m_phaseNs {};
};
} // namespace xmq
