#!/usr/bin/env bash
# One command: check the stand, run the whole bench set, file it, and compare with the 1.1.2
# baseline. The optional argument is the XMQ version under test; with no argument the version is
# read from the source checkout on thinker10 (XMQ_SOURCE_DIR overrides its path).
#
#   ./regression.sh            # version from ~/workspace/xmq_public/VERSION.txt
#   ./regression.sh 1.2.0      # explicit version
#
# The bench profile is bench-1.1.2 unless BENCH_DIR names another one: every published series on
# this stand was measured with that profile, and a different one is not the same instrument.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER="${XMQ_SERVER:-thinker11}"
BASELINE="${BASELINE_VERSION:-1.1.2}"
bench_dir="${BENCH_DIR:-bench-1.1.2}"

die() { echo "regression: $*" >&2; exit 1; }

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    cat <<EOF
Usage: $(basename "$0") [XMQ_VERSION]

  Run the full home-stand bench set for XMQ_VERSION and compare it with $BASELINE.
  With no argument the version is read from ~/workspace/xmq_public/VERSION.txt.
  Checks readiness (persistence off, Release builds) before starting.
EOF
    exit 0
fi
if (( $# > 1 )); then
    die "at most one argument: XMQ_VERSION"
fi
version="${1:-}"
if [[ -z "$version" ]]; then
    source_dir="${XMQ_SOURCE_DIR:-$HOME/workspace/xmq_public}"
    version_file="$source_dir/VERSION.txt"
    [[ -f "$version_file" ]] || die "no $version_file - pass XMQ_VERSION or set XMQ_SOURCE_DIR"
    version=$(head -1 "$version_file" | tr -d '[:space:]')
    echo "version from $version_file: $version"
fi
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "'$version' is not an X.Y.Z version"

# A mismatched client changes measured latency by ~60us without touching the broker: that is what
# the Debug-build episode was. Refuse to mix versions instead of publishing a bogus comparison.
client_version=$(command -v xmq_scn >/dev/null 2>&1 && xmq_scn --version 2>/dev/null | head -1 || true)
[[ "$client_version" == "$version" ]] || die "client xmq_scn is '$client_version', need '$version'"
server_version=$(ssh -o BatchMode=yes -o ConnectTimeout=10 "$SERVER" \
    '~/.local/bin/xmq_server --version 2>/dev/null | head -1' 2>/dev/null || true)
[[ "$server_version" == "$version" ]] || die "broker on $SERVER is '$server_version', need '$version'"

# Persistence and both build types must be right before forty minutes are spent.
"$SCRIPT_DIR/stand_readiness.sh" --server "$SERVER" --set "$SCRIPT_DIR/Scenario-Set-Bench.txt" || exit 1

stamp=$(date +%Y-%m-%d-%H%M)
log="$SCRIPT_DIR/results/raw/regression-$stamp-xmq$version.log"
mkdir -p "$(dirname "$log")" || die "cannot create $(dirname "$log")"

echo "== bench set: XMQ $version, profile $bench_dir on $SERVER =="
# Readiness was checked above; --skip-readiness keeps stand_run.sh from repeating it.
"$SCRIPT_DIR/agent_run.sh" --wait --skip-readiness --log "$log" \
    --version "$version" --bench-dir "$bench_dir" --file
rc=$?
if (( rc )); then
    echo "regression: the set failed - last lines of $log:" >&2
    tail -15 "$log" >&2
    exit "$rc"
fi

if [[ "$version" == "$BASELINE" ]]; then
    echo "== $version is the baseline; no comparison printed =="
    exit 0
fi

echo
echo "== comparison: $BASELINE -> $version (interval medians) =="
python3 - "$SCRIPT_DIR/results/Bench" "$BASELINE" "$version" <<'PY'
import pathlib, re, sys

root, old_v, new_v = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]

def read(version):
    out = {}
    for path in sorted((root / version).glob('*.txt')):
        text = path.read_text(errors='replace')
        median = re.search(r'^Median\s+(\d+)us', text, re.M)
        p99 = re.search(r'^p99\s+(\d+)us', text, re.M)
        cpu = re.search(r'server CPU\s+mean (\d+)%', text)
        if median:
            out[path.stem] = (int(median.group(1)),
                              int(p99.group(1)) if p99 else 0,
                              int(cpu.group(1)) if cpu else 0)
    return out

old, new = read(old_v), read(new_v)
if not old or not new:
    print(f'  missing results in Bench/{old_v} or Bench/{new_v}')
    sys.exit(1)

# The published per-version files are single runs. This stand's run-to-run spread on P2P is about
# 10% (1.1.2 itself measured 207-231us on different days), so a small delta here is not evidence
# of a regression: confirm it with a same-day A/B using one client and one configuration.
print('  NOTE: published files are single runs; ~10% run-to-run spread is normal on this stand.')
print('        Confirm any small delta with a same-day A/B (one client, one config).')
print()
print(f"  {'Scenario':40} {'old':>8} {'new':>8} {'delta':>9} {'p99 old':>9} {'p99 new':>9} {'cpu old':>8} {'cpu new':>8}")
missing = False
for name in old:
    if name not in new:
        print(f'  {name:40} {"-":>8} {"MISSING":>8}')
        missing = True
        continue
    a, b = old[name], new[name]
    pct = (b[0] - a[0]) * 100.0 / a[0] if a[0] else 0.0
    print(f'  {name:40} {a[0]:7}us {b[0]:7}us {pct:+8.1f}% '
          f'{a[1]:8}us {b[1]:8}us {a[2]:7}% {b[2]:7}%')
sys.exit(1 if missing else 0)
PY
