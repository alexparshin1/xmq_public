#include "run_control.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

using namespace std;
namespace fs = std::filesystem;

namespace stand {

namespace {

constexpr auto killAfter = chrono::seconds(5);

string failureDiagnostic(const string& path)
{
    ifstream input(path, ios::binary | ios::ate);
    const auto end = input.tellg();
    if (end <= 0)
    {
        return {};
    }
    constexpr streamoff limit = 16 * 1024;
    const bool truncated = end > limit;
    input.seekg(truncated ? end - limit : streampos(0));
    string line, last, diagnostic;
    if (truncated)
    {
        getline(input, line); // Skip a potentially partial line.
    }
    while (getline(input, line))
    {
        const auto first = line.find_first_not_of(" \t\r");
        if (first == string::npos)
        {
            continue;
        }
        line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
        for (auto& ch: line)
        {
            if (static_cast<unsigned char>(ch) < 32 || ch == 127)
            {
                ch = ' ';
            }
        }
        if (line.starts_with("stand_run: ") && !line.starts_with("stand_run: WARNING:"))
        {
            diagnostic = line;
        }
        last = line;
    }
    return diagnostic.empty() ? last : diagnostic;
}

string quote(const string& value)
{
    string quoted {"'"};
    for (const auto character: value)
    {
        quoted += character == '\'' ? string {"'\\''"} : string {character};
    }
    return quoted + '\'';
}

/// The run directory is created here rather than by stand_run.sh so that the log and the pid file
/// have somewhere to live from the first moment: a start that fails immediately still says why.
bool ensureDirectory(const fs::path& directory, string& problem)
{
    error_code error;
    fs::create_directories(directory, error);
    if (error)
    {
        problem = "cannot create " + directory.string() + ": " + error.message();
        return false;
    }
    return true;
}

/// The pid file records the start time as well: a pid is enough to stop a process, but not enough
/// to say how long it has been running after the screen that started it is gone.
fs::path pidFile(const fs::path& out)
{
    return out / ".stand.pid";
}

// Field 22 of /proc/<pid>/stat is stable across exec, unlike cmdline. Skip comm
// as a unit: the parenthesized executable name may contain spaces and ')'.
string processStart(long pid)
{
    ifstream stat("/proc/" + to_string(pid) + "/stat");
    string line;
    getline(stat, line);
    const auto end = line.rfind(')');
    if (end == string::npos)
    {
        return {};
    }
    istringstream fields(line.substr(end + 1));
    string value;
    if (!(fields >> value) || value == "Z" || value == "X")
    {
        return {};
    }
    for (int index = 4; index <= 22; ++index)
    {
        if (!(fields >> value))
        {
            return {};
        }
    }
    return value;
}

} // namespace

RunControl::RunControl(RunSpec spec)
    : m_spec(std::move(spec))
{
    m_state.record = (fs::path(m_spec.out) / "record.txt").string();
    m_state.log = (fs::path(m_spec.out) / "run.log").string();

    if (m_spec.file.empty() || m_spec.out.empty())
    {
        m_state.phase = RunPhase::NoRunFile;
        return;
    }
    m_state.phase = RunPhase::Idle;
    attachToExisting();
}

void RunControl::attachToExisting()
{
    readPidFile();
}

void RunControl::readPidFile()
{
    ifstream input(pidFile(m_spec.out));
    long     pid = 0;
    long     started = 0;
    if (!(input >> pid))
    {
        return;
    }
    input >> started;

    m_state.pid = pid;
    getline(input, m_state.monitorDir); // Consume the newline after the start time.
    getline(input, m_state.monitorDir);
    getline(input, m_processStart);
    if (!m_state.monitorDir.empty())
    {
        m_state.log = (fs::path(m_state.monitorDir) / "run.log").string();
        m_state.record = (fs::path(m_state.monitorDir) / "record.txt").string();
    }
    if (alive())
    {
        m_state.phase = RunPhase::Running;
        m_state.elapsedSec = max(0L, static_cast<long>(time(nullptr)) - started);
        m_startedAt = chrono::steady_clock::now() - chrono::seconds(m_state.elapsedSec);
        m_state.lastAction = "attached to the run already in progress";
    }
    else
    {
        m_state.phase = RunPhase::Idle;
        m_state.lastAction = "a previous run is no longer running";
    }
}

void RunControl::writePidFile() const
{
    ofstream output(pidFile(m_spec.out), ios::trunc);
    output << m_state.pid << '\n' << time(nullptr) << '\n' << m_state.monitorDir << '\n'
           << m_processStart << '\n';
}

bool RunControl::alive() const
{
    if (m_state.pid <= 0)
    {
        return false;
    }
    if (kill(static_cast<pid_t>(m_state.pid), 0) != 0)
    {
        return false;
    }
    if (!m_processStart.empty())
    {
        return processStart(m_state.pid) == m_processStart;
    }
    // Compatibility for pid files written before the process start time was recorded.
    ifstream cmdline("/proc/" + to_string(m_state.pid) + "/cmdline", ios::binary);
    string   command((istreambuf_iterator<char>(cmdline)), istreambuf_iterator<char>());
    return command.find(m_spec.file.filename().string()) != string::npos;
}

bool RunControl::canStart() const
{
    return m_state.phase == RunPhase::Idle || m_state.phase == RunPhase::Exited;
}

bool RunControl::canStop() const
{
    return m_state.phase == RunPhase::Running;
}

bool RunControl::canFile() const
{
    if (m_state.phase == RunPhase::NoRunFile || m_state.phase == RunPhase::Running ||
        (m_state.phase == RunPhase::Exited && m_state.exitCode != 0 && m_ownedChild))
    {
        return false;
    }
    // This also works after reopening the screen, when waitpid cannot give us the old exit code.
    ifstream record(m_state.record);
    bool complete = false;
    for (string line; getline(record, line) && !line.starts_with("Summary -");)
    {
        if (line == "Campaign status: failed" || line.starts_with("Elapsed so far: "))
        {
            return false;
        }
        complete |= line.starts_with("Total execution time: ");
    }
    return complete;
}

void RunControl::setOverrides(const string& broker, const string& version)
{
    m_overrideBroker = broker;
    m_overrideVersion = version;
}

void RunControl::start()
{
    if (!canStart())
    {
        return;
    }

    m_state.failure.clear();
    string problem;
    if (!ensureDirectory(m_spec.out, problem))
    {
        m_state.lastAction = problem;
        return;
    }

    const auto monitorTemplate = (fs::absolute(m_spec.out) / ".stand-run-XXXXXX").string();
    vector<char> monitorPath(monitorTemplate.begin(), monitorTemplate.end());
    monitorPath.push_back('\0');
    if (mkdtemp(monitorPath.data()) == nullptr)
    {
        m_state.lastAction = string("cannot create temporary log directory: ") + strerror(errno);
        return;
    }
    m_state.monitorDir = monitorPath.data();
    m_state.log = (fs::path(m_state.monitorDir) / "run.log").string();
    m_state.record = (fs::path(m_state.monitorDir) / "record.txt").string();

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, m_state.log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);

    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    // Its own session: a stop then reaches the shell, stand_run.sh, run_scenario_set.sh and xmq_scn
    // together, and the run outlives this screen, which is what lets a set be watched by one
    // terminal and started from another.
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);

    // The run file's own flags come first; these extras come last so stand_run.sh sees them last
    // and they win without the file ever being changed.
    vector<string> values {m_spec.file.string(), "--out", m_state.monitorDir, "--existing-broker",
                           "--results-dir", resultsDirectory().string()};
    if (!m_overrideBroker.empty())
    {
        values.push_back("--broker");
        values.push_back(m_overrideBroker);
    }
    if (!m_overrideVersion.empty())
    {
        values.push_back("--version");
        values.push_back(m_overrideVersion);
    }
    vector<char*> arguments;
    for (auto& value: values)
    {
        arguments.push_back(value.data());
    }
    arguments.push_back(nullptr);

    pid_t      pid = 0;
    const auto spawned = posix_spawnp(&pid, m_spec.file.c_str(), &actions, &attributes,
                                      arguments.data(), environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);

    if (spawned != 0)
    {
        m_state.lastAction = "cannot execute " + m_spec.file.string() + ": " + strerror(spawned);
        return;
    }

    // A fresh run: whatever was there belonged to the run before it.
    m_state.exitCode = 0;
    m_state.pid = pid;
    m_processStart = processStart(pid);
    m_ownedChild = true;
    m_state.phase = RunPhase::Running;
    m_startedAt = chrono::steady_clock::now();
    m_stopping = false;
    writePidFile();
    m_state.lastAction = "started, logging to " + m_state.log;
}

void RunControl::stop()
{
    if (!canStop())
    {
        return;
    }

    // The process group, so the shell, the scripts and xmq_scn all get it; a broker restart is on
    // the far side of ssh and is left to fail its own way rather than half-done.
    if (kill(-static_cast<pid_t>(m_state.pid), SIGTERM) != 0 && kill(static_cast<pid_t>(m_state.pid), SIGTERM) != 0)
    {
        m_state.lastAction = string("cannot stop: ") + strerror(errno);
        return;
    }
    m_stopping = true;
    m_stopDeadline = chrono::steady_clock::now() + killAfter;
    m_state.lastAction = "stopping (SIGTERM)";
}

string RunControl::filingTarget() const
{
    // The record, not the run file, contains the broker and version used for this run.
    ifstream record(m_state.record);
    string line, broker, version;
    const auto value = [](const string& text)
    {
        const auto first = text.find_first_not_of(" \t\r");
        return first == string::npos ? string{} : text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
    };
    while (getline(record, line) && !line.starts_with("Summary -"))
    {
        if (broker.empty() && line.starts_with("Server:"))
        {
            broker = value(line.substr(7));
        }
        if (version.empty() && line.starts_with("Version:"))
        {
            version = value(line.substr(8));
        }
        if (!broker.empty() && !version.empty())
        {
            break;
        }
    }
    // Match the directory naming in file_results.py.
    broker = value(broker.substr(0, broker.find('(')));
    for (auto& ch: broker)
    {
        if (ch == ' ') ch = '-';
    }
    return broker.empty() || version.empty() ? string{} : "Bench/" + broker + "/" + version;
}

void RunControl::file()
{
    if (!canFile())
    {
        return;
    }

    const auto script = loadTestDirectory() / "file_results.py";
    const auto command = quote(script.string()) + " --env Bench --results-dir " +
                         quote(resultsDirectory().string()) + " " + quote(m_state.record) + " 2>&1";

    vector<string> output;
    if (FILE* pipe = popen(command.c_str(), "r"); pipe != nullptr)
    {
        char buffer[512];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
        {
            string line(buffer);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            {
                line.pop_back();
            }
            if (!line.empty())
            {
                output.push_back(line);
            }
        }
        pclose(pipe);
    }

    if (output.empty())
    {
        m_state.lastAction = "file_results.py printed nothing";
        return;
    }
    // The last line is what it concluded; anything before it is the detail of what was written.
    m_state.lastAction = output.back();
}

void RunControl::poll()
{
    if (m_state.phase != RunPhase::Running)
    {
        return;
    }

    int        status = 0;
    const auto result = waitpid(static_cast<pid_t>(m_state.pid), &status, WNOHANG);
    if (result == static_cast<pid_t>(m_state.pid))
    {
        m_state.phase = RunPhase::Exited;
        m_state.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (m_state.exitCode == 0)
        {
            m_state.lastAction = "run finished; f files the result";
        }
        else if (m_stopping && WIFSIGNALED(status))
        {
            m_state.lastAction = "stopped";
        }
        else
        {
            m_state.lastAction = WIFSIGNALED(status) ? "run terminated by signal " + to_string(WTERMSIG(status))
                                                     : "run failed with status " + to_string(m_state.exitCode);
            m_state.failure = failureDiagnostic(m_state.log);
            if (m_state.failure.empty())
            {
                m_state.failure = "The run produced no readable diagnostic.";
            }
        }
        return;
    }

    if (m_stopping)
    {
        if (!alive())
        {
            m_state.phase = RunPhase::Exited;
            m_state.exitCode = -1;
            m_stopping = false;
            m_state.lastAction = "stopped";
            return;
        }
        if (chrono::steady_clock::now() > m_stopDeadline)
        {
            kill(-static_cast<pid_t>(m_state.pid), SIGKILL);
            m_state.lastAction = "stopping (SIGKILL)";
        }
        return;
    }

    m_state.elapsedSec = chrono::duration_cast<chrono::seconds>(chrono::steady_clock::now() - m_startedAt).count();

    // Not our child any more - the screen was restarted while the run went on - so the pid is all
    // there is to go by, and it says only whether the run is still there.
    if (result < 0 && !alive())
    {
        m_state.phase = RunPhase::Exited;
        m_state.exitCode = -1;
        m_state.lastAction = "run ended (exit status unknown: not started by this screen)";
    }
}

} // namespace stand
