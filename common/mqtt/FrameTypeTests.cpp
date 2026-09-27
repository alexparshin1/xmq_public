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

#include "FrameTypeTests.h"

using namespace std;

namespace xmq::mqtt {

std::string frameTypeToString(const FrameTypeTests& frameType)
{
    string output;
    switch (frameType)
    {
        using enum FrameTypeTests;
        case Connect:
            output = "Connect";
            break;
        case ConnAck:
            output = "ConnAck";
            break;
        case Publish:
            output = "Publish";
            break;
        case PubAck:
            output = "PubAck";
            break;
        case PubRec:
            output = "PubRec";
            break;
        case PubRel:
            output = "PubRel";
            break;
        case PubComp:
            output = "PubComp";
            break;
        case Subscribe:
            output = "Subscribe";
            break;
        case SubAck:
            output = "SubAck";
            break;
        case Unsubscribe:
            output = "Unsubscribe";
            break;
        case UnsubAck:
            output = "UnsubAck";
            break;
        case PingReq:
            output = "PingReq";
            break;
        case PingResp:
            output = "PingResp";
            break;
        case Disconnect:
            output = "Disconnect";
            break;
        case Undefined:
            output = "Undefined";
            break;
    }
    return output;
}

} // namespace xmq::mqtt
