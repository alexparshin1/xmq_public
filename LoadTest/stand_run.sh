#!/usr/bin/env bash
# One bench run on the home stand, from the client host: thinker10 drives, thinker11 serves.
#
# Why this exists: the bench procedure is a dozen decisions - which bench profile is installed on
# the server, which restart command, which interface the client binds from, where the record goes,
# which version string it claims. Every one of them has been got wrong by hand at least once, and
# a record that names the wrong version or the wrong profile is worse than no record: it is
# comparable-looking and wrong. This script makes those decisions once, in one place: the version is
# the one thing a caller normally supplies, and the bench profile is taken to be bench-<version> on
# the server unless --bench-dir says otherwise.
#
#   ./stand_run.sh --version 1.1.2
#   ./stand_run.sh --version 1.1.2 --file
#   ./stand_run.sh --version 1.1.2 --dry-run
#   ./stand_run.sh --version 1.1.2 --bench-dir bench-1.1.2-smoke   # profile not named after it
#
# It runs the whole set (Scenario-Set-Bench.txt by default), restarts the broker before every
# scenario, waits for it to carry a message, samples its CPU and RSS, watches the client NIC,
# writes a record, and - with --file - files the record into results/Bench/<version>/.
#
# It does not decide whether the numbers are good. Read the summary it prints at the end and
# compare with results/Bench/<previous version>/ before filing by hand; --file is there for the
# case where that comparison has already been made.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

version=""
bench_dir=""
set_file="$SCRIPT_DIR/Scenario-Set-Bench.txt"
server="thinker11"
nic="enp86s0"
subnet="10.1.1"
first_octet="150"
count="29"
user="user"
password="Secret#123"
out_dir=""
server_note=""
client_note=""
setup_cmd=""
file_results=0
dry_run=0
skip_preflight=0
skip_readiness=0
stop_other_brokers=1
declare -a notes=()

usage() {
    cat <<EOF
Usage: $(basename "$0") --version V [options]

  --version V            XMQ version this run measures, e.g. 1.1.2. Required: it goes into the
                         record header, names the results directory the record is filed under, and
                         names the bench profile on the server.
  --bench-dir DIR        Bench profile directory on the server that holds restart.sh and
                         xmq_server.conf (default: bench-<version>, e.g. bench-1.1.2). It decides
                         what the server actually runs, and nothing on the client can see that, so
                         pass it whenever the profile is not named after the version.

Options:
  --set FILE             Scenario set to run (default: Scenario-Set-Bench.txt next to this script).
  --server HOST          Server host (default: thinker11).
  --nic IFACE            Client NIC to watch for drops (default: enp86s0).
  --subnet NET           Client source subnet (default: 10.1.1).
  --first OCTET          First host octet in that subnet (default: 150).
  --count N              Number of source addresses (default: 29).
  --user NAME            Broker account (default: user).
  --password PASS        Broker password (default: Secret#123).
  --out DIR              Per-scenario log directory (default: results/raw/<date-time>-xmq<V>-bench).
  --server-note TEXT     Record's Host: line. Default describes the version and profile; pass the
                         previous record's Host line to keep a series comparable.
  --client-note TEXT     Record's Client: line. Default: this host, the bound-address count, xmq_scn.
  --setup-cmd CMD        Passed to run_scenario_set.sh --setup-cmd, with \$XMQ_SCENARIO set.
  --note TEXT            Free-text line for the record; repeatable.
  --file                 File the record with file_results.py --env Bench when the set succeeds.
  --skip-preflight       Do not run preflight.sh on either host.
  --skip-readiness       Do not run stand_readiness.sh (persistence off, Release builds). Use only
                         for an intentionally different configuration, never for a published set.
  --keep-other-brokers   Refuse to start if any broker on the server is not this bench profile.
                         By default such a broker - the systemd unit, or one left in
                         ~/.local/bin - is stopped first: the profile's restart.sh kills
                         xmq_server by name anyway, and one that keeps 1883 would be measured
                         while the record named the bench version. A systemd unit is stopped
                         through systemd, because Restart=always would bring it back.
  --stop-other-brokers   Accepted for compatibility; this is the default.
  --dry-run              Print what would run and exit; nothing is started.
  -h, --help             This.

Exit status: 0 only if the set ran to the end, wrote a record and every scenario produced a
result. A failed scenario, an unreachable server or a missing bench profile exits non-zero.
EOF
}

die() { echo "stand_run: $*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version)     version="$2"; shift 2 ;;
        --bench-dir)   bench_dir="$2"; shift 2 ;;
        --set)         set_file="$2"; shift 2 ;;
        --server)      server="$2"; shift 2 ;;
        --nic)         nic="$2"; shift 2 ;;
        --subnet)      subnet="$2"; shift 2 ;;
        --first)       first_octet="$2"; shift 2 ;;
        --count)       count="$2"; shift 2 ;;
        --user)        user="$2"; shift 2 ;;
        --password)    password="$2"; shift 2 ;;
        --out)         out_dir="$2"; shift 2 ;;
        --server-note) server_note="$2"; shift 2 ;;
        --client-note) client_note="$2"; shift 2 ;;
        --setup-cmd)   setup_cmd="$2"; shift 2 ;;
        --note)        notes+=("$2"); shift 2 ;;
        --file)        file_results=1; shift ;;
        --skip-preflight) skip_preflight=1; shift ;;
        --skip-readiness) skip_readiness=1; shift ;;
        --stop-other-brokers) stop_other_brokers=1; shift ;;
        --keep-other-brokers) stop_other_brokers=0; shift ;;
        --dry-run)     dry_run=1; shift ;;
        -h|--help)     usage; exit 0 ;;
        *)             echo "stand_run: unknown option $1" >&2; usage >&2; exit 2 ;;
    esac
done

[[ -n "$version" ]]   || { usage >&2; die "--version is required"; }
# The profile is named after the version unless told otherwise: that is the convention on the
# server, and a run that names one profile while measuring another is the mistake this script
# exists to prevent.
bench_dir_note=""
[[ -n "$bench_dir" ]] || { bench_dir="bench-$version"; bench_dir_note=" (from --version)"; }
[[ -r "$set_file" ]]  || die "cannot read set file: $set_file"

[[ "$(hostname -s)" == "thinker10" ]] ||
    echo "stand_run: WARNING: this is $(hostname -s), not thinker10; the client notes will say so" >&2

# The bench profile is the only thing that decides what the server runs, so it is checked before
# anything is started rather than discovered at the first restart, twenty minutes in.
remote_restart="~/$(basename "$bench_dir")/restart.sh"
if ! ssh -o BatchMode=yes -o ConnectTimeout=10 "$server" "test -x $remote_restart"; then
    die "no executable $remote_restart on $server$bench_dir_note: build the profile there first (cmake . && make -j8 install, then the per-version bench directory)"
fi
if ! ssh -o BatchMode=yes -o ConnectTimeout=10 "$server" "grep -q '^' ~/$(basename "$bench_dir")/xmq_server.conf"; then
    die "no configuration file in $bench_dir on $server"
fi

# The extensions are read from xmq_extensions.conf and xmq_extensions.d *beside the configuration
# file*, not from the installed directory. A profile that has its own xmq_server.conf but no
# extension config there has no authenticator, and with anonymous connections off it admits
# nobody: the broker answers on 1883, the port wait passes, and every scenario fails on login.
# That is silent for forty minutes, so it is refused here instead.
profile_would_authenticate=$(ssh -o BatchMode=yes -o ConnectTimeout=10 "$server" bash -s -- \
        "$(basename "$bench_dir")" <<'EOF'
set -u
d="$HOME/$1"
if grep -q '"allow_anonymous"[[:space:]]*:[[:space:]]*true' "$d/xmq_server.conf" 2>/dev/null; then
    echo anonymous; exit 0
fi
[ -s "$d/xmq_extensions.conf" ] && { echo yes; exit 0; }
for f in "$d"/xmq_extensions.d/*.conf; do
    [ -s "$f" ] && { echo yes; exit 0; }
done
echo no
EOF
)
if [[ "$profile_would_authenticate" == "no" ]]; then
    echo "stand_run: $bench_dir on $server has no authenticator: no xmq_extensions.conf and no" >&2
    echo "stand_run: non-empty xmq_extensions.d beside its xmq_server.conf, and anonymous" >&2
    echo "stand_run: connections are off. The broker would listen and refuse every client." >&2
    die "add the node's extension config to ~/$bench_dir (see STAND-RUNBOOK.md, 'The bench profile')"
fi

# A recorded figure needs the broker's CPU and memory next to it, and a run measured while
# another client was publishing measures neither. run_scenario_set.sh refuses the second of
# those on its own; this is the first.
pgrep -x xmq_scn >/dev/null && die "xmq_scn is already running - another load test is in progress"

# Port 1883 belongs to the bench broker for the duration of the run, and the profile's restart.sh
# kills xmq_server by name, so another broker there is stopped either way. What this decides is
# whether that happens deliberately and is written into the record, or happens by accident on the
# first restart. A broker that keeps 1883 - the systemd unit, or one left in ~/.local/bin - makes
# the set measure it while the record names the version in the bench directory.
systemd_active=0
ssh -o BatchMode=yes -o ConnectTimeout=10 "$server" 'systemctl is-active --quiet xmq_server' >/dev/null 2>&1 \
    && systemd_active=1
others=$(ssh -o BatchMode=yes -o ConnectTimeout=10 "$server" \
    "pgrep -af xmq_server 2>/dev/null | grep -v '$(basename "$bench_dir")/' | grep -v pgrep | grep -v 'bash -c' || true")
if [[ -n "$others" ]] || (( systemd_active )); then
    if (( ! stop_other_brokers )); then
        (( systemd_active )) && echo "stand_run: the xmq_server systemd unit is active on $server" >&2
        [[ -n "$others" ]] && { echo "stand_run: another broker is running on $server and would answer on 1883:" >&2
                                echo "$others" | sed 's/^/  /' >&2; }
        echo "stand_run: drop --keep-other-brokers to stop it, or stop it yourself." >&2
        exit 1
    fi
    echo "== stopping other brokers on $server =="
    (( systemd_active )) && echo "  systemd unit xmq_server is active (Restart=always: SIGTERM alone would only delay it)"
    [[ -n "$others" ]] && echo "$others" | sed 's/^/  /'
    if (( dry_run )); then
        echo "  dry run: would stop"
    else
        # A unit with Restart=always comes back five seconds after its process is killed, so a
        # systemd-owned broker has to go through systemd; only a stray process takes a signal.
        if (( systemd_active )); then
            ssh -o BatchMode=yes "$server" \
                'sudo -n systemctl stop xmq_server 2>/dev/null || systemctl --user stop xmq_server 2>/dev/null' \
                || die "the xmq_server systemd unit is active on $server and could not be stopped"
            notes+=("the xmq_server systemd unit on $server was stopped for the run")
        fi
        ssh -o BatchMode=yes "$server" 'pkill -TERM -x xmq_server' >/dev/null 2>&1 || true
        for _ in $(seq 1 30); do
            ssh -o BatchMode=yes "$server" 'pgrep -x xmq_server' >/dev/null 2>&1 || break
            sleep 1
        done
        if ssh -o BatchMode=yes "$server" 'pgrep -x xmq_server' >/dev/null 2>&1; then
            ssh -o BatchMode=yes "$server" 'pkill -KILL -x xmq_server' >/dev/null 2>&1 || true
            sleep 2
        fi
        if ssh -o BatchMode=yes "$server" 'pgrep -x xmq_server' >/dev/null 2>&1; then
            die "still running on $server after SIGTERM and SIGKILL - stop it by hand"
        fi
        if [[ -n "$others" ]]; then
            notes+=("other brokers on $server were stopped before the run")
        fi
    fi
fi

bound_addresses=$(ip -o addr show dev "$nic" 2>/dev/null | grep -c "inet $subnet\." || true)
if (( bound_addresses < count )); then
    echo "stand_run: WARNING: $bound_addresses address(es) in $subnet.x on $nic, need $count" >&2
    echo "stand_run:          run ./make_ip_addresses.sh --iface $nic --subnet $subnet --first $first_octet --count $count" >&2
fi

if (( ! skip_readiness )); then
    # Unlike preflight, these refuse the run: an enabled persistence or a Debug client changes the
    # numbers by tens of percent and looks exactly like a broker regression.
    "$SCRIPT_DIR/stand_readiness.sh" --server "$server" --set "$set_file" || exit 1
fi

if (( ! skip_preflight )); then
    # Preflight names the limits that silently corrupted a run before. It is reported, not
    # enforced: the two checks this stand is known to miss - 55536 source ports per address
    # on the client and cleared NOTRACK rules on the server - print as NOTE, and a NOTE is
    # never a reason to reconfigure a host or to refuse to start.
    echo "== preflight =="
    for target in "" "-H $server -r server"; do
        # shellcheck disable=SC2086
        output=$("$SCRIPT_DIR/preflight.sh" $target 2>&1)
        echo "$output" | grep -E "FAIL|NOTE|warn|===" || true
    done
    echo "  (NOTE = known state on this stand, run proceeds; only FAIL needs attention)"
fi

if [[ -z "$out_dir" ]]; then
    out_dir="$SCRIPT_DIR/results/raw/$(date +%Y-%m-%d-%H%M)-xmq$version-bench"
fi
mkdir -p "$out_dir" || die "cannot create $out_dir"

record="$out_dir/record.txt"
[[ -n "$server_note" ]] || server_note="$server; XMQ $version; bench profile $bench_dir; MQTT1883"
if [[ -z "$client_note" ]]; then
    client_note="$(hostname -s), $bound_addresses bound source addresses, xmq_scn $version"
fi

restart_cmd="ssh -o BatchMode=yes $server $remote_restart"

cmd=("$SCRIPT_DIR/run_scenario_set.sh" "$set_file"
     --restart-cmd "$restart_cmd"
     --wait-for "$server:1883"
     --stats-host "$server"
     --nic "$nic"
     --out "$out_dir"
     --record "$record"
     --version "$version"
     --server-note "$server_note"
     --client-note "$client_note")
[[ -n "$setup_cmd" ]] && cmd+=(--setup-cmd "$setup_cmd")
for note in "${notes[@]+"${notes[@]}"}"; do cmd+=(--note "$note"); done
cmd+=(-- --skip-ip-setup --subnet "$subnet" --first "$first_octet" --count "$count"
      -h "$server" -u "$user" -P "$password")

echo "== run =="
echo "  version    : $version"
echo "  profile    : $bench_dir on $server"
echo "  set        : $set_file"
echo "  logs       : $out_dir"
echo "  record     : $record"
echo "  restart    : $restart_cmd"

if (( dry_run )); then
    echo "== dry run, not starting =="
    printf '  %q ' "${cmd[@]}"
    echo
    exit 0
fi

started=$(date +%s)
"${cmd[@]}"
status=$?
elapsed=$(( $(date +%s) - started ))

if (( status != 0 )); then
    echo "stand_run: the set failed after $((elapsed / 60))m$((elapsed % 60))s; logs are in $out_dir" >&2
    [[ -r "$record" ]] && tail -20 "$record" >&2
    exit "$status"
fi
[[ -r "$record" ]] || die "the set reported success but wrote no record at $record"

echo "== result ($((elapsed / 60))m$((elapsed % 60))s) =="
sed -n '/^Summary/,$p' "$record" | head -20
echo "  record: $record"

if (( file_results )); then
    "$SCRIPT_DIR/file_results.py" --env Bench "$record" || die "file_results.py refused the record"
fi
