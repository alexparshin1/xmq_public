#include "run_control.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

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
    if (alive())
    {
        m_state.phase = RunPhase::Running;
        m_startedAt = chrono::steady_clock::now() - chrono::seconds(max(0L, static_cast<long>(time(nullptr)) - started));
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
    output << m_state.pid << '\n' << time(nullptr) << '\n';
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
    // A pid can be reused; the command line is what says this pid is still this run.
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
    error_code error;
    return m_state.phase != RunPhase::Running && fs::is_regular_file(m_state.record, error);
}

void RunControl::start()
{
    if (!canStart())
    {
        return;
    }

    string problem;
    if (!ensureDirectory(m_spec.out, problem))
    {
        m_state.lastAction = problem;
        return;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, m_state.log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, m_state.log.c_str(), O_WRONLY | O_APPEND, 0644);

    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    // Its own session: a stop then reaches the shell, stand_run.sh, run_scenario_set.sh and xmq_scn
    // together, and the run outlives this screen, which is what lets a set be watched by one
    // terminal and started from another.
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);

    array<char*, 2> arguments {const_cast<char*>(m_spec.file.c_str()), nullptr};
    pid_t           pid = 0;
    const auto      spawned = posix_spawnp(&pid, m_spec.file.c_str(), &actions, &attributes,
                                           arguments.data(), environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);

    if (spawned != 0)
    {
        m_state.lastAction = string("cannot start: ") + strerror(spawned);
        return;
    }

    // A fresh run: whatever was there belonged to the run before it.
    m_state.exitCode = 0;
    m_state.pid = pid;
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

void RunControl::file()
{
    if (!canFile())
    {
        return;
    }

    const auto script = loadTestDirectory() / "file_results.py";
    const auto command = quote(script.string()) + " --env Bench " + quote(m_state.record) + " 2>&1";

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
        m_state.lastAction = m_state.exitCode == 0 ? "run finished; f files the result"
                                                   : "run failed with status " + to_string(m_state.exitCode);
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
