/**
 * stand_runner - runs a load-test campaign on the home stand and shows it while it runs.
 *
 * Reads only what a run already writes: a scenario's own log (MEASUREMENT-START and the
 * INTERVAL lines xmq_scn emits under --output runner), the presence of its .block, and the
 * client host's /proc. Nothing here polls the broker, and nothing here is on the run's path.
 *
 * With --broker it also subscribes to the broker's $SYS topics through a child xmq_sub, which is
 * how the broker's own CPU and memory appear next to the client's. Without it nothing outside
 * this machine is contacted at all.
 *
 *   stand_runner --edit [run-file]                      fill in a run and write it
 *   stand_runner --view <run-dir>                       live, full screen
 *   stand_runner --view <run-dir> --once                one plain-text status, for a script or a log
 *   stand_runner --view <run-dir> --broker HOST[:P]     also show the broker's own CPU and memory
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
#include <map>
#include <optional>
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

#include "run_control.h"
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
    long                  durationSec {0};
    long                  windowMs {0};      ///< Width of one reported interval, from the log itself.
    vector<Interval>      intervals;
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
        Interval interval;
        interval.timeMs = static_cast<long>(field(line, "time_ms").value_or(0));
        interval.count = static_cast<long>(field(line, "count").value_or(0));
        interval.avgUs = field(line, "avg_us").value_or(0);
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

vector<Scenario> scanRunDir(const filesystem::path& dir)
{
    vector<Scenario> scenarios;
    for (const auto& entry: filesystem::directory_iterator(dir))
    {
        if (entry.path().extension() != ".log")
        {
            continue;
        }
        Scenario scenario;
        scenario.log = entry.path();
        scenario.name = entry.path().stem().string();
        scenario.finished = filesystem::exists(entry.path().parent_path() / (scenario.name + ".block"));
        scenario.lastWrite = chrono::clock_cast<chrono::system_clock>(filesystem::last_write_time(entry.path()));
        scenarios.push_back(std::move(scenario));
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
        const auto colon = target.rfind(':');
        const auto host = colon == string::npos ? target : target.substr(0, colon);
        const auto port = colon == string::npos ? string("1883") : target.substr(colon + 1);
        if (host.empty() || port.empty())
        {
            m_load.state = "bad --broker";
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

        array<string, 8> storage {"xmq_sub", "-h", host, "-p", port, "-t", "$SYS/broker/system/#", "-v"};
        array<char*, 9>  arguments {};
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
            if (waitpid(m_pid, &status, WNOHANG) == m_pid)
            {
                m_pid = -1;
            }
            close(m_fd);
            m_fd = -1;
            m_load.state = WIFEXITED(status) ? "xmq_sub exited (" + to_string(WEXITSTATUS(status)) + ")"
                                             : "xmq_sub stopped";
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
};

struct Report
{
    vector<Scenario> scenarios;
    HostLoad         host;
    BrokerLoad       broker;
    size_t           current {0};
    long             elapsedSec {0};
    long             etaSec {0};
};

Report buildReport(const filesystem::path& dir)
{
    Report report;
    report.host = sampleHostLoad();
    report.scenarios = scanRunDir(dir);
    report.current = report.scenarios.size();
    long remaining = 0;
    for (size_t i = 0; i < report.scenarios.size(); ++i)
    {
        auto& scenario = report.scenarios[i];
        parseScenario(scenario);
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
        printf("broker: cpu %d%% rss %ldMb   (its own $SYS figures)\n", broker.cpuPct, broker.rssMb);
        return;
    }
    if (broker.state == "live")
    {
        printf("broker: last figures %ds ago, cpu %d%% rss %ldMb\n", broker.ageSec, broker.cpuPct, broker.rssMb);
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

/// One character for each tenth of the scenario, filled by elapsed time: coarse on purpose, since
/// the point is to see at a glance whether the run is moving, not to measure it.
string progressBar(long elapsedSec, long durationSec)
{
    if (durationSec <= 0)
    {
        return "         ";
    }
    const auto filled = min<long>(10, elapsedSec * 10 / durationSec);
    string bar;
    for (long i = 0; i < 10; ++i)
    {
        bar += i < filled ? '#' : '.';
    }
    return bar;
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

void drawScreen(const Report& report, const filesystem::path& dir, const stand::RunControl* control, const Pending& pending)
{
    const auto load = report.host;
    printf("\033[H\033[2J");
    printf("stand_runner  %s\n", dir.c_str());
    printf("%-44s %-8s %-9s %6s %7s %7s %7s %7s\n",
           "scenario", "state", "progress", "rate/s", "avg", "p99", "cpu", "rss");
    for (const auto& scenario: report.scenarios)
    {
        if (!scenario.started)
        {
            printf("%-44s %-8s\n", scenario.name.c_str(), scenario.finished ? "done" : "waiting");
            continue;
        }
        const auto& last = scenario.intervals.empty() ? Interval {} : scenario.intervals.back();
        const auto  elapsedSec = scenario.intervals.empty() ? 0 : (last.timeMs + scenario.windowMs) / 1000;
        printf("%-44s %-8s %s %3lds/%-3lds %6ld %6.0fus %6.0fus %6d%% %6ldMb\n",
               scenario.name.c_str(), scenario.finished ? "done" : "running",
               progressBar(elapsedSec, scenario.durationSec).c_str(),
               min(elapsedSec, scenario.durationSec), scenario.durationSec,
               last.rate, last.avgUs, last.p99Us, last.cpuPct, last.rssMb);
    }
    printf("\nclient cpu %3.0f%%  used %ldMb   eta %s\n", load.cpuPct, load.usedMb,
           humanDuration(report.etaSec).c_str());
    printBroker(report.broker);
    printf("\n");
    printOperator(control, pending);
    printf("\nq quit                                 %s\n",
           report.current < report.scenarios.size() ? report.scenarios[report.current].name.c_str() : "run complete");
    fflush(stdout);
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
        else if (arg == "--broker" && i + 1 < argc)
        {
            brokerTarget = argv[++i];
        }
        else if (arg == "--once")
        {
            once = true;
        }
        else if (arg == "--help" || arg == "-h")
        {
            printf("Usage:\n"
                   "  stand_runner --edit [run-file]                        fill in a run and write it\n"
                   "  stand_runner --view <run-dir> [--once] [--broker <host[:port]>]\n"
                   "              [--run <run-file>]                        watch, and start or stop\n"
                   "\n"
                   "  --edit [run-file]      A form over the settings a bench run needs. F2 writes the\n"
                   "                         file; it is a shell script that execs stand_run.sh, so it\n"
                   "                         runs as it stands and can be edited by hand afterwards.\n"
                   "                         Given an existing file, its values are loaded first.\n"
                   "  --view <run-dir>       Watch a run's own output: the INTERVAL lines xmq_scn writes\n"
                   "                         under --output runner, plus each scenario's .block.\n"
                   "  --run <run-file>       The run this screen may start, stop and file. It also\n"
                   "                         supplies <run-dir> and --broker when they are not given,\n"
                   "                         from the --out and --server the file already names.\n"
                   "                         Keys: s start, x stop, f file the result, q quit; each\n"
                   "                         action asks for a y first.\n"
                   "  --once                 Print one plain-text status and exit, for a script or a log.\n"
                   "  --broker <host[:port]> Also read the broker's CPU and memory from its $SYS topics,\n"
                   "                         subscribing with xmq_sub; port defaults to 1883. Omitted, no\n"
                   "                         connection is made anywhere.\n");
            return 0;
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
                    fprintf(stderr, "stand_runner: cannot read %s\n", editTarget.c_str());
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
        if (brokerTarget.empty() && !spec.server.empty())
        {
            brokerTarget = spec.server;
        }
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
            fprintf(stderr, "stand_runner: %s is not a directory\n", dir.c_str());
            return 2;
        }
    }

    BrokerWatch watch;
    if (!brokerTarget.empty())
    {
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
        auto report = buildReport(dir);
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
    while (tui::Terminal::terminateFlag() == 0)
    {
        if (control)
        {
            control->poll();
        }
        watch.refresh();

        // A confirmation left unanswered for five seconds is a changed mind.
        if (pending.kind != Pending::Kind::None && chrono::steady_clock::now() > pending.deadline)
        {
            pending.kind = Pending::Kind::None;
        }

        if (interactive)
        {
            // Redraw before reading, then act on the key: the screen shows the state the key was
            // pressed on rather than the one before the last tick.
            auto report = buildReport(dir);
            report.broker = watch.load();
            drawScreen(report, dir, control ? &*control : nullptr, pending);

            const auto press = terminal.readKey(1000);
            if (tui::Terminal::terminateFlag() != 0)
            {
                break;
            }
            if (terminal.takeResized() || press.key == tui::Key::None)
            {
                continue;
            }
            if (press.key == tui::Key::Escape || (press.key == tui::Key::Character && (press.text == "q" || press.text == "Q")))
            {
                break;
            }
            if (press.key != tui::Key::Character)
            {
                pending.kind = Pending::Kind::None;
                continue;
            }

            if (press.text == "y" || press.text == "Y")
            {
                if (pending.kind == Pending::Kind::Start && control && control->canStart())
                {
                    control->start();
                }
                else if (pending.kind == Pending::Kind::Stop && control && control->canStop())
                {
                    control->stop();
                }
                else if (pending.kind == Pending::Kind::File && control && control->canFile())
                {
                    control->file();
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

        auto report = buildReport(dir);
        report.broker = watch.load();
        drawScreen(report, dir, control ? &*control : nullptr, pending);
        this_thread::sleep_for(chrono::seconds(1));
    }
    printf("\033[2J\033[H");
    return 0;
}
