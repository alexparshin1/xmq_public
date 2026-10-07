#!/usr/bin/env bash
# Runs a whole scenario set - one of the Scenario-Set-*.txt files - through run_load_test.sh,
# one scenario at a time, and prints the interval median for each plus a summary at the end.
#
# The set files say WHICH scenarios and in WHAT order; this says HOW a set is run. Two rules from
# results/README.md are enforced here rather than left to whoever is running it:
#
#   - the broker is restarted before every scenario (--restart-cmd). A broker still holding the
#     previous scenario's sessions is not the clean broker a record claims to have measured.
#   - a run counts as done only if it printed a result line. An exit code of 0 does not mean a
#     run produced anything: 24 consecutive runs once passed every check and produced nothing.
#   - and the converse, which cost a wrong figure in the 0.9.16 record: a result line does not
#     mean the run finished. A scenario that paces its connections prints a table per phase, so a
#     run that aborts during publishing still leaves the connect tables behind - and they look
#     exactly like a measurement. Both the exit code and how far the run got are checked here.
#
# Everything after -- is passed to run_load_test.sh unchanged, so the server host, credentials
# and bind addresses are specified there exactly as for a single run.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
set_file=""
restart_cmd=""
stats_host=""
stats_process="xmq_server"
setup_cmd=""
wait_host=""
wait_port="1883"
out_dir=""
record=""
nic=""
version=""
broker="XMQ"
server_note=""
client_note=""
declare -a notes=()

usage() {
  cat <<EOF
Usage: $(basename "$0") [options] <set file> [-- <run_load_test.sh options>]

  <set file>                A Scenario-Set-*.txt file: one scenario per line, '#' comments and
                            blank lines ignored. Scenario names are resolved relative to the set
                            file's own directory.

Options:
  --setup-cmd CMD           Shell command run before the restart, with the scenario's file name
                            in \$XMQ_SCENARIO. This is where a scenario that needs the broker
                            configured differently gets it configured: in the standing sets
                            exactly one scenario runs with persistence on, and a set is only
                            worth running unattended if that happens by itself.
  --restart-cmd CMD         Shell command that restarts the broker; run before every scenario.
                            Omitted, nothing is restarted and each run inherits the state the
                            one before it left - which is not how the recorded sets were measured.
                            Example:
                              --restart-cmd 'ssh bench-host sudo -n systemctl restart xmq_server'
  --stats-host HOST         ssh target where the broker runs. With it, each scenario records the
                            broker's mean and peak CPU and its peak RSS. Without it those columns
                            are empty on the site, which is how every run from 2026-09-09 to
                            2026-09-12 was published: a dash where the rival brokers have numbers.
  --stats-process NAME      Process to sample on that host (default xmq_server). Name it for the
                            broker being measured - mosquitto, emqx, flashmq.
  --wait-for HOST[:PORT]    After the restart, wait for this TCP port to accept before running
                            (default port 1883). Without it a scenario can start against a broker
                            that has not finished coming up.
  --nic IFACE               Client network interface to watch. Its dropped-packet counters are
                            read before and after every scenario and written into the record. A
                            client whose NIC drops received packets reports a latency that is
                            partly its own: the default ring on this hardware is 256 descriptors
                            and overflows under a 100k-connection run. Named explicitly and never
                            guessed - guessing it from the default route is how 50 bench addresses
                            once ended up on a VPN interface.
  --out DIR                 Where per-scenario logs go (default: a dated directory under
                            results/raw/, which is not kept in git).

Writing the record:
  --record FILE             Also write the run up as a record: a header,
                            a summary of interval medians, then every scenario's own table. The
                            file is rewritten after each scenario, so a series that dies at hour
                            two still leaves the hours before it.
  --version V               Broker version for the record's header. Required with --record: a
                            recorded latency without the version that produced it cannot be
                            reproduced, and none of the broker install scripts pin one.
  --broker NAME             Broker name for the record's Server: line (default: XMQ).
  --server-note TEXT        The record's Host: line - what the broker ran on. A file is only
                            comparable with another whose Host: line is the same, so this is
                            the CPU, the RAM and the kernel, not just a name.
  --client-note TEXT        The record's Client: line (default: this host and the xmq_scn
                            version).
  --note TEXT               A free-text line after the header; repeatable. What was being asked,
                            what was different this time.
  -h, --help                This.

Example - the bench set, restarting the broker on the server host before each scenario:

  ./run_scenario_set.sh Scenario-Set-Bench.txt \\
      --restart-cmd 'ssh 10.0.0.1 sudo -n systemctl restart xmq_server' \\
      --wait-for 10.0.0.1:1883 \\
      -- --skip-ip-setup --subnet 10.1.1 --first 150 -h 10.0.0.1 -u user -P secret
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --setup-cmd)   setup_cmd="$2"; shift 2 ;;
    --restart-cmd) restart_cmd="$2"; shift 2 ;;
    --stats-host) stats_host="$2"; shift 2 ;;
    --stats-process) stats_process="$2"; shift 2 ;;
    --wait-for)    wait_host="${2%%:*}"; [[ "$2" == *:* ]] && wait_port="${2##*:}"; shift 2 ;;
    --nic)         nic="$2"; shift 2 ;;
    --out)         out_dir="$2"; shift 2 ;;
    --record)      record="$2"; shift 2 ;;
    --version)     version="$2"; shift 2 ;;
    --broker)      broker="$2"; shift 2 ;;
    --server-note) server_note="$2"; shift 2 ;;
    --client-note) client_note="$2"; shift 2 ;;
    --note)        notes+=("$2"); shift 2 ;;
    -h|--help)     usage; exit 0 ;;
    --)            shift; break ;;
    -*)            echo "Unknown option: $1" >&2; usage; exit 1 ;;
    *)             set_file="$1"; shift ;;
  esac
done
passthrough=("$@")

[[ -z "$set_file" ]] && { echo "No set file given." >&2; usage; exit 1; }
[[ -n "$record" && -z "$version" ]] && {
  echo "--record needs --version: a recorded figure without the version that produced it" \
       "cannot be reproduced later." >&2; exit 1; }
[[ -n "$record" && -z "$stats_host" ]] && {
  echo "--record needs --stats-host: every recorded test shows the broker's CPU and memory, and" \
       "a record without them is one the site would show with dashes." >&2; exit 1; }
[[ -r "$set_file" ]] || { echo "Cannot read set file: $set_file" >&2; exit 1; }

set_dir="$(cd "$(dirname "$set_file")" && pwd)"
mapfile -t scenarios < <(sed 's/#.*//' "$set_file" | awk 'NF {print $1}')
(( ${#scenarios[@]} )) || { echo "No scenarios in $set_file" >&2; exit 1; }

# Next to the set file, or next to this script: a set kept somewhere else - a one-off tail of a
# series, a set under review - still names scenarios that live here.
declare -A scenario_path=()
for scenario in "${scenarios[@]}"; do
  if [[ -r "$set_dir/$scenario" ]]; then
    scenario_path["$scenario"]="$set_dir/$scenario"
  elif [[ -r "$SCRIPT_DIR/$scenario" ]]; then
    scenario_path["$scenario"]="$SCRIPT_DIR/$scenario"
  else
    echo "Missing scenario file: $scenario (looked in $set_dir and $SCRIPT_DIR)" >&2
    exit 1
  fi
done

# Two load tests at once measure neither, and both look plausible while they do it. This has
# happened: a second series started from a stale marker while the first was still running, and
# the broker refused connections it would otherwise have served.
if pgrep -x xmq_scn > /dev/null; then
  echo "xmq_scn is already running - another load test is in progress. Not starting." >&2
  ps -o pid,etime,cmd -C xmq_scn | head -3 >&2
  exit 1
fi

if [[ -z "$out_dir" ]]; then
  out_dir="$SCRIPT_DIR/results/raw/$(date +%Y-%m-%d-%H%M)"
fi
mkdir -p "$out_dir" || exit 1

set_started_epoch=$(date +%s)
set_started_at=$(date '+%Y-%m-%d %H:%M:%S %Z')
set_finished=0
format_elapsed() {
  local seconds=$1
  printf '%02d:%02d:%02d' "$((seconds / 3600))" "$(((seconds % 3600) / 60))" "$((seconds % 60))"
}

[[ -z "$restart_cmd" ]] && echo "WARNING: no --restart-cmd, so every scenario after the first" \
                                "runs against whatever state the one before it left." >&2

# rx_fifo_errors and tx_dropped: the counters that say whether the measurement is the broker's
# or the client's own losses.
nic_counters() {
  [[ -z "$nic" ]] && return 0
  awk -v want="$nic:" '$1 == want { print $6, $13 }' /proc/net/dev
}

# How far the run got, from the progress markers: a run that ends at 85% has aborted, whatever
# tables it left. This is what a "did not complete" looks like from outside - the client prints
# 'Not connected' over its own progress bar and exits - and reading the tables alone cannot see it.
# Silent when the run was made without --progress: then only the exit code is left to go on.
progress_shortfall() {
  local log="$1"
  tr '\r' '\n' < "$log" | grep -oE '\([0-9]+/[0-9]+\)' | tail -1 |
    awk -F'[(/)]' '$2 + 0 < $3 + 0 { printf "%s of %s", $2, $3 }'
}

# The interval median: the run average is dragged by the ramp-up and the tail-off, and scoring by
# it once voided a day of measurements. xmq_scn prints it itself, as a Median line under the table,
# and that is the number used - one definition, the client's. A client from before that line
# existed gets the old reckoning here, with the first and last row dropped, and says so in its
# fifth field so the record can tell which number it holds.
summarise() {
  local log="$1"
  tr '\r' '\n' < "$log" |
  awk '
    # The run prints a table per phase - connecting the publishers, connecting the subscribers,
    # then publishing - and only the last one is the measurement. Starting over at each heading
    # leaves exactly that one: publishing is always last, because it cannot begin until the
    # clients are connected. Before scenarios paced their connections there was only ever one
    # table, and reading them all together gave a median mixed from connect and publish latencies.
    /^Scenario:/ { n = 0; reported = ""; next }
    /^[0-9]+ms[ \t]+[0-9]+[ \t]+[0-9]+us[ \t]+[0-9]+[ \t]*$/ {
      v = $3; sub("us", "", v); latency[n++] = v + 0
    }
    /^Median[ \t]+[0-9]+us[ \t]*$/ { reported = $2; sub("us", "", reported) }
    END {
      if (n == 0) { print "no-result"; exit }
      if (reported != "") {
        low = latency[0]; high = latency[0]
        for (i = 1; i < n; i++) { if (latency[i] < low) low = latency[i]; if (latency[i] > high) high = latency[i] }
        printf "%d %d %d %d %s\n", reported, n, low, high, "xmq_scn"
        exit
      }
      first = (n > 2) ? 1 : 0
      last  = (n > 2) ? n - 2 : n - 1
      m = 0
      for (i = first; i <= last; i++) sample[m++] = latency[i]
      for (i = 1; i < m; i++) {          # insertion sort: m is ten, not ten thousand
        v = sample[i]
        for (j = i - 1; j >= 0 && sample[j] > v; j--) sample[j + 1] = sample[j]
        sample[j + 1] = v
      }
      median = (m % 2) ? sample[int(m / 2)] : int((sample[m / 2 - 1] + sample[m / 2]) / 2)
      printf "%d %d %d %d %s\n", median, m, sample[0], sample[m - 1], (n > 2 ? "dropped" : "kept")
    }'
}

# The scenario's own table, with the name the set file calls it by rather than the one the
# scenario file calls itself: a record is read against the previous version's file, and the two
# line up by file name.
extract_block() {
  local log="$1" name="$2"
  # Held and printed at the end, for the same reason the median is taken from the last table only:
  # the phases before publishing have tables of their own, and the measurement is the last one.
  tr '\r' '\n' < "$log" | awk -v name="$name" '
    /^Scenario:/ { count = 0; block[count++] = "Scenario: " name; started = 1; next }
    !started { next }
    /^Interval[ \t]+Count/ { block[count++] = $0; next }
    /^[0-9]+ms[ \t]+[0-9]+[ \t]+[0-9]+us[ \t]+[0-9]+[ \t]*$/ { block[count++] = $0; next }
    /^Average[ \t]/ { block[count++] = $0; next }
    /^Median[ \t]/ { block[count++] = $0; next }
    length($0) > 0 && $0 !~ /[A-Za-z0-9]/ { block[count++] = $0 }
    END { for (i = 0; i < count; i++) print block[i] }'
}

# The broker's memory over the measurement, one row per interval of the latency table: the most it
# held during that interval. xmq_scn prints no times, so the intervals are placed by the end of the
# run - the measurement is the last table, and it ends as the run does - which puts them within the
# few seconds the clients take to disconnect. Nothing is printed without samples or a table.
memory_block() {
  local block_file="$1" samples_file="$2" ended="$3"
  [[ -s "$samples_file" && -s "$block_file" ]] || return 0
  awk -v ended="$ended" '
    FNR == NR { if (NF >= 3) { rss[++n] = $2; at[n] = $3 } next }
    /^[0-9]+ms[ \t]/ { sub(/ms$/, "", $1); row[++rows] = $1 + 0 }
    END {
      if (rows < 2 || n == 0) exit
      step = row[2] - row[1]
      start = ended - (row[rows] + step) / 1000
      print ""
      print "Memory"
      print "Interval            RSS"
      for (r = 1; r <= rows; r++) {
        from = start + row[r] / 1000; to = from + step / 1000; peak = 0
        for (i = 1; i <= n; i++) if (at[i] >= from && at[i] < to && rss[i] > peak) peak = rss[i]
        if (peak > 0) printf "%-12s %10d Mb\n", row[r] "ms", peak / 1024
      }
    }' "$samples_file" "$block_file"
}

# The broker's RSS as the interval table's last column, rather than a table of its own: one row
# per interval, read alongside the latency it went with.
add_memory_column() {
  local block_file="$1" samples_file="$2" ended="$3"
  local memory
  memory="$(memory_block "$block_file" "$samples_file" "$ended")"
  [[ -n "$memory" ]] || return 0
  awk -v memory="$memory" '
    BEGIN {
      n = split(memory, lines, "\n")
      for (i = 1; i <= n; i++)
        if (lines[i] ~ /^[0-9]+ms[ \t]/) { split(lines[i], f, /[ \t]+/); rss[f[1]] = f[2] " " f[3] }
    }
    /^Interval[ \t]+Count/ { printf "%s%11s\n", $0, "RSS"; next }
    /^[0-9]+ms[ \t]+[0-9]+[ \t]+[0-9]+us/ { if ($1 in rss) printf "%s%11s\n", $0, rss[$1]; else print; next }
    /^─+$/ { print $0 "───────────"; next }
    { print }' "$block_file" > "$block_file.tmp" && mv "$block_file.tmp" "$block_file"
}

# A scenario that did not finish is still a result: the intervals it got through, the broker's CPU
# and memory, and the RSS row by row show where it went wrong - a broker that ran out of memory, or a
# latency that kept growing. Recorded in the same form as a finished one, under a line that says it
# did not finish, so that every file reads alike.
record_unfinished() {
  local name="$1" status="$2"
  [[ -z "$record" ]] && return 0
  {
    extract_block "$log" "$name" | grep -q . || echo "Scenario: $name"
    extract_block "$log" "$name"
    echo "  $status"
    [[ -n "${broker_stats:-}" ]] && echo "$broker_stats"
  } > "$out_dir/$name.block"
  add_memory_column "$out_dir/$name.block" "$out_dir/$name.samples" "$run_ended_epoch"
  blocks+=("$out_dir/$name.block")
}

# Rewritten from scratch after every scenario, so that a series interrupted at hour two still
# leaves a record of the hours before it.
write_record() {
  [[ -z "$record" ]] && return 0
  {
    echo "Server:   $broker"
    echo "Version:  $version"
    echo "Host:     ${server_note:-${wait_host:-unknown}}"
    echo "Client:   ${client_note:-$(hostname), xmq_scn $(xmq_scn --version 2>/dev/null | head -1)}"
    echo "Date:     $(date +%Y-%m-%d)"
    echo "Started:  $set_started_at"
    if (( set_finished )); then
      echo "Total execution time: $(format_elapsed "$((set_finished_epoch - set_started_epoch))")"
    else
      echo "Elapsed so far: $(format_elapsed "$(($(date +%s) - set_started_epoch))")"
    fi
    if (( ${#notes[@]} )); then
      echo
      printf '%s\n' "${notes[@]}"
    fi
    echo
    if [[ -n "$restart_cmd" ]]; then
      echo "Broker restarted before each scenario."
    else
      echo "NOT restarted between scenarios: each ran against the state the one before it left."
    fi
    echo
    if (( ${median_reckoned_here:-0} )); then
      echo "Summary - interval medians, first and last row dropped:"
    else
      echo "Summary - interval medians, as xmq_scn reports them:"
    fi
    echo
    # The header is copied rather than computed: it has to match the files already in
    # the earlier hand-written records character for character, and those were laid out by hand.
    echo "  Scenario                              Median   Samples"
    printf "  %s\n" "──────────────────────────────────────────────────────"
    local line name median samples
    for line in "${summary[@]}"; do
      IFS='|' read -r name median samples <<<"$line"
      if [[ -z "$samples" ]]; then
        printf "  %-36s %s\n" "$name" "$median"
      else
        printf "  %-36s %8s %9s\n" "$name" "$median" "$samples"
      fi
    done
    if (( ${#scenario_times[@]} )); then
      echo
      echo "Scenario execution time (including setup and restart):"
      for line in "${scenario_times[@]}"; do
        IFS='|' read -r name seconds <<<"$line"
        printf '  %-36s %s\n' "$name" "$(format_elapsed "$seconds")"
      done
    fi
    # Three blank lines after the summary, one between scenarios: the spacing the files in
    # the records already use.
    local block first=1
    for block in "${blocks[@]}"; do
      if (( first )); then echo; echo; echo; first=0; else echo; fi
      cat "$block"
    done
  } > "$record"
}

# The broker's own cost, sampled on the host it runs on. Two functions rather than one because
# the samples have to be taken while the scenario runs and read after it ends.
#
# It reads /proc directly rather than asking ps for %cpu: ps reports an average over the whole life
# of the process, and a broker started before the run and left running after it is not the run.
stats_dir=/tmp/xmq_broker_stats

start_broker_stats() {
  [[ -z "$stats_host" ]] && return 0
  ssh "$stats_host" "PROCESS='$stats_process' DIR='$stats_dir' bash -s" <<'SAMPLER' >/dev/null 2>&1
    # Whatever the last scenario left behind goes first, or two samplers write one file.
    [ -f "$DIR/pid" ] && kill "$(cat "$DIR/pid")" 2>/dev/null
    rm -rf "$DIR"; mkdir -p "$DIR"
    pid=$(pgrep -x "$PROCESS" | head -1)
    [ -n "$pid" ] || { echo "no process named $PROCESS" > "$DIR/error"; exit 0; }
    hz=$(getconf CLK_TCK)
    setsid bash -c '
      pid=$1; hz=$2; dir=$3
      prev=$(awk "{print \$14 + \$15}" "/proc/$pid/stat" 2>/dev/null)
      while [ -d "/proc/$pid" ]; do
        sleep 2
        now=$(awk "{print \$14 + \$15}" "/proc/$pid/stat" 2>/dev/null) || break
        [ -n "$now" ] || break
        rss=$(awk "/VmRSS/{print \$2}" "/proc/$pid/status" 2>/dev/null)
        echo "$(( (now - prev) * 100 / hz / 2 )) ${rss:-0} $(date +%s)" >> "$dir/samples"
        prev=$now
      done
    ' _ "$pid" "$hz" "$DIR" >/dev/null 2>&1 &
    echo $! > "$DIR/pid"
SAMPLER
}

# Prints "server CPU" and "server RSS" lines for the scenario's block, or nothing when not sampling.
# The samples themselves - CPU, RSS in kB, time - are kept in $1 for the memory block.
read_broker_stats() {
  local samples_file="$1"
  [[ -z "$stats_host" ]] && return 0
  ssh "$stats_host" "DIR='$stats_dir' bash -s" <<'READER' 2>/dev/null > "$samples_file"
    [ -f "$DIR/pid" ] && kill "$(cat "$DIR/pid")" 2>/dev/null
    [ -f "$DIR/samples" ] && cat "$DIR/samples"
    rm -rf "$DIR"
READER
    awk '
      # The first sample spans the gap between the sampler starting and the scenario starting, so
      # it covers the connect ramp rather than the measurement. Dropped, like the first row of a
      # result table.
      NR > 1 { total += $1; if ($1 > peakCpu) peakCpu = $1; if ($2 > peakRss) peakRss = $2; n++ }
      END {
        if (n > 0) {
          printf "  server CPU   mean %d%%, peak %d%%\n", total / n, peakCpu
          printf "  server RSS   %d Mb peak\n", peakRss / 1024
        }
      }' "$samples_file"
}

declare -a summary
declare -a blocks
declare -a scenario_times
failed=0

for scenario in "${scenarios[@]}"; do
  scenario_started_epoch=$(date +%s)
  name="${scenario%.json}"
  log="$out_dir/$name.log"

  if [[ -n "$setup_cmd" ]]; then
    echo "=== $(date +%H:%M:%S) $name: setup"
    if ! XMQ_SCENARIO="$scenario" eval "$setup_cmd"; then
      echo "    setup failed, skipping $name" >&2
      summary+=("$name|setup failed")
      scenario_times+=("$name|$(($(date +%s) - scenario_started_epoch))")
      write_record
      failed=1
      continue
    fi
  fi

  if [[ -n "$restart_cmd" ]]; then
    echo "=== $(date +%H:%M:%S) $name: restarting the broker"
    if ! eval "$restart_cmd"; then
      echo "    restart failed, skipping $name" >&2
      summary+=("$name|restart failed")
      scenario_times+=("$name|$(($(date +%s) - scenario_started_epoch))")
      write_record
      failed=1
      continue
    fi
    if [[ -n "$wait_host" ]]; then
      up=0
      for _ in $(seq 1 60); do
        if timeout 2 bash -c ": >/dev/tcp/$wait_host/$wait_port" 2>/dev/null; then up=1; break; fi
        sleep 1
      done
      if [[ "$up" -eq 0 ]]; then
        echo "    $wait_host:$wait_port never accepted, skipping $name" >&2
        summary+=("$name|broker did not come back")
        scenario_times+=("$name|$(($(date +%s) - scenario_started_epoch))")
        write_record
        failed=1
        continue
      fi
    fi
  fi

  echo "=== $(date +%H:%M:%S) $name: running"
  run_started="$(date '+%Y-%m-%d %H:%M:%S')"
  read -r nic_rx_before nic_tx_before <<<"$(nic_counters)"
  start_broker_stats
  bash "$SCRIPT_DIR/run_load_test.sh" "${passthrough[@]}" "${scenario_path[$scenario]}" > "$log" 2>&1
  rc=$?
  run_ended_epoch=$(date +%s)
  broker_stats="$(read_broker_stats "$out_dir/$name.samples")"

  # A machine that slept through part of a run produces a plausible-looking log and a wrong
  # number - or a failure that looks like the broker's. Ask the kernel, not the log.
  if journalctl -k --since "$run_started" --no-pager 2>/dev/null | grep -q "PM: suspend entry"; then
    echo "=== $(date +%H:%M:%S) $name: INVALID - this machine suspended during the run"
    summary+=("$name|invalid: client suspended")
    scenario_times+=("$name|$(($(date +%s) - scenario_started_epoch))")
    write_record
    failed=1
    continue
  fi

  # The client's own exit code, which this script used to collect and never look at. The 0.9.16
  # record's 100k/s row was written from a run that exited 1: it aborted at 90% of publishing, and
  # the 450us recorded for it is the connect phase.
  if (( rc != 0 )); then
    echo "=== $(date +%H:%M:%S) $name: FAILED (exit $rc), see $log"
    tr '\r' '\n' < "$log" | grep -viE 'connecting|publishing|^$' | tail -3 | sed 's/^/    /'
    summary+=("$name|failed (exit $rc)")
    record_unfinished "$name" "FAILED (exit $rc) - the intervals above are those completed before it stopped"
    scenario_times+=("$name|$(($(date +%s) - scenario_started_epoch))")
    write_record
    failed=1
    continue
  fi

  if short="$(progress_shortfall "$log")"; [[ -n "$short" ]]; then
    echo "=== $(date +%H:%M:%S) $name: INCOMPLETE - stopped at $short, see $log"
    summary+=("$name|incomplete: $short")
    record_unfinished "$name" "INCOMPLETE - stopped at $short; the intervals above are those completed"
    scenario_times+=("$name|$(($(date +%s) - scenario_started_epoch))")
    write_record
    failed=1
    continue
  fi

  read -r median samples low high dropped <<<"$(summarise "$log")"
  if [[ "$median" == "no-result" ]]; then
    echo "=== $(date +%H:%M:%S) $name: NO RESULT LINE (exit $rc), see $log"
    tr '\r' '\n' < "$log" | grep -viE 'connecting|publishing|^$' | tail -3 | sed 's/^/    /'
    summary+=("$name|no result (exit $rc)")
    record_unfinished "$name" "NO RESULT LINE (exit $rc) - the intervals above are those completed"
    scenario_times+=("$name|$(($(date +%s) - scenario_started_epoch))")
    write_record
    failed=1
    continue
  fi

  note="(first and last dropped)"
  [[ "$dropped" == "kept" ]] && note="(too few intervals to drop any)"
  if [[ "$dropped" == "xmq_scn" ]]; then
    note="(as xmq_scn reports it)"
  else
    median_reckoned_here=1
  fi
  echo "=== $(date +%H:%M:%S) $name: median ${median}us over $samples samples $note, range ${low}-${high}us"
  summary+=("$name|${median}us|$samples")

  nic_line=""
  if [[ -n "$nic" ]]; then
    read -r nic_rx_after nic_tx_after <<<"$(nic_counters)"
    rx_lost=$(( ${nic_rx_after:-0} - ${nic_rx_before:-0} ))
    tx_lost=$(( ${nic_tx_after:-0} - ${nic_tx_before:-0} ))
    if (( rx_lost == 0 && tx_lost == 0 )); then
      nic_line="  client NIC $nic: no dropped packets during the run"
    else
      nic_line="  WARNING: client NIC $nic dropped $rx_lost received and $tx_lost sent packet(s) during this run - part of this latency is the client's own"
      echo "    $nic_line"
    fi
  fi

  {
    extract_block "$log" "$name"
    echo "  interval median ${median}us over $samples samples $note, range ${low}-${high}us"
    [[ -n "$broker_stats" ]] && echo "$broker_stats"
    [[ -n "$nic_line" ]] && echo "$nic_line"
  } > "$out_dir/$name.block"
  add_memory_column "$out_dir/$name.block" "$out_dir/$name.samples" "$run_ended_epoch"
  blocks+=("$out_dir/$name.block")
  scenario_times+=("$name|$(($(date +%s) - scenario_started_epoch))")
  write_record
done

set_finished_epoch=$(date +%s)
set_finished=1
write_record

echo
echo "  Scenario                              Median   Samples"
echo "  ──────────────────────────────────────────────────────"
for line in "${summary[@]}"; do
  IFS='|' read -r name median samples <<<"$line"
  printf "  %-36s %8s %9s\n" "$name" "$median" "${samples:-}"
done
echo
echo "Logs: $out_dir"
[[ -n "$record" ]] && echo "Record: $record"
echo "Total execution time: $(format_elapsed "$((set_finished_epoch - set_started_epoch))")"
exit $failed
