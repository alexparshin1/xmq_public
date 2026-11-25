/* SPDX-License-Identifier: MPL-2.0 */
#include "server/Settings/ConfigurationStore.h"
#include "common/DirectoryNames.h"

#include <gtest/gtest.h>
#include <sptk5/cdatabase>

#include <filesystem>

using namespace std;
using namespace sptk;
using namespace xmq;

TEST(XMQ_ConfigurationStoreTests, rowsBelongToOneNodeAndNamesAreUniqueThere)
{
    const auto file = filesystem::temp_directory_path() / "xmq-data-integration-store-tests.db";
    filesystem::remove(file);
    const auto uri = DirectoryNames::sqliteUri(file);
    auto       pool = make_shared<DatabaseConnectionPool>(uri, 2);
    {
        const AutoDatabaseConnection connection(*pool);
        Query                        statement(connection.connection(),
            "CREATE TABLE xmq_node (node_id varchar(36) PRIMARY KEY, name varchar(80) NOT NULL)");
        statement.exec();
        // Both rows exist before the store is built, exactly as ClusterStore leaves them.
        Query node(connection.connection(),
            "INSERT INTO xmq_node (node_id, name) VALUES (:id, :name)");
        node.param("id") = "11111111-1111-4111-8111-111111111111";
        node.param("name") = "node-one";
        node.exec();
        node.param("id") = "22222222-2222-4222-8222-222222222222";
        node.param("name") = "node-two";
        node.exec();
    }

    ConfigurationStore store(pool, "11111111-1111-4111-8111-111111111111");
    store.createSchema();
    store.createSchema(); // idempotent

    ConfigurationStore::Row orders;
    orders.m_extension = "message_export";
    orders.m_extensionInstance = "message_export-1";
    orders.m_name = "Order export";
    orders.m_settings = {{"url", "https://orders.example.org/export"}};
    store.insert(orders);

    EXPECT_TRUE(store.nameTaken("order export"));
    EXPECT_FALSE(store.nameTaken("Order export", orders.m_extensionInstance));
    EXPECT_TRUE(store.find("message_export", orders.m_extensionInstance).has_value());

    ConfigurationStore::Row duplicate = orders;
    duplicate.m_extensionInstance = "message_export-2";
    EXPECT_THROW(store.insert(duplicate), Exception) << "two rows of one node cannot share a name";

    ConfigurationStore::Row otherNode = orders;
    otherNode.m_extensionInstance = "node-two-row";
    otherNode.m_name = "Order export";
    ConfigurationStore otherStore(pool, "22222222-2222-4222-8222-222222222222");
    EXPECT_NO_THROW(otherStore.insert(otherNode));

    store.setName("message_export", orders.m_extensionInstance, "Orders to warehouse");
    auto row = store.find("message_export", orders.m_extensionInstance);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ("Orders to warehouse", row->m_name);
    EXPECT_EQ("https://orders.example.org/export", row->m_settings.at("url"));

    store.setEnabled("message_export", orders.m_extensionInstance, false);
    row = store.find("message_export", orders.m_extensionInstance);
    ASSERT_TRUE(row.has_value());
    EXPECT_FALSE(row->m_enabled);

    ASSERT_EQ(1U, store.list().size());
    store.remove("message_export", orders.m_extensionInstance);
    EXPECT_FALSE(store.find("message_export", orders.m_extensionInstance).has_value());
    EXPECT_TRUE(store.list().empty());

    error_code errorCode;
    filesystem::remove(file, errorCode);
}

TEST(XMQ_ConfigurationStoreTests, storesRowsOnSqliteWhenTheNodeRowExists)
{
    const auto file = filesystem::temp_directory_path() / "xmq-data-integration-sqlite-only.db";
    filesystem::remove(file);
    auto pool = make_shared<DatabaseConnectionPool>(DirectoryNames::sqliteUri(file), 2);

    {
        const AutoDatabaseConnection connection(*pool);
        Query                        node(connection.connection(),
            "CREATE TABLE xmq_node (node_id varchar(36) PRIMARY KEY, name varchar(80) NOT NULL)");
        node.exec();
        Query insert(connection.connection(),
            "INSERT INTO xmq_node (node_id, name) VALUES (:id, :name)");
        insert.param("id") = "33333333-3333-4333-8333-333333333333";
        insert.param("name") = "node-three";
        insert.exec();
    }

    ConfigurationStore store(pool, "33333333-3333-4333-8333-333333333333");
    store.createSchema();

    ConfigurationStore::Row row;
    row.m_extension = "message_export";
    row.m_extensionInstance = "message_export-1";
    row.m_name = "Warehouse export";
    EXPECT_NO_THROW(store.insert(row));
    EXPECT_TRUE(store.find("message_export", row.m_extensionInstance).has_value());

    error_code errorCode;
    filesystem::remove(file, errorCode);
}

TEST(XMQ_ConfigurationStoreTests, refusesToAddAnIntegrationWhenTheNodeRowIsMissing)
{
    const auto file = filesystem::temp_directory_path() / "xmq-data-integration-no-node.db";
    filesystem::remove(file);
    auto pool = make_shared<DatabaseConnectionPool>(DirectoryNames::sqliteUri(file), 2);
    {
        const AutoDatabaseConnection connection(*pool);
        Query                        node(connection.connection(),
            "CREATE TABLE xmq_node (node_id varchar(36) PRIMARY KEY, name varchar(80) NOT NULL)");
        node.exec();
    }
    ConfigurationStore store(pool, "55555555-5555-4555-8555-555555555555");
    store.createSchema();
    ConfigurationStore::Row row;
    row.m_extension = "message_export";
    row.m_extensionInstance = "message_export-1";
    row.m_name = "No node";
    EXPECT_THROW(store.insert(row), Exception);

    error_code errorCode;
    filesystem::remove(file, errorCode);
}
