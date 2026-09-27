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
#include "LogSubject.h"

using namespace std;
using namespace sptk;
using namespace xmq;

string to_string(const LogSubject subject)
{
    switch (subject)
    {
        case LogSubject::Ack:
            return "ack";
        case LogSubject::Publish:
            return "publish";
        case LogSubject::Subscribe:
            return "subscribe";
        case LogSubject::Unsubscribe:
            return "unsubscribe";
        case LogSubject::Connect:
            return "connect";
        case LogSubject::Disconnect:
            return "disconnect";
        case LogSubject::ServerConnections:
            return "server_connections";
        case LogSubject::ServerEvents:
            return "server_events";
        case LogSubject::SessionErrors:
            return "session_errors";
        case LogSubject::ClusterConnections:
            return "cluster_connections";
        case LogSubject::ClusterEvents:
            return "cluster_events";
        case LogSubject::StorageEvents:
            return "storage_events";
        default:
            return "unknown";
    }
}
