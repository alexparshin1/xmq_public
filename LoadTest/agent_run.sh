#!/usr/bin/env bash
# Start a bench run from an agent session and return immediately.
#
#   ./agent_run.sh --version 1.1.2 --bench-dir bench-1.1.2 --file
#
# Why this exists: an agent on this host runs its model on this host, and this host is the load
# client. While the agent is generating tokens, the client is busy - and a set that takes forty
# minutes cannot be held inside one tool call anyway. So the run is started detached, its output
# goes to a log file, and the agent's turn ends. The agent then reads the log and the record when
# it is asked again, instead of pretending an answer it never saw.
#
# Everything after this script's own options goes to stand_run.sh unchanged, so the two cannot
# disagree about what a run is. stand_run.sh is still the thing that writes the record: this only
# starts it in the background and tells you where to look.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    cat <<'EOF'
Usage: agent_run.sh [own options] [stand_run.sh options]

  Starts stand_run.sh detached, prints where its output and status are, and exits at once.
  The agent must end its turn here: every token it generates afterwards is CPU taken from the
  load client while the run is measuring.

Own options:
  --wait                 Run in the foreground instead of detaching. Only for a person watching;
                         an agent that does this holds a tool call open for the whole set.
  --log FILE             Log file for the run (default: results/raw/agent-<date-time>.log).
  -h, --help             This.

Everything else - --version, --bench-dir, --set, --file, --dry-run - is passed to stand_run.sh.
--version and --bench-dir are still required; read ./stand_run.sh --help for the rest.

Exit status: 0 once the run is started (or once a --wait run finished with its status), non-zero
if it cannot be started at all. The run's own exit status is written to <log>.status and, when
the run ends, printed there.
EOF
}

log=""
wait_mode=0
declare -a forward=()

while (( $# )); do
    case "$1" in
        --wait)     wait_mode=1; shift ;;
        --log)      log="$2"; shift 2 ;;
        -h|--help)  usage; exit 0 ;;
        *)          forward+=("$1"); shift ;;
    esac
done

if pgrep -x xmq_scn >/dev/null 2>&1; then
    echo "agent_run: xmq_scn is already running - another load test is in progress" >&2
    exit 1
fi

# A lock rather than a pattern match: pgrep -f would match any shell whose command line mentions
# stand_run.sh, including the one that called this script. It lives outside the repository so a
# second checkout cannot start a second set against the same server.
lock="${TMPDIR:-/tmp}/xmq-stand-run.lock"
exec 9>"$lock" || { echo "agent_run: cannot open $lock" >&2; exit 1; }
if ! flock -n 9; then
    echo "agent_run: another run holds $lock - wait for it, or read its log" >&2
    exit 1
fi

# The record path is decided by stand_run.sh from --version, so name the log the same way and
# print both: the agent should be able to say where the result will be before there is one.
version=""
for ((i = 0; i < ${#forward[@]}; i++)); do
    case "${forward[i]}" in
        --version) version="${forward[i + 1]:-}" ;;
        --version=*) version="${forward[i]#--version=}" ;;
    esac
done

stamp=$(date +%Y-%m-%d-%H%M)
[[ -n "$log" ]] || log="$SCRIPT_DIR/results/raw/agent-$stamp-xmq${version:-unknown}.log"
mkdir -p "$(dirname "$log")" || { echo "agent_run: cannot create $(dirname "$log")" >&2; exit 1; }
status_file="$log.status"

if (( wait_mode )); then
    "$SCRIPT_DIR/stand_run.sh" "${forward[@]}" 2>&1 | tee "$log"
    echo "exit=${PIPESTATUS[0]} finished=$(date '+%Y-%m-%d %H:%M:%S')" >"$status_file"
    exit "$(sed -n 's/^exit=\([0-9]*\).*/\1/p' "$status_file")"
fi

: >"$status_file"
setsid nohup bash -c '
    log=$1 status=$2 runner=$3; shift 3
    started=$SECONDS
    "$runner" "$@" >"$log" 2>&1
    rc=$?
    printf "exit=%s elapsed=%ss finished=%s\n" "$rc" "$((SECONDS - started))" \
        "$(date "+%Y-%m-%d %H:%M:%S")" >"$status"
' _ "$log" "$status_file" "$SCRIPT_DIR/stand_run.sh" "${forward[@]}" >/dev/null 2>&1 </dev/null &

echo "started: pid $! (stand_run.sh detached)"
echo "log:     $log"
echo "status:  $status_file   (exit code and duration appear here when the set ends)"
echo "record:  results/raw/$(date +%Y-%m-%d-%H%M)-xmq${version:-<version>}-bench/record.txt"
echo
echo "stop reading here: end the turn. The client must stay idle while the set runs."
