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

#include "Sink.h"

#include <map>
#include <string>

namespace xmq::exporter {

struct SinkModule
{
    SinkModule() = default;
    ~SinkModule();
    SinkModule(const SinkModule&) = delete;
    SinkModule& operator=(const SinkModule&) = delete;

    bool open(const std::string& name);
    static const char* setting(void* context, const char* key);

    const XmqSinkV1* api{};
    const xmq_extension* metadata{};
    std::map<std::string, std::string> settings;

private:
    void* handle{};
};

} // namespace xmq::exporter
