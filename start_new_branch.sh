#!/usr/bin/env bash
#
# Opens the next version: a branch, VERSION (which CMakeLists.txt reads), the installer's own copy
# of the number, and the package checks reset to measure against the release just made.
#
#   ./start_new_branch.sh                 next patch version (0.9.17 -> 0.9.18)
#   ./start_new_branch.sh 0.10.0          a version named outright
#   ./start_new_branch.sh --packages DIR  take the released packages from there rather than the farm
#
# Written because opening a version by hand took several separate edits, two of which nobody would
# think of: the installer keeps its own copy of the version and needs a new product code with it,
# and the package inspection compares against the previous release, so its baselines have to be
# re-read from the packages that were just published or it keeps checking against the release
# before last. VERSION itself used to be three SET() lines inside CMakeLists.txt, one edit each;
# 0.9.19 simplified that to the one file this script now writes.
#
set -u

here=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
cd "$here" || exit 1
farm=${FARM:-theater}
packages_dir=

while [ $# -gt 0 ]; do
    case "$1" in
        --packages) packages_dir=${2:?a directory}; shift 2 ;;
        -h|--help)  sed -n '2,14p' "$0"; exit 0 ;;
        *)          new_version=$1; shift ;;
    esac
done

say() { echo "$(date +%H:%M:%S) $*"; }
die() { echo "STOPPED: $*" >&2; exit 1; }

[ -z "$(git status --porcelain --untracked-files=no)" ] || die "the working tree has changes; commit or stash them first"

old_version=$(cat VERSION 2>/dev/null)
[ -n "$old_version" ] || die "cannot read the version from VERSION"
if [ -z "${new_version:-}" ]; then
    new_version=$(echo "$old_version" | awk -F. '{printf "%s.%s.%s", $1, $2, $3 + 1}')
fi
say "$old_version -> $new_version"

git rev-parse --verify --quiet "$new_version" >/dev/null && die "branch $new_version already exists"
git checkout -q -b "$new_version" || die "could not create the branch"

# 1. The version itself. CMakeLists.txt reads it from here rather than keeping its own copy.
echo "$new_version" > VERSION || die "could not write VERSION"

# 2. The installer's own copy, with a new product code beside it. Advanced Installer does both when
#    the version is changed in its interface: a new version carrying the previous product code is a
#    minor upgrade and will not replace an installed copy. The upgrade code never changes - it is
#    what makes these one product across releases. Written as bytes: the file has CRLF endings, and
#    reading it as text rewrites all 1200 lines.
python3 - "$new_version" <<'PY' || die "could not write the version into msi/XMQ.aip"
import re, sys, uuid
version = sys.argv[1].encode()
data = open('msi/XMQ.aip', 'rb').read()
code = re.search(rb'<ROW Property="ProductCode" Value="1033:\{[0-9A-F-]+\} " Type="16"/>', data)
assert code, 'ProductCode not found'
data = data.replace(code.group(0),
                    b'<ROW Property="ProductCode" Value="1033:{' + str(uuid.uuid4()).upper().encode() + b'} " Type="16"/>')
old = re.search(rb'<ROW Property="ProductVersion" Value="[0-9.]+" Options="32"/>', data)
assert old, 'ProductVersion not found'
data = data.replace(old.group(0), b'<ROW Property="ProductVersion" Value="' + version + b'" Options="32"/>')
open('msi/XMQ.aip', 'wb').write(data)
PY

# 3. The package checks. Their baselines are "the previous release", so after a release they must be
#    re-read from the packages that release published - otherwise every check keeps comparing with
#    the release before it, and the differences it reports are a release out of date. The expected
#    lists go with them: each line was a decision about one release, and they say so themselves.
staging=$(mktemp -d)
trap 'rm -rf "$staging"' EXIT
if [ -z "$packages_dir" ]; then
    say "fetching the $old_version packages from $farm"
    if scp -q -r "$farm:build/output/XMQ-$old_version" "$staging/" 2>/dev/null; then
        packages_dir="$staging/XMQ-$old_version"
    else
        say "WARNING: no packages for $old_version on $farm - the baselines are left as they are"
    fi
fi

if [ -n "$packages_dir" ] && [ -d "$packages_dir" ]; then
    refreshed=0
    for image_dir in "$packages_dir"/*/; do
        image=$(basename "$image_dir")
        package=$(ls "$image_dir"/*.deb "$image_dir"/*.rpm "$image_dir"/*.pkg 2>/dev/null | head -1)
        [ -n "$package" ] || continue
        [ -f "packages/baseline/$image.json" ] || continue
        if python3 packages/inspect_package.py "$package" --write-baseline "packages/baseline/$image.json" >/dev/null; then
            refreshed=$((refreshed + 1))
        else
            say "WARNING: could not read $package"
        fi
    done
    say "baselines refreshed from $old_version: $refreshed"
fi

for list in expected-removals expected-dependencies; do
    python3 - "packages/baseline/$list.txt" <<'PY'
import re, sys
# A release's entries are written as a comment naming the version and the lines that follow it, up
# to a blank line. Dropping only the comment leaves its continuation behind, orphaned and confusing.
path = sys.argv[1]
kept, dropping = [], False
for line in open(path):
    if re.match(r'#\s*\d+\.\d+\.\d+:', line):
        dropping = True
        continue
    if dropping:
        dropping = bool(line.strip())
        continue
    kept.append(line)
open(path, 'w').write(''.join(kept).rstrip('\n') + '\n')
PY
done
say "expected removals and dependencies cleared"

git add -A VERSION msi/XMQ.aip packages/baseline
git commit -q -m "$new_version opens.

VERSION, which CMakeLists.txt reads, and the installer's own copy of it in msi/XMQ.aip, whose
product code is regenerated with it. The package baselines are re-read from the $old_version
packages, and the expected removals and dependencies are cleared: each line in them was a
decision about $old_version."

say "branch $new_version committed - $(git log --oneline -1 | cut -c1-60)"
cat <<NEXT

Not done here, because both reach outside this repository:
  git push -u origin $new_version
  the farm's XMQ_VERSION, which names the branch every system builds and travels to the site as
  XMQ_VERSION_DEV. SPTK's version and its own installer live in the SPTK tree and move separately.

And XMQ_SPTK_VERSION in CMakeLists.txt, which pins the SPTK release this XMQ is built against, with
FIND_PACKAGE ... EXACT. It follows SPTK, not XMQ, so this script leaves it alone - but when SPTK
opens a new version too, it must move with it. The night of 2026-09-18 failed FreeBSD and Windows on
exactly this: the farm installed 5.6.12 and XMQ still asked for exactly 5.6.11.
NEXT
