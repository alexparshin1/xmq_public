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

#pragma once

#include <atomic>
#include "ServerNode.h"

namespace xmq::cluster {

class Cluster;

enum class StoreNodeMode
{
    InsertOnly,
    InsertOrUpdate
};

class ServerNodes
{
    friend class Cluster;

public:
    /**
     * @brief Constructor
     */
    explicit ServerNodes(Cluster* cluster, const SStorage& storage);
    /**
     * @brief Destructor
     */
    virtual ~ServerNodes() = default;

    void                     addNode(const SServerNode& node, StoreNodeMode mode);
    void                     setNodeToOffline(const SServerNode& node);
    void                     clear();
    bool                     empty() const;
    size_t                   size() const;
    /// Whether any node is connected, readable without taking the lock the nodes are changed under.
    bool anyNodes() const noexcept
    {
        return m_count.load(std::memory_order_relaxed) != 0;
    }
    SServerNode              findNode(const std::string& nodeName) const;
    std::vector<SServerNode> nodes() const;
    sptk::Strings            getNodeNames() const;
    void                     logMessage(sptk::LogPriority priority, const std::function<std::string()>& output) const;
    void                     updateNodeRecord(const SServerNode& node, bool offline) const;

private:
    mutable std::shared_mutex                       m_mutex;
    std::map<std::string, SServerNode, std::less<>> m_nodes;
    std::atomic<size_t>                             m_count {0}; ///< m_nodes.size(), for anyNodes().
    Cluster*                                        m_cluster;
    SStorage                                        m_storage;

    void storeNodeRecord(const SServerNode& node) const;
    void setNodeRecordToOffline(const SServerNode& node) const;
};

} // namespace xmq::cluster
