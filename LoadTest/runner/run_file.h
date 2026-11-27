/**
 * The settings of one bench run, and the file that carries them.
 *
 * A run file is a small shell script: it execs stand_run.sh with one flag per line. That choice is
 * what makes the form worth having - the thing it writes is not a private format that only the
 * runner understands, it is the same invocation a person would type, so it can be read, edited,
 * diffed and run directly, and stand_run.sh stays the only thing that decides what a run is.
 */
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace stand {

struct RunSpec
{
    std::string version;              ///< XMQ version under test, e.g. 1.1.2.
    std::string setFile {"Scenario-Set-Bench.txt"};
    std::string server {"thinker11"};
    std::string brokerUser {"user"};
    std::string brokerPassword {"Secret#123"};
    std::string nic {"enp86s0"};
    std::string subnet {"10.1.1"};
    std::string firstOctet {"150"};
    std::string count {"29"};
    std::string benchDir;             ///< Optional; stand_run.sh derives bench-<version> when empty.
    std::string out;                  ///< Where the run writes its per-scenario logs.
    std::string note;                 ///< Free-text line for the record.
    bool        fileResults {false};
    bool        keepOtherBrokers {false};
    bool        skipPreflight {false};

    /// Where this run file lives. Not part of the script's own content, only of its header.
    std::filesystem::path file;
};

/// The directory holding stand_run.sh and the scenario sets: constructed once by the caller.
std::filesystem::path loadTestDirectory();

/// Shared run library, falling back to the checkout until the shared directory is provisioned.
std::filesystem::path runDirectory();

/// Scenario set files that exist next to stand_run.sh, sorted, for the form's chooser.
std::vector<std::string> availableScenarioSets();

/// Published result directories in results/Bench, newest first, for the comparison chooser.
std::vector<std::string> availableResultVersions();

/// Newest XMQ version that has published bench results, as the form's starting value.
std::string suggestedVersion();

/// results/raw/<stamp>-xmq<version>-bench, the same name stand_run.sh would derive on its own.
std::string defaultOutDirectory(const RunSpec& spec);

/// A path as the screen should show it: relative to the LoadTest directory when it is under it,
/// because a run file's home is runs/ there and the full path is noise in a field this narrow.
std::string displayPath(const std::filesystem::path& path);

/// Resolves a path typed in the form: a relative one is taken as relative to the LoadTest
/// directory, which is where run files live, not to whatever directory the program was started in.
std::filesystem::path resolveRunPath(const std::filesystem::path& path);

/// <stamp>-xmq<version>.run.sh under runDirectory().
std::filesystem::path defaultRunFile(const RunSpec& spec);

/// The whole script, ready to write and to run.
std::string toScript(const RunSpec& spec);

/// Reads a run file this program wrote, or one written by hand in the same shape. Unknown flags
/// are ignored; a missing flag keeps the default. Returns false if the file cannot be read.
bool fromScript(const std::filesystem::path& file, RunSpec& spec);

/// Why this cannot be run, in words meant for the person at the form. Empty when it can.
std::vector<std::string> validate(const RunSpec& spec);

} // namespace stand
