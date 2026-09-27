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

#include "TestOptions.h"
#include "test/ClusterTests/ClusterTests.h"

using namespace std;
using namespace sptk;
using namespace xmq;

/**
 * Test that cluster nodes know the common time from the database.
 */
TEST_F(XMQ_ClusterTests, currentTime)
{
    const string databaseUri = "postgresql://gtest@localhost/xmq_test";
    const auto   primaryNode = createNode("primary", 1880, true);
    const auto   cluster = server()->getCluster();
    const auto   currentTime = DateTime::Now();
    const auto   timediff = chrono::duration_cast<chrono::milliseconds>(currentTime - cluster->getClusterTime());

    // The tolerance follows the build, because what is being measured does. Storage samples its
    // clock offset by taking the midpoint of the round trip to Redis, which cancels the symmetric
    // half of the latency - and in a release build that is nearly all of it. A debug build spends
    // as long parsing the reply as the network spends carrying it, and every bit of that parsing
    // falls after the server stamped its time: the midpoint lands late, and the storage clock reads
    // a few hundred milliseconds behind. 780ms was measured on Windows.
    //
    // The clocks themselves are not the reason. Compared directly, this host, the Redis host and
    // the Windows host agree within tens of milliseconds. Tightening this back to 100ms would only
    // fail the build that is slower at reading, which is not what the test is for: it is here to
    // catch a node that has no shared clock at all, or one that is out by hours.
#ifdef NDEBUG
    constexpr auto tolerance = 100ms;
#else
    constexpr auto tolerance = 1500ms;
#endif
    EXPECT_GE(tolerance.count(), timediff.count())
        << "the cluster clock is " << timediff.count() << "ms behind this node's";
}