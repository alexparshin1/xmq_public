#include "Sink.h"
#include "DbSinkSettings.h"
#include "extension/XmqExtension.h"
#include <sptk5/db/AutoDatabaseConnection.h>
#include <sptk5/db/DatabaseConnectionPool.h>
#include <sptk5/db/DatabaseConnectionString.h>
#include <sptk5/db/PoolDatabaseConnection.h>
#include <sptk5/db/Query.h>
#include <cstdio>
#include <filesystem>
#include <memory>

namespace {

class DbSink : public xmq::XmqExtensionBase
{
public:
    using XmqExtensionBase::XmqExtensionBase;
    bool start() override { return false; } // The exporter owns delivery instances.
};

constexpr xmq_setting settings[] = {
    XMQ_DB_SINK_SETTINGS,
};

std::string databaseUri(XmqSinkV1::Setting get, void* context)
{
    const std::string type = get(context, "db_type");
    if (type.empty() || type == "Use saved URI")
        return get(context, "database_uri");
    if (type == "sqlite3")
    {
        const std::string path = get(context, "db_path");
        if (!std::filesystem::path(path).is_absolute())
            throw std::invalid_argument("Enter an absolute SQLite file path");
        return "sqlite3://localhost/" + path;
    }
    if (type != "postgresql" && type != "mysql" && type != "oracle" && type != "mssql")
        throw std::invalid_argument("Unknown database type");
    const std::string host = get(context, "db_host");
    const std::string port = get(context, "db_port");
    const std::string name = get(context, "db_name");
    if (host.empty() || host.find_first_of("/@?# \t\r\n") != host.npos ||
        name.empty() || name.find_first_of("/?#\r\n") != name.npos)
        throw std::invalid_argument("Enter a valid database host and database name");
    if (!port.empty() && (port.size() > 5 || port.find_first_not_of("0123456789") != port.npos ||
                          std::stoi(port) < 1 || std::stoi(port) > 65535))
        throw std::invalid_argument("Database port must be between 1 and 65535");
    sptk::DatabaseConnectionString uri((type + "://" + host + (port.empty() ? "" : ":" + port) + "/" + name).c_str());
    uri.userName(get(context, "db_user"));
    uri.password(get(context, "db_password"));
    return uri.toString(true).c_str();
}

struct DbDelivery
{
    std::string uri, insert;
    std::unique_ptr<sptk::DatabaseConnectionPool> pool;
};

const XmqSinkV1 api{
    [](XmqSinkV1::Setting get, void* context, char* error, size_t size) -> void*
    {
        try
        {
            auto state = std::make_unique<DbDelivery>();
            state->uri = databaseUri(get, context);
            state->insert = get(context, "insert_sql");
            if (state->uri.empty() || state->insert.empty())
                throw std::invalid_argument("database_uri and insert_sql are required");
            return state.release();
        }
        catch (const std::exception& exception)
        {
            if (error && size)
                snprintf(error, size, "%s", exception.what());
            return nullptr;
        }
    },
    [](void* instance) { delete static_cast<DbDelivery*>(instance); },
    [](void* instance, const char* body, size_t length, const char* const* names,
       const char* const* values, size_t count) -> int
    {
        auto& state = *static_cast<DbDelivery*>(instance);
        try
        {
            if (!state.pool)
                state.pool = std::make_unique<sptk::DatabaseConnectionPool>(state.uri, 1);
            const sptk::AutoDatabaseConnection held(*state.pool);
            try
            {
                sptk::Query query(held.connection(), state.insert.c_str());
                for (size_t i = 0; i < count; ++i)
                {
                    if (std::string_view(names[i]) == "fields")
                        continue;
                    try
                    {
                        query.param(names[i]) = values[i];
                    }
                    catch (const sptk::Exception&)
                    {
                        // A rule may produce fields the destination does not store.
                    }
                }
                query.param("fields") = std::string(body, length).c_str();
                query.exec();
                return 0;
            }
            catch (...)
            {
                if (held.active())
                    return 1;
                throw;
            }
        }
        catch (...)
        {
            state.pool.reset();
            return 2;
        }
    }
};
} // namespace

extern "C" XMQ_EXTENSION_EXPORT const XmqSinkV1* xmq_sink_v1()
{
    return &api;
}

XMQ_DEFINE_EXTENSION_DESCRIBED(DbSink, "db_sink", "1.2", XMQ_CAP_SINK,
                               "Writes exported JSON to a database using a parameterized INSERT.", settings)
