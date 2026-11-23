/* SPDX-License-Identifier: MPL-2.0 */
#include "server/Settings/DataIntegrationStore.h"
#include "common/DirectoryNames.h"

#include <gtest/gtest.h>
#include <sptk5/cdatabase>

#include <filesystem>

using namespace std;
using namespace sptk;
using namespace xmq;

TEST(XMQ_DataIntegrationStoreTests, rowsBelongToOneNodeAndNamesAreUniqueThere)
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
        Query node(connection.connection(),
            "INSERT INTO xmq_node (node_id, name) VALUES (:id, :name)");
        node.param("id") = "11111111-1111-4111-8111-111111111111";
        node.param("name") = "node-one";
        node.exec();
        node.param("id") = "22222222-2222-4222-8222-222222222222";
        node.param("name") = "node-two";
        node.exec();
    }

    DataIntegrationStore store(pool, "11111111-1111-4111-8111-111111111111");
    store.createSchema();
    store.createSchema(); // idempotent

    DataIntegrationStore::Row orders;
    orders.m_extension = "message_export";
    orders.m_instance = "message_export-1";
    orders.m_name = "Order export";
    orders.m_settings = {{"url", "https://orders.example.org/export"}};
    store.insert(orders);

    EXPECT_TRUE(store.nameTaken("order export"));
    EXPECT_FALSE(store.nameTaken("Order export", orders.m_instance));
    EXPECT_TRUE(store.find("message_export", orders.m_instance).has_value());

    DataIntegrationStore::Row duplicate = orders;
    duplicate.m_instance = "message_export-2";
    EXPECT_THROW(store.insert(duplicate), Exception) << "two rows of one node cannot share a name";

    DataIntegrationStore::Row otherNode = orders;
    otherNode.m_instance = "node-two-row";
    otherNode.m_name = "Order export";
    DataIntegrationStore otherStore(pool, "22222222-2222-4222-8222-222222222222");
    EXPECT_NO_THROW(otherStore.insert(otherNode));

    store.setName("message_export", orders.m_instance, "Orders to warehouse");
    auto row = store.find("message_export", orders.m_instance);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ("Orders to warehouse", row->m_name);
    EXPECT_EQ("https://orders.example.org/export", row->m_settings.at("url"));

    store.setEnabled("message_export", orders.m_instance, false);
    row = store.find("message_export", orders.m_instance);
    ASSERT_TRUE(row.has_value());
    EXPECT_FALSE(row->m_enabled);

    ASSERT_EQ(1U, store.list().size());
    store.remove("message_export", orders.m_instance);
    EXPECT_FALSE(store.find("message_export", orders.m_instance).has_value());
    EXPECT_TRUE(store.list().empty());

    error_code errorCode;
    filesystem::remove(file, errorCode);
}
