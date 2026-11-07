#!/usr/bin/env bash
# Two CAPIO "nodes" on one machine, for reproducing multi-node problems without
# the cluster. Each node runs in its own user/mount/UTS/PID namespace (its own
# hostname, /etc/hosts and private /dev/shm); --kill-child guarantees nothing
# outlives the run.
#
#   repro.sh ENGINE MODE [-p]        ENGINE: io_uring | posix   MODE: seq | conc
#
# Environment:
#   B        CAPIO build dir (default: build-test next to this repo)
#   O        one_to_one binary
#   RUN      run directory (default: ${TMPDIR:-/tmp}/capio_local2_$USER)
#   ATAGS    "producer-tag consumer-tag" of node A (default "g0 -")
#   BTAGS    same for node B (default "- g0"): A produces g0, B consumes it
#            (cross conc both ways: ATAGS="g0 g1" BTAGS="g1 g0")
#   NF FS CH files, file size, chunk (one_to_one -n -f -c)
#   GDB=1    run the consumers under gdb and collect backtraces on a hang
#   SRVWRAP  command prefix for the servers (e.g. strace)
#   WRAP     command prefix for the clients
set -u
S=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$S/../../.." && pwd)
export S B=${B:-$REPO/build-test} O=${O:-$REPO/examples/io_uring/build/one_to_one}
export ENGINE=$1 MODE=$2 POS=${3:-} R=${RUN:-${TMPDIR:-/tmp}/capio_local2_$USER}
# Disk guard: /tmp and /dev/shm may be RAM; refuse to run when low.
for m in /tmp /dev/shm /; do
    free_kb=$(df -Pk "$m" | awk 'NR==2{print $4}')
    [ "$free_kb" -lt 2097152 ] && { echo "ABORT: less than 2 GB free on $m"; exit 9; }
done
rm -rf "$R"; mkdir -p "$R/capio_dir" "$R/tok" "$R/logs" "$R/sync"
printf '127.0.0.1 localhost nodeA nodeB\n' >"$R/hosts"
cat >"$R/cfg.json" <<J
{"name":"rep","IO_Graph":[{"name":"producer","input_stream":[],"output_stream":["*file_*.dat"],"streaming":[{"name":["*file_*.dat"],"committed":"on_close","mode":"no_update"}]},{"name":"consumer","input_stream":["*file_*.dat"],"output_stream":[]}]}
J
unshare -Urm --uts --pid --fork --kill-child bash "$S/node.sh" nodeA 7701 ${ATAGS:-g0 -} & a=$!
unshare -Urm --uts --pid --fork --kill-child bash "$S/node.sh" nodeB 7702 ${BTAGS:-- g0} & b=$!
if [ -n "${GDB:-}" ]; then
    # hang detector: after 15 s with a consumer still running, SIGINT every client and
    # server so gdb prints the backtraces
    ( sleep 15; if pgrep -f "one_to_one -r consumer" >/dev/null; then echo "HANG detected: collecting backtraces"; pkill -INT -x one_to_one; pkill -INT -x capio_server; sleep 5; touch "$R/sync/done.nodeA" "$R/sync/done.nodeB"; fi; sleep 40; kill -9 $a $b 2>/dev/null ) & w=$!
else
    ( sleep 60; kill -9 $a $b 2>/dev/null ) & w=$!
fi
wait $a $b; kill $w 2>/dev/null
# sweep: nothing of this run may outlive it (servers hold RAM in /dev/shm and the ports)
for pat in "$R/cfg.json" "$R/capio_dir"; do pkill -9 -f -- "$pat" 2>/dev/null; done
rm -rf "$R"/capio_dir/*.dat "$R"/capio_dir/capio_logs # data of this run; logs stay
for n in nodeA nodeB; do [ -f "$R/logs/$n/consumer.out" ] && echo "$n consumer says: $(tail -1 "$R/logs/$n/consumer.out")"; done
