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

#include "DataIntegrationStore.h"

#include <sptk5/Exception.h>
#include <sptk5/xdoc/Document.h>

#include <ranges>

using namespace std;
using namespace sptk;

namespace xmq {

namespace {

constexpr const char* tableName = "xmq_data_integration";
constexpr const char* columns = "node_id, extension, instance, name, enabled, settings_json";

DataIntegrationStore::Row readRow(Query& query)
{
    DataIntegrationStore::Row row;
    row.m_extension = query["extension"].asString();
    row.m_instance = query["instance"].asString();
    row.m_name = query["name"].asString();
    row.m_enabled = query["enabled"].asInt64() != 0;
    row.m_settings = DataIntegrationStore::decodeSettings(query["settings_json"].asString());
    return row;
}

} // namespace

DataIntegrationStore::DataIntegrationStore(shared_ptr<DatabaseConnectionPool> database, string nodeId)
    : m_database(std::move(database))
    , m_nodeId(std::move(nodeId))
{
    if (!m_database)
    {
        throw Exception("DataIntegrationStore needs a database");
    }
}

bool DataIntegrationStore::tableExists() const
{
    try
    {
        const AutoDatabaseConnection connection(*m_database);
        Query                        probe(connection.connection(), "SELECT count(*) FROM " + String(tableName));
        probe.open();
        probe.close();
        return true;
    }
    catch (const Exception&)
    {
        return false;
    }
}

void DataIntegrationStore::createSchema() const
{
    if (tableExists())
    {
        return;
    }
    String driver = m_database->driverName();
    String suffix;
    if (driver.starts_with("mysql") || driver.starts_with("mariadb"))
    {
        suffix = " ENGINE=InnoDB";
    }
    const AutoDatabaseConnection connection(*m_database);
    Query                        statement(connection.connection(),
        "CREATE TABLE " + String(tableName) +
        " (node_id varchar(36) NOT NULL,"
        "  extension varchar(80) NOT NULL,"
        "  instance varchar(80) NOT NULL,"
        "  name varchar(200) NOT NULL,"
        "  enabled int NOT NULL DEFAULT 1,"
        "  settings_json TEXT NOT NULL,"
        "  CONSTRAINT pk_xmq_data_integration PRIMARY KEY (node_id, extension, instance),"
        "  CONSTRAINT uq_xmq_data_integration_name UNIQUE (node_id, name),"
        "  CONSTRAINT fk_xmq_data_integration_node FOREIGN KEY (node_id)"
        "    REFERENCES xmq_node(node_id) ON DELETE CASCADE)" + suffix);
    statement.exec();
}

vector<DataIntegrationStore::Row> DataIntegrationStore::list() const
{
    vector<Row> rows;
    if (!tableExists())
    {
        return rows;
    }
    const AutoDatabaseConnection connection(*m_database);
    Query                        query(connection.connection(),
        "SELECT " + String(columns) + " FROM " + String(tableName) +
        " WHERE node_id = :node_id ORDER BY name");
    query.param("node_id") = m_nodeId;
    query.open();
    while (!query.eof())
    {
        rows.push_back(readRow(query));
        query.next();
    }
    query.close();
    return rows;
}

optional<DataIntegrationStore::Row> DataIntegrationStore::find(const string_view extension,
                                                               const string_view instance) const
{
    if (!tableExists())
    {
        return nullopt;
    }
    const AutoDatabaseConnection connection(*m_database);
    Query                        query(connection.connection(),
        "SELECT " + String(columns) + " FROM " + String(tableName) +
        " WHERE node_id = :node_id AND extension = :extension AND instance = :instance");
    query.param("node_id") = m_nodeId;
    query.param("extension") = string(extension);
    query.param("instance") = string(instance);
    query.open();
    if (query.eof())
    {
        query.close();
        return nullopt;
    }
    auto row = readRow(query);
    query.close();
    return row;
}

bool DataIntegrationStore::nameTaken(const string_view name, const string_view exceptInstance) const
{
    if (!tableExists())
    {
        return false;
    }
    const AutoDatabaseConnection connection(*m_database);
    Query                        query(connection.connection(),
        "SELECT count(*) AS count FROM " + String(tableName) +
        " WHERE node_id = :node_id AND lower(name) = lower(:name) AND instance != :except_instance");
    query.param("node_id") = m_nodeId;
    query.param("name") = string(name);
    query.param("except_instance") = string(exceptInstance);
    query.open();
    const auto count = query["count"].asInt64();
    query.close();
    return count > 0;
}

void DataIntegrationStore::insert(const Row& row) const
{
    createSchema();
    const AutoDatabaseConnection connection(*m_database);
    Query                        statement(connection.connection(),
        "INSERT INTO " + String(tableName) + " (" + String(columns) + ") VALUES"
        " (:node_id, :extension, :instance, :name, :enabled, :settings_json)");
    statement.param("node_id") = m_nodeId;
    statement.param("extension") = row.m_extension;
    statement.param("instance") = row.m_instance;
    statement.param("name") = row.m_name;
    statement.param("enabled") = row.m_enabled ? 1 : 0;
    statement.param("settings_json") = encodeSettings(row.m_settings);
    statement.exec();
}

void DataIntegrationStore::update(const Row& row) const
{
    const AutoDatabaseConnection connection(*m_database);
    Query                        statement(connection.connection(),
        "UPDATE " + String(tableName) +
        " SET name = :name, enabled = :enabled, settings_json = :settings_json"
        " WHERE node_id = :node_id AND extension = :extension AND instance = :instance");
    statement.param("node_id") = m_nodeId;
    statement.param("extension") = row.m_extension;
    statement.param("instance") = row.m_instance;
    statement.param("name") = row.m_name;
    statement.param("enabled") = row.m_enabled ? 1 : 0;
    statement.param("settings_json") = encodeSettings(row.m_settings);
    statement.exec();
}

void DataIntegrationStore::remove(const string_view extension, const string_view instance) const
{
    const AutoDatabaseConnection connection(*m_database);
    Query                        statement(connection.connection(),
        "DELETE FROM " + String(tableName) +
        " WHERE node_id = :node_id AND extension = :extension AND instance = :instance");
    statement.param("node_id") = m_nodeId;
    statement.param("extension") = string(extension);
    statement.param("instance") = string(instance);
    statement.exec();
}

void DataIntegrationStore::setName(const string_view extension, const string_view instance,
                                   const string_view name) const
{
    auto row = find(extension, instance);
    if (!row)
    {
        throw Exception("No such data integration");
    }
    row->m_name = name;
    update(*row);
}

void DataIntegrationStore::setEnabled(const string_view extension, const string_view instance,
                                      const bool enabled) const
{
    const AutoDatabaseConnection connection(*m_database);
    Query                        statement(connection.connection(),
        "UPDATE " + String(tableName) + " SET enabled = :enabled"
        " WHERE node_id = :node_id AND extension = :extension AND instance = :instance");
    statement.param("node_id") = m_nodeId;
    statement.param("extension") = string(extension);
    statement.param("instance") = string(instance);
    statement.param("enabled") = enabled ? 1 : 0;
    statement.exec();
}

string DataIntegrationStore::encodeSettings(const map<string, string>& settings)
{
    xdoc::Document document;
    document.load(String("{}"));
    for (const auto& [key, value]: settings)
    {
        document.root()->set(key, value);
    }
    Buffer written;
    document.exportTo(xdoc::DataFormat::JSON, written, true);
    return written.c_str();
}

map<string, string> DataIntegrationStore::decodeSettings(const string& json)
{
    map<string, string> settings;
    if (json.empty())
    {
        return settings;
    }
    try
    {
        xdoc::Document document;
        document.load(String(json));
        for (const auto& node: document.root()->nodes())
        {
            settings[string(node->getName())] = node->getString().c_str();
        }
    }
    catch (const Exception&)
    {
        // A value written by a newer version is not a reason to lose the row; the extension will
        // fall back to its declared defaults for anything that cannot be read here.
    }
    return settings;
}

} // namespace xmq
