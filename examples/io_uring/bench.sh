#!/usr/bin/env bash
# Benchmarks the four configurations of the example and prints a table + CSV:
#
#     FS-posix     FS-uring        (baselines, on the real filesystem)
#     CAPIO-posix  CAPIO-uring     (through the CAPIO middleware)
#
# For each: run the producer then the consumer REPS times, report the median
# throughput (MB/s) of each, and confirm the consumer verifies every file.
#
# CAPIO-uring is expected to FAIL today: CAPIO intercepts POSIX syscalls but not
# io_uring, so its files never reach CAPIO storage. That failure is the point —
# it is the measured motivation for the interception work (thesis phase F3). The
# script records it as "not intercepted" instead of aborting.
#
# Usage: bench.sh [binary] [-n N] [-f BYTES] [-c BYTES] [-q DEPTH] [-r REPS] [-D DIR]
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bin="$here/one_to_one"
config="$here/capio_config.json"
workflow="io_uring_example"

n=100 fsize=1048576 chunk=65536 q=32 reps=5
# Real disk by default (NOT tmpfs) so numbers reflect storage, not RAM.
work="${TMPDIR:-/var/tmp}/o2o_bench"

while getopts "b:n:f:c:q:r:D:" opt; do
    case $opt in
        b) bin="$OPTARG" ;;
        n) n="$OPTARG" ;;  f) fsize="$OPTARG" ;;  c) chunk="$OPTARG" ;;
        q) q="$OPTARG" ;;  r) reps="$OPTARG" ;;   D) work="$OPTARG" ;;
        *) echo "usage: bench.sh [-b bin] [-n N] [-f bytes] [-c bytes] [-q depth] [-r reps] [-D dir]" >&2; exit 2 ;;
    esac
done

[ "${1:-}" ] && [ -x "${1:-}" ] && bin="$1"
[ -x "$bin" ] || { echo "binary not found: $bin (build it first)" >&2; exit 1; }
common=(-n "$n" -f "$fsize" -c "$chunk" -q "$q")
bytes=$(( n * fsize ))

median() {  # median of stdin numbers
    sort -n | awk '{a[NR]=$1} END{ if(NR==0){print 0} else if(NR%2){print a[(NR+1)/2]} else {print (a[NR/2]+a[NR/2+1])/2} }'
}
mbps_of() { grep -o 'MBps=[0-9.]*' | cut -d= -f2; }   # pull the metric from a run

server_pid=""
stop_server() { [ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null; server_pid=""; wait 2>/dev/null; }
# CAPIO names its shared memory after the workflow, not "CAPIO*". Scope the
# removal to this workflow's objects only — never a blanket /dev/shm/* wipe,
# which would clobber unrelated processes.
clean_shm() { rm -f /dev/shm/"$workflow"* /dev/shm/sem."$workflow"* 2>/dev/null; }
cleanup() { stop_server; rm -rf "$work" 2>/dev/null; clean_shm; }
trap cleanup EXIT

csv="$work/results.csv"
mkdir -p "$work"
echo "config,engine,layer,producer_MBps,consumer_MBps,verified,reason" > "$csv"

# run_plain <engine>  -> echoes "prodMBps consMBps verified_ok(0/1)"
run_plain() {
    local engine="$1" d="$work/plain" pm cm ok=1
    local prods=() conss=()
    for ((i=0;i<reps;i++)); do
        rm -rf "$d"; mkdir -p "$d"
        prods+=("$("$bin" -r producer -e "$engine" "${common[@]}" -d "$d" | mbps_of)")
        local out; out="$("$bin" -r consumer -e "$engine" "${common[@]}" -d "$d")"
        conss+=("$(printf '%s' "$out" | mbps_of)")
        printf '%s' "$out" | grep -q "verified $n/$n" || ok=0
    done
    pm=$(printf '%s\n' "${prods[@]}" | median)
    cm=$(printf '%s\n' "${conss[@]}" | median)
    echo "$pm $cm $ok"
}

# run_capio <engine> -> same, but under the CAPIO server. io_uring is expected
# to fail AND to hang (the consumer blocks forever waiting for data CAPIO never
# received, because it did not intercept the ring), so every app run is wrapped
# in `timeout`: a timeout counts as "not intercepted", not a script hang.
run_capio() {
    local engine="$1" d ok=1 pm cm reason=""
    local prods=() conss=()
    for ((i=0;i<reps;i++)); do
        clean_shm
        d="$(mktemp -d "$work/capio.XXXX")"
        CAPIO_DIR="$d" capio_server -c "$config" >"$d/server.log" 2>&1 &
        server_pid=$!
        sleep 2
        local penv=(CAPIO_DIR="$d" CAPIO_WORKFLOW_NAME="$workflow" LD_PRELOAD=libcapio_posix.so)

        # Redirect each app's output to a log (stderr included) so an engine
        # failure never bleeds into the results table; harvest from the logs.
        timeout 30 env "${penv[@]}" CAPIO_APP_NAME=producer \
            "$bin" -r producer -e "$engine" "${common[@]}" -d "$d" >"$d/prod.log" 2>&1
        timeout 30 env "${penv[@]}" CAPIO_APP_NAME=consumer \
            "$bin" -r consumer -e "$engine" "${common[@]}" -d "$d" >"$d/cons.log" 2>&1

        prods+=("$(mbps_of <"$d/prod.log")")
        conss+=("$(mbps_of <"$d/cons.log")")
        if ! grep -q "verified $n/$n" "$d/cons.log"; then
            ok=0
            # Keep the failure reason as evidence (e.g. the io_uring EBADF that
            # motivates the interception work) instead of discarding it.
            reason="$(grep -m1 -iE "failed|error|bad file descriptor|terminated" \
                        "$d/prod.log" "$d/cons.log" 2>/dev/null | sed 's/.*: //' | head -c 40)"
        fi

        stop_server
        pkill -9 -x one_to_one 2>/dev/null   # reap any process the timeout left
    done
    pm=$(printf '%s\n' "${prods[@]}" | median)
    cm=$(printf '%s\n' "${conss[@]}" | median)
    # Emit a reason only on failure; on success the field stays empty.
    [ "$ok" = 1 ] && reason=""
    echo "$pm $cm $ok $reason"
}

printf '\nio_uring example benchmark\n'
printf '  files=%d  file_size=%d B  chunk=%d B  qd=%d  reps=%d  total=%d MB\n' \
       "$n" "$fsize" "$chunk" "$q" "$reps" "$(( bytes / 1000000 ))"
printf '  work dir: %s\n\n' "$work"
printf '%-14s %-9s %12s %12s   %s\n' "CONFIG" "ENGINE" "PROD MB/s" "CONS MB/s" "VERIFIED"
printf '%s\n' "----------------------------------------------------------------------"

report() {  # report <label> <engine> <layer> <"pm cm ok [reason...]">
    local label="$1" engine="$2" layer="$3" pm cm ok reason
    read -r pm cm ok reason <<<"$4"
    reason="${reason//_/ }"
    local vtxt; [ "$ok" = 1 ] && vtxt="yes" || vtxt="NO — ${reason:-not intercepted}"
    printf '%-14s %-9s %12.1f %12.1f   %s\n' "$label" "$engine" "$pm" "$cm" "$vtxt"
    echo "$label,$engine,$layer,$pm,$cm,$ok,\"${reason}\"" >> "$csv"
}

report "FS-posix"    posix    fs    "$(run_plain posix)"
report "FS-uring"    io_uring fs    "$(run_plain io_uring)"
report "CAPIO-posix" posix    capio "$(run_capio posix)"
report "CAPIO-uring" io_uring capio "$(run_capio io_uring)"

printf '\nCSV written to %s\n' "$csv"
cp "$csv" "$here/bench_results.csv" 2>/dev/null && printf 'copied to %s\n' "$here/bench_results.csv"
