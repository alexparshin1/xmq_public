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

#include "SerialId.h"
#include "service/CBridge.h"

namespace xmq {

class Settings;

/**
 * @brief Bridge manager controls bridges in the server's configuration.
 */
class XMQ_EXPORT BridgeManager final : SerialId
{
public:
    using OnChangeEvent = std::function<void()>;

    /**
     * @brief Constructor.
     * @param settings          Settings object used for configuration management.
     * @param onChange          A callback function that is triggered when a change event occurs.
     */
    BridgeManager(Settings* settings, const OnChangeEvent& onChange);

    /**
     * @brief Constructor.
     * @param onChange          A callback function that is triggered when a change event occurs.
     */
    BridgeManager(const BridgeManager& onChange) = default;

    void initialize(bool& changed);

    /**
     * @brief Manages operations on a bridge object based on the given action.
     *
     * This method allows adding, modifying, or removing a bridge from the configuration.
     * It performs the operation based on the specified action ("add", "modify", or "remove").
     *
     * @param action            The operation to perform ("add", "modify", or "remove").
     * @param bridge            Bridge object to operate on.
     *
     * @return The unique identifier (`id`) of the bridge upon successful completion of the operation.
     *
     * @throws sptk::Exception Thrown if
     *         - A bridge already exists during an "add" operation with matching `id`.
     *         - A bridge does not exist during a "modify" or "remove" operation.
     *         - The bridge duplicates another one's name or remote endpoint.
     *         - The action specified is invalid.
     */
    uint64_t control(const std::string& action, const CBridge& bridge);

private:
    /**
     * @brief Reject a bridge that duplicates an existing one.
     *
     * Two bridges may share neither a node name nor a remote endpoint. The name identifies the
     * remote in the log and marks messages that arrived over the link, and a second bridge to
     * the same broker would carry every message twice. Names and host names are compared
     * case-insensitively, and an endpoint with no port is taken as the default MQTT port, so
     * "remote" and "remote:1883" count as the same broker.
     *
     * @param bridge            Bridge being added or modified.
     * @param id                Its id, excluded from the comparison so a bridge is not a
     *                          duplicate of itself.
     *
     * @throws sptk::Exception Thrown if another bridge has the same name or endpoint.
     */
    void checkUnique(const CBridge& bridge, uint64_t id) const;

    Settings*     m_settings; ///< The Server configuration.
    OnChangeEvent m_onChange; ///< Callback method to execute if the bridge changes.
};

} // namespace xmq
