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
// language: cpp
#include <gtest/gtest.h>
#include <vector>

#include "server/Settings/LogSubject.h"
#include "server/Settings/LogSubjectPriorities.h"

using namespace std;
using namespace sptk;
using namespace xmq;

using enum LogSubject;
using enum LogPriority;

TEST(LogSubjectPrioritiesTest, DefaultPrioritiesAreInfo)
{
    LogSubjectPriorities p;

    // The default constructor sets subject priorities to Info
    EXPECT_TRUE(p.logSubjectEnabled(Ack, LogPriority::Info));

    // Info is higher than Debug, so Debug-level messages should not be enabled
    EXPECT_FALSE(p.logSubjectEnabled(Publish, LogPriority::Debug));

    // Error is above Info, so Error-level messages should be enabled by default
    EXPECT_TRUE(p.logSubjectEnabled(Ack, LogPriority::Error));
}

TEST(LogSubjectPrioritiesTest, SetSubjectPriority)
{
    LogSubjectPriorities p;

    // change single subject to Error
    p.setLogSubjectPriority(Ack, Error);
    EXPECT_TRUE(p.logSubjectEnabled(Ack, LogPriority::Error));
    // other subjects remain at the default Info level
    EXPECT_TRUE(p.logSubjectEnabled(Publish, LogPriority::Error));
}

TEST(LogSubjectPrioritiesTest, SetAllSubjectsWithEmptyInitializerList)
{
    LogSubjectPriorities p;

    // The empty initializer list should set all subjects to the given priority
    p.setLogSubjectsPriority({}, Debug);

    EXPECT_TRUE(p.logSubjectEnabled(Ack, LogPriority::Debug));
    EXPECT_TRUE(p.logSubjectEnabled(Publish, LogPriority::Debug));
    // Debug should still be less than or equal to Info, so Info-level checks remain true
    EXPECT_TRUE(p.logSubjectEnabled(Subscribe, LogPriority::Info));
}

TEST(LogSubjectPrioritiesTest, SetMultipleSubjectsVector)
{
    LogSubjectPriorities p;

    // set specific subjects to Warn
    const vector subjects = {Publish, Subscribe};
    p.setLogSubjectsPriority(subjects, Warning);

    EXPECT_TRUE(p.logSubjectEnabled(Publish, LogPriority::Warning));
    EXPECT_TRUE(p.logSubjectEnabled(Subscribe, LogPriority::Warning));

    // other subjects should remain at their previous level (Info), so Warn-level should be enabled
    EXPECT_TRUE(p.logSubjectEnabled(Ack, LogPriority::Warning));
}
