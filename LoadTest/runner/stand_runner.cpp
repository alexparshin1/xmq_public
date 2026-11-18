/**
 * stand_runner - runs a load-test campaign on the home stand and shows it while it runs.
 *
 * Reads only what a run already writes: a scenario's own log (MEASUREMENT-START and the
 * INTERVAL lines xmq_scn emits under --output runner), the presence of its .block, and the
 * client host's /proc. Nothing here polls the broker, and nothing here is on the run's path.
 *
 *   stand_runner --attach <run-dir>          live, full screen
 *   stand_runner --attach <run-dir> --once   one plain-text status, for a script or a log
 */
#include <algorithm>
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
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

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

struct Report
{
    vector<Scenario> scenarios;
    HostLoad         host;
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

void drawScreen(const Report& report, const filesystem::path& dir)
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
    printf("\nq quit   r refresh now            %s\n",
           report.current < report.scenarios.size() ? report.scenarios[report.current].name.c_str() : "run complete");
    fflush(stdout);
}

} // namespace

int main(int argc, char** argv)
{
    filesystem::path dir;
    bool              once = false;
    for (int i = 1; i < argc; ++i)
    {
        const string arg = argv[i];
        if (arg == "--attach" && i + 1 < argc)
        {
            dir = argv[++i];
        }
        else if (arg == "--once")
        {
            once = true;
        }
        else if (arg == "--help" || arg == "-h")
        {
            printf("Usage: stand_runner --attach <run-dir> [--once]\n");
            return 0;
        }
        else
        {
            fprintf(stderr, "stand_runner: unknown argument %s\n", arg.c_str());
            return 2;
        }
    }
    if (dir.empty())
    {
        fprintf(stderr, "stand_runner: --attach <run-dir> is required\n");
        return 2;
    }
    if (!filesystem::is_directory(dir))
    {
        fprintf(stderr, "stand_runner: %s is not a directory\n", dir.c_str());
        return 2;
    }

    if (once)
    {
        // Two samples, so the CPU figure is a rate rather than a zero from having no previous one.
        sampleHostLoad();
        this_thread::sleep_for(chrono::milliseconds(200));
        printOnce(buildReport(dir));
        return 0;
    }

    sampleHostLoad();
    while (true)
    {
        drawScreen(buildReport(dir), dir);
        pollfd input {STDIN_FILENO, POLLIN, 0};
        if (poll(&input, 1, 1000) > 0)
        {
            char key = 0;
            if (read(STDIN_FILENO, &key, 1) == 1 && (key == 'q' || key == 'Q'))
            {
                break;
            }
        }
    }
    printf("\033[2J\033[H");
    return 0;
}
