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

#include "ExportDefaults.h"
#include "HttpPeer.h"
#include "SinkModule.h"
#include "WebhookEntry.h"

#ifdef XMQ_PERSISTENT_DELIVERY
#include "DeliveryQueue.h"
#endif

#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>

namespace xmq::exporter {
namespace {

/// State and workers for one configured export destination.
struct Destination
{
    std::string                                      name;
    Url                                              url;
    std::string                                      secret;
    int                                              timeoutMs{defaultTimeoutMs};
    int                                              retries{defaultRetries};
    std::string                                      tmpl; ///< Never empty; the setting falls back to defaultTemplate.
    std::string                                      logTemplate;
    bool                                             sendMessages{true};
    bool                                             sendLog{false};
    std::vector<std::string>                         topicFilters{"#"};
    size_t                                           maxPayload{std::numeric_limits<size_t>::max()};
    uint32_t                                         minLevel{XMQ_LOG_ERROR};
    std::vector<std::string>                         logSubjects;
    HttpHeaders                                      headers;

    /// How many requests may be in flight at once. One connection per destination is enough on
    /// loopback and far too few across a network: a synchronous reply per record caps a destination
    /// at 1/RTT, which is a hundred records a second at 10 ms.
    int concurrency{4};

    std::mutex                                      lock;
    std::condition_variable                         condition;
    std::deque<WebhookEntry>                         queue;
    std::vector<webhook_sql::SqlRule>                messageRules, logRules;
    std::unique_ptr<SinkModule>                     sink;
#ifdef XMQ_PERSISTENT_DELIVERY
    std::shared_ptr<xmq::exporter::DeliveryQueue>     spool;
    std::string                                     spoolPath, spoolKey;
    std::int64_t                                    spoolLimit{};
    std::mutex                                      spoolLock;
    std::condition_variable                         spoolReady;
#endif
    bool                                            stopping{false};
    std::vector<std::thread>                        threads;
    std::atomic<uint64_t>                           sent{0};
    std::atomic<uint64_t>                           failed{0};
    std::atomic<uint64_t>                           dropped{0};
    std::atomic<uint64_t>                           invalidJson{0};
    std::atomic<uint64_t>                           filtered{0};
};

} // namespace
} // namespace xmq::exporter
