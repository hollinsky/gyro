#!/usr/bin/env bash
#
# The one build. Configure, build and test gyro while holding an exclusive lock, so that two agents
# working in the same tree cannot run ninja in the same build directory at the same time.
#
# Ninja keeps its dependency cache in `.ninja_deps` and its timing cache in `.ninja_log`, and it takes
# no lock on either: a second ninja writing them concurrently leaves a file the next run rejects, and
# a rejected deps log is a full rebuild — several minutes on this machine — on every change after it.
# Serialising costs nothing besides waiting, which two builds on four cores were paying anyway.
#
# Usage:
#   Tools/Build.sh                  configure if needed, build everything, run the tests
#   Tools/Build.sh Gyro.Core.Test   build just those targets, then run the tests
#   Tools/Build.sh --no-test        stop after the build
#   Tools/Build.sh --wait 600       give up if the lock is not free within ten minutes
#
# Environment: BUILD_DIR (default `build`), CMAKE_ARGS, NINJA_ARGS, CTEST_ARGS.

set -euo pipefail

Root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
BuildDir=${BUILD_DIR:-build}
[[ $BuildDir == /* ]] || BuildDir=$Root/$BuildDir

RunTests=1
Wait=3600
Targets=()

while (($#)); do
	case $1 in
	-n | --no-test | --no-tests) RunTests=0 ;;
	-w | --wait)
		Wait=$2
		shift
		;;
	-h | --help)
		sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	--)
		shift
		Targets+=("$@")
		break
		;;
	-*)
		echo "Tools/Build.sh: unknown option $1" >&2
		exit 2
		;;
	*) Targets+=("$1") ;;
	esac
	shift
done

mkdir -p "$BuildDir"
Lock=$BuildDir/.build.lock
exec {LockFd}>"$Lock"

if ! flock --nonblock "$LockFd"; then
	Holder=$(cat "$BuildDir/.build.holder" 2>/dev/null || true)
	echo "Tools/Build.sh: waiting for the build lock${Holder:+ (held by $Holder)}" >&2
	if ! flock --wait "$Wait" "$LockFd"; then
		echo "Tools/Build.sh: the build lock was still held after ${Wait}s" >&2
		exit 1
	fi
fi
printf 'pid %s since %s\n' "$$" "$(date -Is)" >"$BuildDir/.build.holder"
trap 'rm -f "$BuildDir/.build.holder"' EXIT

if [[ ! -f $BuildDir/build.ninja ]]; then
	# shellcheck disable=SC2086
	cmake -S "$Root" -B "$BuildDir" -G Ninja ${CMAKE_ARGS-}
fi

# Ninja names a cache it could not read in its own words, and every one of them means the same thing:
# throw the file away rather than leave the tree rebuilding from scratch on every edit.
Corrupt='deps log|build log|bad deps log signature|premature end of file|invalid record|recompacting'

RunNinja() {
	local Out Status=0
	Out=$(mktemp)
	# shellcheck disable=SC2086
	ninja -C "$BuildDir" ${NINJA_ARGS-} "$@" 2>&1 | tee "$Out" || Status=${PIPESTATUS[0]}
	if ((Status != 0)) && grep -Eqi "$Corrupt" "$Out"; then
		echo "Tools/Build.sh: ninja rejected its own caches — deleting them and retrying once" >&2
		rm -f "$BuildDir/.ninja_deps" "$BuildDir/.ninja_log"
		rm -f "$Out"
		# shellcheck disable=SC2086
		ninja -C "$BuildDir" ${NINJA_ARGS-} "$@"
		return
	fi
	rm -f "$Out"
	return "$Status"
}

RunNinja "${Targets[@]}"

if ((RunTests)); then
	# shellcheck disable=SC2086
	ctest --test-dir "$BuildDir" --output-on-failure ${CTEST_ARGS-}
fi
