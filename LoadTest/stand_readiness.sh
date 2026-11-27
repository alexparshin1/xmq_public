#!/usr/bin/env bash
# Checks that must hold before a bench set is started. Unlike preflight.sh, which reports host
# limits and lets a NOTE through, these refuse the run: both have silently produced numbers
# labelled with the wrong version or the wrong configuration.
#
#   1) the broker's persistence is off, unless this is a persistence campaign;
#   2) the client and the broker are Release builds - a Debug xmq_scn costs ~60us of measured
#      latency per round trip and looks exactly like a broker regression.
#
# Exit status: 0 when every check passes, 1 otherwise.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

server="thinker11"
set_file="$SCRIPT_DIR/Scenario-Set-Bench.txt"
usage() {
    cat <<EOF
Usage: $(basename "$0") [--server HOST] [--set FILE]

  --server HOST   broker host (default: thinker11)
  --set FILE      scenario set; a name containing 'persist' skips the persistence-off check
  -h, --help      this

Called by stand_run.sh/agent_run.sh before a set starts; can also be run by hand.
EOF
    exit 0
}
while (( $# )); do
    case "$1" in
        --server) server="$2"; shift 2 ;;
        --set)    set_file="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "stand_readiness: unknown option $1" >&2; exit 1 ;;
    esac
done

fails=0
fail() { printf "  FAIL  %s\n" "$1"; ((fails++)); }
ok()   { printf "  ok    %s\n" "$1"; }

release_binary() {
    local label=$1 path=$2
    if [[ ! -x "$path" ]]; then
        fail "$label: $path is not executable"
        return
    fi
    local description
    description=$(file -L "$path")
    if grep -q 'debug_info' <<< "$description"; then
        fail "$label: $path carries debug_info - this is a Debug/RelWithDebInfo build, not Release"
        return
    fi
    ok "$label: Release build ($path)"
}

echo "== readiness checks =="

# Client: xmq_scn is on the local PATH, and the in-source CMake cache says which build type
# produced it when the cache is present.
client_bin=$(command -v xmq_scn || true)
release_binary "client xmq_scn" "${client_bin:-$HOME/.local/bin/xmq_scn}"
for cache in "$HOME/workspace/xmq_public/CMakeCache.txt" "$HOME/workspace/xmq/CMakeCache.txt"; do
    if [[ -f "$cache" ]]; then
        build_type=$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "$cache" | head -1)
        if [[ "$build_type" == "Release" ]]; then
            ok "client build type: Release ($cache)"
        else
            fail "client build type: $build_type in $cache - rebuild with -DCMAKE_BUILD_TYPE=Release"
        fi
        break
    fi
done

# Broker: same two checks over ssh. The binary is the installed one, not the build-tree copy.
remote_client=$(ssh -o BatchMode=yes -o ConnectTimeout=10 "$server" '
    path="$HOME/.local/bin/xmq_server"
    test -x "$path" || exit 1
    file -L "$path"
' 2>/dev/null)
if [[ -z "$remote_client" ]]; then
    fail "server xmq_server: cannot inspect ~/.local/bin/xmq_server on $server"
elif grep -q 'debug_info' <<< "$remote_client"; then
    fail "server xmq_server: carries debug_info - rebuild with -DCMAKE_BUILD_TYPE=Release"
else
    ok "server xmq_server: Release build"
fi
# The CMake cache on thinker11 has an empty CMAKE_BUILD_TYPE even though the broker was compiled
# with -O3 -DNDEBUG: flags.make is what the compiler actually saw, so trust that first. A cache
# that disagrees is a stale cache, not an unoptimized broker.
remote_type=$(ssh -o BatchMode=yes -o ConnectTimeout=10 "$server" '
    flags="$HOME/workspace/xmq/server/CMakeFiles/xmq_server_core.dir/flags.make"
    if [[ -f "$flags" ]] && grep -q -- "-DNDEBUG" "$flags"; then
        opts=$(grep -oE -- "-O[0-9]" "$flags" | sort -u | tr "\n" " ")
        echo "optimized:${opts:-none}"
        exit 0
    fi
    cache="$HOME/workspace/xmq/CMakeCache.txt"
    if [[ -f "$cache" ]]; then
        type=$(sed -n "s/^CMAKE_BUILD_TYPE:STRING=//p" "$cache" | head -1)
        echo "cache:${type:-unset}"
    fi
' 2>/dev/null)
case "$remote_type" in
    optimized:*)  ok "server build type: Release-equivalent (${remote_type#optimized:}-DNDEBUG)" ;;
    cache:Release) ok "server build type: Release" ;;
    cache:*)      fail "server build type: ${remote_type#cache:} - rebuild with -DCMAKE_BUILD_TYPE=Release" ;;
esac

# Persistence lives in the configuration database, so this answer does not depend on a broker
# being up at check time. Dedicated persistence campaigns are the only exception.
if [[ "$(basename "$set_file")" =~ [Pp]ersist ]]; then
    printf "  NOTE  %s\n" "persistence-enabled set: persistence-off check skipped"
else
    persisted=$(ssh -o BatchMode=yes -o ConnectTimeout=10 "$server" '
        set -a; . "$HOME/xmq-stand.env"; set +a
        psql "$XMQ_DATABASE_URI" -Atc \
            "select persist_enabled from xmq_node where name='\''$XMQ_NODE_NAME'\''"
    ' 2>/dev/null | tr -d '[:space:]')
    case "$persisted" in
        f|0|false) ok "broker persistence: off" ;;
        t|1|true)  fail "broker persistence is on - numbered runs assume it is off" ;;
        *)         fail "broker persistence: cannot read it from $server (got '${persisted:-empty}')" ;;
    esac
fi

if (( fails )); then
    echo "stand_readiness: $fails check(s) failed - the set was not started" >&2
    exit 1
fi
echo "  (all readiness checks passed)"
