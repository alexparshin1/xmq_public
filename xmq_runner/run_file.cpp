#include "run_file.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

using namespace std;
namespace fs = std::filesystem;

namespace stand {

namespace {

/// Quoting for a shell: single quotes around anything that is not trivially safe, with the one
/// character single quotes cannot contain handled the way sh itself spells it.
string quote(const string& value)
{
    if (!value.empty() && value.find_first_of(" \t'\"\\$`;&|<>()*?[]#~=%") == string::npos)
    {
        return value;
    }
    string quoted {"'"};
    for (const auto character: value)
    {
        if (character == '\'')
        {
            quoted += "'\\''";
        }
        else
        {
            quoted += character;
        }
    }
    quoted += '\'';
    return quoted;
}

string unquote(const string& value)
{
    if (value.size() < 2)
    {
        return value;
    }
    if (value.front() == '\'' && value.back() == '\'')
    {
        return value.substr(1, value.size() - 2);
    }
    if (value.front() == '"' && value.back() == '"')
    {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

string stamp()
{
    const auto now = chrono::system_clock::now();
    const auto time = chrono::system_clock::to_time_t(now);
    tm         parts {};
    localtime_r(&time, &parts);
    char buffer[32] {};
    strftime(buffer, sizeof(buffer), "%Y-%m-%d-%H%M", &parts);
    return buffer;
}

/// Splits a shell line the way it would be read for these flags: whitespace between words, single
/// and double quotes grouping, backslash escaping the next character.
vector<string> splitWords(const string& line)
{
    vector<string> words;
    string         word;
    bool           inWord = false;
    char           quoteCharacter = 0;
    for (size_t at = 0; at < line.size(); ++at)
    {
        const auto character = line[at];
        if (quoteCharacter != 0)
        {
            if (character == quoteCharacter)
            {
                quoteCharacter = 0;
            }
            else if (character == '\\' && quoteCharacter == '"' && at + 1 < line.size())
            {
                word += line[++at];
            }
            else
            {
                word += character;
            }
            continue;
        }
        if (character == '\'' || character == '"')
        {
            quoteCharacter = character;
            inWord = true;
            continue;
        }
        if (character == '\\' && at + 1 < line.size())
        {
            word += line[++at];
            inWord = true;
            continue;
        }
        if (character == ' ' || character == '\t')
        {
            if (inWord)
            {
                words.push_back(word);
                word.clear();
                inWord = false;
            }
            continue;
        }
        word += character;
        inWord = true;
    }
    if (inWord)
    {
        words.push_back(word);
    }
    return words;
}

} // namespace

fs::path loadTestDirectory()
{
    // Resolve relative to the executable first: this supports both a checkout and an install
    // staged under a different prefix. System configuration can also be an absolute path.
    error_code error;
    const auto self = fs::read_symlink("/proc/self/exe", error);
    if (!error && !self.empty())
    {
        const auto directory = self.parent_path();
        const auto installed = directory.parent_path() / XMQ_RUNNER_CONF_INSTALL_DESTINATION;
        if (fs::is_regular_file(installed / "stand_run.sh", error))
        {
            return installed;
        }
        for (auto candidate = directory; !candidate.empty(); candidate = candidate.parent_path())
        {
            if (fs::is_regular_file(candidate / "LoadTest" / "stand_run.sh", error))
            {
                return candidate / "LoadTest";
            }
            if (candidate == candidate.parent_path())
            {
                break;
            }
        }
    }
    return XMQ_RUNNER_CONF_DIRECTORY;
}

vector<string> availableScenarioSets()
{
    vector<string> sets;
    error_code     error;
    const auto     directory = loadTestDirectory();
    for (const auto& entry: fs::directory_iterator(directory, error))
    {
        const auto name = entry.path().filename().string();
        if (entry.is_regular_file(error) && name.starts_with("Scenario-Set-") && name.ends_with(".txt"))
        {
            sets.push_back(name);
        }
    }
    sort(sets.begin(), sets.end());
    return sets;
}

vector<string> availableResultVersions()
{
    // Each broker has its own directory under results/Bench; offer "Broker/version" pairs.
    vector<string> versions;
    error_code     error;
    const auto     root = resultsDirectory() / "Bench";
    for (const auto& broker: fs::directory_iterator(root, error))
    {
        error_code brokerError;
        if (!broker.is_directory(brokerError))
        {
            continue;
        }
        for (filesystem::directory_iterator it(broker.path(), brokerError), end;
             !brokerError && it != end; it.increment(brokerError))
        {
            error_code versionError;
            if (it->is_directory(versionError))
            {
                versions.push_back(broker.path().filename().string() + "/" + it->path().filename().string());
            }
        }
    }
    sort(versions.begin(), versions.end(), greater<string>{});
    return versions;
}

string suggestedVersion()
{
    vector<string> versions;
    error_code     error;
    const auto     directory = resultsDirectory() / "Bench" / "XMQ";
    for (const auto& entry: fs::directory_iterator(directory, error))
    {
        const auto name = entry.path().filename().string();
        // Published per-version directories are versions; a non-XMQ broker's directory carries its
        // own name and is not a version this form could run.
        if (entry.is_directory(error) && !name.empty() && name.find_first_not_of("0123456789.") == string::npos)
        {
            versions.push_back(name);
        }
    }
    if (versions.empty())
    {
        return {};
    }

    const auto parts = [](const string& version)
    {
        vector<int> numbers;
        string      number;
        for (const auto character: version + ".")
        {
            if (character == '.')
            {
                numbers.push_back(number.empty() ? 0 : stoi(number));
                number.clear();
            }
            else
            {
                number += character;
            }
        }
        return numbers;
    };
    return *max_element(versions.begin(), versions.end(), [&parts](const string& a, const string& b)
    {
        return parts(a) < parts(b);
    });
}

string defaultOutDirectory(const RunSpec& spec)
{
    return (resultsDirectory() / "raw" / (stamp() + "-xmq" + (spec.version.empty() ? "unknown" : spec.version) + "-bench")).string();
}

string displayPath(const fs::path& path)
{
    if (path.empty())
    {
        return {};
    }
    error_code error;
    const auto absolute = fs::absolute(path, error);
    if (error)
    {
        return path.string();
    }
    const auto base = fs::absolute(loadTestDirectory(), error);
    if (error)
    {
        return path.string();
    }
    const auto relative = absolute.lexically_relative(base);
    if (relative.empty() || relative.native().starts_with(".."))
    {
        return path.string();
    }
    return relative.string();
}

fs::path resolveRunPath(const fs::path& path)
{
    if (path.empty() || path.is_absolute())
    {
        return path;
    }
    return runDirectory() / path;
}

fs::path runDirectory()
{
    const fs::path shared {XMQ_RUNNER_STATE_DIRECTORY};
    error_code error;
    fs::create_directories(shared, error);
    return shared;
}

fs::path resultsDirectory()
{
    return runDirectory() / "results";
}

fs::path defaultRunFile(const RunSpec& spec)
{
    const auto broker = spec.broker.empty() ? string("XMQ") : spec.broker;
    return runDirectory() /
           (stamp() + "-" + broker + (spec.version.empty() ? "unknown" : spec.version) + ".run.sh");
}

string toScript(const RunSpec& spec)
{
    const auto script = loadTestDirectory() / "stand_run.sh";
    // stand_run.sh resolves --set against the current directory, not its own, so the file names it
    // in full: this script has to work when run from anywhere, including from screen's home.
    const auto setPath = fs::path(spec.setFile).is_absolute() ? fs::path(spec.setFile)
                                                              : loadTestDirectory() / spec.setFile;

    ostringstream text;
    text << "#!/bin/bash\n"
         << "# XMQ stand run file, written by `xmq_runner --edit`. It is a script, not a config\n"
         << "# format: everything below is the invocation stand_run.sh would get from a person, one\n"
         << "# flag per line, so editing a line is the whole edit and running this file is the whole\n"
         << "# run. stand_run.sh still decides what a set is, where the record goes and what its\n"
         << "# version line says.\n"
         << "#\n"
         << "#   ./" << spec.file.filename().string() << "                 run it in a window (or under screen)\n"
         << "#   xmq_runner --run <this file>   watch it, and start or stop it\n"
         << "#   from another window; the broker " << spec.server << " comes from the file.\n"
         << "#\n"
         << "# No `set -e`: stand_run.sh reports its own failures with a message worth reading, and the\n"
         << "# exit status of this file is its exit status.\n"
         << "exec " << quote(script.string()) << " \\\n"
         << "    --existing-broker \\\n"
         << "    --results-dir " << quote(resultsDirectory().string()) << " \\\n"
         << "    --broker " << quote(spec.broker.empty() ? "XMQ" : spec.broker) << " \\\n"
         << "    --version " << quote(spec.version) << " \\\n"
          << "    --set " << quote(setPath.string()) << " \\\n"
          << "    --server " << quote(spec.server) << " \\\n"
          << "    --user " << quote(spec.brokerUser) << " \\\n"
          << "    --password " << quote(spec.brokerPassword) << " \\\n"
         << "    --nic " << quote(spec.nic) << " \\\n"
         << "    --subnet " << quote(spec.subnet) << " \\\n"
         << "    --first " << quote(spec.firstOctet) << " \\\n"
         << "    --count " << quote(spec.count);
    if (!spec.benchDir.empty())
    {
        text << " \\\n    --bench-dir " << quote(spec.benchDir);
    }
    if (!spec.out.empty())
    {
        text << " \\\n    --out " << quote(spec.out);
    }
    if (!spec.note.empty())
    {
        text << " \\\n    --note " << quote(spec.note);
    }
    if (spec.fileResults)
    {
        text << " \\\n    --file";
    }
    if (spec.keepOtherBrokers)
    {
        text << " \\\n    --keep-other-brokers";
    }
    if (spec.skipPreflight)
    {
        text << " \\\n    --skip-preflight";
    }
    text << " \\\n    \"$@\"\n";
    return text.str();
}

bool fromScript(const fs::path& file, RunSpec& spec)
{
    // A directory opens as a stream on Linux and reads as an empty file, which would look like a
    // run file with no settings - and then a save would try to write over the directory.
    error_code error;
    if (!fs::is_regular_file(file, error))
    {
        return false;
    }
    ifstream input(file);
    if (!input)
    {
        return false;
    }

    vector<string> arguments;
    string         line;
    while (getline(input, line))
    {
        const auto first = line.find_first_not_of(" \t");
        if (first == string::npos || line[first] == '#')
        {
            continue;
        }
        // A trailing backslash continues the command; the split only needs the words either way.
        for (const auto& word: splitWords(line))
        {
            arguments.push_back(unquote(word));
        }
    }

    const auto valueAfter = [&arguments](const size_t index) -> string
    {
        return index + 1 < arguments.size() ? arguments[index + 1] : string {};
    };

    for (size_t at = 0; at < arguments.size(); ++at)
    {
        const auto& argument = arguments[at];
        if (argument == "--version")
        {
            spec.version = valueAfter(at);
        }
        else if (argument == "--broker")
        {
            spec.broker = valueAfter(at);
        }
        else if (argument == "--set")
        {
            spec.setFile = valueAfter(at);
        }
        else if (argument == "--server")
        {
            spec.server = valueAfter(at);
        }
        else if (argument == "--user")
        {
            spec.brokerUser = valueAfter(at);
        }
        else if (argument == "--password")
        {
            spec.brokerPassword = valueAfter(at);
        }
        else if (argument == "--nic")
        {
            spec.nic = valueAfter(at);
        }
        else if (argument == "--subnet")
        {
            spec.subnet = valueAfter(at);
        }
        else if (argument == "--first")
        {
            spec.firstOctet = valueAfter(at);
        }
        else if (argument == "--count")
        {
            spec.count = valueAfter(at);
        }
        else if (argument == "--bench-dir")
        {
            spec.benchDir = valueAfter(at);
        }
        else if (argument == "--out")
        {
            spec.out = valueAfter(at);
        }
        else if (argument == "--note")
        {
            spec.note = valueAfter(at);
        }
        else if (argument == "--file")
        {
            spec.fileResults = true;
        }
        else if (argument == "--keep-other-brokers")
        {
            spec.keepOtherBrokers = true;
        }
        else if (argument == "--skip-preflight")
        {
            spec.skipPreflight = true;
        }
    }

    spec.file = file;
    return true;
}

vector<string> validate(const RunSpec& spec)
{
    vector<string> problems;

    if (spec.version.empty())
    {
        problems.push_back("--version is required: it goes into the record and names the profile to restart");
    }
    else if (spec.version.find_first_of(" \t/'\"") != string::npos)
    {
        problems.push_back("--version cannot contain spaces or quotes");
    }

    if (spec.setFile.empty())
    {
        problems.push_back("--set is required");
    }
    else
    {
        error_code error;
        const auto setPath = fs::path(spec.setFile).is_absolute() ? fs::path(spec.setFile)
                                                                  : loadTestDirectory() / spec.setFile;
        if (!fs::is_regular_file(setPath, error))
        {
            problems.push_back("scenario set not found: " + setPath.string());
        }
    }

    if (spec.server.empty())
    {
        problems.push_back("--server is required");
    }
    if (spec.count.empty() || spec.count.find_first_not_of("0123456789") != string::npos)
    {
        problems.push_back("--count must be a number");
    }
    if (spec.firstOctet.empty() || spec.firstOctet.find_first_not_of("0123456789") != string::npos)
    {
        problems.push_back("--first must be a number");
    }

    if (spec.file.empty())
    {
        problems.push_back("run file is empty: give it a path, e.g. runs/1.1.2-bench.run.sh");
    }
    else if (error_code error; fs::is_directory(spec.file, error))
    {
        problems.push_back("run file is a directory: " + spec.file.string() + " - give it a file name");
    }
    return problems;
}

} // namespace stand
