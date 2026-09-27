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

#include "client/MqttClient.h"
#include <memory>
#include <sptk5/cutils>

namespace xmq {

class UtilityClient final : public client::MqttClient
{
public:
    /**
     * @brief Constructor.
     */
    UtilityClient(const std::shared_ptr<sptk::LogEngine>& logEngine = {}, const sptk::String& bindAddress = "")
        : MqttClient(logEngine, "", bindAddress)
    {
    }

    /**
     * @brief Destructor.
     */
    virtual ~UtilityClient() = default;

    void                             setTopics(const sptk::Strings& topics);
    const std::vector<sptk::String>& getTopics() const
    {
        return m_topics;
    }

private:
    std::vector<sptk::String> m_topics;

    sptk::String substituteTopic(const sptk::String& topic) const;
};

using SUtilityClient = std::shared_ptr<UtilityClient>;

} // namespace xmq
