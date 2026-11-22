#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace xmq::exporter {

// Used by the export workers, never by the MQTT receive path. One delivery
// worker owns the head until the sink acknowledges it.
class DeliveryQueue
{
    using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
    // A separate database keeps an ownership lock for the life of the queue
    // without holding a transaction open on the data database.
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> m_owner{nullptr, sqlite3_close};
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> m_db{nullptr, sqlite3_close};

    void check(int code) const
    {
        if (code != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(m_db.get()));
    }

    void exec(const char* sql) const
    {
        check(sqlite3_exec(m_db.get(), sql, nullptr, nullptr, nullptr));
    }

    Statement statement(const char* sql) const
    {
        sqlite3_stmt* raw{};
        const auto code = sqlite3_prepare_v2(m_db.get(), sql, -1, &raw, nullptr);
        Statement result(raw, sqlite3_finalize);
        check(code);
        return result;
    }

    void bind(sqlite3_stmt* stmt, int index, const std::string& value) const
    {
        check(sqlite3_bind_blob64(stmt, index, value.data(), value.size(), SQLITE_TRANSIENT));
    }

    bool step(sqlite3_stmt* stmt) const
    {
        const auto code = sqlite3_step(stmt);
        if (code == SQLITE_ROW)
            return true;
        if (code != SQLITE_DONE)
            check(code);
        return false;
    }

public:
    struct Record
    {
        std::int64_t id;
        // Opaque encoded body and bindings; may contain embedded NULs.
        std::string data;
    };

    DeliveryQueue(const std::string& path, const std::string& destinationKey)
    {
        if (path.empty() || path == ":memory:" || destinationKey.empty())
            throw std::invalid_argument("delivery queue requires a disk path and destination identity");
        sqlite3* owner{};
        const auto ownerCode = sqlite3_open_v2((path + ".owner").c_str(), &owner,
                                               SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        m_owner.reset(owner);
        if (ownerCode != SQLITE_OK ||
            sqlite3_exec(owner, "BEGIN EXCLUSIVE", nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("cannot acquire delivery queue ownership");
        sqlite3* raw{};
        const auto code = sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        m_db.reset(raw);
        if (!raw)
            throw std::runtime_error("cannot allocate delivery queue connection");
        check(code);
        check(sqlite3_busy_timeout(raw, 5000));
        {
            auto wal = statement("PRAGMA journal_mode=WAL");
            if (!step(wal.get()) || std::string(reinterpret_cast<const char*>(
                sqlite3_column_text(wal.get(), 0))) != "wal")
                throw std::runtime_error("delivery queue requires SQLite WAL mode");
        }
        exec("PRAGMA synchronous=FULL");
        exec("BEGIN IMMEDIATE");
        exec("CREATE TABLE IF NOT EXISTS destination (id INTEGER PRIMARY KEY CHECK(id = 1), key BLOB NOT NULL)");
        exec("CREATE TABLE IF NOT EXISTS records (id INTEGER PRIMARY KEY AUTOINCREMENT, data BLOB NOT NULL)");
        exec("CREATE TABLE IF NOT EXISTS usage (id INTEGER PRIMARY KEY CHECK(id = 1), bytes INTEGER NOT NULL)");
        exec("INSERT OR IGNORE INTO usage SELECT 1, COALESCE(SUM(length(data)), 0) FROM records");
        exec("CREATE TRIGGER IF NOT EXISTS records_added AFTER INSERT ON records BEGIN "
             "UPDATE usage SET bytes = bytes + length(NEW.data) WHERE id = 1; END");
        exec("CREATE TRIGGER IF NOT EXISTS records_removed AFTER DELETE ON records BEGIN "
             "UPDATE usage SET bytes = bytes - length(OLD.data) WHERE id = 1; END");
        {
            auto insert = statement("INSERT OR IGNORE INTO destination VALUES (1, ?)");
            bind(insert.get(), 1, destinationKey);
            step(insert.get());
            auto verify = statement("SELECT 1 FROM destination WHERE id = 1 AND key = ?");
            bind(verify.get(), 1, destinationKey);
            if (!step(verify.get()))
                throw std::runtime_error("delivery queue belongs to a different destination");
        }
        exec("COMMIT");
    }

    // Bound the live encoded bytes, not the physical SQLite/WAL file sizes.
    // The check and insert are one statement, even across SQLite connections.
    bool append(const std::string& data, std::int64_t maxBytes)
    {
        if (maxBytes <= 0 || data.empty() || data.size() > static_cast<std::uint64_t>(maxBytes))
            return false;
        auto stmt = statement("INSERT INTO records(data) SELECT ?1 WHERE "
                              "(SELECT bytes FROM usage WHERE id = 1) <= ?2 - length(?1)");
        bind(stmt.get(), 1, data);
        check(sqlite3_bind_int64(stmt.get(), 2, maxBytes));
        step(stmt.get());
        return sqlite3_changes(m_db.get()) == 1;
    }

    std::optional<Record> front() const
    {
        auto stmt = statement("SELECT id, data FROM records ORDER BY id LIMIT 1");
        if (!step(stmt.get()))
            return std::nullopt;
        return Record{sqlite3_column_int64(stmt.get(), 0),
                      std::string(static_cast<const char*>(sqlite3_column_blob(stmt.get(), 1)),
                                  sqlite3_column_bytes(stmt.get(), 1))};
    }

    bool acknowledge(std::int64_t id)
    {
        auto stmt = statement("DELETE FROM records WHERE id = ? AND id = (SELECT MIN(id) FROM records)");
        check(sqlite3_bind_int64(stmt.get(), 1, id));
        step(stmt.get());
        return sqlite3_changes(m_db.get()) == 1;
    }
};

} // namespace xmq::exporter
