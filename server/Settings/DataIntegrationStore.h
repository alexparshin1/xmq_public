#pragma once

#include <sptk5/cdatabase>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace xmq {

/**
 * @brief The data integrations configured for one node, in the shared cluster SQL database.
 *
 * A data integration is one settings set of an export or import extension. The rows live beside
 * xmq_node and carry its node_id: each node has its own integrations, and a name only has to be
 * unique within that node. The extension and the instance identify a row technically; the name is
 * what the operator sees and edits.
 */
class DataIntegrationStore
{
public:
    struct Row
    {
        std::string                        m_extension;
        std::string                        m_instance;
        std::string                        m_name;
        bool                               m_enabled {true};
        std::map<std::string, std::string> m_settings;
    };

    DataIntegrationStore(std::shared_ptr<sptk::DatabaseConnectionPool> database, std::string nodeId);

    /// Creates the table when it is not there yet. Called at every configuration load, like the
    /// other stores' createSchema(), so a schema added by an upgrade appears without a migration.
    void createSchema() const;

    [[nodiscard]] std::vector<Row> list() const;
    [[nodiscard]] std::optional<Row> find(std::string_view extension, std::string_view instance) const;
    [[nodiscard]] bool nameTaken(std::string_view name, std::string_view exceptInstance = {}) const;

    /// Throws when the name is already used on this node or the row cannot be written.
    void insert(const Row& row) const;
    void update(const Row& row) const;
    void remove(std::string_view extension, std::string_view instance) const;
    void setName(std::string_view extension, std::string_view instance, std::string_view name) const;
    void setEnabled(std::string_view extension, std::string_view instance, bool enabled) const;

    [[nodiscard]] static std::string encodeSettings(const std::map<std::string, std::string>& settings);
    [[nodiscard]] static std::map<std::string, std::string> decodeSettings(const std::string& json);

private:
    [[nodiscard]] bool tableExists() const;

    std::shared_ptr<sptk::DatabaseConnectionPool> m_database;
    std::string                                   m_nodeId;
};

} // namespace xmq
