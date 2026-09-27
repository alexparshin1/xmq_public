/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
║  code review                                                                 ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#include "SubscriptionOptions.h"

namespace xmq {

std::string toString(const SubscribeRetainHandling retainHandling)
{
    std::string result;
    switch (retainHandling)
    {
        using enum SubscribeRetainHandling;
        case RetainAlways:
            result = "RetainAlways";
            break;
        case DoNotRetain:
            result = "RetainNever";
            break;
        case RetainIfNew:
            result = "RetainIfNew";
            break;
    }
    return result;
}

} // namespace xmq
