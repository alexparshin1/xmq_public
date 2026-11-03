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

/**
 * @file TapRecorder.cpp
 * @brief A test-only message tap: writes each delivered message to a file.
 *
 * Not a sample. It exists so the host's tap delivery can be observed from a test without an HTTP
 * server in the middle: one line per message, payload in hex so a binary body survives the file.
 */

#include "extension/XmqExtension.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>

namespace {

class TapRecorder : public xmq::XmqExtensionBase
{
public:
    using XmqExtensionBase::XmqExtensionBase;

    bool start() override
    {
        m_path = setting("path");
        m_delay = std::chrono::milliseconds(std::stoi(setting("delay_ms", "0")));
        if (m_path.empty())
        {
            return false;
        }
        return subscribeMessages(setting("filter", "#"),
                                 static_cast<size_t>(std::stoull(setting("max_payload", "0"))));
    }

    void onMessage(const xmq_message& message) override
    {
        if (m_delay.count() > 0)
        {
            std::this_thread::sleep_for(m_delay);
        }
        std::ostringstream line;
        line << xmq::view(message.client_id) << '|' << xmq::view(message.username) << '|'
             << xmq::view(message.topic) << '|';
        for (size_t at = 0; at < message.payload.length; ++at)
        {
            line << std::hex << std::setw(2) << std::setfill('0')
                 << static_cast<unsigned>(static_cast<unsigned char>(message.payload.data[at]));
        }
        line << '|' << static_cast<unsigned>(message.qos) << '|' << static_cast<unsigned>(message.retain)
             << '\n';

        std::ofstream out(m_path, std::ios::app);
        out << line.str();
    }

private:
    std::string               m_path;
    std::chrono::milliseconds m_delay {0};
};

} // namespace

XMQ_DEFINE_EXTENSION(TapRecorder, "tap-recorder", "1.0", XMQ_CAP_MESSAGE_TAP)
