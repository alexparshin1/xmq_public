#include "run_file.h"

#include <algorithm>
#include <chrono>
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
    // The binary lives in LoadTest/runner; the script it describes lives one directory up. An
    // absolute path here is what lets a run file name stand_run.sh from any working directory.
    error_code error;
    const auto self = fs::read_symlink("/proc/self/exe", error);
    if (!error && !self.empty())
    {
        const auto directory = self.parent_path();
        for (auto candidate = directory; !candidate.empty(); candidate = candidate.parent_path())
        {
            if (fs::exists(candidate / "stand_run.sh", error))
            {
                return candidate;
            }
            if (candidate == candidate.parent_path())
            {
                break;
            }
        }
    }
    // A copy run from somewhere else: the caller's current directory, one level up from runner/.
    return fs::current_path();
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

string suggestedVersion()
{
    vector<string> versions;
    error_code     error;
    const auto     directory = loadTestDirectory() / "results" / "Bench";
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
    return (loadTestDirectory() / "results" / "raw" / (stamp() + "-xmq" + (spec.version.empty() ? "unknown" : spec.version) + "-bench")).string();
}

fs::path defaultRunFile(const RunSpec& spec)
{
    return loadTestDirectory() / "runs" / (stamp() + "-xmq" + (spec.version.empty() ? "unknown" : spec.version) + ".run.sh");
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
         << "# XMQ stand run file, written by `stand_runner --edit`. It is a script, not a config\n"
         << "# format: everything below is the invocation stand_run.sh would get from a person, one\n"
         << "# flag per line, so editing a line is the whole edit and running this file is the whole\n"
         << "# run. stand_run.sh still decides what a set is, where the record goes and what its\n"
         << "# version line says.\n"
         << "#\n"
         << "#   ./" << spec.file.filename().string() << "                 run it in a window (or under screen)\n"
         << "#   stand_runner --view <out-dir>   watch it from another window; <out-dir> is\n"
         << "#   " << (spec.out.empty() ? "<out-dir>" : spec.out) << "\n"
         << "#   and add --broker " << spec.server << " for the broker's own CPU and memory.\n"
         << "#\n"
         << "# No `set -e`: stand_run.sh reports its own failures with a message worth reading, and the\n"
         << "# exit status of this file is its exit status.\n"
         << "exec " << quote(script.string()) << " \\\n"
         << "    --version " << quote(spec.version) << " \\\n"
         << "    --set " << quote(setPath.string()) << " \\\n"
         << "    --server " << quote(spec.server) << " \\\n"
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
        else if (argument == "--set")
        {
            spec.setFile = valueAfter(at);
        }
        else if (argument == "--server")
        {
            spec.server = valueAfter(at);
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
