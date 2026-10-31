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

#include "common/IpWhitelist.h"

#include <gtest/gtest.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

using namespace std;
using namespace xmq;

namespace {

/// An address as sin_addr holds it, written the way a person thinks of it.
uint32_t address(const char* text)
{
    return inet_addr(text);
}

} // namespace

// An empty list is what a listener has by default, and it must keep admitting everyone: the
// feature is an addition, not a change of behaviour for anyone who does not set it.
TEST(XMQ_IpWhitelistTests, anEmptyListAllowsAnyone)
{
    const IpWhitelist whitelist("");
    EXPECT_TRUE(whitelist.empty());
    EXPECT_TRUE(whitelist.allows(address("10.1.1.1")));
    EXPECT_TRUE(whitelist.allows(address("192.168.0.1")));
}

TEST(XMQ_IpWhitelistTests, aBareAddressMatchesExactlyThatAddress)
{
    const IpWhitelist whitelist("10.1.1.242");
    EXPECT_FALSE(whitelist.empty());
    EXPECT_TRUE(whitelist.allows(address("10.1.1.242")));
    EXPECT_FALSE(whitelist.allows(address("10.1.1.243")));
    EXPECT_FALSE(whitelist.allows(address("10.1.1.0")));
}

TEST(XMQ_IpWhitelistTests, aPrefixMatchesTheWholeNetwork)
{
    const IpWhitelist whitelist("10.1.1.1/24");
    EXPECT_TRUE(whitelist.allows(address("10.1.1.0")));
    EXPECT_TRUE(whitelist.allows(address("10.1.1.255")));
    EXPECT_FALSE(whitelist.allows(address("10.1.2.1")));
}

TEST(XMQ_IpWhitelistTests, theExamplesFromTheInterfaceAllWork)
{
    const IpWhitelist whitelist("10.1.1.1/24, 10.1.2.123/32, 10.1.1.242");
    EXPECT_TRUE(whitelist.allows(address("10.1.1.7")));
    EXPECT_TRUE(whitelist.allows(address("10.1.2.123")));
    EXPECT_TRUE(whitelist.allows(address("10.1.1.242")));
    EXPECT_FALSE(whitelist.allows(address("10.1.2.124")));
    EXPECT_FALSE(whitelist.allows(address("8.8.8.8")));
}

// The first matching entry decides, and a match is an allow: an address inside an admitted network
// is admitted even when a later, narrower entry would not match it.
TEST(XMQ_IpWhitelistTests, theFirstMatchingEntryAllows)
{
    const IpWhitelist whitelist("10.1.1.0/24,10.1.1.5/32");
    EXPECT_TRUE(whitelist.allows(address("10.1.1.200")));
}

TEST(XMQ_IpWhitelistTests, aZeroPrefixAllowsEverything)
{
    const IpWhitelist whitelist("0.0.0.0/0");
    EXPECT_TRUE(whitelist.allows(address("10.1.1.1")));
    EXPECT_TRUE(whitelist.allows(address("255.255.255.255")));
}

TEST(XMQ_IpWhitelistTests, refusesWhatItCannotParse)
{
    for (const auto* text: {"10.1.1", "10.1.1.1/33", "10.1.1.1/", "10.1.1.256",
                            "10.1.1.1,", ",10.1.1.1", "10.1.1.1,,10.1.1.2", "ten.1.1.1",
                            "10.1.1.1/-1", "10.1.1.1/1.5"})
    {
        EXPECT_FALSE(IpWhitelist::isValid(text)) << "accepted '" << text << "'";
        EXPECT_THROW((void) IpWhitelist(text), std::invalid_argument) << "parsed '" << text << "'";
    }
}

TEST(XMQ_IpWhitelistTests, acceptsWhatAPersonWouldWrite)
{
    for (const auto* text: {"", "  ", "10.1.1.1", " 10.1.1.1 ", "10.1.1.1/24",
                            "10.1.1.1,10.1.2.2", "10.1.1.1 , 10.1.2.2", "0.0.0.0/0"})
    {
        EXPECT_TRUE(IpWhitelist::isValid(text)) << "refused '" << text << "'";
    }
}

// The text is kept trimmed, which is what the settings file and the node row hold: what was
// written there is what comes back out.
TEST(XMQ_IpWhitelistTests, keepsTheTextTrimmed)
{
    const IpWhitelist whitelist("  10.1.1.1/24, 10.1.2.2  ");
    EXPECT_EQ("10.1.1.1/24, 10.1.2.2", whitelist.text());
}
