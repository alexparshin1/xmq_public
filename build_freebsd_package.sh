#!/bin/sh
#
# Builds an XMQ package for FreeBSD from a clean checkout of both repositories.
#
# It fetches SPTK and XMQ, builds and installs SPTK into a prefix of its own, builds XMQ against
# it, and produces a pkg(8) package. Nothing is written outside the work directory and that prefix
# unless --install is given.
#
# The compiler is the base Clang, by not naming one at all. What libc++ lacks is worked around in
# the sources - std::atomic<std::shared_ptr> by common/AtomicSharedPtr.h, std::jthread by
# -fexperimental-library - and in exchange the broker links libc++ and libcxxrt from the base
# system, so the package depends on no port: 13 MB installed against the 401 MB that lang/gcc15
# used to bring with it. CC and CXX still override, and both trees must then be built by the same
# compiler: a library built by one and linked by the other fails with nothing but undefined std::
# symbols to explain it.

set -e

WORK_DIR="${WORK_DIR:-$HOME/xmq-package-build}"
SPTK_REPO="${SPTK_REPO:-git@github.com:alexparshin1/sptk5.git}"
XMQ_REPO="${XMQ_REPO:-git@github.com:alexparshin1/xmq.git}"
SPTK_BRANCH="${SPTK_BRANCH:-}"
XMQ_BRANCH="${XMQ_BRANCH:-}"

# Where SPTK goes. Not /usr/local: on FreeBSD that belongs to the ports tree, and a build has no
# business writing into it. The package carries its own copy of SPTK's shared libraries, so what
# is installed here is only ever needed to build with.
SPTK_PREFIX="${SPTK_PREFIX:-$WORK_DIR/prefix}"

# The prefix compiled into the packaged binaries, and where the package unpacks. It is not where
# anything is installed by this script.
PKG_PREFIX="${PKG_PREFIX:-/usr/local}"

# The base Clang unless told otherwise, and that is the point. The broker then links
# libc++ and libcxxrt from the base system and the package depends on no port at all - 13 MB against
# the 401 MB that lang/gcc15 used to drag onto every machine that only wanted to run a broker. Set
# CC and CXX in the environment to build with something else; packages/CMakeLists.txt follows the
# compiler and declares the runtime dependency only where there is one.
CC="${CC:-}"
CXX="${CXX:-}"

# Named outright, every run, even when the environment names nothing. Leaving the choice to cmake
# sounds equivalent and is not: cmake writes the compiler into its cache, and a work directory left
# by an earlier run goes on using whatever is recorded there, whatever this run intends. That is not
# theoretical - a rerun in a directory once configured with g++15 built the whole package with it
# while this script cheerfully reported the base clang, and the result declared a dependency on
# lang/gcc15: thirteen packages and some 400 MB, in a package whose reason for existing is to need
# no port at all.
CC_PATH="${CC:-$(command -v cc)}"
CXX_PATH="${CXX:-$(command -v c++)}"
COMPILER_ARGS="-DCMAKE_C_COMPILER=$CC_PATH -DCMAKE_CXX_COMPILER=$CXX_PATH"

# cmake refuses to change the compiler of an existing build directory - it stops and tells you to
# start again - so a work directory left by another compiler is discarded here instead.
#
# All of it, not the offending tree alone. The prefix holds SPTK and the googletest it installs,
# and those are just as compiler-bound as the build trees: discarding only the trees once left a
# clang build linking a googletest built by g++15, which fails with nothing but undefined std::
# symbols to explain it. One compiler per work directory, or none of it.
discard_work_built_by_another_compiler()
{
    for tree in "$WORK_DIR/sptk5/code/build-freebsd" "$WORK_DIR/xmq/build-freebsd"; do
        cache="$tree/CMakeCache.txt"
        [ -f "$cache" ] || continue
        cached=$(sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p' "$cache" | head -1)
        if [ -n "$cached" ] && [ "$cached" != "$CXX_PATH" ]; then
            echo "$tree was built with $cached and this run uses $CXX_PATH:"
            echo "discarding both build trees and $SPTK_PREFIX so that one compiler produces all of it."
            rm -rf "$WORK_DIR/sptk5/code/build-freebsd" "$WORK_DIR/xmq/build-freebsd" "$SPTK_PREFIX"
            return 0
        fi
    done
}
# Runs a command as the user who invoked this script rather than as root. Most of the build needs
# root; reaching another machine over ssh is the one part that must not have it, because the keys
# belong to the person, not to root.
run_as_invoker()
{
    if [ "$(id -u)" -eq 0 ] && [ -n "${SUDO_USER:-}" ]; then
        su -m "$SUDO_USER" -c "$1"
    else
        eval "$1"
    fi
}

JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
INSTALL_PACKAGE="no"
PUBLISH_PACKAGE="yes"

# Where a finished package goes, beside the Linux ones the build farm puts there. The layout is
# the download area's own: one directory per SPTK release, one per operating system inside it,
# which is what the site's download page reads back.
PUBLISH_HOST="${PUBLISH_HOST:-theater}"
PUBLISH_DIR="${PUBLISH_DIR:-/var/www/html/sptk/download}"
# In the style of the directories already there - ubuntu-plucky, fedora-43, debian-trixie. The
# major version alone, because the package's ABI is FreeBSD:15:amd64 and it installs on any 15.x.
PUBLISH_OS_DIR="${PUBLISH_OS_DIR:-freebsd-15}"

usage()
{
    cat <<USAGE
Usage: $0 [--install] [--clean]

  --install   pkg install the package once it is built. This is how XMQ is installed on
              FreeBSD - never by hand into \$PKG_PREFIX, which would leave pkg's database
              and the filesystem disagreeing about what is there.
  --clean     Discard the work directory first and start from nothing.
  --no-publish
              Do not copy the finished package to the download area. It is copied there by
              default, with a .sha256 beside it.

Environment: WORK_DIR SPTK_REPO XMQ_REPO SPTK_BRANCH XMQ_BRANCH SPTK_PREFIX PKG_PREFIX CC CXX JOBS
             PUBLISH_HOST PUBLISH_DIR PUBLISH_OS_DIR
Current:     WORK_DIR=$WORK_DIR SPTK_PREFIX=$SPTK_PREFIX PKG_PREFIX=$PKG_PREFIX CC=$CC JOBS=$JOBS
             PUBLISH_HOST=$PUBLISH_HOST PUBLISH_DIR=$PUBLISH_DIR PUBLISH_OS_DIR=$PUBLISH_OS_DIR
USAGE
}

for argument in "$@"; do
    case "$argument" in
        --install) INSTALL_PACKAGE="yes" ;;
        --no-publish) PUBLISH_PACKAGE="no" ;;
        --clean)   rm -rf "$WORK_DIR" ;;
        -h|--help) usage; exit 0 ;;
        *)         echo "Unknown option: $argument" >&2; usage; exit 2 ;;
    esac
done

step() { echo; echo "───── $* ─────"; }

step "Checking the tools"
missing=""
# python3 is here because add_package_message needs it to put the first-start message into the
# manifest. It used to be missing from this list, and the failure was quiet: the message step
# warned and the script went on, so the package installed without the one thing it has to say.
# FreeBSD installs the interpreter as python3.12 and only the "python3" metapackage provides the
# plain name this script calls, so a machine can have Python and still fail here.
for tool in git cmake npm pkg python3; do
    command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done
# Only when one was asked for: an unset CC or CXX means the base compiler, which is always there.
if [ -n "$CC" ] && [ ! -x "$CC" ]; then
    missing="$missing $CC"
fi
if [ -n "$CXX" ] && [ ! -x "$CXX" ]; then
    missing="$missing $CXX"
fi
if [ -n "$missing" ]; then
    echo "Not found:$missing" >&2
    echo "Install them with: pkg install git cmake npm python3" >&2
    exit 1
fi
echo "Compiler: $(${CXX_PATH} --version | head -1)"

# Clone, or bring an existing checkout to the requested revision. Fetched rather than re-cloned so
# that running this twice is cheap.
checkout()
{
    repository="$1"; directory="$2"; branch="$3"
    if [ -d "$directory/.git" ]; then
        git -C "$directory" fetch --prune origin
    else
        git clone "$repository" "$directory"
    fi
    if [ -n "$branch" ]; then
        git -C "$directory" checkout "$branch"
        git -C "$directory" reset --hard "origin/$branch"
    else
        git -C "$directory" pull --ff-only
    fi
    echo "$directory: $(git -C "$directory" rev-parse --short HEAD) $(git -C "$directory" rev-parse --abbrev-ref HEAD)"
}

mkdir -p "$WORK_DIR"

step "Fetching the sources"

# SPTK first and on whatever branch the checkout is on, because the version files that say which
# release is current live inside it. The build farm reads the same two files, so a FreeBSD package
# is built from the same revisions as the Linux ones without anyone having to remember a second
# place to change. Either branch can still be named in the environment, which is what to do when
# building something other than the current release.
checkout "$SPTK_REPO" "$WORK_DIR/sptk5" ""

versionFile()
{
    file="$WORK_DIR/sptk5/build.scripts/$1"
    [ -f "$file" ] && head -1 "$file"
}

if [ -z "$SPTK_BRANCH" ]; then
    SPTK_BRANCH=$(versionFile SPTK_VERSION)
    [ -n "$SPTK_BRANCH" ] && echo "SPTK branch from build.scripts/SPTK_VERSION: $SPTK_BRANCH"
fi
if [ -z "$XMQ_BRANCH" ]; then
    XMQ_BRANCH=$(versionFile XMQ_VERSION)
    [ -n "$XMQ_BRANCH" ] && echo "XMQ branch from build.scripts/XMQ_VERSION: $XMQ_BRANCH"
fi

# Again, now that the branch is known. A no-op when it was already on it.
checkout "$SPTK_REPO" "$WORK_DIR/sptk5" "$SPTK_BRANCH"
checkout "$XMQ_REPO"  "$WORK_DIR/xmq"   "$XMQ_BRANCH"

step "Building SPTK"
# Out of source, so that a second run with different settings does not inherit the first one's
# cache. USE_GTEST stays on: SPTK builds googletest from its own vendored copy and installs it, and
# XMQ is pointed at that copy below rather than at the one under /usr/local - not for ABI reasons
# now that both are built with the base Clang, but so that a run cannot silently link a googletest
# nobody in it produced.
discard_work_built_by_another_compiler
# shellcheck disable=SC2086 # COMPILER_ARGS is two -D flags, and must word-split
cmake -S "$WORK_DIR/sptk5/code" -B "$WORK_DIR/sptk5/code/build-freebsd" \
      -DCMAKE_BUILD_TYPE=Release \
      $COMPILER_ARGS \
      -DCMAKE_INSTALL_PREFIX="$SPTK_PREFIX"
cmake --build "$WORK_DIR/sptk5/code/build-freebsd" -j "$JOBS"
cmake --install "$WORK_DIR/sptk5/code/build-freebsd"

# wsdl2cxx lives here, and XMQ regenerates its web service sources with it whenever xmq.wsdl looks
# newer than what is checked in - which a fresh clone makes a coin toss, since git gives every file
# the same checkout time.
PATH="$SPTK_PREFIX/bin:$PATH"
export PATH

# Where googletest actually is, rather than where it might have been. SPTK builds its vendored copy
# and installs it beside itself under GCC - but with the base Clang on BSD it links the copy the
# googletest package puts in /usr/local and installs none, which is deliberate and documented in
# SPTK's CMakeLists. This script named the prefix either way, so a clang build - the one it is
# written to produce - got all the way to the last link and stopped at "unable to find library
# -lgtest", with a full build behind it.
if [ -f "$SPTK_PREFIX/lib/libgtest.so" ]; then
    GTEST_PREFIX="$SPTK_PREFIX"
elif [ -f "/usr/local/lib/libgtest.so" ]; then
    GTEST_PREFIX="/usr/local"
else
    echo "No googletest under $SPTK_PREFIX/lib or /usr/local/lib." >&2
    echo "Install it with: pkg install googletest" >&2
    exit 1
fi
echo "googletest: $GTEST_PREFIX/lib"

step "Building XMQ"
# Every path into SPTK is named outright rather than searched for, and that is not belt and braces.
# XMQ looks in its own install prefix first - /usr/local here - and then in ~/.local, so a machine
# that has ever had SPTK installed by hand offers a second copy that wins over the one this script
# just built. The package would then carry libraries nobody in this run produced. The same goes for
# googletest: only the test binary links it and the package does not ship that, but the tree still
# has to link, so it is named too - at $GTEST_PREFIX, chosen just above.
# shellcheck disable=SC2086 # COMPILER_ARGS is two -D flags, and must word-split
cmake -S "$WORK_DIR/xmq" -B "$WORK_DIR/xmq/build-freebsd" \
      -DCMAKE_BUILD_TYPE=Release \
      $COMPILER_ARGS \
      -DCMAKE_INSTALL_PREFIX="$PKG_PREFIX" \
      -DSPTK_INCLUDE_DIR="$SPTK_PREFIX/include" \
      -DSPTK_LIBRARY_CORE="$SPTK_PREFIX/lib/libsputil5.so" \
      -DSPTK_LIBRARY_WSDL="$SPTK_PREFIX/lib/libspwsdl5.so" \
      -DSPTK_LIBRARY_SPDB="$SPTK_PREFIX/lib/libspdb5.so" \
      -DGTest_INCLUDE_DIR="$GTEST_PREFIX/include/gtest" \
      -DGTest_LIBRARY="$GTEST_PREFIX/lib/libgtest.so" \
      -DGTest_MAIN_LIBRARY="$GTEST_PREFIX/lib/libgtest_main.so" \
      -DGTEST_INCLUDE_DIR="$GTEST_PREFIX/include" \
      -DGTEST_LIBRARY="$GTEST_PREFIX/lib/libgtest.so"
cmake --build "$WORK_DIR/xmq/build-freebsd" -j "$JOBS"

# The unit tests are outside the default target, so that installing the broker does not compile them.
# Named here, or the build farm runs whatever xmq_unit_tests an earlier run left in the directory and
# reports its result as this version's: on the BSD machine that was a binary from before the exclusion,
# and nothing said so.
cmake --build "$WORK_DIR/xmq/build-freebsd" -j "$JOBS" --target xmq_unit_tests

step "Building the package"
# cpack, not "cmake --install": the install rules would write into $PKG_PREFIX for real, and the
# point is to produce something pkg(8) can install and remove again.
#
# Under sudo, because a pkg(8) archive records the ownership of the files it was built from and
# pkg preserves that on install - so a package built as an ordinary user hands out a broker, its
# configuration and its rc.d script owned by whoever built it. packages/FreeBSDOwnership.cmake
# stamps root:wheel on the staged tree, and needs to be root to do it.
#
# With PATH carried across explicitly: sudo resets it, and cpack starts by running the build's
# "preinstall" target, which regenerates the web service sources with wsdl2cxx whenever xmq.wsdl
# looks newer than they do - the coin toss described above. Under a bare sudo that regeneration
# stopped at "wsdl2cxx: not found", after a build that had just succeeded, and the package was
# never made.
( cd "$WORK_DIR/xmq/build-freebsd" && sudo env "PATH=$PATH" cpack )

# Back to the invoking user, so that the rest of this script and --clean can still remove what sudo
# has just created.
sudo chown -R "$(id -u):$(id -g)" "$WORK_DIR/xmq/build-freebsd"

package=$(ls -t "$WORK_DIR/xmq/build-freebsd"/*.pkg 2>/dev/null | head -1)
if [ -z "$package" ]; then
    echo "cpack reported success but produced no package" >&2
    exit 1
fi

step "Adding the first-start message and the install script"
# pkg(8) prints a package's message after installing it and runs the scripts in its manifest, and
# CPack's FreeBSD generator has a setting for neither - it fills the manifest through libpkg from a
# fixed list of CPACK_FREEBSD_* variables, and "messages" and "scripts" are not among them. So the
# package cpack built is unpacked, its manifest is given both, and pkg-create(8) builds it again
# from the very same files.
#
# Under sudo throughout: the archive carries root:wheel ownership that packages/FreeBSDOwnership
# stamped on, tar only restores it as root, and pkg records what it finds on disk. Unpacking as
# an ordinary user would quietly hand the whole installation back to whoever built it.
add_package_manifest_extras()
{
    message_file="$WORK_DIR/xmq/build-freebsd/packages/FirstStartMessage.txt"
    post_install_file="$WORK_DIR/xmq/build-freebsd/packages/FreeBSDPostInstall.sh"
    if [ ! -f "$message_file" ]; then
        echo "No $message_file - the package will install without a message." >&2
        return 1
    fi
    if [ ! -f "$post_install_file" ]; then
        echo "No $post_install_file - the package would install with no account to run as." >&2
        return 1
    fi

    repack_dir="$WORK_DIR/xmq/build-freebsd/pkg-repack"
    sudo rm -rf "$repack_dir"
    mkdir -p "$repack_dir/root" "$repack_dir/out"

    # -p to keep ownership and modes as the archive records them.
    sudo tar -xpf "$package" -C "$repack_dir/root" || return 1
    if [ ! -f "$repack_dir/root/+MANIFEST" ]; then
        echo "No +MANIFEST in $package - not a pkg archive?" >&2
        return 1
    fi

    # The manifest is JSON. "messages" is an array of objects with a "message" and, left out here,
    # a "type" that would narrow it to install or upgrade only - said on both, since an upgrade of
    # a broker nobody ever set up still has the same thing left to do. "scripts" maps a stage to
    # the shell to run at it; post-install is after the files are in place, which is when there is
    # something to give the account.
    sudo python3 - "$repack_dir/root/+MANIFEST" "$message_file" "$post_install_file" <<'PYTHON' || return 1
import json
import sys

manifest_path, message_path, post_install_path = sys.argv[1], sys.argv[2], sys.argv[3]
with open(manifest_path) as manifest_file:
    manifest = json.load(manifest_file)
with open(message_path) as message_file:
    manifest["messages"] = [{"message": message_file.read().strip()}]
with open(post_install_path) as post_install_file:
    scripts = manifest.get("scripts", {})
    scripts["post-install"] = post_install_file.read()
    manifest["scripts"] = scripts
with open(manifest_path, "w") as manifest_file:
    json.dump(manifest, manifest_file)
PYTHON

    # The manifest lists every file with its checksum, and the files are the ones just unpacked,
    # so pkg has everything it needs to build the same package with a different manifest.
    sudo pkg create -M "$repack_dir/root/+MANIFEST" -r "$repack_dir/root" -o "$repack_dir/out" || return 1

    rebuilt=$(ls -t "$repack_dir/out"/*.pkg 2>/dev/null | head -1)
    if [ -z "$rebuilt" ]; then
        echo "pkg create produced no package" >&2
        return 1
    fi

    sudo mv "$rebuilt" "$package"
    sudo chown "$(id -u):$(id -g)" "$package"
    sudo rm -rf "$repack_dir"
    return 0
}

if add_package_manifest_extras; then
    echo "The package will make its service account and say what is left to set up."
else
    # Loud, and fatal for the script part: a package with no message still installs and runs, but
    # one with no post-install script leaves the broker with no account to run as and directories
    # it cannot write, and rc.d would start nothing.
    echo >&2
    echo "WARNING: the manifest could not be completed. Do not publish this package." >&2
    echo >&2
fi

# Acceptance, first layer: what this package will do to a machine that already runs XMQ, read from its
# manifest against the previous release's list kept in the tree - a live file under etc/xmq, a
# configuration template readable by all, a declared config file, a path the last release had and
# this one drops. After the manifest is complete, because that is the package that gets published.
step "Inspecting the package"
inspect_dir="$WORK_DIR/xmq/packages"
baseline="$inspect_dir/baseline/freebsd-$(uname -r | cut -d. -f1).json"
inspect_args="--expected-removals $inspect_dir/baseline/expected-removals.txt --expected-dependencies $inspect_dir/baseline/expected-dependencies.txt"
[ -f "$baseline" ] && inspect_args="$inspect_args --baseline $baseline"
if ! python3 "$inspect_dir/inspect_package.py" "$package" $inspect_args; then
    echo >&2
    echo "The package failed inspection. Do not publish it." >&2
    PUBLISH_PACKAGE=no
fi

# The checksum is made whether or not the package is going anywhere: it is what somebody who
# downloads it checks against, and the Windows installer is unsigned until 1.0, so for now it is
# the only thing that says a download arrived intact. Coreutils' format, the one "shasum -a 256 -c"
# reads, rather than FreeBSD sha256(1)'s own - the people checking it are mostly not on FreeBSD.
step "Checksum"
sha256 -q "$package" | awk -v name="$(basename "$package")" '{print $1 "  " name}' > "$package.sha256"
cat "$package.sha256"

if [ "$PUBLISH_PACKAGE" = "yes" ]; then
    step "Publishing"
    # Which SPTK release this was built with, read from the tree it was built from rather than
    # from build.scripts/SPTK_VERSION, which is a file somebody has to remember to change and
    # currently disagrees with the sources.
    sptkVersion=$(awk -F\" '/^SET\(VERSION_(MAJOR|MINOR|PATCH)/ {printf "%s%s", separator, $2; separator="."}' \
                  "$WORK_DIR/sptk5/code/CMakeLists.txt")
    if [ -z "$sptkVersion" ]; then
        echo "Could not read the SPTK version out of $WORK_DIR/sptk5/code/CMakeLists.txt" >&2
        exit 1
    fi

    destination="$PUBLISH_DIR/SPTK-$sptkVersion/$PUBLISH_OS_DIR"
    echo "$PUBLISH_HOST:$destination"
    # Not fatal. The package is built and good; a machine that cannot reach the download area has
    # simply not published it, and saying so beats failing a build that succeeded.
    # Copied as whoever invoked the script, not as root. The build has to run as root - cpack sets
    # the ownership of files inside the package and there is no setting for it - but root on a build
    # machine has no reason to hold a key to the download area, and on the FreeBSD builder it does
    # not: the copy failed with "Permission denied (publickey)" while the same copy as the invoking
    # user succeeds. That was the whole of the "package is not copied to theater" fault.
    publishCommand="ssh -o BatchMode=yes $PUBLISH_HOST 'mkdir -p $destination'"
    publishCommand="$publishCommand && scp -o BatchMode=yes -q $package $package.sha256 $PUBLISH_HOST:$destination/"

    # The reason is printed, not swallowed. A bare "could not copy" sends whoever reads it looking
    # for a fault in the build, when the answer is almost always about reaching PUBLISH_HOST.
    if publishError=$(run_as_invoker "$publishCommand" 2>&1); then
        echo "Copied, with its checksum."
    else
        echo >&2
        echo "WARNING: could not copy to $PUBLISH_HOST:" >&2
        echo "  ${publishError:-no message from ssh}" >&2
        echo "The package itself is fine: $package" >&2
        echo "Set PUBLISH_HOST to a name this machine can reach, or pass --no-publish." >&2
        echo >&2
    fi
fi

step "Done"
echo "$package"
pkg info -F "$package" | sed -n '1,12p'

if [ "$INSTALL_PACKAGE" = "yes" ]; then
    step "Installing"
    sudo pkg install -y "$package"
fi
