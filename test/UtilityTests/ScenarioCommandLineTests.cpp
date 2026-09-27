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

#include "utilities/ScenarioCommandLine.h"
#include <gtest/gtest.h>

using namespace std;
using namespace xmq;

/**
 * Options that carry a non-empty default (host, port, ...) must not report as
 * "specified" unless the user actually typed them on the command line: sptk::CommandLine
 * stores parameter defaults in the same map it stores parsed values in, so hasOption()
 * alone can't tell the two apart. Scenario::overrideScenarioParameters() relies on
 * optionSpecified() to avoid clobbering scenario-file values with unrelated defaults.
 */
TEST(ScenarioCommandLineTests, DefaultsAreNotReportedAsSpecified)
{
    const ScenarioCommandLine commandLine({"xmq_scn", "--disable-clean-session"});

    EXPECT_TRUE(commandLine.getError().empty());
    EXPECT_FALSE(commandLine.optionSpecified("host"));
    EXPECT_FALSE(commandLine.optionSpecified("port"));
    EXPECT_FALSE(commandLine.optionSpecified("username"));
    EXPECT_FALSE(commandLine.optionSpecified("password"));
    EXPECT_FALSE(commandLine.optionSpecified("payload-size"));
    EXPECT_FALSE(commandLine.optionSpecified("publish-rate"));
    EXPECT_FALSE(commandLine.optionSpecified("publish-count"));
    EXPECT_FALSE(commandLine.optionSpecified("duration"));
    EXPECT_FALSE(commandLine.optionSpecified("connection-rate"));
    EXPECT_FALSE(commandLine.optionSpecified("qos"));
    EXPECT_FALSE(commandLine.optionSpecified("protocol-version"));
    EXPECT_FALSE(commandLine.optionSpecified("id-prefix"));

    // hasOption() still sees the default value: it's present in the parsed value map.
    EXPECT_TRUE(commandLine.hasOption("host"));
    EXPECT_EQ(commandLine.getOptionValue("host"), "localhost");
    EXPECT_EQ(commandLine.getOptionValue("protocol-version"), "3");
}

TEST(ScenarioCommandLineTests, ExplicitLongOptionIsReportedAsSpecified)
{
    const ScenarioCommandLine commandLine({"xmq_scn", "--publish-count", "500"});

    EXPECT_TRUE(commandLine.optionSpecified("publish-count"));
    EXPECT_EQ(commandLine.getOptionValue("publish-count"), "500");
}

TEST(ScenarioCommandLineTests, ExplicitShortOptionIsReportedAsSpecified)
{
    const ScenarioCommandLine commandLine({"xmq_scn", "-C", "500"});

    EXPECT_TRUE(commandLine.optionSpecified("publish-count"));
    EXPECT_EQ(commandLine.getOptionValue("publish-count"), "500");
}

/**
 * publish-count (-C) and disable-clean-session (-c) are distinct, case-sensitive short
 * options. Regression test for a short-option collision that existed in an earlier draft.
 */
TEST(ScenarioCommandLineTests, PublishCountAndDisableCleanSessionShortOptionsDoNotCollide)
{
    const ScenarioCommandLine commandLine({"xmq_scn", "-c"});

    EXPECT_TRUE(commandLine.hasOption("disable-clean-session"));
    EXPECT_FALSE(commandLine.optionSpecified("publish-count"));
    EXPECT_EQ(commandLine.getOptionValue("publish-count"), "0");
}

TEST(ScenarioCommandLineTests, AllOverridableShortOptionsAreRecognized)
{
    const ScenarioCommandLine commandLine({"xmq_scn",
                                           "-h", "10.0.0.5",
                                           "-p", "1885",
                                           "-u", "bob",
                                           "-P", "secret",
                                           "-s", "some.json",
                                           "-m", "64",
                                           "-r", "500",
                                           "-C", "1000",
                                           "-d", "30",
                                           "-R", "200",
                                           "-q", "1",
                                           "-V", "4"});

    EXPECT_TRUE(commandLine.getError().empty());
    for (const auto* name: {"host", "port", "username", "password", "scenario",
                            "payload-size", "publish-rate", "publish-count",
                            "duration", "connection-rate", "qos", "protocol-version"})
    {
        EXPECT_TRUE(commandLine.optionSpecified(name)) << "Option not recognized: " << name;
    }
}

TEST(ScenarioCommandLineTests, InvalidQosValueIsRejected)
{
    const ScenarioCommandLine commandLine({"xmq_scn", "--qos", "5"});

    EXPECT_FALSE(commandLine.getError().empty());
}

TEST(ScenarioCommandLineTests, InvalidProtocolVersionValueIsRejected)
{
    const ScenarioCommandLine commandLine({"xmq_scn", "--protocol-version", "6"});

    EXPECT_FALSE(commandLine.getError().empty());
}

TEST(ScenarioCommandLineTests, ExplicitProtocolVersionShortOptionIsReportedAsSpecified)
{
    const ScenarioCommandLine commandLine({"xmq_scn", "-V", "5"});

    EXPECT_TRUE(commandLine.optionSpecified("protocol-version"));
    EXPECT_EQ(commandLine.getOptionValue("protocol-version"), "5");
}

TEST(ScenarioCommandLineTests, BindToInterfacesAcceptsListAndMaskSyntax)
{
    const ScenarioCommandLine listForm({"xmq_scn", "--bind-to-interfaces", "10.1.1.24,10.1.1.100"});
    EXPECT_TRUE(listForm.getError().empty());
    EXPECT_TRUE(listForm.optionSpecified("bind-to-interfaces"));

    const ScenarioCommandLine maskForm({"xmq_scn", "--bind-to-interfaces", "10.1.1.1/8"});
    EXPECT_TRUE(maskForm.getError().empty());
    EXPECT_EQ(maskForm.getOptionValue("bind-to-interfaces"), "10.1.1.1/8");
}

TEST(ScenarioCommandLineTests, BindToInterfacesRejectsNonNumericSyntax)
{
    // Interface names (eth0, ...) aren't supported: only IP lists or CIDR masks are.
    const ScenarioCommandLine commandLine({"xmq_scn", "--bind-to-interfaces", "eth0"});

    EXPECT_FALSE(commandLine.getError().empty());
}

TEST(ScenarioCommandLineTests, IdPrefixLongOptionIsReportedAsSpecified)
{
    const ScenarioCommandLine commandLine({"xmq_scn", "--id-prefix", "host1-"});

    EXPECT_TRUE(commandLine.getError().empty());
    EXPECT_TRUE(commandLine.optionSpecified("id-prefix"));
    EXPECT_EQ(commandLine.getOptionValue("id-prefix"), "host1-");
}

TEST(ScenarioCommandLineTests, ListScenariosOptionIsRecognized)
{
    const ScenarioCommandLine commandLine({"xmq_scn", "--list-scenarios"});

    EXPECT_TRUE(commandLine.getError().empty());
    EXPECT_TRUE(commandLine.hasOption("list-scenarios"));
}


#ifdef __linux__
TEST(ScenarioCommandLineTests, CpuAffinityIsOptIn)
{
    const ScenarioCommandLine defaultCommandLine({"xmq_scn"});
    EXPECT_FALSE(defaultCommandLine.hasOption("use-cpu-affinity"));

    const ScenarioCommandLine pinnedCommandLine({"xmq_scn", "--use-cpu-affinity"});
    EXPECT_TRUE(pinnedCommandLine.getError().empty());
    EXPECT_TRUE(pinnedCommandLine.hasOption("use-cpu-affinity"));

    const ScenarioCommandLine removedOption({"xmq_scn", "--no-cpu-affinity"});
    EXPECT_FALSE(removedOption.getError().empty());
}
#endif
