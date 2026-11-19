/**
 * stand_runner - runs a load-test campaign on the home stand and shows it while it runs.
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
 *   stand_runner --edit [run-file]                      fill in a run and write it
 *   stand_runner --view <run-dir>                       live, full screen
 *   stand_runner --view --run <run-file>                watch, start, stop and file
 *   stand_runner --view <run-dir> --once                one plain-text status, for a script or a log
 */
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

#include "frame.h"
#include "run_control.h"
#include "theme.h"
#include "run_file.h"
#include "run_form.h"
#include "tui.h"

using namespace std;

namespace {

struct Interval
{
    long   timeMs {0};
    long   count {0};
    double avgUs {0};
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
        if (line.starts_with("MEASUREMENT-START"))
        {
            // A scenario can report a connection-measurement phase before its actual publish run.
            // Each phase has its own MEASUREMENT-START; the screen shows only the latest phase,
            // rather than concatenating connection intervals with the run's intervals.
            scenario.intervals.clear();
            scenario.durationSec = 0;
            scenario.windowMs = 0;
            ++scenario.measurementStarts;
            // Traffic scenarios connect publisher and subscriber groups before the measured run.
            // The dedicated Connections test has only one measurement phase, which is the test.
            scenario.runPhase = scenario.measurementStarts >= 3 ||
                                scenario.name.find("Connections") != string::npos;
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
        interval.p95Us = field(line, "p95_us").value_or(0);
        interval.p99Us = field(line, "p99_us").value_or(0);
        interval.rate = static_cast<long>(field(line, "rate").value_or(0));
        interval.cpuPct = static_cast<int>(field(line, "cpu_pct").value_or(0));
        interval.rssMb = static_cast<long>(field(line, "rss_mb").value_or(0));
        // Closing the preceding connection phase can race with the first publish windows and emit
        // a late, partial interval at the same timestamp. For duplicate timestamps, keep the
        // substantive window rather than displaying that small tail as an extra measurement row.
        if (!scenario.intervals.empty() && scenario.intervals.back().timeMs == interval.timeMs)
        {
            if (interval.count > scenario.intervals.back().count)
            {
                scenario.intervals.back() = interval;
            }
        }
        else
        {
            scenario.intervals.push_back(interval);
        }
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
    long             elapsedSec {0};
    long             etaSec {0};
};

long plannedDuration(const filesystem::path& setFile)
{
    if (setFile.empty())
    {
        return 0;
    }
    ifstream input(setFile);
    string line;
    long total = 0;
    const regex durationPattern {R"("duration_sec"\s*:\s*(\d+))"};
    while (getline(input, line))
    {
        const auto comment = line.find('#');
        if (comment != string::npos)
        {
            line.erase(comment);
        }
        const auto first = line.find_first_not_of(" \t\r");
        if (first == string::npos)
        {
            continue;
        }
        line = line.substr(first);
        const auto last = line.find_last_not_of(" \t\r");
        line.erase(last + 1);
        const auto scenarioFile = stand::loadTestDirectory() / line;
        const auto content = readLog(scenarioFile);
        smatch match;
        if (regex_search(content, match, durationPattern))
        {
            total += stol(match[1].str());
        }
    }
    return total;
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
        report.etaSec += max(0L, plannedDuration(scenarioSet) - knownDuration);
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
    printf("host: cpu %.0f%%, used %ldMb   eta %s\n", report.host.cpuPct, report.host.usedMb,
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

    Kind                             kind {Kind::None};
    chrono::steady_clock::time_point deadline {};
};

string promptFor(const Pending& pending, const stand::RunControl& control)
{
    const auto& spec = control.spec();
    switch (pending.kind)
    {
        case Pending::Kind::Start:
            return "start " + spec.file.filename().string() + "?  a full set takes about 40 minutes   y/N";
        case Pending::Kind::Stop:
            return "stop the run (pid " + to_string(control.state().pid) + ")?  it will not be filed   y/N";
        case Pending::Kind::File:
            return "file the result into results/Bench/" + spec.version + "/?  this replaces the directory   y/N";
        case Pending::Kind::Exit:
            return "stop the running set and quit?  the partial run will not be filed   y/N";
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

string clipTo(const string& text, const int columns)
{
    if (columns <= 0)
    {
        return {};
    }
    int    used = 0;
    size_t at = 0;
    while (at < text.size() && used < columns)
    {
        at += 1;
        while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80)
        {
            ++at;
        }
        ++used;
    }
    return text.substr(0, at);
}

string padded(const string& text, const int columns)
{
    const auto clipped = clipTo(text, columns);
    return clipped + string(static_cast<size_t>(max(0, columns - static_cast<int>(clipped.size()))), ' ');
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
    client << "client: CPU " << static_cast<int>(report.host.cpuPct) << "%, used " << report.host.usedMb << "Mb";
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
    if (!state.lastAction.empty() && !state.lastAction.starts_with("started, logging to "))
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
string intervalRow(const Scenario& scenario, const Interval& interval)
{
    ostringstream text;
    text << ' ' << left2(to_string((interval.timeMs + scenario.windowMs) / 1000) + "s/" +
                            to_string(scenario.durationSec) + "s",
                        14) << ' '
         << right2(to_string(interval.count), 11) << ' '
         << right2(to_string(interval.rate), 9) << ' '
         << right2(to_string(static_cast<long>(interval.avgUs)) + "us", 9) << ' '
         << right2(to_string(static_cast<long>(interval.p95Us)) + "us", 9) << ' '
         << right2(to_string(static_cast<long>(interval.p99Us)) + "us", 9) << ' '
         << right2(to_string(interval.cpuPct) + "%", 7) << ' '
         << right2(to_string(interval.rssMb) + "Mb", 8);
    return text.str();
}

string testSummary(const Scenario& scenario)
{
    if (scenario.intervals.empty())
    {
        return " Summary: waiting for measured intervals";
    }
    vector<double> averages, p95s, p99s;
    long totalCount = 0;
    long totalRate = 0;
    long totalCpu = 0;
    long peakRss = 0;
    for (const auto& interval: scenario.intervals)
    {
        averages.push_back(interval.avgUs);
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
    ostringstream text;
    text << " Avg " << static_cast<long>(average(averages)) << "us"
         << "   Median " << static_cast<long>(median(averages)) << "us"
         << "   p95 " << static_cast<long>(median(p95s)) << "us"
         << "   p99 " << static_cast<long>(median(p99s)) << "us"
         << "   count " << totalCount
         << "   rate " << totalRate / static_cast<long>(scenario.intervals.size()) << "/s"
         << "   CPU " << totalCpu / static_cast<long>(scenario.intervals.size()) << "%"
         << "   peak RSS " << peakRss << "Mb";
    return text.str();
}

/// A Turbo Vision window on the blue desktop: double frame with its title in the top line, a
/// shadow, the run's own lines above the table and the keys on a status line at the bottom.
void drawScreen(const Report& report, const filesystem::path& dir, const stand::RunControl* control,
                const Pending& pending, const int width, const int height,
                const size_t chosen, const long scrollBack)
{
    // A run whose first log has not appeared yet has no scenario to show, but the screen still has
    // a table in it.
    static const Scenario emptyScenario;

    // One frame buffer for the life of the process: it is what remembers the last screen sent.
    static tui::Frame frameStorage;
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
    for (int row = headerRow; row < bottom; ++row)
    {
        put(row, left + 1, theme::dialog, string(static_cast<size_t>(inner), ' '));
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
            text << " test " << (report.scenarios.empty() ? 0 : shown + 1) << '/' << report.scenarios.size() << "  "
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
        ostringstream text;
        text << ' ' << left2("elapsed", 14) << ' ' << right2("count", 11) << ' ' << right2("rate/s", 9) << ' '
             << right2("avg", 9) << ' ' << right2("p95", 9) << ' ' << right2("p99", 9) << ' '
             << right2("cpu", 7) << ' ' << right2("rss", 8);
        put(headerRow, left + 2, theme::header, padded(text.str(), inner - 2));
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
    if (first > 0 && row < bottom)
    {
        put(row++, left + 2, theme::dialog, padded(" ... " + to_string(first) + " earlier interval(s), PageUp", inner - 2));
    }
    if (rowsCount == 0)
    {
        ostringstream text;
        text << ' ' << left2(scenario.started ? "starting" : (scenario.finished ? "done" : "waiting"), 12);
        put(row++, left + 2, theme::dialog, padded(text.str(), inner - 2));
    }
    for (long index = first; index < last && row < bottom - 1; ++index)
    {
        put(row++, left + 2, theme::dialog, padded(intervalRow(scenario, scenario.intervals[static_cast<size_t>(index)]), inner - 2));
    }
    if (offset > 0 && row < bottom - 1)
    {
        put(row, left + 2, theme::dialog,
            padded(" ... " + to_string(offset) + " newer interval(s), PageDown", inner - 2));
    }
    put(bottom - 1, left + 2, theme::header, padded(testSummary(scenario), inner - 2));

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

        put(trackTop, left + inner, theme::scroll, "▲");
        put(trackBottom, left + inner, theme::scroll, "▼");
        for (int index = 0; index < trackRows; ++index)
        {
            const bool inThumb = index >= position && index < position + thumbSize;
            put(trackTop + 1 + index, left + inner, theme::scroll, inThumb ? "█" : "░");
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
        put(height, 2, theme::warning, promptFor(pending, *control));
    }
    else
    {
        drawKeysLine(frameStorage, height, control);
    }

    // Only the rows that changed go out, so an idle second sends nothing at all.
    frame->flush();
}


} // namespace

int main(int argc, char** argv)
{
    filesystem::path dir;
    string            brokerTarget;
    string            runFile;
    bool              once = false;
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
            // The directory is optional when --run names a file that already says where its logs go.
            if (i + 1 < argc && argv[i + 1][0] != '-')
            {
                dir = argv[++i];
            }
        }
        else if (arg == "--run" && i + 1 < argc)
        {
            runFile = argv[++i];
        }
        else if (arg == "--once")
        {
            once = true;
        }
        else if (arg == "--help" || arg == "-h")
        {
            printf("Usage:\n"
                   "  stand_runner --edit [run-file]                        fill in a run and write it\n"
                   "  stand_runner --view <run-dir> [--once] [--run <run-file>]\n"
                   "                                                       watch, and start or stop\n"
                   "\n"
                   "  --edit [run-file]      A form over the settings a bench run needs. F2 writes the\n"
                   "                         file; it is a shell script that execs stand_run.sh, so it\n"
                   "                         runs as it stands and can be edited by hand afterwards.\n"
                   "                         Given an existing file, its values are loaded first.\n"
                   "  --view <run-dir>       Watch a run's own output: the INTERVAL lines xmq_scn writes\n"
                   "                         under --output runner, plus each scenario's .block.\n"
                   "  --run <run-file>       The run this screen may start, stop and file. It also\n"
                   "                         supplies <run-dir> and the broker to watch, from the --out\n"
                   "                         and --server the file already names.\n"
                   "                         Keys: s start, x stop, f file the result, q quit; each\n"
                   "                         action asks for a y first.\n"
                   "  --once                 Print one plain-text status and exit, for a script or a log.\n"
                   );
            return 0;
        }
        else if (arg == "--broker")
        {
            // The flag existed for an afternoon: it named a host the run file also names, and the
            // two could disagree. Whoever typed it deserves the sentence that replaces it.
            fprintf(stderr,
                    "stand_runner: --broker is gone - the broker comes from the run file.\n"
                    "               Use: stand_runner --view --run <run-file>\n");
            return 2;
        }
        else
        {
            fprintf(stderr, "stand_runner: unknown argument %s\n", arg.c_str());
            return 2;
        }
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
                                        ? "stand_runner: %s is a directory - give a run file path, or omit the argument\n"
                                        : "stand_runner: cannot read %s\n",
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

    if (dir.empty() && runFile.empty())
    {
        fprintf(stderr, "stand_runner: --view <run-dir> is required (or --edit to write a run)\n");
        return 2;
    }

    // A run file carries its own output directory and broker, so watching what it will do needs no
    // second set of arguments: the same file that starts the run says where to look.
    optional<stand::RunSpec> runSpec;
    if (!runFile.empty())
    {
        stand::RunSpec spec;
        if (!stand::fromScript(runFile, spec))
        {
            fprintf(stderr, "stand_runner: cannot read %s\n", runFile.c_str());
            return 2;
        }
        if (spec.out.empty())
        {
            fprintf(stderr, "stand_runner: %s names no --out, so there is no run directory to watch\n", runFile.c_str());
            return 2;
        }
        if (dir.empty())
        {
            dir = spec.out;
        }
        // The broker is what the run file says it is: a second way to name it could only disagree
        // with the file the run is executed from.
        brokerTarget = spec.server;
        runSpec = spec;
    }

    if (!filesystem::is_directory(dir))
    {
        // Before a run there is nothing there yet, and that is exactly the moment one is started.
        if (runSpec)
        {
            error_code error;
            filesystem::create_directories(dir, error);
        }
        if (!filesystem::is_directory(dir))
        {
            // The usual reason: the directory a run writes into does not exist until the run
            // starts, and the way to start one from here is --run, not a path that is not there.
            fprintf(stderr,
                    "stand_runner: %s does not exist yet - a run creates it when it starts.\n"
                    "               To start and watch a run: stand_runner --view --run <run-file>\n",
                    dir.c_str());
            return 2;
        }
    }

    BrokerWatch watch;
    if (!brokerTarget.empty())
    {
        watch.setCredentials(runSpec->brokerUser, runSpec->brokerPassword);
        watch.start(brokerTarget);
    }

    optional<stand::RunControl> control;
    if (runSpec)
    {
        control.emplace(*runSpec);
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

        // A confirmation left unanswered for five seconds is a changed mind.
        if (pending.kind != Pending::Kind::None && chrono::steady_clock::now() > pending.deadline)
        {
            pending.kind = Pending::Kind::None;
        }

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
            drawScreen(report, dir, control ? &*control : nullptr, pending, terminal.width(), terminal.height(),
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
            if (press.key == tui::Key::Escape ||
                (press.key == tui::Key::Character && (press.text == "q" || press.text == "Q")))
            {
                if (control && control->canStop())
                {
                    pending.kind = Pending::Kind::Exit;
                    pending.deadline = chrono::steady_clock::now() + chrono::seconds(5);
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
                    pending.deadline = chrono::steady_clock::now() + chrono::seconds(5);
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

            if (press.text == "y" || press.text == "Y")
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
                pending.kind = Pending::Kind::None;
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
                pending.deadline = chrono::steady_clock::now() + chrono::seconds(5);
            }
            else if (press.text == "x" && control->canStop())
            {
                pending.kind = Pending::Kind::Stop;
                pending.deadline = chrono::steady_clock::now() + chrono::seconds(5);
            }
            else if (press.text == "f" && control->canFile())
            {
                pending.kind = Pending::Kind::File;
                pending.deadline = chrono::steady_clock::now() + chrono::seconds(5);
            }
            continue;
        }

        drawScreen(report, dir, control ? &*control : nullptr, pending, terminal.width(), terminal.height(),
                   chosen, scrollBack);
        this_thread::sleep_for(chrono::seconds(1));
    }
    printf("\033[2J\033[H");
    return 0;
}
