/**
 * Starting, stopping and filing a run from the same screen that watches it.
 *
 * The run file stays the definition of a run: this only executes it, detached, and remembers the
 * pid so the same screen can stop it again. Nothing here invents a flag or reorders an invocation -
 * if a run must be changed, the file is changed with --edit.
 */
#pragma once

#include "run_file.h"

#include <chrono>
#include <string>

namespace stand {

enum class RunPhase
{
    NoRunFile, ///< --view without --run: watch only, nothing to start or stop.
    Idle,      ///< A run file is known and nothing is running.
    Running,
    Exited,    ///< Finished during this session; see RunState::exitCode.
};

struct RunState
{
    RunPhase  phase {RunPhase::NoRunFile};
    long      pid {0};
    int       exitCode {0};
    long      elapsedSec {0};
    std::string log;         ///< The detached run's console log.
    std::string record;      ///< The input to filing.
    std::string monitorDir;  ///< Persistent per-attempt output under the configured --out.
    std::string lastAction;  ///< What the screen should say about the last start, stop or filing.
    std::string failure;     ///< Diagnostic taken from this attempt's console log.
};

class RunControl final
{
public:
    /// @param spec The run this screen may control; only out, version and file are used.
    explicit RunControl(RunSpec spec);

    /// Re-check the process and the record: called once per screen tick.
    void poll();

    bool canStart() const;
    bool canStop() const;
    bool canFile() const;

    /// Executes the run file detached, in its own session, with a fresh output directory. The
    /// session matters: a stop has to reach xmq_scn and the shell that started it, not just the
    /// first process.
    void start();

    /// Override the broker and version for this start only, without changing the run file.
    void setOverrides(const std::string& broker, const std::string& version);

    /// SIGTERM to the run's process group; a stop that is ignored becomes a SIGKILL after a few
    /// seconds, so a hung run cannot hold the stand.
    void stop();

    /// Runs file_results.py --env Bench on the record. It replaces results/Bench/<version>/ whole,
    /// which is why the screen asks before calling this.
    void file();

    [[nodiscard]] const RunState& state() const
    {
        return m_state;
    }

    /// The run this screen controls, for the title line.
    [[nodiscard]] const RunSpec& spec() const
    {
        return m_spec;
    }

private:
    void attachToExisting();
    void readPidFile();
    void writePidFile() const;
    bool alive() const;

    RunSpec   m_spec;
    RunState  m_state;
    std::chrono::steady_clock::time_point m_startedAt;
    std::chrono::steady_clock::time_point m_stopDeadline;
    std::string m_processStart;
    std::string m_overrideBroker;
    std::string m_overrideVersion;
    bool      m_stopping {false};
    bool      m_ownedChild {false};
};

} // namespace stand
