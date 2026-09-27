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

#include <base/Message.h>
#include <gtest/gtest.h>

using namespace std;
using namespace sptk;
using namespace xmq;

TEST(XMQ_Message, reasonCodeToString)
{
    using enum ReasonCode;
    EXPECT_STREQ(toString(ErrorInvalidProtocol).c_str(), "Invalid protocol");
    EXPECT_STREQ(toString(ErrorIdentifierRejected).c_str(), "Identifier rejected");
    EXPECT_STREQ(toString(ErrorServerNotAvailable).c_str(), "Server not available");
    EXPECT_STREQ(toString(ErrorAuthenticationFailed).c_str(), "Authentication failed");
    EXPECT_STREQ(toString(ErrorNotAuthorized).c_str(), "Not authorized");
    EXPECT_STREQ(toString(UnspecifiedError).c_str(), "Unspecified error");
    EXPECT_STREQ(toString(MalformedPacket).c_str(), "Malformed packet");
    EXPECT_STREQ(toString(ProtocolError).c_str(), "Protocol error");
    EXPECT_STREQ(toString(ImplementationSpecificError).c_str(), "Implementation specific error");
    EXPECT_STREQ(toString(UnsupportedProtocolVersion).c_str(), "Unsupported Protocol Version");
    EXPECT_STREQ(toString(IdentifierRejected).c_str(), "Client Identifier not valid");
    EXPECT_STREQ(toString(AuthenticationFailed).c_str(), "Bad user name or password");
    EXPECT_STREQ(toString(NotAuthorized).c_str(), "Client not authorized to connect");
    EXPECT_STREQ(toString(ServerUnavailable).c_str(), "The MQTT server unavailable");
    EXPECT_STREQ(toString(ServerBusy).c_str(), "Server busy");
    EXPECT_STREQ(toString(Banned).c_str(), "The client banned by administrative action");
    EXPECT_STREQ(toString(BadAuthenticationMethod).c_str(), "The authentication method is not supported");
    EXPECT_STREQ(toString(WillTopicNameInvalid).c_str(), "The will topic Name is not malformed, but is not accepted by this Server");
    EXPECT_STREQ(toString(PacketTooLarge).c_str(), "The CONNECT packet exceeded the maximum permissible size");
    EXPECT_STREQ(toString(QuotaExceeded).c_str(), "An implementation or administrative imposed limit has been exceeded");
    EXPECT_STREQ(toString(PayloadFormatInvalid).c_str(), "The will payload does not match the specified Payload Format Indicator");
    EXPECT_STREQ(toString(RetainNotSupported).c_str(), "The Server does not support retained messages, and Will Retain was set to 1");
    EXPECT_STREQ(toString(WillQoSNotSupported).c_str(), "The Server does not support the QoS set in Will QoS");
    EXPECT_STREQ(toString(UseAnotherServer).c_str(), "The Client should temporarily use another server");
    EXPECT_STREQ(toString(ServerMoved).c_str(), "The Client should permanently use another server");
    EXPECT_STREQ(toString(ConnectionRateExceeded).c_str(), "The Server has exceeded the connection rate limit");
}
