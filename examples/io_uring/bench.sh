#!/usr/bin/env bash
# Benchmarks the four configurations of the example and prints a table + CSV:
#
#     FS-posix     FS-uring        (baselines, on the real filesystem)
#     CAPIO-posix  CAPIO-uring     (through the CAPIO middleware)
#
# For each: run the producer then the consumer REPS times, report the median
# throughput (MB/s) of each, and confirm the consumer verifies every file.
#
# CAPIO-uring worked as of thesis phase F3: CAPIO owns the io_uring ring and
# serves its SQEs, so the four configurations are all expected to verify. (Before
# F3 this row failed with EBADF; the script still records any failure as
# evidence instead of aborting.) The CAPIO runs need the io_uring-capable build
# and INTERCEPT_ALL_OBJS=1 -- point at them with CAPIO_BIN_DIR / CAPIO_LIB, or
# rely on an installed CAPIO for the plain POSIX baseline.
#
# Usage: bench.sh [binary] [-n N] [-f BYTES] [-c BYTES] [-q DEPTH] [-r REPS] [-D DIR]
# Env:   CAPIO_LIB      path to libcapio_posix.so (default: libcapio_posix.so, found via loader)
#        CAPIO_SERVER   path to capio_server binary (default: capio_server on PATH)
#        CAPIO_INTERCEPT_ALL  set to 0 to disable INTERCEPT_ALL_OBJS (default: 1)
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bin="$here/one_to_one"
config="$here/capio_config.json"
workflow="io_uring_example"

# CAPIO bits: overridable so the benchmark can target a local build that has the
# io_uring handlers, not just an installed CAPIO.
capio_lib="${CAPIO_LIB:-libcapio_posix.so}"
capio_server_bin="${CAPIO_SERVER:-capio_server}"
intercept_all="${CAPIO_INTERCEPT_ALL:-1}"

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

median() {  # median of stdin numbers; 0 if there are none (e.g. failed runs)
    sort -n | awk '$1!=""{a[++m]=$1} END{ if(m==0){print 0} else if(m%2){print a[(m+1)/2]} else {print (a[m/2]+a[m/2+1])/2} }'
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
    echo "$pm|$cm|$ok|"
}

# run_capio <engine> -> same, but under the CAPIO server. Every app run is
# wrapped in `timeout` so a hang (e.g. a broken run where the consumer waits for
# data that never arrives) counts as a failure rather than hanging the script.
run_capio() {
    local engine="$1" d ok=1 pm cm reason=""
    local prods=() conss=()
    for ((i=0;i<reps;i++)); do
        clean_shm
        d="$(mktemp -d "$work/capio.XXXX")"
        CAPIO_DIR="$d" "$capio_server_bin" -c "$config" >"$d/server.log" 2>&1 &
        server_pid=$!
        sleep 2
        local penv=(CAPIO_DIR="$d" CAPIO_WORKFLOW_NAME="$workflow" LD_PRELOAD="$capio_lib")
        # io_uring under CAPIO needs liburing patched too (dev mode).
        [ "$intercept_all" = 1 ] && penv+=(INTERCEPT_ALL_OBJS=1)

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
    echo "$pm|$cm|$ok|$reason"
}

printf '\nio_uring example benchmark\n'
printf '  files=%d  file_size=%d B  chunk=%d B  qd=%d  reps=%d  total=%d MB\n' \
       "$n" "$fsize" "$chunk" "$q" "$reps" "$(( bytes / 1000000 ))"
printf '  work dir: %s\n\n' "$work"
printf '%-14s %-9s %12s %12s   %s\n' "CONFIG" "ENGINE" "PROD MB/s" "CONS MB/s" "VERIFIED"
printf '%s\n' "----------------------------------------------------------------------"

report() {  # report <label> <engine> <layer> <"pm cm ok [reason...]">
    local label="$1" engine="$2" layer="$3" pm cm ok reason
    IFS='|' read -r pm cm ok reason <<<"$4"
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
