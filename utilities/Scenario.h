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

#pragma once

#include "Utility.h"
#include <filesystem>
#include <map>
#include <sptk5/cutils>
#include <vector>

namespace xmq {

class CTestScenario;

/**
 * @brief Scenario testing utility.
 */
class Scenario final : public Utility
{
public:
    /**
     * @brief Constructor.
     * @param args Command line arguments.
     */
    explicit Scenario(const std::vector<std::string>& args);

    /**
     * @brief Run the application (execute the scenario).
     * @return Exit code.
     */
    int run() override;

    /**
     * @brief Execute scenario.
     * Use the scenario file provided in the command line, resolved with resolveScenarioFile().
     * The command line parameters override same parameters defined in the scenario file.
     */
    void executeScenario() const;

    /**
     * @brief Override scenario parameters with the ones provided in the command line.
     * Exposed (rather than kept private) so it can be exercised directly by unit tests,
     * without requiring a live scenario file or broker connection.
     * @param scenario Loaded scenario.
     */
    void overrideScenarioParameters(CTestScenario& scenario) const;

    /**
     * @brief Resolve a --bind-to-interfaces definition into a list of local interface IP addresses.
     * Accepts either a comma-separated IP address list (e.g. 10.1.1.24,10.1.1.100) or a
     * mask selecting matching local interfaces (e.g. 10.1.1.1/8).
     * @param definition IP address list or interface mask.
     * @return Resolved, de-duplicated list of local interface IP addresses.
     */
    static sptk::Strings resolveBindInterfaces(const sptk::String& definition);

    /**
     * @brief Resolve a --scenario file path.
     * An absolute path is used as is. A relative path is first looked up against the
     * current working directory; if not found there, it is looked up against the installed
     * scenario directory (${CMAKE_INSTALL_PREFIX}/share/xmq), located relative to this
     * utility's executable.
     * @param scenarioFile Scenario file path, as provided on the command line.
     * @param executableDirectory Directory containing this utility's executable.
     * @return Resolved scenario file path.
     */
    static std::filesystem::path resolveScenarioFile(const std::filesystem::path& scenarioFile, const std::filesystem::path& executableDirectory);

    /**
     * @brief List available scenario files, grouped by their containing sub-directory.
     * Enumerates the installed scenario directory (${CMAKE_INSTALL_PREFIX}/share/xmq), located
     * relative to the given executable directory. Each immediate sub-directory of it is a group;
     * every '*.json' file directly inside a group is listed as a scenario, using the path
     * (relative to the installed scenario directory) that can be passed to --scenario.
     * @param executableDirectory Directory containing this utility's executable.
     * @return Scenario file paths, grouped by sub-directory name, sorted by group and file name.
     * Empty when the installed scenario directory does not exist.
     */
    static std::map<std::string, std::vector<std::string>, std::less<>> listScenarios(const std::filesystem::path& executableDirectory);

    /**
     * @brief Print the scenario files returned by listScenarios(), grouped by sub-directory.
     */
    void printScenarioList() const;

private:
    std::filesystem::path m_executableDirectory;
};

} // namespace xmq
