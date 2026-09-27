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


#include "server/Settings/UriSecret.h"

#include <gtest/gtest.h>

using namespace std;
using namespace sptk;
using namespace xmq;

TEST(XMQ_UriSecretTests, hidesThePasswordAndNothingElse)
{
    EXPECT_EQ("postgresql://someone:*****@dbhost/accounts",
              UriSecret::hidden("postgresql://someone:secret@dbhost/accounts"));
    EXPECT_EQ("redis://:*****@cache-host:6379",
              UriSecret::hidden("redis://:hunter2@cache-host:6379"));
}

// Nothing to hide is left alone: a mask where there was no password would show one that is not
// there, and would come back as a password nobody set.
TEST(XMQ_UriSecretTests, leavesAUriWithNoPasswordAsItIs)
{
    for (const auto* uri: {"redis://redis_server:6379",
                           "postgresql://someone@dbhost/accounts",
                           "sqlite3://localhost//var/lib/xmq/xmq_users.db",
                           "not a uri at all",
                           ""})
    {
        EXPECT_EQ(String(uri), UriSecret::hidden(uri)) << "changed '" << uri << "'";
    }
}

// A password may contain the characters a naive parse would split on.
TEST(XMQ_UriSecretTests, findsThePasswordWhenItLooksLikePartOfTheAddress)
{
    EXPECT_EQ("postgresql://someone:*****@dbhost/accounts",
              UriSecret::hidden("postgresql://someone:p@ssw:rd@dbhost/accounts"))
        << "a password containing '@' or ':' was mis-parsed";

    // The path may hold an '@' too, and it is not the credentials separator.
    EXPECT_EQ("postgresql://someone:*****@dbhost/db@2026",
              UriSecret::hidden("postgresql://someone:secret@dbhost/db@2026"));
}

TEST(XMQ_UriSecretTests, putsTheStoredPasswordBackWhenTheMaskComesHome)
{
    const String stored("postgresql://someone:secret@dbhost/accounts");

    EXPECT_EQ(stored, UriSecret::restored("postgresql://someone:*****@dbhost/accounts", stored))
        << "the password was lost by a save that changed nothing else";

    // The rest of the URI is whatever came back, not whatever was stored: this is how a host or a
    // database name is changed without retyping the password.
    EXPECT_EQ("postgresql://someone:secret@other-host/other-db",
              UriSecret::restored("postgresql://someone:*****@other-host/other-db", stored));
}

TEST(XMQ_UriSecretTests, aRealPasswordIsTakenAsGiven)
{
    const String stored("postgresql://someone:secret@dbhost/accounts");

    EXPECT_EQ("postgresql://someone:brand-new@dbhost/accounts",
              UriSecret::restored("postgresql://someone:brand-new@dbhost/accounts", stored))
        << "a changed password was refused";

    // Including a change to a URI with no password at all, which is how one is cleared.
    EXPECT_EQ("postgresql://someone@dbhost/accounts",
              UriSecret::restored("postgresql://someone@dbhost/accounts", stored));
}

// The mask must never reach storage. If it comes back for something that has no password on
// record, there is nothing to put back and keeping it would store '*****' as the password.
TEST(XMQ_UriSecretTests, theMaskIsNeverStored)
{
    // The invariant is that the mask does not reach storage, not that the URI comes back in one
    // particular shape: this only arises if something fabricates a mask where hidden() made none.
    const auto restored = UriSecret::restored("redis://:*****@cache-host:6379", "redis://cache-host:6379");
    EXPECT_EQ(String::npos, restored.find("*****")) << "the mask was stored as the password: " << restored;
    EXPECT_NE(String::npos, restored.find("cache-host:6379")) << "the address was lost: " << restored;
}
