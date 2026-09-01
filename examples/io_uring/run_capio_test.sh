#!/usr/bin/env bash
# Integration check for the io_uring example under CAPIO.
#
#   T8  the producer/consumer pipeline round-trips through the middleware
#       (consumer reports "verified N/N" and exits 0);
#   T9  the interception is real — the files never touch the backing filesystem,
#       so reading a produced file WITHOUT the CAPIO library must fail.
#
# Needs: capio_server and libcapio_posix.so on the system (installed CAPIO),
# and the one_to_one binary built (see CMakeLists.txt). Pass its path as $1,
# or let the script use ./one_to_one.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bin="${1:-$here/one_to_one}"
config="$here/capio_config.json"
mnt="$(mktemp -d)"
workflow="io_uring_example"

n=4 fsize=10000 chunk=4096
q=32
common=(-n "$n" -f "$fsize" -c "$chunk" -q "$q" -d "$mnt")

fail() { echo "FAIL: $*" >&2; cleanup; exit 1; }
cleanup() {
    [ -n "${server_pid:-}" ] && kill "$server_pid" 2>/dev/null
    rm -rf "$mnt" /dev/shm/"$workflow"* /dev/shm/sem."$workflow"* 2>/dev/null
}
trap cleanup EXIT

[ -x "$bin" ] || fail "binary not found or not executable: $bin (build it first)"

rm -rf /dev/shm/"$workflow"* /dev/shm/sem."$workflow"* 2>/dev/null

# --- start the server -------------------------------------------------------
CAPIO_DIR="$mnt" capio_server -c "$config" >"$mnt/server.log" 2>&1 &
server_pid=$!
sleep 2
kill -0 "$server_pid" 2>/dev/null || fail "server did not start (see $mnt/server.log)"

run() {  # run() <app_name> <extra args...>
    local app="$1"; shift
    CAPIO_DIR="$mnt" CAPIO_WORKFLOW_NAME="$workflow" CAPIO_APP_NAME="$app" \
        LD_PRELOAD=libcapio_posix.so INTERCEPT_ALL_OBJS=1 \
        "$bin" -r "$app" "${common[@]}" "$@"
}

check_engine() {  # check_engine <engine>
    local engine="$1"
    run producer -e "$engine" > "$mnt/producer.$engine.out" 2>&1 || fail "producer exited non-zero ($engine)"
    if ! run consumer -e "$engine" > "$mnt/consumer.$engine.out" 2>&1; then
        cat "$mnt/consumer.$engine.out" >&2
        fail "consumer verification failed under CAPIO ($engine)"
    fi
    grep -q "verified $n/$n files OK" "$mnt/consumer.$engine.out" \
        || fail "consumer did not verify all files for $engine: $(cat "$mnt/consumer.$engine.out")"
}

# --- T8: pipeline round-trips through CAPIO ---------------------------------
check_engine posix
check_engine io_uring
echo "T8 OK: pipeline verified $n/$n files through CAPIO for posix and io_uring"

# --- T9: interception is real (files are not on the real filesystem) --------
# Same reader binary, but WITHOUT LD_PRELOAD -> it hits the bare filesystem,
# where the CAPIO-only files do not exist, so it must fail.
if "$bin" -r consumer -e posix "${common[@]}" >/dev/null 2>&1; then
    fail "files are readable without CAPIO — interception did NOT happen"
fi
echo "T9 OK: files absent from the real filesystem (interception confirmed)"

echo "ALL CAPIO INTEGRATION CHECKS PASSED"
