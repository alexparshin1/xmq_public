/**
 * xmq_runner - runs a load-test campaign on the home stand and shows it while it runs.
 *
 * Linux, by design: the run it starts is a chain of POSIX shell scripts that restart a broker over
 * ssh, and a second implementation of that chain would be a second answer to what a run is.
 *
 * Reads only what a run already writes: a scenario's own log (MEASUREMENT-START and the
 * INTERVAL lines xmq_scn emits under --output runner), the presence of its .block, and the
 * client host's /proc. Nothing here polls the broker, and nothing here is on the run's path.
 *
 * Given the run file, it also subscribes to the broker's $SYS topics through a child xmq_sub,
 * which is how the broker's own CPU and memory appear next to the client's. Watching a bare run
 * directory contacts nothing outside this machine.
 *
 *   xmq_runner --edit [run-file]                      fill in a run and write it
 *   xmq_runner --view <run-dir>                       live, full screen
 *   xmq_runner --run <run-file>                        watch, start, stop and file
 *   xmq_runner --view <run-dir> --once                one plain-text status, for a script or a log
 */
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "dialog.h"
#include "frame.h"
#include "run_control.h"
#include "theme.h"
#include "run_file.h"
#include "run_form.h"
#include "tui.h"

using namespace std;

using tui::clipTo;
using tui::padded;

namespace {

struct Interval
{
    long   timeMs {0};
    long   count {0};
    double avgUs {0};
    optional<double> p50Us;
    double p95Us {0};
    double p99Us {0};
    long   rate {0};
    int    cpuPct {0};
    long   rssMb {0};
};

struct Scenario
{
    string                name;
    filesystem::path     log;
    bool                  finished {false};
    bool                  started {false};
    bool                  runPhase {false};
    size_t                measurementStarts {0};
    long                  warmupSec {30};    ///< From --warmup in the Executing: line, default 30.
    long                  durationSec {0};
    long                  windowMs {0};      ///< Width of one reported interval, from the log itself.
    vector<Interval>      intervals;
    filesystem::path      block;
    chrono::system_clock::time_point lastWrite;
};

/// A log is small (a few hundred kB with runner output, a few MB without), and reading it whole
/// keeps the parser honest about out-of-order writes: the run appends while this reads.
string readLog(const filesystem::path& path)
{
    ifstream     input(path, ios::binary);
    string       content;
    if (!input)
    {
        return content;
    }
    input.seekg(0, ios::end);
    const auto size = input.tellg();
    input.seekg(0, ios::beg);
    content.resize(static_cast<size_t>(size));
    input.read(content.data(), size);
    return content;
}

/// The one number a watcher needs from the command line: value after "key=".
optional<double> field(const string& line, const string& key)
{
    const auto at = line.find(key + "=");
    if (at == string::npos)
    {
        return nullopt;
    }
    const auto start = at + key.size() + 1;
    const auto end = line.find_first_of(" \r\n", start);
    try
    {
        return stod(line.substr(start, end == string::npos ? string::npos : end - start));
    }
    catch (...)
    {
        return nullopt;
    }
}

void parseScenario(Scenario& scenario)
{
    scenario.intervals.clear();
    scenario.durationSec = 0;
    scenario.started = false;
    scenario.warmupSec = 30;
    const regex warmupPattern {R"(--warmup[ =]+(\d+))"};

    for (const auto& line: [&]
         {
             const auto content = readLog(scenario.log);
             vector<string> lines;
             for (size_t at = 0; at < content.size();)
             {
                 const auto end = content.find('\n', at);
                 const auto stop = end == string::npos ? content.size() : end;
                 lines.emplace_back(content.substr(at, stop - at));
                 at = stop + 1;
             }
             return lines;
         }())
    {
        if (line.starts_with("Executing:"))
        {
            smatch match;
            if (regex_search(line, match, warmupPattern))
            {
                scenario.warmupSec = stol(match[1].str());
            }
            continue;
        }
        if (line.starts_with("MEASUREMENT-START"))
        {
            // A scenario can report a connection-measurement phase before its actual publish run.
            // Each phase has its own MEASUREMENT-START; the screen shows only the latest phase,
            // rather than concatenating connection intervals with the run's intervals.
            scenario.intervals.clear();
            scenario.durationSec = 0;
            scenario.windowMs = 0;
            ++scenario.measurementStarts;
            // Traffic scenarios connect clients, warm up, then measure. With --warmup 0 there is no
            // warm-up phase. The dedicated Connections test has only one phase, which is the test.
            const bool connections = scenario.name.find("Connections") != string::npos;
            const size_t dataPhase = connections || scenario.warmupSec == 0 ? 1 : 3;
            scenario.runPhase = scenario.measurementStarts >= dataPhase || connections;
            scenario.started = true;
            if (const auto seconds = field(line, "duration_seconds"))
            {
                scenario.durationSec = static_cast<long>(*seconds);
            }
            continue;
        }
        if (!line.starts_with("INTERVAL "))
        {
            continue;
        }
        if (!scenario.runPhase)
        {
            continue;
        }
        Interval interval;
        interval.timeMs = static_cast<long>(field(line, "time_ms").value_or(0));
        interval.count = static_cast<long>(field(line, "count").value_or(0));
        interval.avgUs = field(line, "avg_us").value_or(0);
        interval.p50Us = field(line, "p50_us");
        interval.p95Us = field(line, "p95_us").value_or(0);
        interval.p99Us = field(line, "p99_us").value_or(0);
        interval.rate = static_cast<long>(field(line, "rate").value_or(0));
        interval.cpuPct = static_cast<int>(field(line, "cpu_pct").value_or(0));
        interval.rssMb = static_cast<long>(field(line, "rss_mb").value_or(0));
        scenario.intervals.push_back(interval);
    }
    // The window width is not on the line; consecutive starts give it, and the last one is the
    // only way to know how much of the run the final line already accounts for.
    scenario.windowMs = 0;
    if (scenario.intervals.size() > 1)
    {
        scenario.windowMs = scenario.intervals.back().timeMs - scenario.intervals[scenario.intervals.size() - 2].timeMs;
    }
    else if (scenario.durationSec > 0 && !scenario.intervals.empty())
    {
        scenario.windowMs = scenario.durationSec * 1000 / 10;
    }
}

vector<Scenario> scanRunDir(const filesystem::path& logDir, const filesystem::path& blockDir)
{
    vector<Scenario> scenarios;
    error_code directoryError;
    if (!filesystem::is_directory(logDir, directoryError) || directoryError)
    {
        return scenarios;
    }
    const auto addLog = [&scenarios, &blockDir](const filesystem::path& logPath, const string& name)
    {
        if (logPath.extension() != ".log" || name == "run" || name.starts_with("preflight"))
        {
            return;
        }
        Scenario scenario;
        scenario.log = logPath;
        scenario.name = name;
        scenario.block = blockDir / (name + ".block");
        scenario.lastWrite = chrono::clock_cast<chrono::system_clock>(filesystem::last_write_time(logPath));

        // A .block left by an earlier attempt in the same directory would otherwise mark this run
        // as finished before it starts: it counts only if it is not older than the log beside it.
        error_code error;
        if (filesystem::exists(scenario.block, error))
        {
            const auto blockTime = chrono::clock_cast<chrono::system_clock>(filesystem::last_write_time(scenario.block, error));
            scenario.finished = !error && blockTime >= scenario.lastWrite;
        }
        scenarios.push_back(std::move(scenario));
    };

    for (const auto& entry: filesystem::directory_iterator(logDir))
    {
        if (entry.path().filename().string().starts_with(".stand-run-"))
        {
            continue;
        }
        if (entry.is_directory())
        {
            // Managed runs keep each scenario's live log in its own temporary directory.
            for (const auto& file: filesystem::directory_iterator(entry.path()))
            {
                if (file.is_regular_file() && file.path().extension() == ".log")
                {
                    addLog(file.path(), entry.path().filename().string());
                }
            }
        }
        else if (entry.is_regular_file() && entry.path().extension() == ".log")
        {
            // Bare-directory viewing reads the legacy flat layout.
            addLog(entry.path(), entry.path().stem().string());
        }
    }
    sort(scenarios.begin(), scenarios.end(),
         [](const Scenario& a, const Scenario& b) { return a.lastWrite < b.lastWrite; });
    return scenarios;
}

/// This host's CPU share and memory in use, sampled the way any other process sees them.
struct HostLoad
{
    double cpuPct {0};
    long   usedMb {0};
};

HostLoad sampleHostLoad()
{
    static unsigned long long lastBusy = 0;
    static unsigned long long lastTotal = 0;

    HostLoad load;
    if (ifstream stat("/proc/stat"); stat)
    {
        string cpu;
        unsigned long long busy = 0, total = 0;
        stat >> cpu;
        unsigned long long fields[10] {};
        for (auto& value: fields)
        {
            stat >> value;
        }
        // user, nice, system, idle, iowait, irq, softirq, steal
        total = fields[0] + fields[1] + fields[2] + fields[3] + fields[4] + fields[5] + fields[6] + fields[7];
        busy = total - fields[3] - fields[4];
        if (lastTotal && total > lastTotal)
        {
            load.cpuPct = 100.0 * static_cast<double>(busy - lastBusy) / static_cast<double>(total - lastTotal);
        }
        lastBusy = busy;
        lastTotal = total;
    }
    if (ifstream meminfo("/proc/meminfo"); meminfo)
    {
        string key;
        long long value = 0;
        string unit;
        long long totalKb = 0, availableKb = 0;
        while (meminfo >> key >> value >> unit)
        {
            if (key == "MemTotal:")
            {
                totalKb = value;
            }
            else if (key == "MemAvailable:")
            {
                availableKb = value;
            }
        }
        load.usedMb = (totalKb - availableKb) / 1024;
    }
    return load;
}

string humanDuration(long seconds)
{
    if (seconds < 0)
    {
        seconds = 0;
    }
    if (seconds < 60)
    {
        return to_string(seconds) + "s";
    }
    if (seconds < 3600)
    {
        return to_string(seconds / 60) + "m" + to_string(seconds % 60) + "s";
    }
    return to_string(seconds / 3600) + "h" + to_string(seconds % 3600 / 60) + "m";
}

/// What the broker says about itself on $SYS, as of the last line read.
struct BrokerLoad
{
    string state {"off"};   ///< off, waiting, live, exited - what the watcher is doing, in a word.
    int    cpuPct {0};      ///< Broker process, percent of one core; may exceed 100.
    long   rssMb {0};       ///< Broker process, resident memory.
    bool   haveCpu {false};
    bool   haveRss {false};
    int    ageSec {0};      ///< Seconds since the last line, so a silent broker is not shown as live.
};

/**
 * Watches the broker's own CPU and memory through its $SYS topics.
 *
 * The subscription is a child `xmq_sub`, not a linked client: the runner is a watcher of a run and
 * nothing else on the stand, and pulling the MQTT client library into it for two numbers would put
 * a second copy of that library on the machine generating the load. A child process also dies with
 * its parent, so a killed runner leaves no subscriber holding a session open on the broker.
 *
 * Both topics are retained and published once a second, so the first values arrive with the
 * subscription acknowledgement rather than a second later.
 *
 * If the child exits - the broker restarted under it, or the binary is missing - the state stays
 * 'exited' rather than restarting it: a subscriber that keeps failing every second is noise, and
 * one line saying so is enough for someone to look.
 */
class BrokerWatch final
{
public:
    ~BrokerWatch()
    {
        stop();
    }

    /// @param target "host" or "host:port", as given on the command line.
    void start(const string& target)
    {
        m_target = target;
        const auto colon = target.rfind(':');
        const auto host = colon == string::npos ? target : target.substr(0, colon);
        const auto port = colon == string::npos ? string("1883") : target.substr(colon + 1);
        if (host.empty() || port.empty())
        {
            m_load.state = "bad broker in the run file";
            return;
        }

        int fds[2] {};
        if (pipe(fds) != 0)
        {
            m_load.state = "pipe failed";
            return;
        }

        // posix_spawn rather than fork + exec, so a missing xmq_sub is reported here as a failed
        // spawn instead of an exit code from a child that never became anything.
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
        // stdin comes from nowhere: xmq_sub does not read it, and inheriting the terminal would put
        // a second process on the keyboard the runner is polling for 'q'.
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        // stderr goes nowhere: xmq_sub's complaints are not the runner's, and one of them would
        // land in the middle of the screen.
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addclose(&actions, fds[0]);
        posix_spawn_file_actions_addclose(&actions, fds[1]);

        array<string, 12> storage {"xmq_sub", "-h", host, "-p", port, "-u", m_username,
                                   "-P", m_password, "-t", "$SYS/broker/system/#", "-v"};
        array<char*, 13>  arguments {};
        for (size_t i = 0; i < storage.size(); ++i)
        {
            arguments[i] = storage[i].data();
        }

        const auto spawned = posix_spawnp(&m_pid, "xmq_sub", &actions, nullptr, arguments.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (spawned != 0)
        {
            m_pid = -1;
            close(fds[0]);
            close(fds[1]);
            m_load.state = string("xmq_sub: ") + strerror(spawned);
            m_retryAt = chrono::steady_clock::now() + chrono::seconds(2);
            return;
        }

        close(fds[1]);
        m_fd = fds[0];
        // Read on each screen tick without blocking on it: the child may be quiet for a second.
        const auto flags = fcntl(m_fd, F_GETFL, 0);
        fcntl(m_fd, F_SETFL, flags | O_NONBLOCK);
        m_load.state = "waiting";
        m_sinceLine = chrono::steady_clock::now();
    }

    void setCredentials(const string& username, const string& password)
    {
        m_username = username;
        m_password = password;
    }

    void stop()
    {
        if (m_pid > 0)
        {
            kill(m_pid, SIGTERM);
            int status = 0;
            waitpid(m_pid, &status, 0);
            m_pid = -1;
        }
        if (m_fd >= 0)
        {
            close(m_fd);
            m_fd = -1;
        }
    }

    /// Drain whatever the subscriber has printed since the last call.
    void refresh()
    {
        if (m_fd < 0)
        {
            if (!m_target.empty() && chrono::steady_clock::now() >= m_retryAt)
            {
                start(m_target);
            }
            return;
        }

        char  chunk[4096];
        bool  ended = false;
        while (true)
        {
            const auto received = read(m_fd, chunk, sizeof(chunk));
            if (received > 0)
            {
                m_buffer.append(chunk, static_cast<size_t>(received));
                continue;
            }
            if (received == 0)
            {
                ended = true;
            }
            break; // EAGAIN, or the pipe closed.
        }

        for (size_t at = 0; (at = m_buffer.find('\n')) != string::npos;)
        {
            parseLine(m_buffer.substr(0, at));
            m_buffer.erase(0, at + 1);
        }

        if (ended && m_pid > 0)
        {
            int status = 0;
            const auto waited = waitpid(m_pid, &status, WNOHANG);
            const bool childExited = waited == m_pid;
            if (childExited)
            {
                m_pid = -1;
            }
            close(m_fd);
            m_fd = -1;
            if (childExited)
            {
                m_load.state = WIFEXITED(status) ? "xmq_sub exited (" + to_string(WEXITSTATUS(status)) + ")"
                                                 : "xmq_sub stopped";
            }
            else
            {
                m_load.state = "broker connection lost; reconnecting";
                kill(m_pid, SIGTERM);
                waitpid(m_pid, nullptr, 0);
                m_pid = -1;
            }
            m_retryAt = chrono::steady_clock::now() + chrono::seconds(2);
            return;
        }

        if (m_load.haveCpu || m_load.haveRss)
        {
            const auto now = chrono::steady_clock::now();
            m_load.ageSec = static_cast<int>(chrono::duration_cast<chrono::seconds>(now - m_sinceLine).count());
            m_load.state = "live";
        }
    }

    [[nodiscard]] const BrokerLoad& load() const
    {
        return m_load;
    }

private:
    /// One `xmq_sub -v` line looks like
    ///   Publish id=0 sender=test-client qos=0, payload=1 bytes, topic='$SYS/.../cpu/percent' [128]
    /// The payload is the text between the last "[" and "]"; anything else on the line is ignored,
    /// which is what keeps the counter header and footer lines harmless.
    void parseLine(const string& line)
    {
        const auto topicAt = line.find("topic='");
        if (topicAt == string::npos)
        {
            return;
        }
        const auto topicEnd = line.find('\'', topicAt + 7);
        if (topicEnd == string::npos)
        {
            return;
        }
        const auto topic = line.substr(topicAt + 7, topicEnd - topicAt - 7);

        const auto valueAt = line.rfind('[');
        if (valueAt == string::npos || valueAt < topicEnd)
        {
            return;
        }
        auto value = line.substr(valueAt + 1);
        if (const auto end = value.rfind(']'); end != string::npos)
        {
            value.erase(end);
        }
        char*        parsed = nullptr;
        const double number = strtod(value.c_str(), &parsed);
        if (parsed == nullptr || parsed == value.c_str())
        {
            return;
        }

        if (topic == "$SYS/broker/system/cpu/percent")
        {
            m_load.cpuPct = static_cast<int>(number);
            m_load.haveCpu = true;
        }
        else if (topic == "$SYS/broker/system/memory/rss_bytes")
        {
            m_load.rssMb = static_cast<long>(number / (1024.0 * 1024.0));
            m_load.haveRss = true;
        }
        else
        {
            return;
        }
        m_sinceLine = chrono::steady_clock::now();
    }

    BrokerLoad m_load;
    string     m_buffer;
    pid_t      m_pid {-1};
    int        m_fd {-1};
    chrono::steady_clock::time_point m_sinceLine {chrono::steady_clock::now()};
    string m_username {"user"};
    string m_password {"Secret#123"};
    string m_target;
    chrono::steady_clock::time_point m_retryAt {};
};

struct Report
{
    vector<Scenario> scenarios;
    HostLoad         host;
    BrokerLoad       broker;
    bool             preflight {false};
    bool             foreign {false}; ///< Logs written by a run this screen did not start.
    size_t           current {0};
    size_t           plannedTests {0};
    long             elapsedSec {0};
    long             etaSec {0};
};

struct ScenarioPlan
{
    size_t count {0};
    long   durationSec {0};
};

ScenarioPlan readScenarioPlan(const filesystem::path& setFile)
{
    ScenarioPlan plan;
    if (setFile.empty())
    {
        return plan;
    }
    ifstream input(setFile);
    string line;
    const regex durationPattern {R"("duration_sec"\s*:\s*(\d+))"};
    while (getline(input, line))
    {
        const auto comment = line.find('#');
        if (comment != string::npos)
        {
            line.erase(comment);
        }
        // Like run_scenario_set.sh, take the first field of each non-comment line.
        istringstream fields(line);
        string scenario;
        if (!(fields >> scenario))
        {
            continue;
        }
        ++plan.count;
        auto scenarioFile = setFile.parent_path() / scenario;
        error_code error;
        if (!filesystem::is_regular_file(scenarioFile, error))
        {
            scenarioFile = stand::loadTestDirectory() / scenario;
        }
        smatch match;
        const auto content = readLog(scenarioFile);
        if (regex_search(content, match, durationPattern))
        {
            plan.durationSec += stol(match[1].str());
        }
    }
    return plan;
}

/// A fresh run is one whose log was written after this screen opened (or after the operator pressed
/// start). Its own intervals are what the table shows; whatever was in the directory before that is
/// a previous run and must not appear as if it had just been measured.
Report buildReport(const filesystem::path& dir,
                   const filesystem::path& blockDir,
                   const chrono::system_clock::time_point since = chrono::system_clock::time_point {},
                   const bool managedScreen = false,
                   const bool ownRunRunning = false,
                   const filesystem::path& scenarioSet = {})
{
    Report report;
    report.host = sampleHostLoad();
    report.scenarios = scanRunDir(dir, blockDir);
    if (managedScreen && !ownRunRunning)
    {
        // A run file is not executed just by opening its viewer. If logs are changing while this
        // screen's RunControl is idle, another process owns that output; do not present its
        // preflight or intervals as this screen's run.
        const auto recent = chrono::system_clock::now() - chrono::minutes(2);
        report.foreign = any_of(report.scenarios.begin(), report.scenarios.end(),
                                [recent](const Scenario& scenario) { return scenario.lastWrite > recent; });
        error_code error;
        const auto runLogTime = filesystem::last_write_time(dir / "run.log", error);
        report.foreign = report.foreign ||
                         (!error && chrono::clock_cast<chrono::system_clock>(runLogTime) > recent);
        report.scenarios.clear();
    }
    // Old logs are not part of a managed run. Clearing their measurements but leaving the entries
    // in the scenario list made the screen navigate through empty pages before reaching this run.
    if (since != chrono::system_clock::time_point {})
    {
        erase_if(report.scenarios, [since](const Scenario& scenario) { return scenario.lastWrite < since; });
    }
    for (auto& scenario: report.scenarios)
    {
        parseScenario(scenario);
    }
    // stand_run.sh writes preflight diagnostics to run.log. They describe setup, not a load-test
    // scenario, so expose the phase as run state rather than adding it to the result table. On a
    // managed run, old scenario logs may still be in the output directory. A scenario log can be
    // created/truncated while the broker is restarting, so preflight lasts until the current
    // scenario actually reports MEASUREMENT-START, not merely until its file is touched.
    report.preflight = false;
    const auto now = chrono::system_clock::now();
    const auto currentLogSince = since == chrono::system_clock::time_point {} ? now - chrono::minutes(2) : since;
    const bool currentScenarioStarted = any_of(report.scenarios.begin(), report.scenarios.end(),
                                                [currentLogSince](const Scenario& scenario)
                                                {
                                                    return scenario.started && scenario.lastWrite >= currentLogSince;
                                                });
    if ((!managedScreen || ownRunRunning) && !currentScenarioStarted)
    {
        const auto log = dir / "run.log";
        const auto content = readLog(log);
        if (managedScreen && ownRunRunning)
        {
            // RunControl truncates run.log as it starts the script. The child can write the
            // preflight banner before start() returns, so an mtime comparison with the timestamp
            // captured by the parent would race; the active run owns this freshly truncated log.
            report.preflight = content.find("== preflight ==") != string::npos;
        }
        else
        {
            error_code error;
            const auto written = filesystem::last_write_time(log, error);
            if (!error && chrono::clock_cast<chrono::system_clock>(written) >= currentLogSince)
            {
                report.preflight = content.find("== preflight ==") != string::npos;
            }
        }
    }
    report.current = report.scenarios.size();
    long remaining = 0;
    for (size_t i = 0; i < report.scenarios.size(); ++i)
    {
        auto& scenario = report.scenarios[i];
        // A scenario is "the one running" when it is the newest log without a .block and it has
        // reported a measurement. Everything newer can only be an artefact, e.g. the empty log a
        // restart leaves behind.
        if (!scenario.finished && scenario.started)
        {
            report.current = i;
            const auto elapsedMs = scenario.intervals.empty() ? 0 : scenario.intervals.back().timeMs + scenario.windowMs;
            report.elapsedSec = scenario.durationSec > 0 ? min(elapsedMs / 1000, scenario.durationSec)
                                                         : elapsedMs / 1000;
            remaining += scenario.durationSec > 0 ? scenario.durationSec - report.elapsedSec : 0;
        }
        else if (i > report.current && scenario.durationSec > 0)
        {
            remaining += scenario.durationSec;
        }
    }
    report.etaSec = remaining;
    if (!scenarioSet.empty())
    {
        long knownDuration = 0;
        for (const auto& scenario: report.scenarios)
        {
            knownDuration += scenario.durationSec;
        }
        const auto plan = readScenarioPlan(scenarioSet);
        report.plannedTests = plan.count;
        report.etaSec += max(0L, plan.durationSec - knownDuration);
    }
    return report;
}

/// The broker line, or why there is none. A stale figure is worse than no figure: a broker that
/// stopped answering at minute three should not keep showing what it said then.
void printBroker(const BrokerLoad& broker)
{
    if (broker.state == "off")
    {
        return;
    }
    if (broker.state == "live" && broker.ageSec <= 5)
    {
        printf("broker: CPU %d%%, RSS %.1fGb   (its own $SYS figures)\n",
               broker.cpuPct, broker.rssMb / 1024.0);
        return;
    }
    if (broker.state == "live")
    {
        printf("broker: last figures %ds ago, CPU %d%%, RSS %.1fGb\n",
               broker.ageSec, broker.cpuPct, broker.rssMb / 1024.0);
        return;
    }
    if (broker.state == "waiting")
    {
        printf("broker: subscribed, no figures yet\n");
        return;
    }
    printf("broker: %s\n", broker.state.c_str());
}

void printOnce(const Report& report)
{
    printf("run: %zu scenario(s)\n", report.scenarios.size());
    for (size_t i = 0; i < report.scenarios.size(); ++i)
    {
        const auto& scenario = report.scenarios[i];
        if (!scenario.started)
        {
            printf("  %-44s %s\n", scenario.name.c_str(),
                   scenario.finished ? "done" : "not started");
            continue;
        }
        const auto& last = scenario.intervals.empty() ? Interval {} : scenario.intervals.back();
        const auto  elapsedSec = scenario.intervals.empty()
                                     ? 0
                                     : (last.timeMs + scenario.windowMs) / 1000;
        printf("  %-44s %-7s %3lds/%-3lds rate=%-7ld avg=%4.0fus p99=%5.0fus cpu=%3d%% rss=%4ldMb\n",
               scenario.name.c_str(), scenario.finished ? "done" : "running",
               min(elapsedSec, scenario.durationSec), scenario.durationSec,
               last.rate, last.avgUs, last.p99Us, last.cpuPct, last.rssMb);
    }
    printf("host: cpu %.0f%%, used %.1fGb   eta %s\n", report.host.cpuPct, report.host.usedMb / 1024.0,
           humanDuration(report.etaSec).c_str());
    printBroker(report.broker);
}


/// A request waiting for its confirmation. Start, stop and filing all change something that cannot
/// be undone from here - a set is long, an interrupted set is not filed, and filing replaces a
/// version's whole directory - so a key on its own only asks.
struct Pending
{
    enum class Kind
    {
        None,
        Start,
        Stop,
        File,
        Exit,
    };

    Kind kind {Kind::None};
    bool yesSelected {true};
};

string promptFor(const Pending& pending, const stand::RunControl& control)
{
    const auto& spec = control.spec();
    switch (pending.kind)
    {
        case Pending::Kind::Start:
            return "Start " + spec.file.filename().string() + "? Takes about 40 minutes.";
        case Pending::Kind::Stop:
            return "Stop the running test set? Partial results will not be filed.";
        case Pending::Kind::File:
        {
            const auto target = control.filingTarget();
            return (target.empty() ? "File results using the broker and version in the record?"
                                   : "File results to " + target + "?") +
                   string(" Existing version data will be replaced.");
        }
        case Pending::Kind::Exit:
            return "Stop the running set and quit? Partial results will not be filed.";
        case Pending::Kind::None:
            break;
    }
    return {};
}

void printOperator(const stand::RunControl* control, const Pending& pending)
{
    if (control == nullptr)
    {
        printf("run:   watching only - no --run given, so nothing can be started from here\n");
        return;
    }

    const auto& state = control->state();
    switch (state.phase)
    {
        case stand::RunPhase::Running:
            printf("run:   running %s, pid %ld\n", humanDuration(state.elapsedSec).c_str(), state.pid);
            break;
        case stand::RunPhase::Exited:
            printf("run:   exited with status %d\n", state.exitCode);
            break;
        case stand::RunPhase::Idle:
            printf("run:   idle\n");
            break;
        case stand::RunPhase::NoRunFile:
            printf("run:   nothing to start\n");
            break;
    }
    if (!state.lastAction.empty())
    {
        printf("       %s\n", state.lastAction.c_str());
    }
    if (!state.failure.empty())
    {
        printf("       %s\n       log: %s\n", state.failure.c_str(), state.log.c_str());
    }

    if (pending.kind != Pending::Kind::None)
    {
        printf("\n\033[1m%s\033[0m\n", promptFor(pending, *control).c_str());
        return;
    }

    printf("\ns start%s   x stop%s   f file result%s\n",
           control->canStart() ? "" : " (unavailable)",
           control->canStop() ? "" : " (unavailable)",
           control->canFile() ? "" : " (unavailable)");
}


string left2(const string& text, const size_t width)
{
    const auto clipped = text.substr(0, width);
    return clipped + string(width - clipped.size(), ' ');
}

string right2(const string& text, const size_t width)
{
    const auto clipped = text.size() > width ? text.substr(text.size() - width) : text;
    return string(width - clipped.size(), ' ') + clipped;
}

/// One line of the framed screen: the text, and the palette entry it is drawn in.
struct ScreenLine
{
    string      text;
    const char* color {theme::dialog};
};

struct TestResult
{
    bool   valid {false};
    long   averageUs {0};   ///< Whole-run mean latency from the Average row.
    long   medianUs {0};    ///< xmq_scn's median of interval latencies.
    long   p99Us {0};
    size_t samples {0};
    long   minUs {0};
    long   maxUs {0};
    vector<long> values;    ///< Interval latencies, in order.
};

TestResult readTestResult(const filesystem::path& path)
{
    TestResult result;
    ifstream input(path);
    if (!input)
    {
        return result;
    }
    string line;
    bool inValues = false;
    while (getline(input, line))
    {
        string rest = line;
        const auto first = rest.find_first_not_of(" \t\r");
        if (first == string::npos)
        {
            continue;
        }
        rest.erase(0, first);
        while (!rest.empty() && (rest.back() == '\r' || rest.back() == ' '))
        {
            rest.pop_back();
        }
        if (rest.find("\xE2\x94\x80") != string::npos)
        {
            inValues = !inValues;
            continue;
        }
        istringstream fields(rest);
        string key;
        fields >> key;
        if (key == "Average")
        {
            string count;
            fields >> count >> key; // key now holds the latency, e.g. 185us.
            if (key.size() > 2 && key.back() == 's' && key[key.size() - 2] == 'u')
            {
                result.averageUs = stol(key.substr(0, key.size() - 2));
                result.valid = true;
            }
        }
        else if (key == "Median")
        {
            fields >> key;
            if (key.size() > 2 && key.back() == 's')
            {
                result.medianUs = stol(key.substr(0, key.size() - 2));
            }
        }
        else if (key == "p99")
        {
            fields >> key;
            if (key.size() > 2)
            {
                result.p99Us = stol(key.substr(0, key.size() - 2));
            }
        }
        else if (inValues && key.find_first_of("0123456789") == 0 && key.size() > 2 &&
                 key.back() == 'm' && key[key.size() - 2] == 's')
        {
            string count, latency;
            fields >> count >> latency;
            if (latency.size() > 2 && latency.back() == 's' && latency[latency.size() - 2] == 'u')
            {
                result.values.push_back(stol(latency.substr(0, latency.size() - 2)));
            }
        }
        else if (key == "interval" && rest.find("median ") != string::npos)
        {
            const auto at = rest.find("range ");
            if (at != string::npos)
            {
                const auto dash = rest.find('-', at);
                if (dash != string::npos)
                {
                    result.minUs = stol(rest.substr(at + 6, dash - at - 6));
                    auto maxText = rest.substr(dash + 1);
                    const auto unit = maxText.find("us");
                    if (unit != string::npos)
                    {
                        result.maxUs = stol(maxText.substr(0, unit));
                    }
                }
            }
            const auto over = rest.find(" over ");
            if (over != string::npos)
            {
                result.samples = static_cast<size_t>(stoul(rest.substr(over + 6)));
            }
        }
    }
    return result;
}

struct ComparisonRow
{
    string name;
    TestResult first;
    TestResult second;
};

string resultPath(const string& version)
{
    // A bare version means XMQ; other brokers are named as Broker/version.
    return version.find('/') == string::npos ? "XMQ/" + version : version;
}

string screenText(string text);

string resultLabel(const string& version)
{
    auto label = resultPath(version);
    label[label.find('/')] = ' ';
    return screenText(label);
}

vector<ComparisonRow> compareVersions(string first, string second, string& problem)
{
    vector<ComparisonRow> rows;
    first = resultPath(first);
    second = resultPath(second);
    const auto root = stand::resultsDirectory() / "Bench";
    set<string> firstTests, secondTests;
    const auto collect = [&root](const string& version, set<string>& tests)
    {
        error_code error;
        for (filesystem::directory_iterator it(root / version, error), end; !error && it != end; it.increment(error))
        {
            if (it->path().extension() == ".txt" && it->is_regular_file(error))
            {
                tests.insert(it->path().stem().string());
            }
        }
    };
    collect(first, firstTests);
    collect(second, secondTests);
    vector<string> common;
    set_intersection(firstTests.begin(), firstTests.end(), secondTests.begin(), secondTests.end(),
                     back_inserter(common));
    for (const auto& name: common)
    {
        auto row = ComparisonRow {name, readTestResult(root / first / (name + ".txt")),
                                  readTestResult(root / second / (name + ".txt"))};
        if (row.first.valid && row.second.valid)
        {
            rows.push_back(std::move(row));
        }
    }
    if (rows.empty())
    {
        problem = "No completed tests in both " + resultLabel(first) + " and " + resultLabel(second);
    }
    return rows;
}

string percentDifference(long first, long second)
{
    if (first == 0)
    {
        return "n/a";
    }
    ostringstream text;
    text << fixed << setprecision(1) << (static_cast<double>(second - first) * 100 / first) << '%';
    return text.str();
}

void drawComparison(tui::Frame& frame, const vector<ComparisonRow>& rows, const string& first,
                    const string& second, size_t chosen, int width, int height)
{
    frame.resize(width, height);
    frame.clear(theme::desktop);
    frame.put(1, 1, theme::status, string(static_cast<size_t>(width), ' '));
    const auto firstLabel = resultLabel(first);
    const auto secondLabel = resultLabel(second);
    frame.put(1, 2, theme::statusKey, "compare " + firstLabel + " -> " + secondLabel);
    if (width < 8 || height < 8)
    {
        frame.flush();
        return;
    }
    const auto left = 1;
    const auto inner = width - 2;
    const auto bottom = height - 1;
    string border;
    for (int i = 0; i < width - 2; ++i)
    {
        border += "═";
    }
    frame.put(2, left, theme::frame, "╔" + border + "╗");
    for (int row = 3; row < bottom; ++row)
    {
        frame.put(row, left, theme::frame, "║");
        frame.put(row, width, theme::frame, "║");
    }
    frame.put(bottom, left, theme::frame, "╚" + border + "╝");
    frame.put(2, 3, theme::frameActive, clipTo(" Comparison ", inner - 2));

    const auto widths = [&]()
    {
        array<int, 6> result {4, 7, 7, 7, 7, 7};
        int spare = max(0, width - 47); // Three frame/margin cells and five column gaps.
        const array desired {tui::columnsOf(firstLabel), tui::columnsOf(secondLabel)};
        while (spare && (result[1] < desired[0] || result[2] < desired[1]))
        {
            for (int i = 0; i < 2 && spare; ++i)
            {
                if (result[i + 1] < desired[i])
                {
                    ++result[i + 1];
                    --spare;
                }
            }
        }
        result[0] += min(spare, 60);
        return result;
    }();
    const auto drawColumns = [&](const int row, const array<string, 6>& cells, const char* color)
    {
        int column = 3;
        for (int i = 0; i < 6 && column < width; ++i)
        {
            frame.put(row, column, color, padded(cells[i], min(widths[i], width - column)));
            column += widths[i] + 1;
        }
    };
    drawColumns(4, {"Test", firstLabel, secondLabel, "D avg", "D med", "D p99"}, theme::header);
    const auto room = static_cast<size_t>(max(0, bottom - 7));
    const auto rowsPerPage = room == 0 ? size_t {1} : room;
    const auto firstRow = min(chosen / rowsPerPage * rowsPerPage,
                              rows.size() > room ? rows.size() - room : size_t {0});
    for (size_t i = 0; i < room && firstRow + i < rows.size(); ++i)
    {
        const auto& row = rows[firstRow + i];
        const auto color = firstRow + i == chosen ? theme::selected : theme::fileList;
        drawColumns(5 + static_cast<int>(i),
                    {row.name, to_string(row.first.averageUs) + "us", to_string(row.second.averageUs) + "us",
                     percentDifference(row.first.averageUs, row.second.averageUs),
                     percentDifference(row.first.medianUs, row.second.medianUs),
                     percentDifference(row.first.p99Us, row.second.p99Us)}, color);
    }
    if (rows.empty())
    {
        frame.put(6, 3, theme::warningDesktop, clipTo("No comparable tests", inner - 4));
    }
    frame.put(height, 1, theme::status, string(static_cast<size_t>(width), ' '));
    tui::putMarked(frame, height, 2, theme::status, theme::statusKey, "~↑/↓~ move   ~F10~ Back");
    frame.flush();
}

void drawAlert(tui::Frame& frame, const int width, const int height, const string& message,
               const bool yesSelected)
{
    if (width < 28 || height < 9)
    {
        frame.put(max(1, height / 2), 1, theme::dialog, clipTo(message, width));
        frame.put(max(1, height / 2 + 1), 1, theme::dialog,
                  clipTo(yesSelected ? "[Yes]   Cancel" : " Yes   [Cancel]", width));
        return;
    }
    const int boxWidth = min(58, width - 3);
    const int textWidth = boxWidth - 6;
    vector<string> lines;
    auto rest = message;
    while (!rest.empty() && static_cast<int>(lines.size()) < height - 8)
    {
        auto line = clipTo(rest, textWidth);
        if (line.size() < rest.size())
        {
            const auto space = line.rfind(' ');
            if (space != string::npos && space != 0)
            {
                line.resize(space);
            }
        }
        rest.erase(0, line.size());
        rest.erase(0, rest.find_first_not_of(' '));
        lines.push_back(std::move(line));
    }
    if (!rest.empty())
    {
        lines.back() = clipTo(lines.back(), textWidth - 1) + "…";
    }
    const int boxHeight = static_cast<int>(lines.size()) + 6;
    tui::Dialog dialog(frame, width, height, boxWidth, boxHeight, " Confirm ", {"~Y~es", "Cancel"});
    for (size_t i = 0; i < lines.size(); ++i)
    {
        dialog.line(static_cast<int>(i), lines[i]);
    }
    dialog.drawButtons(yesSelected ? 0 : 1);
}

/// The client, broker and run lines, as the screen shows them. Separate from the drawing so that
/// --once can print the same words without a frame around them.
/// What the run is doing, as the lines above the table: the client it is loading from, the broker
/// it is loading, and the run itself. Above the table because the table's height changes with the
/// run's own output, and a line that moves is a line that gets misread.
vector<ScreenLine> infoLines(const Report& report, const stand::RunControl* control, const bool stale)
{
    vector<ScreenLine> lines;

    ostringstream client;
    client << "client: CPU " << static_cast<int>(report.host.cpuPct) << "%, used " << fixed << setprecision(1)
           << report.host.usedMb / 1024.0 << "Gb";
    // No estimate until something is running: an eta computed from an abandoned run's last window
    // is a number about nothing.
    if (!stale && report.etaSec > 0)
    {
        client << ", ETA " << humanDuration(report.etaSec);
    }
    lines.push_back({client.str(), theme::desktop});

    switch (report.broker.state == "live" ? (report.broker.ageSec <= 5 ? 0 : 1) : 2)
    {
        case 0:
        {
            ostringstream broker;
            broker << "broker: CPU " << report.broker.cpuPct << "%, RSS " << fixed << setprecision(1)
                   << report.broker.rssMb / 1024.0 << "Gb   (its own $SYS figures)";
            lines.push_back({broker.str(), theme::desktop});
            break;
        }
        case 1:
            lines.push_back({"broker: last figures " + to_string(report.broker.ageSec) + "s ago", theme::warningDesktop});
            break;
        default:
            if (report.broker.state != "off")
            {
                lines.push_back({"broker: " + report.broker.state, theme::desktop});
            }
            break;
    }

    if (control == nullptr)
    {
        lines.push_back({"run: watching only - no --run given, so nothing can be started here", theme::desktop});
        return lines;
    }

    const auto& state = control->state();
    switch (state.phase)
    {
        case stand::RunPhase::Running:
            lines.push_back({"run: running " + humanDuration(state.elapsedSec) + ", pid " + to_string(state.pid), theme::desktop});
            break;
        case stand::RunPhase::Exited:
            lines.push_back({"run: exited with status " + to_string(state.exitCode),
                             state.exitCode == 0 ? theme::desktop : theme::warningDesktop});
            break;
        case stand::RunPhase::Idle:
            lines.push_back({"run: idle", theme::desktop});
            break;
        case stand::RunPhase::NoRunFile:
            lines.push_back({"run: nothing to start", theme::desktop});
            break;
    }
    if (!state.failure.empty())
    {
        lines.push_back({state.failure, theme::warningDesktop});
        lines.push_back({"log: " + state.log, theme::desktop});
    }
    else if (!state.lastAction.empty() && !state.lastAction.starts_with("started, logging to "))
    {
        lines.push_back({state.lastAction, theme::desktop});
    }
    return lines;
}

/// Function-key actions on the status bar: available key names are red, labels black; disabled
/// actions are both grey.
void drawKeysLine(tui::Frame& frame, const int row, const stand::RunControl* control)
{
    int column = 2;
    const auto action = [&frame, row, &column](const string& key, const string& label, const bool available)
    {
        frame.put(row, column, available ? theme::statusKey : theme::statusDim, key);
        column += tui::columnsOf(key);
        frame.put(row, column, available ? theme::status : theme::statusDim, " " + label);
        column += 1 + tui::columnsOf(label);
    };
    action("F2", "File Results", control != nullptr && control->canFile());
    frame.put(row, column, theme::status, "   ");
    column += 3;
    action("F5", "Start Test", control != nullptr && control->canStart());
    frame.put(row, column, theme::status, "   ");
    column += 3;
    action("F8", "Stop Test", control != nullptr && control->canStop());
    frame.put(row, column, theme::status, "   ");
    column += 3;
    action("F10", "Quit", true);
    frame.put(row, column, theme::status, "    ⇄ Test  ⇅ Scroll");
}

/// Which test the table shows: the one somebody chose, or else the one that is running.
size_t shownScenario(const Report& report, const size_t chosen)
{
    if (report.scenarios.empty())
    {
        return 0;
    }
    if (chosen < report.scenarios.size())
    {
        return chosen;
    }
    // Follow the newest measured run, not a later scenario that is still connecting. If the new
    // run has started but has not emitted its first interval, keep the last completed result shown.
    for (size_t index = report.scenarios.size(); index > 0; --index)
    {
        const auto& scenario = report.scenarios[index - 1];
        if (scenario.runPhase && scenario.started && !scenario.finished && !scenario.intervals.empty())
        {
            return index - 1;
        }
    }
    for (size_t index = report.scenarios.size(); index > 0; --index)
    {
        const auto& scenario = report.scenarios[index - 1];
        if (scenario.runPhase && scenario.finished && !scenario.intervals.empty())
        {
            return index - 1;
        }
    }
    // Before the first measured result, show the current test as a status, even if its setup table
    // is intentionally empty.
    for (size_t index = report.scenarios.size(); index > 0; --index)
    {
        if (report.scenarios[index - 1].started && !report.scenarios[index - 1].finished)
        {
            return index - 1;
        }
    }
    // Nothing is running yet (a fresh screen, or every test already finished): the first test is
    // what the run will begin with, so that is what the table shows rather than the last one.
    return 0;
}

/// One reported interval, as a table row. The elapsed column is where the window ended, which is
/// what "how far into the scenario" means to somebody watching it.
string intervalColumns(const array<string, 9>& cells)
{
    constexpr array<size_t, 9> widths {11, 9, 7, 7, 7, 7, 7, 5, 6};
    auto text = " " + left2(cells.front(), widths.front());
    for (size_t i = 1; i < cells.size(); ++i)
    {
        text += " " + right2(cells[i], widths[i]);
    }
    return text;
}

string intervalRow(const Scenario& scenario, const Interval& interval)
{
    const auto latency = [](double us) { return to_string(static_cast<long>(us)) + "us"; };
    return intervalColumns({
        to_string((interval.timeMs + scenario.windowMs) / 1000) + "s/" + to_string(scenario.durationSec) + "s",
        to_string(interval.count), to_string(interval.rate), latency(interval.avgUs),
        interval.p50Us ? latency(*interval.p50Us) : "-", latency(interval.p95Us), latency(interval.p99Us),
        to_string(interval.cpuPct) + "%", to_string(interval.rssMb) + "Mb"});
}

string testSummary(const Scenario& scenario, const int width)
{
    if (scenario.intervals.empty())
    {
        return " Summary: waiting for measured intervals";
    }
    vector<double> averages, p50s, p95s, p99s;
    long totalCount = 0;
    long totalRate = 0;
    long totalCpu = 0;
    long peakRss = 0;
    for (const auto& interval: scenario.intervals)
    {
        averages.push_back(interval.avgUs);
        if (interval.p50Us)
        {
            p50s.push_back(*interval.p50Us);
        }
        p95s.push_back(interval.p95Us);
        p99s.push_back(interval.p99Us);
        totalCount += interval.count;
        totalRate += interval.rate;
        totalCpu += interval.cpuPct;
        peakRss = max(peakRss, interval.rssMb);
    }
    const auto median = [](vector<double>& values)
    {
        sort(values.begin(), values.end());
        const auto middle = values.size() / 2;
        return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
    };
    const auto average = [](const vector<double>& values)
    {
        double total = 0;
        for (const auto value: values)
        {
            total += value;
        }
        return total / values.size();
    };
    const auto windows = static_cast<long>(scenario.intervals.size());
    const array fields {
        "Avg " + to_string(static_cast<long>(average(averages))) + "us",
        p50s.empty() ? string("Median -") : "Median " + to_string(static_cast<long>(median(p50s))) + "us",
        "p95 " + to_string(static_cast<long>(median(p95s))) + "us",
        "p99 " + to_string(static_cast<long>(median(p99s))) + "us",
        "count " + to_string(totalCount),
        "rate " + to_string(totalRate / windows) + "/s",
        "CPU " + to_string(totalCpu / windows) + "%",
        "peak RSS " + to_string(peakRss) + "Mb"
    };
    string text;
    for (const auto& field: fields)
    {
        if (tui::columnsOf(text) + 1 + tui::columnsOf(field) > width)
        {
            break;
        }
        text += " " + field;
    }
    return text;
}

/// A Turbo Vision window on the blue desktop: double frame with its title in the top line, a
/// shadow, the run's own lines above the table and the keys on a status line at the bottom.
/// The first table row before any interval arrives: which phase xmq_scn is in now.
string phaseRow(const Scenario& scenario)
{
    if (!scenario.started)
    {
        return scenario.finished ? "done" : "waiting";
    }
    const bool connections = scenario.name.find("Connections") != string::npos;
    if (scenario.runPhase && scenario.intervals.empty())
    {
        return "Data Collection Started";
    }
    if (!connections && scenario.warmupSec > 0 && scenario.measurementStarts == 2)
    {
        return "Warming up (" + to_string(scenario.warmupSec) + "s)";
    }
    return "starting";
}

void drawScreen(tui::Frame& frameStorage, const Report& report, const filesystem::path& dir,
                const stand::RunControl* control, const Pending& pending, const int width, const int height,
                const size_t chosen, const long scrollBack)
{
    // A run whose first log has not appeared yet has no scenario to show, but the screen still has
    // a table in it.
    static const Scenario emptyScenario;

    frameStorage.resize(width, height);
    frameStorage.clear(theme::desktop);

    auto* const frame = &frameStorage;
    const auto  put = [frame](const int row, const int column, const char* color, const string& text)
    {
        frame->put(row, column, color, text);
    };

    // Edge to edge, like a maximised Turbo Vision window: the table has as many columns as the
    // terminal can show, and the frame is the screen's own border.
    const auto left = 1;
    const auto inner = max(40, width - 2);
    const auto top = 2;
    // The frame's last line is the line above the status bar: a window that stopped short of it
    // left a strip of desktop with nothing in it.
    // Reserve the last terminal row for status even when the terminal is shorter than the
    // preferred frame. Frame contents may be clipped on a tiny screen; the keys must not vanish.
    const auto bottom = min(height - 1, max(top + 8, height - 1));

    // The strip across the top is this screen's title, not a menu: the run file it is showing, and
    // what that file says the run is. Turbo Vision put a menu bar there; ours has one thing to say
    // and says it, in the same grey bar with the name in red.
    // The run file is named in the frame title below; the top strip says what the run is -
    // its version and the broker it names. A bare-directory viewer still identifies what it watches.
    const bool managingRun = control != nullptr && !control->spec().file.empty();
    put(1, 1, theme::status, string(static_cast<size_t>(width), ' '));
    if (managingRun)
    {
        const auto& spec = control->spec();
        put(1, 2, theme::statusKey, "run " + spec.version + "    " + spec.server);
    }
    else
    {
        put(1, 2, theme::statusKey, "no run file");
        put(1, 14, theme::status, " watching " + dir.string() + " - give --run to start one from here");
    }

    // Output that was in the directory before this screen was opened is not a run in progress: a
    // freshly opened viewer showing yesterday's numbers read as if something were happening. It is
    // said out loud, and the test line says "earlier run" rather than "running".
    const auto now = chrono::system_clock::now();
    auto       latestWrite = chrono::system_clock::time_point {};
    for (const auto& each: report.scenarios)
    {
        latestWrite = max(latestWrite, each.lastWrite);
    }
    const bool runningNow = control != nullptr && control->state().phase == stand::RunPhase::Running;
    const bool stale = !runningNow && latestWrite != chrono::system_clock::time_point {} &&
                       now - latestWrite > chrono::minutes(2);

    auto info = infoLines(report, control, stale);
    if (control && !control->state().failure.empty())
    {
        const int columns = max(1, width - 5);
        vector<ScreenLine> wrapped;
        for (const auto& line: info)
        {
            if (line.text != control->state().failure && !line.text.starts_with("log: "))
            {
                wrapped.push_back(line);
                continue;
            }
            auto rest = line.text;
            for (int part = 0; !rest.empty() && part < 3; ++part)
            {
                auto text = clipTo(rest, columns);
                if (part == 2 && text.size() < rest.size() && columns > 1)
                {
                    text = clipTo(text, columns - 1) + "…";
                    wrapped.push_back({text, line.color});
                    break;
                }
                wrapped.push_back({text, line.color});
                rest.erase(0, text.size());
            }
        }
        info = std::move(wrapped);
    }
    if (stale)
    {
        const auto age = chrono::duration_cast<chrono::seconds>(now - latestWrite).count();
        info.insert(info.begin(), {string("an earlier run in this directory, last written ") +
                                       humanDuration(static_cast<long>(age)) + " ago - s starts a new one",
                                   theme::warningDesktop});
    }
    if (report.foreign)
    {
        info.push_back({"another process is writing here; this screen did not start it",
                        theme::warningDesktop});
    }
    // One line per info line, a blank, the test's own line, then the table's column header.
    const auto headerRow = top + 2 + static_cast<int>(info.size()) + 2;
    const auto tableTop = headerRow + 1;
    // The last row belongs to the scrollbar's lower arrow, so the rows stop above it.
    const auto tableHeight = max(1, bottom - 1 - tableTop);

    // The window is blue, like every Turbo Vision window: the run's own lines are read on the
    // desktop colour. The table is the one part on grey, because it is the part whose rows come
    // and go and a moving list needs a surface of its own.
    for (int row = top + 1; row < bottom; ++row)
    {
        put(row, left + 1, theme::desktop, string(static_cast<size_t>(inner), ' '));
    }
    const auto tableLeft = left + 1;
    const auto scrollbarColumn = left + inner;
    const auto bodyWidth = scrollbarColumn - tableLeft;
    const auto putTable = [&](int row, const char* color, const string& text)
    {
        put(row, tableLeft, color, padded(text, bodyWidth));
    };
    for (int row = tableTop; row < bottom - 1; ++row)
    {
        putTable(row, theme::dialog, {});
    }

    int row = top + 2;
    for (const auto& line: info)
    {
        put(row++, left + 2, line.color, padded(" " + line.text, inner - 2));
    }

    // One test at a time. A full set is five scenarios of several minutes each, so a table of all
    // of them is unreadable by the second one and unusable by the third; the arrows choose, and
    // unless somebody chooses, the table follows the test that is running.
    const size_t shown = shownScenario(report, chosen);

    const auto& scenario = report.scenarios.empty() ? emptyScenario : report.scenarios[shown];

    // Which test is on the table, and how far into it.
    {
        ostringstream text;
        // Preflight is a phase of a run that is going on now: a fresh log alone is not enough when
        // this screen controls a run, because a set that was stopped just after preflight would
        // otherwise keep saying "preflight" while nothing runs.
        if (report.foreign)
        {
            text << " run state: external activity - not started here";
        }
        else if (report.preflight && (control == nullptr || runningNow))
        {
            text << " run state: preflight";
        }
        else
        {
            const auto total = report.plannedTests != 0 ? report.plannedTests : report.scenarios.size();
            text << " test " << (report.scenarios.empty() ? 0 : shown + 1) << '/' << total << "  "
                 << scenario.name << "   ";
            if (stale)
            {
                text << "earlier run";
            }
            else if (!scenario.started)
            {
                text << (scenario.finished ? "done" : "waiting");
            }
            else
            {
                const auto elapsedSec = scenario.intervals.empty() ? 0 : (scenario.intervals.back().timeMs + scenario.windowMs) / 1000;
                text << (scenario.finished ? "done" : "running") << "  " << elapsedSec << "s/" << scenario.durationSec << "s";
            }
        }
        put(headerRow - 1, left + 2, theme::frameActive, padded(text.str(), inner - 2));
    }

    {
        const auto headings = intervalColumns({"elapsed", "count", "rate/s", "avg", "median", "p95", "p99", "cpu", "rss"});
        putTable(headerRow, theme::header, headings);
    }

    // The intervals of that test, newest at the bottom. The default view is the newest ones; Up
    // and PageUp walk back through the ones already measured, and the count in the first row says
    // how many are above what is on screen.
    const auto rowsCount = static_cast<long>(scenario.intervals.size());
    const auto room = static_cast<long>(max(1, tableHeight));
    const auto offset = min(max(0L, scrollBack), max(0L, rowsCount - room));
    const auto last = rowsCount - offset;
    const auto first = max(0L, last - room);

    row = tableTop;
    if (first > 0 && row < bottom - 1)
    {
        putTable(row++, theme::dialog, " ... " + to_string(first) + " earlier interval(s), PageUp");
    }
    if (rowsCount == 0 && row < bottom - 1)
    {
        ostringstream text;
        text << ' ' << left2(phaseRow(scenario), 26);
        putTable(row++, theme::dialog, text.str());
    }
    for (long index = first; index < last && row < bottom - 1; ++index)
    {
        putTable(row++, theme::dialog, intervalRow(scenario, scenario.intervals[static_cast<size_t>(index)]));
    }
    if (offset > 0 && row < bottom - 1)
    {
        putTable(row, theme::dialog, " ... " + to_string(offset) + " newer interval(s), PageDown");
    }
    putTable(bottom - 1, theme::header, testSummary(scenario, bodyWidth));

    // The scrollbar down the right edge of the table, as every Turbo Vision window had one: arrows
    // at the ends, a track, and a thumb whose length is how much of the test is on screen and whose
    // place says how far back it is. It is drawn even when everything fits - a thumb that fills the
    // track is itself the answer to "is there more".
    {
        // Arrows at the ends of the column header and of the last row; the track, and the rows
        // beside it, between them.
        const auto trackTop = headerRow;
        const auto trackBottom = bottom - 1;
        const auto trackRows = max(0, trackBottom - trackTop - 1);
        const auto total = max<int64_t>(1, rowsCount);
        const auto visible = min<int64_t>(room, total);
        const auto thumbSize = max<int64_t>(1, static_cast<int64_t>(trackRows) * visible / total);
        const auto maxOffset = max<int64_t>(0, total - visible);
        const auto position = maxOffset > 0 ? (maxOffset - min<int64_t>(offset, maxOffset)) * (trackRows - thumbSize) / maxOffset : 0;

        put(trackTop, scrollbarColumn, theme::scroll, "▲");
        put(trackBottom, scrollbarColumn, theme::scroll, "▼");
        for (int index = 0; index < trackRows; ++index)
        {
            const bool inThumb = index >= position && index < position + thumbSize;
            put(trackTop + 1 + index, scrollbarColumn, theme::scroll, inThumb ? "█" : "░");
        }
    }

    // The frame, with its title in the top line and a shadow down the right and along the bottom.
    const auto boxWidth = inner + 2;
    string topLine {"╔"};
    string bottomLine {"╚"};
    const string doubleLine {"═"};
    for (int column = 0; column < inner; ++column)
    {
        topLine += doubleLine;
        bottomLine += doubleLine;
    }
    topLine += "╗";
    bottomLine += "╝";
    put(top, left, theme::frame, topLine);
    put(bottom, left, theme::frame, bottomLine);

    for (int side = top + 1; side < bottom; ++side)
    {
        put(side, left, theme::frame, "║");
        put(side, left + boxWidth - 1, theme::frame, "║");
        put(side, left + boxWidth, theme::desktopDim, " ");
    }
    put(bottom + 1, left + 2, theme::desktopDim, string(static_cast<size_t>(inner + 1), ' '));

    put(top, left + 1, theme::frameActive, "[■]");
    put(top, left + inner - 2, theme::frame, "[↕]");
    const auto titlePath = control != nullptr && !control->spec().file.empty()
                               ? stand::displayPath(control->spec().file)
                               : dir.string();
    const auto title = " " + titlePath + " ";
    const auto titleRoom = inner - 10;
    put(top, left + 5 + max(0, (titleRoom - static_cast<int>(title.size())) / 2), theme::frameActive, clipTo(title, titleRoom));

    // One place for keys: always draw it on the last terminal row, regardless of how the frame
    // had to shrink to fit.
    const bool asking = pending.kind != Pending::Kind::None && control != nullptr;
    put(height, 1, theme::status, string(static_cast<size_t>(width), ' '));

    if (asking)
    {
        put(height, 2, theme::status, "←/→ Select   Enter Choose   Esc No");
        drawAlert(frameStorage, width, height, promptFor(pending, *control), pending.yesSelected);
    }
    else
    {
        drawKeysLine(frameStorage, height, control);
    }

    // Only the rows that changed go out, so an idle second sends nothing at all.
    frame->flush();
}


int monitorRun(filesystem::path dir, const string& runFile, bool once, bool runNow = false,
               string* browserError = nullptr, const string* brokerName = nullptr,
               const string* brokerVersion = nullptr);
int browseRuns();
int showComparison(const string& first, const string& second);

} // namespace

int main(int argc, char** argv)
{
    if (argc == 1)
    {
        return browseRuns();
    }

    filesystem::path dir;
    string            runFile;
    bool              view = false;
    bool              once = false;
    bool              runNow = false;
    bool              compare = false;
    string            compareFirst, compareSecond;
    bool              edit = false;
    string            editTarget;
    for (int i = 1; i < argc; ++i)
    {
        const string arg = argv[i];
        if (arg == "--edit")
        {
            edit = true;
            // An existing run file to change, or a path to write; either is optional.
            if (i + 1 < argc && argv[i + 1][0] != '-')
            {
                editTarget = argv[++i];
            }
        }
        else if (arg == "--view")
        {
            view = true; // A bare --view is accepted with --run for older commands.
            if (i + 1 < argc && argv[i + 1][0] != '-')
            {
                dir = argv[++i];
            }
        }
        else if (arg == "--run" || arg == "--run-now")
        {
            runNow = arg == "--run-now";
            if (i + 1 >= argc || argv[i + 1][0] == '-')
            {
                fprintf(stderr, "xmq_runner: %s requires a run file\n", arg.c_str());
                return 2;
            }
            runFile = argv[++i];
        }
        else if (arg == "--compare")
        {
            compare = true;
            if (i + 2 >= argc || argv[i + 1][0] == '-' || argv[i + 2][0] == '-')
            {
                fprintf(stderr, "xmq_runner: --compare needs two versions\n");
                return 2;
            }
            compareFirst = argv[++i];
            compareSecond = argv[++i];
        }
        else if (arg == "--once")
        {
            once = true;
        }
        else if (arg == "--help" || arg == "-h")
        {
            printf("Usage:\n"
                   "  xmq_runner                                         choose a run file\n"
                   "  xmq_runner --edit [run-file]                        fill in a run and write it\n"
                   "  xmq_runner --run <run-file> [--once]               watch, start, stop and file\n"
                   "  xmq_runner --run-now <run-file> [--once]           start immediately, then watch\n"
                   "  xmq_runner --compare <broker1/version1> <broker2/version2>  compare filed results\n"
                   "  xmq_runner --view <run-dir> [--once]               watch only\n"
                   "\n"
                   "  No arguments:          Up/Down selects a file; F3 creates, F4 edits, F5 asks for\n"
                   "                         the broker and version, then runs; F6 compares, F8 deletes,\n"
                   "                         F10 exits.\n"
                   "  --edit [run-file]      A form over the settings a bench run needs. F2 writes the\n"
                   "                         file; it is a shell script that execs stand_run.sh, so it\n"
                   "                         runs as it stands and can be edited by hand afterwards.\n"
                   "                         Given an existing file, its values are loaded first.\n"
                   "  --view <run-dir>       Watch a run's own output: the INTERVAL lines xmq_scn writes\n"
                   "                         under --output runner, plus each scenario's .block.\n"
                   "  --run <run-file>       The run this screen may start, stop and file. It also\n"
                   "                         supplies the output directory and broker, from the --out\n"
                   "                         and --server the file already names.\n"
                   "                         Keys: s start, x stop, f file the result, q quit; each\n"
                   "                         action asks for a y first.\n"
                   "  --run-now <run-file>   Same, but starts the run on opening; no second F5.\n"
                   "  --compare B1/V1 B2/V2  Compare completed tests in results/Bench (bare V means XMQ/V).\n"
                    "  --once                 Print one plain-text status and exit, for a script or a log.\n"
                   );
            return 0;
        }
        else if (arg == "--broker")
        {
            // The flag existed for an afternoon: it named a host the run file also names, and the
            // two could disagree. Whoever typed it deserves the sentence that replaces it.
            fprintf(stderr,
                    "xmq_runner: --broker is gone - the broker comes from the run file.\n"
                    "               Use: xmq_runner --run[-now] <run-file>\n");
            return 2;
        }
        else
        {
            fprintf(stderr, "xmq_runner: unknown argument %s\n", arg.c_str());
            return 2;
        }
    }

    if ((edit && (view || !runFile.empty() || once || runNow)) || (!dir.empty() && !runFile.empty()))
    {
        fprintf(stderr, "xmq_runner: choose one of --edit, --run[-now] <run-file>, or --view <run-dir>\n");
        return 2;
    }

    if (edit)
    {
        stand::RunSpec spec;
        if (!editTarget.empty())
        {
            if (filesystem::exists(editTarget))
            {
                if (!stand::fromScript(editTarget, spec))
                {
                    // A directory is the mistake worth naming: it opens as a stream and reads as an
                    // empty file, which used to reach the form as a run file with no settings.
                    fprintf(stderr, filesystem::is_directory(editTarget)
                                        ? "xmq_runner: %s is a directory - give a run file path, or omit the argument\n"
                                        : "xmq_runner: cannot read %s\n",
                            editTarget.c_str());
                    return 2;
                }
            }
            else
            {
                spec.file = editTarget;
            }
        }
        return stand::editRunFile(spec) == stand::FormAction::Saved ? 0 : 1;
    }

    if (compare)
    {
        return showComparison(compareFirst, compareSecond);
    }

    if (dir.empty() && runFile.empty())
    {
        fprintf(stderr, "xmq_runner: use --run[-now] <run-file>, --view <run-dir>, or --edit\n");
        return 2;
    }

    return monitorRun(dir, runFile, once, runNow);
}

namespace {

int monitorRun(filesystem::path dir, const string& runFile, const bool once, const bool runNow,
               string* browserError, const string* brokerName, const string* brokerVersion)
{
    const auto fail = [browserError](const string& message)
    {
        if (browserError)
        {
            *browserError = message;
        }
        else
        {
            fprintf(stderr, "xmq_runner: %s\n", message.c_str());
        }
        return 2;
    };

    // A run file carries its own output directory and broker, so watching what it will do needs no
    // second set of arguments: the same file that starts the run says where to look.
    optional<stand::RunSpec> runSpec;
    if (!runFile.empty())
    {
        stand::RunSpec spec;
        if (!stand::fromScript(runFile, spec))
        {
            return fail("cannot read " + runFile);
        }
        if (spec.out.empty())
        {
            return fail(runFile + " names no --out, so there is no run directory to watch");
        }
        dir = spec.out;
        runSpec = spec;
    }

    error_code dirError;
    if (!filesystem::is_directory(dir, dirError))
    {
        // Before a run there is nothing there yet, and that is exactly the moment one is started.
        if (runSpec)
        {
            filesystem::create_directories(dir, dirError);
            if (dirError)
            {
                return fail("cannot create " + dir.string() + ": " + dirError.message());
            }
        }
        if (!filesystem::is_directory(dir, dirError))
        {
            // The usual reason: the directory a run writes into does not exist until the run
            // starts, and the way to start one from here is --run, not a path that is not there.
            return fail(dir.string() + " is not an accessible run directory."
                        "\n               To start and watch a run: xmq_runner --run-now <run-file>");
        }
    }

    BrokerWatch watch;
    if (runSpec && !runSpec->server.empty())
    {
        watch.setCredentials(runSpec->brokerUser, runSpec->brokerPassword);
        watch.start(runSpec->server);
    }

    optional<stand::RunControl> control;
    if (runSpec)
    {
        control.emplace(*runSpec);
        if (brokerName && !brokerName->empty())
        {
            runSpec->broker = *brokerName;
        }
        if (brokerVersion && !brokerVersion->empty())
        {
            runSpec->version = *brokerVersion;
        }
        control->setOverrides(brokerName ? *brokerName : string {},
                              brokerVersion ? *brokerVersion : string {});
        if (runNow && control->canStart())
        {
            control->start();
        }
    }

    if (once)
    {
        // Two samples, so the CPU figure is a rate rather than a zero from having no previous one.
        // The broker's figures are retained, so they come with the subscription itself; a second
        // of waiting is what it costs for them to be there rather than a 'waiting' line.
        sampleHostLoad();
        this_thread::sleep_for(chrono::milliseconds(200));
        for (int tick = 0; tick < 10 && watch.load().state == "waiting"; ++tick)
        {
            watch.refresh();
            if (watch.load().state != "waiting")
            {
                break;
            }
            this_thread::sleep_for(chrono::milliseconds(100));
        }
        const auto logDir = control && !control->state().monitorDir.empty()
                                ? filesystem::path(control->state().monitorDir)
                                : dir;
        auto report = buildReport(logDir, logDir, {}, control.has_value(),
                                  control && control->state().phase == stand::RunPhase::Running,
                                  runSpec ? stand::loadTestDirectory() / runSpec->setFile : filesystem::path {});
        report.broker = watch.load();
        printOnce(report);
        if (control)
        {
            printOperator(&*control, Pending {});
        }
        return 0;
    }

    // Raw keys and a restored terminal, the same machinery the form uses: without it every action
    // key would need Enter after it, which is not what a key means on a screen like this.
    tui::Terminal terminal;
    tui::Frame    frame;
    const bool    interactive = tui::Terminal::available();

    sampleHostLoad();
    Pending pending;
    size_t  chosen {static_cast<size_t>(-1)}; // -1: follow the test that is running
    long    scrollBack {0};
    // A managed screen (a run file was given) watches a run, not a results directory: its table
    // starts empty and fills with what this run measures, so a previous run's intervals in the
    // same directory are not shown as if they were current. A bare `--view <dir>` keeps showing
    // what is in the directory - inspecting published results is what it is for.
    bool managedRunStarted = control && control->state().phase == stand::RunPhase::Running;
    bool exitAfterStop = false;
    auto managedRunSince = chrono::system_clock::now();
    if (managedRunStarted)
    {
        managedRunSince -= chrono::seconds(control->state().elapsedSec);
    }
    while (tui::Terminal::terminateFlag() == 0)
    {
        if (control)
        {
            control->poll();
            if (exitAfterStop && control->state().phase != stand::RunPhase::Running)
            {
                break;
            }
            if (control->state().phase == stand::RunPhase::Running && !managedRunStarted)
            {
                managedRunStarted = true;
                managedRunSince = chrono::system_clock::now() - chrono::seconds(control->state().elapsedSec);
            }
        }
        watch.refresh();

        const auto logDir = control && !control->state().monitorDir.empty()
                                ? filesystem::path(control->state().monitorDir)
                                : dir;
        auto report = buildReport(logDir, logDir, managedRunSince, control.has_value(), managedRunStarted,
                                  runSpec ? stand::loadTestDirectory() / runSpec->setFile : filesystem::path {});
        report.broker = watch.load();

        if (interactive)
        {
            // Redraw before reading, then act on the key: the screen shows the state the key was
            // pressed on rather than the one before the last tick.
            drawScreen(frame, report, dir, control ? &*control : nullptr, pending, terminal.width(), terminal.height(),
                       chosen, scrollBack);

            const auto press = terminal.readKey(1000);
            if (tui::Terminal::terminateFlag() != 0)
            {
                break;
            }
            if (terminal.takeResized() || press.key == tui::Key::None)
            {
                continue;
            }
            if (pending.kind != Pending::Kind::None)
            {
                if (press.key == tui::Key::Left || press.key == tui::Key::Right ||
                    press.key == tui::Key::Tab || press.key == tui::Key::BackTab)
                {
                    pending.yesSelected = !pending.yesSelected;
                    continue;
                }
                const bool yes = press.key == tui::Key::Enter
                                     ? pending.yesSelected
                                     : press.key == tui::Key::Character &&
                                           (press.text == "y" || press.text == "Y");
                const bool no = press.key == tui::Key::Escape ||
                                 (press.key == tui::Key::Character &&
                                  (press.text == "n" || press.text == "N" ||
                                   press.text == "c" || press.text == "C")) ||
                                (press.key == tui::Key::Enter && !pending.yesSelected);
                if (yes)
                {
                    if (pending.kind == Pending::Kind::Start && control && control->canStart())
                    {
                        control->start();
                        if (control->state().phase == stand::RunPhase::Running)
                        {
                            managedRunStarted = true;
                            managedRunSince = chrono::system_clock::now();
                        }
                    }
                    else if (pending.kind == Pending::Kind::Stop && control && control->canStop())
                    {
                        control->stop();
                    }
                    else if (pending.kind == Pending::Kind::File && control && control->canFile())
                    {
                        control->file();
                    }
                    else if (pending.kind == Pending::Kind::Exit && control && control->canStop())
                    {
                        control->stop();
                        exitAfterStop = control->state().lastAction == "stopping (SIGTERM)";
                    }
                }
                if (yes || no)
                {
                    pending.kind = Pending::Kind::None;
                }
                continue;
            }
            if (press.key == tui::Key::Escape ||
                (press.key == tui::Key::Character && (press.text == "q" || press.text == "Q")))
            {
                if (control && control->canStop())
                {
                    pending.kind = Pending::Kind::Exit;
                    pending.yesSelected = true;
                    continue;
                }
                break;
            }
            // Arrows belong to the table: which test, and how far back in it.
            switch (press.key)
            {
                case tui::Key::Left:
                case tui::Key::Right:
                {
                    pending.kind = Pending::Kind::None;
                    if (report.scenarios.empty())
                    {
                        break;
                    }
                    const auto current = shownScenario(report, chosen);
                    const auto count = report.scenarios.size();
                    chosen = press.key == tui::Key::Right
                                 ? (current + 1) % count
                                 : (current + count - 1) % count;
                    scrollBack = 0;
                    break;
                }
                case tui::Key::Up:
                    pending.kind = Pending::Kind::None;
                    scrollBack += 1;
                    break;
                case tui::Key::Down:
                    pending.kind = Pending::Kind::None;
                    scrollBack = max(0L, scrollBack - 1);
                    break;
                case tui::Key::PageUp:
                    pending.kind = Pending::Kind::None;
                    scrollBack += 10;
                    break;
                case tui::Key::PageDown:
                    pending.kind = Pending::Kind::None;
                    scrollBack = max(0L, scrollBack - 10);
                    break;
                case tui::Key::Home:
                    pending.kind = Pending::Kind::None;
                    scrollBack = numeric_limits<long>::max() / 2;
                    break;
                case tui::Key::End:
                    pending.kind = Pending::Kind::None;
                    scrollBack = 0;
                    break;
                default:
                    break;
            }
            const auto confirmAction = [&](const Pending::Kind kind)
            {
                if (control.has_value())
                {
                    pending.kind = kind;
                    pending.yesSelected = true;
                }
            };
            if (press.key == tui::Key::F2 && control && control->canFile())
            {
                confirmAction(Pending::Kind::File);
                continue;
            }
            if (press.key == tui::Key::F5 && control && control->canStart())
            {
                confirmAction(Pending::Kind::Start);
                continue;
            }
            if (press.key == tui::Key::F8 && control && control->canStop())
            {
                confirmAction(Pending::Kind::Stop);
                continue;
            }
            if (press.key == tui::Key::F10)
            {
                if (control && control->canStop())
                {
                    confirmAction(Pending::Kind::Exit);
                }
                else
                {
                    break;
                }
                continue;
            }
            if (press.key != tui::Key::Character)
            {
                continue;
            }

            pending.kind = Pending::Kind::None;
            if (!control)
            {
                continue;
            }
            if (press.text == "s" && control->canStart())
            {
                pending.kind = Pending::Kind::Start;
                pending.yesSelected = true;
            }
            else if (press.text == "x" && control->canStop())
            {
                pending.kind = Pending::Kind::Stop;
                pending.yesSelected = true;
            }
            else if (press.text == "f" && control->canFile())
            {
                pending.kind = Pending::Kind::File;
                pending.yesSelected = true;
            }
            continue;
        }

        drawScreen(frame, report, dir, control ? &*control : nullptr, pending, terminal.width(), terminal.height(),
                   chosen, scrollBack);
        this_thread::sleep_for(chrono::seconds(1));
    }
    printf("\033[2J\033[H");
    return 0;
}

struct RunEntry
{
    filesystem::path path;
    filesystem::file_time_type modified;
};

string screenText(string text)
{
    for (auto& ch: text)
    {
        if (static_cast<unsigned char>(ch) < 32 || ch == 127)
        {
            ch = '?';
        }
    }
    return text;
}

string modifiedDate(const filesystem::file_time_type modified)
{
    if (modified == filesystem::file_time_type::min())
    {
        return {};
    }
    const auto time = chrono::system_clock::to_time_t(chrono::file_clock::to_sys(modified));
    tm local {};
    char date[17] {};
    if (localtime_r(&time, &local))
    {
        strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &local);
    }
    return date;
}

int showComparison(const string& first, const string& second)
{
    string problem;
    const auto rows = compareVersions(first, second, problem);
    if (!tui::Terminal::available())
    {
        printf("Comparison: %s -> %s\n", resultLabel(first).c_str(), resultLabel(second).c_str());
        for (const auto& row: rows)
        {
            printf("  %s  %ldus  %ldus  %s\n", row.name.c_str(), row.first.averageUs,
                   row.second.averageUs, percentDifference(row.first.averageUs, row.second.averageUs).c_str());
        }
        if (!problem.empty())
        {
            printf("  %s\n", problem.c_str());
        }
        return 0;
    }

    tui::Terminal terminal;
    tui::Frame frame;
    size_t chosen = 0;
    while (tui::Terminal::terminateFlag() == 0)
    {
        drawComparison(frame, rows, first, second, chosen, terminal.width(), terminal.height());
        const auto key = terminal.readKey(200).key;
        if (key == tui::Key::F10 || key == tui::Key::Escape)
        {
            break;
        }
        if (key == tui::Key::Up && chosen > 0)
        {
            --chosen;
        }
        else if (key == tui::Key::Down && chosen + 1 < rows.size())
        {
            ++chosen;
        }
    }
    printf("\033[2J\033[H");
    return 0;
}

int browseRuns()
{
    if (!tui::Terminal::available())
    {
        fprintf(stderr, "xmq_runner: the run list needs a terminal; use --help for other modes\n");
        return 2;
    }

    filesystem::path selectedPath;
    string message;
    while (tui::Terminal::terminateFlag() == 0)
    {
        const auto dir = stand::runDirectory();
        vector<RunEntry> entries;
        error_code scanError;
        for (filesystem::directory_iterator it(dir, scanError), end; !scanError && it != end; it.increment(scanError))
        {
            error_code error;
            if (it->path().filename().string().ends_with(".run.sh") && it->is_regular_file(error))
            {
                const auto modified = it->last_write_time(error);
                entries.push_back({it->path(), error ? filesystem::file_time_type::min() : modified});
            }
        }
        sort(entries.begin(), entries.end(), [](const RunEntry& a, const RunEntry& b)
        {
            return a.modified == b.modified ? a.path < b.path : a.modified > b.modified;
        });
        const auto selected = find_if(entries.begin(), entries.end(), [&](const RunEntry& entry)
        {
            return entry.path == selectedPath;
        });
        size_t index = selected == entries.end() ? 0 : static_cast<size_t>(selected - entries.begin());
        size_t first = 0;
        tui::Key action = tui::Key::None;
        tui::Key prompt = tui::Key::None;
        bool yesSelected = false;
        string newName;
        filesystem::path newPath;
        vector<string> compareList;
        size_t compareChoice = 0;
        bool compareButtons = false;
        string compareFirst;
        string brokerName;
        string brokerVersion;
        int    brokerFocus {0}; // 0 name, 1 version, 2 buttons.
        bool   brokerCancel {false};
        {
            // The editor and monitor own their own terminals. Release this one before entering either.
            tui::Terminal terminal;
            tui::Frame frame;
            while (tui::Terminal::terminateFlag() == 0)
            {
                const auto width = terminal.width();
                const auto height = terminal.height();
                const auto rows = static_cast<size_t>(max(0, height - 7));
                if (rows > 0)
                {
                    if (index < first)
                    {
                        first = index;
                    }
                    else if (index - first >= rows)
                    {
                        first = index - rows + 1;
                    }
                }

                frame.resize(width, height);
                frame.clear(theme::desktop);
                frame.put(1, 1, theme::status, string(width, ' '));
                frame.put(1, 2, theme::statusKey, "xmq_runner");
                if (width >= 8 && height >= 8)
                {
                    string line;
                    for (int col = 0; col < width - 2; ++col)
                    {
                        line += "═";
                    }
                    frame.put(2, 1, theme::frame, "╔" + line + "╗");
                    frame.put(2, 3, theme::frameActive, clipTo(" Run files ", width - 3));
                    for (int row = 3; row < height - 1; ++row)
                    {
                        frame.put(row, 1, theme::frame, "║");
                        frame.put(row, width, theme::frame, "║");
                    }
                    frame.put(height - 1, 1, theme::frame, "╚" + line + "╝");
                    frame.put(3, 3, theme::desktop, clipTo(screenText(dir.string()), width - 3));
                    frame.put(4, 2, theme::header, string(width - 2, ' '));
                    frame.put(4, 3, theme::header, clipTo("Run file", width - 3));
                    const bool showDate = width >= 44;
                    if (showDate)
                    {
                        frame.put(4, width - 17, theme::header, "Modified");
                    }
                    for (size_t i = 0; i < rows; ++i)
                    {
                        const auto row = 5 + static_cast<int>(i);
                        const bool hasEntry = first + i < entries.size();
                        const auto color = hasEntry && first + i == index ? theme::fileListSelected : theme::fileList;
                        frame.put(row, 2, color, string(width - 2, ' '));
                        if (!hasEntry)
                        {
                            continue;
                        }
                        const auto& entry = entries[first + i];
                        frame.put(row, 3, color, clipTo(screenText(entry.path.filename().string()),
                                                       width - (showDate ? 21 : 3)));
                        if (showDate)
                        {
                            frame.put(row, width - 17, color, modifiedDate(entry.modified));
                        }
                    }
                    const auto status = !message.empty() ? message :
                                        scanError ? "Cannot read run directory: " + scanError.message() :
                                        entries.empty() ? "No .run.sh files" :
                                        to_string(index + 1) + " / " + to_string(entries.size());
                    frame.put(height - 2, 3, theme::desktop, clipTo(screenText(status), width - 3));
                }
                frame.put(height, 1, theme::status, string(width, ' '));
                tui::putMarked(frame, height, 2, theme::status, theme::statusKey, "~F3~ New");
                // F3/F8 need a selected file; F10 quits whatever is in the list, so it stays lit.
                tui::putMarked(frame, height, 11,
                               entries.empty() ? theme::statusDim : theme::status,
                               entries.empty() ? theme::statusDim : theme::statusKey,
                               "~F4~ Edit   ~F5~ Run   ~F6~ Compare   ~F8~ Delete");
                tui::putMarked(frame, height, 54, theme::status, theme::statusKey, "~F10~ Quit");
                if (prompt == tui::Key::F3)
                {
                    const bool framed = width >= 30 && height >= 9;
                    const auto boxWidth = framed ? min(58, width - 3) : width;
                    const auto boxHeight = message.empty() ? 8 : 9;
                    optional<tui::Dialog> dialog;
                    if (framed)
                    {
                        dialog.emplace(frame, width, height, boxWidth, boxHeight, " New run file ",
                                       vector<string> {"~E~dit", "Cancel"});
                    }
                    const auto contentLeft = framed ? dialog->contentLeft() : 1;
                    const auto contentTop = framed ? dialog->contentTop()
                                                   : max(1, (height - boxHeight) / 2 + 1);
                    const auto contentWidth = framed ? dialog->contentWidth() : boxWidth;
                    if (framed)
                    {
                        dialog->line(0, "File name (.run.sh)");
                        dialog->input(1, newName, false);
                        if (!message.empty())
                        {
                            dialog->line(2, message, theme::warning);
                        }
                        dialog->drawButtons(yesSelected ? 0 : 1);
                    }
                    else
                    {
                        frame.put(contentTop, contentLeft, theme::dialog,
                                  padded("File name (.run.sh)", contentWidth));
                        auto visible = newName;
                        const auto room = max(0, contentWidth - 2);
                        while (tui::Terminal::displayWidth(visible) > room)
                        {
                            visible.erase(0, clipTo(visible, 1).size());
                        }
                        frame.put(contentTop + 1, contentLeft, theme::input, string(contentWidth, ' '));
                        frame.put(contentTop + 1, contentLeft + 1, theme::input, visible);
                        if (!message.empty())
                        {
                            frame.put(contentTop + 2, contentLeft, theme::warning, padded(message, contentWidth));
                        }
                        frame.put(contentTop + (message.empty() ? 2 : 3), contentLeft, theme::dialog,
                                  padded(yesSelected ? "[Edit]   Cancel" : " Edit   [Cancel]", contentWidth));
                    }
                }
                else if (prompt == tui::Key::F5)
                {
                    tui::Dialog dialog(frame, width, height, min(60, width - 3), 10, " Run on broker ",
                                       {"Ok", "Cancel"});
                    dialog.field(0, "Broker name:", brokerName, brokerFocus == 0);
                    dialog.field(2, "Broker version:", brokerVersion, brokerFocus == 1);
                    if (!message.empty())
                    {
                        dialog.line(4, message, theme::warning);
                    }
                    dialog.drawButtons(brokerCancel ? 1 : 0);
                }
                else if (prompt == tui::Key::F6)
                {
                    int desiredWidth = max(64, 16 + tui::columnsOf(resultLabel(compareFirst)));
                    for (const auto& version: compareList)
                    {
                        desiredWidth = max(desiredWidth, 6 + tui::columnsOf(resultLabel(version)));
                    }
                    const auto boxWidth = min(desiredWidth, width - 3);
                    const auto shownVersions = min<size_t>(2, compareList.size());
                    tui::Dialog dialog(frame, width, height, boxWidth,
                                       min(height - 2, 10 + static_cast<int>(shownVersions)),
                                       " Compare to version ", {"Ok", "Cancel"});
                    dialog.line(0, "Compare to version:");
                    dialog.line(1, "Run file: " + resultLabel(compareFirst));
                    const auto rows = static_cast<size_t>(max(0, dialog.height() - 9));
                    const auto rowsPerPage = max(size_t{1}, rows);
                    const auto maxFirst = compareList.size() > rows ? compareList.size() - rows : 0;
                    const auto firstRow = min(compareChoice / rowsPerPage * rowsPerPage, maxFirst);
                    const bool scrollable = rows > 0 && maxFirst > 0;
                    const auto listWidth = dialog.width() - 2 - static_cast<int>(scrollable);
                    for (size_t i = 0; i < rows && firstRow + i < compareList.size(); ++i)
                    {
                        const auto color = !compareButtons && firstRow + i == compareChoice
                                               ? theme::selected : theme::fileList;
                        frame.put(dialog.top() + 4 + static_cast<int>(i), dialog.left() + 1, color,
                                  padded(resultLabel(compareList[firstRow + i]), listWidth));
                    }
                    if (scrollable)
                    {
                        const auto thumbSize = max(size_t{1}, rows * rows / compareList.size());
                        const auto thumbStart = firstRow * (rows - thumbSize) / maxFirst;
                        const auto column = dialog.left() + dialog.width() - 2;
                        for (size_t i = 0; i < rows; ++i)
                        {
                            const bool inThumb = i >= thumbStart && i < thumbStart + thumbSize;
                            frame.put(dialog.top() + 4 + static_cast<int>(i), column,
                                      theme::scroll, inThumb ? "█" : "░");
                        }
                    }
                    dialog.drawButtons(compareButtons ? 1 : 0);
                    if (!message.empty())
                    {
                        dialog.line(dialog.height() - 7, message, theme::warning);
                    }
                }
                else if (prompt == tui::Key::F8)
                {
                    drawAlert(frame, width, height,
                              "Delete " + screenText(entries[index].path.filename().string()) + "?", yesSelected);
                }
                frame.flush();

                const auto press = terminal.readKey(200);
                const auto key = press.key;
                if (tui::Terminal::terminateFlag() != 0)
                {
                    break;
                }
                if (prompt != tui::Key::None && (key == tui::Key::Escape || key == tui::Key::F10))
                {
                    prompt = tui::Key::None;
                    message.clear();
                    continue;
                }
                if (prompt == tui::Key::F5)
                {
                    if (key == tui::Key::Escape)
                    {
                        prompt = tui::Key::None;
                        message.clear();
                    }
                    else if (key == tui::Key::Tab)
                    {
                        brokerFocus = (brokerFocus + 1) % 3;
                    }
                    else if (key == tui::Key::Left || key == tui::Key::Right)
                    {
                        if (brokerFocus == 2)
                        {
                            brokerCancel = !brokerCancel;
                        }
                        else
                        {
                            brokerFocus = 2;
                        }
                    }
                    else if (key == tui::Key::Up || key == tui::Key::Down)
                    {
                        brokerFocus = brokerFocus == 0 ? 1 : 0;
                    }
                    else if (brokerFocus != 2 && key == tui::Key::Character &&
                             (brokerFocus == 0 ? brokerName : brokerVersion).size() + press.text.size() <= 128)
                    {
                        if (brokerFocus == 0)
                        {
                            brokerName += press.text;
                        }
                        else
                        {
                            brokerVersion += press.text;
                        }
                        message.clear();
                    }
                    else if (brokerFocus != 2 && key == tui::Key::Backspace)
                    {
                        auto& value = brokerFocus == 0 ? brokerName : brokerVersion;
                        if (!value.empty())
                        {
                            value.resize(value.size() - tui::Terminal::lastCodePointBytes(value));
                        }
                    }
                    else if (key == tui::Key::Enter && brokerCancel)
                    {
                        prompt = tui::Key::None;
                    }
                    else if (key == tui::Key::Enter)
                    {
                        if (brokerName.find_first_not_of(' ') == string::npos ||
                            brokerVersion.find_first_not_of(' ') == string::npos ||
                            brokerName.find('/') != string::npos || brokerVersion.find('/') != string::npos ||
                            brokerName == "." || brokerName == "..")
                        {
                            message = "Enter a broker name and version, without '/'.";
                        }
                        else
                        {
                            action = tui::Key::F5;
                            break;
                        }
                    }
                    continue;
                }
                if (prompt == tui::Key::F6)
                {
                    if (key == tui::Key::Up && !compareButtons && compareChoice > 0)
                    {
                        --compareChoice;
                    }
                    else if (key == tui::Key::Down && !compareButtons && compareChoice + 1 < compareList.size())
                    {
                        ++compareChoice;
                    }
                    else if (key == tui::Key::Tab || key == tui::Key::Left || key == tui::Key::Right)
                    {
                        compareButtons = !compareButtons;
                        yesSelected = !compareButtons;
                    }
                    else if (key == tui::Key::Enter)
                    {
                        action = tui::Key::F6;
                        break;
                    }
                    continue;
                }
                if (prompt != tui::Key::None &&
                    (key == tui::Key::Left || key == tui::Key::Right || key == tui::Key::Tab))
                {
                    yesSelected = !yesSelected;
                    continue;
                }
                if (prompt == tui::Key::F3)
                {
                    if (key == tui::Key::Character && newName.size() + press.text.size() <= 255)
                    {
                        newName += press.text;
                        message.clear();
                    }
                    else if (key == tui::Key::Backspace && !newName.empty())
                    {
                        newName.resize(newName.size() - tui::Terminal::lastCodePointBytes(newName));
                        message.clear();
                    }
                    else if (key == tui::Key::Enter)
                    {
                        if (!yesSelected)
                        {
                            prompt = tui::Key::None;
                            message.clear();
                            continue;
                        }
                        if (newName.find_first_not_of(' ') == string::npos || newName == "." || newName == ".." ||
                            newName.find_first_of("/\\") != string::npos || newName == ".run.sh")
                        {
                            message = "Enter a file name, not a path.";
                            continue;
                        }
                        newPath = dir / (newName.ends_with(".run.sh") ? newName : newName + ".run.sh");
                        error_code error;
                        const auto status = filesystem::symlink_status(newPath, error);
                        if (filesystem::exists(status))
                        {
                            message = "A file with that name already exists.";
                            continue;
                        }
                        if (error && error != errc::no_such_file_or_directory)
                        {
                            message = "Cannot check name: " + error.message();
                            continue;
                        }
                        action = tui::Key::F3;
                        break;
                    }
                    continue;
                }
                if (prompt == tui::Key::F8)
                {
                    if (press.text == "y" || press.text == "Y" || (key == tui::Key::Enter && yesSelected))
                    {
                        action = tui::Key::F8;
                        break;
                    }
                    else if (key == tui::Key::Enter || press.text == "n" || press.text == "N" ||
                             press.text == "c" || press.text == "C")
                    {
                        prompt = tui::Key::None;
                    }
                    continue;
                }
                if (key == tui::Key::F10)
                {
                    return 0;
                }
                if (key == tui::Key::F3)
                {
                    prompt = key;
                    yesSelected = true;
                    newName.clear();
                    message.clear();
                    continue;
                }
                if (key == tui::Key::F6)
                {
                    stand::RunSpec spec;
                    if (!stand::fromScript(entries[index].path, spec) || spec.version.empty())
                    {
                        message = "Selected run file has no version to compare";
                        continue;
                    }
                    compareFirst = (spec.broker.empty() ? "XMQ" : spec.broker) + "/" + spec.version;
                    compareList.clear();
                    for (const auto& version: stand::availableResultVersions())
                    {
                        if (version != compareFirst)
                        {
                            compareList.push_back(version);
                        }
                    }
                    if (compareList.empty())
                    {
                        message = "No other result versions to compare with " + resultLabel(compareFirst);
                        continue;
                    }
                    prompt = key;
                    compareChoice = 0;
                    compareButtons = false;
                    message.clear();
                    continue;
                }
                if (entries.empty())
                {
                    continue;
                }
                switch (key)
                {
                    case tui::Key::Up: if (index > 0) --index; break;
                    case tui::Key::Down: index = min(index + 1, entries.size() - 1); break;
                    case tui::Key::PageUp: index -= min(index, rows); break;
                    case tui::Key::PageDown: index = min(index + rows, entries.size() - 1); break;
                    case tui::Key::Home: index = 0; break;
                    case tui::Key::End: index = entries.size() - 1; break;
                    case tui::Key::F4: action = key; break;
                    case tui::Key::F5:
                    {
                        stand::RunSpec spec;
                        if (!stand::fromScript(entries[index].path, spec))
                        {
                            message = "Cannot read " + entries[index].path.filename().string();
                            break;
                        }
                        brokerName.clear();
                        brokerVersion.clear();
                        brokerFocus = 0;
                        brokerCancel = false;
                        prompt = key;
                        message.clear();
                        break;
                    }
                    case tui::Key::F6: break;
                    case tui::Key::F8: prompt = key; yesSelected = false; break;
                    default: break;
                }
                if (action != tui::Key::None)
                {
                    break;
                }
            }
        }
        if (tui::Terminal::terminateFlag() != 0)
        {
            break;
        }
        if (!entries.empty())
        {
            selectedPath = entries[index].path;
        }
        message.clear();
        if (action == tui::Key::F6)
        {
            showComparison(compareList[compareChoice], compareFirst);
        }
        else if (action == tui::Key::F3)
        {
            stand::RunSpec spec;
            spec.file = newPath;
            if (stand::editRunFile(spec, false, true) == stand::FormAction::Saved)
            {
                selectedPath = spec.file;
            }
        }
        else if (action == tui::Key::F8)
        {
            if (unlink(selectedPath.c_str()) != 0)
            {
                message = "Cannot delete " + selectedPath.filename().string() + ": " + strerror(errno);
            }
            else if (entries.size() > 1)
            {
                selectedPath = entries[index + 1 < entries.size() ? index + 1 : index - 1].path;
            }
        }
        else if (action == tui::Key::F4)
        {
            stand::RunSpec spec;
            if (!stand::fromScript(selectedPath, spec))
            {
                message = "Cannot read " + selectedPath.filename().string();
            }
            else if (stand::editRunFile(spec, false) == stand::FormAction::Saved)
            {
                selectedPath = spec.file;
            }
        }
        else
        {
            monitorRun({}, selectedPath.string(), false, action == tui::Key::F5, &message,
                       action == tui::Key::F5 ? &brokerName : nullptr,
                       action == tui::Key::F5 ? &brokerVersion : nullptr);
        }
    }
    return 0;
}

} // namespace
