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

#include "ConfigurationStore.h"

#include <sptk5/Exception.h>
#include <sptk5/xdoc/Document.h>

#include <ranges>

using namespace std;
using namespace sptk;

namespace xmq {

namespace {

constexpr const char* tableName = "xmq_extension_instance";
constexpr const char* columns = "node_id, extension, extension_instance, name, enabled, settings_json";

ConfigurationStore::Row readRow(Query& query)
{
    ConfigurationStore::Row row;
    row.m_extension = query["extension"].asString();
    row.m_extensionInstance = query["extension_instance"].asString();
    row.m_name = query["name"].asString();
    row.m_enabled = query["enabled"].asInt64() != 0;
    row.m_settings = ConfigurationStore::decodeSettings(query["settings_json"].asString());
    return row;
}

} // namespace

ConfigurationStore::ConfigurationStore(shared_ptr<DatabaseConnectionPool> database, string nodeId)
    : m_database(std::move(database))
    , m_nodeId(std::move(nodeId))
{
    if (!m_database)
    {
        throw Exception("ConfigurationStore needs a database");
    }
}

bool ConfigurationStore::tableExists() const
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

void ConfigurationStore::createSchema() const
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
        "  extension_instance varchar(80) NOT NULL,"
        "  name varchar(200) NOT NULL,"
        "  enabled int NOT NULL DEFAULT 1,"
        "  settings_json TEXT NOT NULL,"
        "  CONSTRAINT pk_xmq_extension_instance PRIMARY KEY (node_id, extension, extension_instance),"
        "  CONSTRAINT uq_xmq_extension_instance_name UNIQUE (node_id, name),"
        "  CONSTRAINT fk_xmq_extension_instance_node FOREIGN KEY (node_id)"
        "    REFERENCES xmq_node(node_id) ON DELETE CASCADE)" + suffix);
    statement.exec();
}

vector<ConfigurationStore::Row> ConfigurationStore::list() const
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

optional<ConfigurationStore::Row> ConfigurationStore::find(const string_view extension,
                                                               const string_view extensionInstance) const
{
    if (!tableExists())
    {
        return nullopt;
    }
    const AutoDatabaseConnection connection(*m_database);
    Query                        query(connection.connection(),
        "SELECT " + String(columns) + " FROM " + String(tableName) +
        " WHERE node_id = :node_id AND extension = :extension AND extension_instance = :extension_instance");
    query.param("node_id") = m_nodeId;
    query.param("extension") = string(extension);
    query.param("extension_instance") = string(extensionInstance);
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

bool ConfigurationStore::nameTaken(const string_view name, const string_view exceptExtensionInstance) const
{
    if (!tableExists())
    {
        return false;
    }
    const AutoDatabaseConnection connection(*m_database);
    Query                        query(connection.connection(),
        "SELECT count(*) AS count FROM " + String(tableName) +
        " WHERE node_id = :node_id AND lower(name) = lower(:name) AND extension_instance != :except_extension_instance");
    query.param("node_id") = m_nodeId;
    query.param("name") = string(name);
    query.param("except_extension_instance") = string(exceptExtensionInstance);
    query.open();
    const auto count = query["count"].asInt64();
    query.close();
    return count > 0;
}

void ConfigurationStore::insert(const Row& row) const
{
    createSchema();

    // xmq_node and this node's row are created by ClusterStore when the shared configuration is
    // loaded, before the broker or this store is built. A missing row means the integration is not
    // connected to the cluster configuration database - adding a row here would hide that fault.
    {
        const AutoDatabaseConnection connection(*m_database);
        Query                        probe(connection.connection(),
            "SELECT count(*) AS count FROM xmq_node WHERE node_id = :id");
        probe.param("id") = m_nodeId;
        probe.open();
        const auto found = probe["count"].asInt64();
        probe.close();
        if (found == 0)
        {
            throw Exception("xmq_node has no row for this node; data integrations need the shared "
                            "configuration database");
        }
    }

    const AutoDatabaseConnection connection(*m_database);
    Query                        statement(connection.connection(),
        "INSERT INTO " + String(tableName) + " (" + String(columns) + ") VALUES"
        " (:node_id, :extension, :extension_instance, :name, :enabled, :settings_json)");
    statement.param("node_id") = m_nodeId;
    statement.param("extension") = row.m_extension;
    statement.param("extension_instance") = row.m_extensionInstance;
    statement.param("name") = row.m_name;
    statement.param("enabled") = row.m_enabled ? 1 : 0;
    statement.param("settings_json") = encodeSettings(row.m_settings);
    statement.exec();
}

void ConfigurationStore::update(const Row& row) const
{
    const AutoDatabaseConnection connection(*m_database);
    Query                        statement(connection.connection(),
        "UPDATE " + String(tableName) +
        " SET name = :name, enabled = :enabled, settings_json = :settings_json"
        " WHERE node_id = :node_id AND extension = :extension AND extension_instance = :extension_instance");
    statement.param("node_id") = m_nodeId;
    statement.param("extension") = row.m_extension;
    statement.param("extension_instance") = row.m_extensionInstance;
    statement.param("name") = row.m_name;
    statement.param("enabled") = row.m_enabled ? 1 : 0;
    statement.param("settings_json") = encodeSettings(row.m_settings);
    statement.exec();
}

void ConfigurationStore::remove(const string_view extension, const string_view extensionInstance) const
{
    const AutoDatabaseConnection connection(*m_database);
    Query                        statement(connection.connection(),
        "DELETE FROM " + String(tableName) +
        " WHERE node_id = :node_id AND extension = :extension AND extension_instance = :extension_instance");
    statement.param("node_id") = m_nodeId;
    statement.param("extension") = string(extension);
    statement.param("extension_instance") = string(extensionInstance);
    statement.exec();
}

void ConfigurationStore::setName(const string_view extension, const string_view extensionInstance,
                                   const string_view name) const
{
    auto row = find(extension, extensionInstance);
    if (!row)
    {
        throw Exception("No such data integration");
    }
    row->m_name = name;
    update(*row);
}

void ConfigurationStore::setEnabled(const string_view extension, const string_view extensionInstance,
                                      const bool enabled) const
{
    const AutoDatabaseConnection connection(*m_database);
    Query                        statement(connection.connection(),
        "UPDATE " + String(tableName) + " SET enabled = :enabled"
        " WHERE node_id = :node_id AND extension = :extension AND extension_instance = :extension_instance");
    statement.param("node_id") = m_nodeId;
    statement.param("extension") = string(extension);
    statement.param("extension_instance") = string(extensionInstance);
    statement.param("enabled") = enabled ? 1 : 0;
    statement.exec();
}

string ConfigurationStore::encodeSettings(const map<string, string>& settings)
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

map<string, string> ConfigurationStore::decodeSettings(const string& json)
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
