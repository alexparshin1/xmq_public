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

#include "../server/Subscription/Subscriptions.h"
#include "MessageReaderTests.h"
#include "TestOptions.h"
#include "TestServers.h"
#include "base/Topic.h"
#include "common/DirectoryNames.h"
#include <gtest/gtest.h>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>

using namespace std;
using namespace xmq;

// Empties the test Redis when each test ends, whatever the outcome.
class RedisCleanup
    : public testing::EmptyTestEventListener
{
public:
    void OnTestEnd(const testing::TestInfo&) override
    {
        TestServers::flushRedis();
    }
};

void linkTests()
{
    const shared_ptr<Storage> storage;
    Settings                  settings;

    auto topicManager = make_shared<TopicManager>();
}

int main(int argc, char* argv[])
{
    // An exception that escapes a thread or a destructor ends the process through std::terminate,
    // and all that is left of it is an exit code - on Windows 0xC0000409, the same as for a
    // stack overrun. Said here, with the test it happened in, before the process goes.
    set_terminate(
        []
        {
            cerr << "\nstd::terminate";
            if (const auto* test = testing::UnitTest::GetInstance()->current_test_info())
            {
                cerr << " in " << test->test_suite_name() << "." << test->name();
            }
            if (const auto exception = current_exception())
            {
                try
                {
                    rethrow_exception(exception);
                }
                catch (const std::exception& e)
                {
                    cerr << ": " << e.what();
                }
                catch (...)
                {
                    cerr << ": an exception of an unknown type";
                }
            }
            cerr << endl;
            abort();
        });

    if (const set<string> args(argv + 1, argv + argc);
        args.contains("--debug"))
    {
        TestOptions::m_debug = true;
    }

    linkTests();

    // Certificates go to a directory of this run's own, for the whole process. Servers issue a
    // pair for themselves when the configuration names none, and the directory they would issue
    // it into on a developer's machine belongs to that machine's own installation - which is not
    // somewhere a test suite gets to write.
    const auto certificates = std::filesystem::temp_directory_path() /
                              ("xmq_test_certs_" + std::to_string(::getpid()));
    std::error_code errorCode;
    std::filesystem::remove_all(certificates, errorCode);
    std::filesystem::create_directories(certificates, errorCode);
    DirectoryNames::setCertsDirectory(certificates);

    // The web interface is served out of this build's own React output rather than out of an
    // installation. The tests that ask it for a page are then testing the pages in this tree, and
    // they no longer need XMQ to have been installed on the machine first.
    DirectoryNames::setWebfaceDirectory(XMQ_REACT_BUILD_DIRECTORY);

    testing::InitGoogleTest(&argc, &argv[0]);

    // Empty Redis after every test.
    //
    // Sessions, their queued messages and the per-node session sets are written to outlive the
    // server that made them - that is the point of persistence - so they also outlive the test
    // that made them. A later test then finds a client id, a node name or a subscription it never
    // created, and fails or passes depending on what ran before it. Shuffling the order made that
    // visible; leaving the database clean is what stops it.
    testing::UnitTest::GetInstance()->listeners().Append(new RedisCleanup);

    const auto result = RUN_ALL_TESTS();

    DirectoryNames::setCertsDirectory({});
    DirectoryNames::setWebfaceDirectory({});
    std::filesystem::remove_all(certificates, errorCode);

    return result;
}
