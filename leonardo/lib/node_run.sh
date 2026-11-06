#!/usr/bin/env bash
# One experiment, on one node. Launched by run_exp (common.sh) as one srun task
# per node; SLURM_NODEID is this node's rank. All parameters come from the
# environment (exported by run_exp):
#
#   EXP EXP_LOG SYNC ROWS   experiment id, log dir, barrier dir, CSV row dir
#   CONFIG                  fs-lustre | fs-tmp | capio
#   ENGINE                  posix | io_uring
#   PAIRS                   producer/consumer pairs started per node
#   PLACEMENT               local (consumer on the producer's node) |
#                           cross (consumer on the next node: rank+1 mod N)
#   MODE                    seq (all producers, then all consumers) |
#                           conc (consumers start first; streaming, CAPIO only)
#   NFILES FSIZE CHUNK QD   workload of each pair (one_to_one -n -f -c -q)
#   POSFLAG                 "" or "-p" (io_uring: off = -1)
#   DATA_DIR                shared directory: data (fs-lustre) or CAPIO_DIR (capio)
#   CAPIO_CFG CAPIO_TOK CAPIO_PORT WF   CAPIO server config, tokens, port, workflow
#
# Pair g (global id) writes files DIR/g<g>_file_NNNN.dat.
set -u
rank="$SLURM_NODEID"
nnodes="$SLURM_NNODES"
host="$(hostname -s)"
log="$EXP_LOG/node$rank"
mkdir -p "$log"
exec >"$log/node.log" 2>&1
echo "node $rank/$nnodes on $host, $(date -Is)"

# --- barrier over the shared filesystem -----------------------------------------
barrier() { # barrier <name> [timeout_s]
    local name="$1" limit="${2:-600}" start=$SECONDS
    touch "$SYNC/$name.$rank"
    until [ "$(find "$SYNC" -maxdepth 1 -name "$name.*" | wc -l)" -ge "$nnodes" ]; do
        if [ $((SECONDS - start)) -ge "$limit" ]; then
            echo "barrier $name: timeout after ${limit}s"
            return 1
        fi
        sleep 0.1
    done
}
fail() { # fail <reason>: tell the others, then leave
    echo "FAIL: $1"
    echo "$host: $1" >>"$SYNC/failed"
    stop_server
    exit 1
}

# --- CAPIO server (stop) -------------------------------------------------------
server_pid=""
stop_server() {
    [ -z "$server_pid" ] && return
    kill -TERM "$server_pid" 2>/dev/null
    for _ in $(seq 100); do kill -0 "$server_pid" 2>/dev/null || break; sleep 0.1; done
    kill -KILL "$server_pid" 2>/dev/null
    wait "$server_pid" 2>/dev/null
    server_pid=""
    rm -f /dev/shm/"$WF"* /dev/shm/sem."$WF"* 2>/dev/null
}

# --- data directory -----------------------------------------------------------
case "$CONFIG" in
fs-lustre | capio) dir="$DATA_DIR" ;;
fs-tmp)
    dir="${TMPDIR:-/tmp}/capio_bench_$EXP"
    mkdir -p "$dir"
    ;;
*) fail "unknown CONFIG $CONFIG" ;;
esac

# --- CAPIO server (start) ------------------------------------------------------
if [ "$CONFIG" = capio ]; then
    # Like CAPIO's multinode integration test: the server runs in CAPIO_DIR on a
    # shared filesystem, so every node sees the others' files_location_<host>.txt.
    (cd "$dir" && CAPIO_DIR="$dir" CAPIO_WORKFLOW_NAME="$WF" exec "$CAPIO_SERVER" \
        -c "$CAPIO_CFG" -r "$dir" -b mtcl --discovery fs --token-directory "$CAPIO_TOK" \
        --backend-options "TCP:$CAPIO_PORT@100000") >"$log/server.log" 2>&1 &
    server_pid=$!
    started=0
    for _ in $(seq 600); do
        grep -q "successfully started" "$log/server.log" && { started=1; break; }
        kill -0 "$server_pid" 2>/dev/null || break
        sleep 0.1
    done
    [ "$started" = 1 ] || fail "CAPIO server did not start"
    # Full mesh: every server logs one "Connected to" line per peer.
    connected=0
    for _ in $(seq 1200); do
        [ "$(grep -c "Connected to" "$log/server.log")" -ge $((nnodes - 1)) ] && { connected=1; break; }
        sleep 0.1
    done
    [ "$connected" = 1 ] || fail "CAPIO servers did not connect ($(grep -c "Connected to" "$log/server.log")/$((nnodes - 1)))"
    echo "server up and connected, $(date -Is)"
fi

# --- one producer or consumer -------------------------------------------------------
run_proc() { # run_proc <producer|consumer> <pair id>
    local role="$1" g="$2" out="$log/${1}_g$2.out"
    local -a prefix=()
    if [ "$CONFIG" = capio ]; then
        prefix=(env CAPIO_DIR="$dir" CAPIO_WORKFLOW_NAME="$WF" CAPIO_APP_NAME="$role"
            INTERCEPT_ALL_OBJS=1 LD_PRELOAD="$CAPIO_LIB")
    fi
    local t0 t1 rc
    t0=$(date +%s.%N)
    timeout -s KILL "${PROC_TIMEOUT:-900}" "${prefix[@]}" "$ONE_TO_ONE" -r "$role" -n "$NFILES" \
        -f "$FSIZE" -c "$CHUNK" -q "$QD" $POSFLAG -e "$ENGINE" -d "$dir" -t "g${g}_" >"$out" 2>&1
    rc=$?
    t1=$(date +%s.%N)
    local metrics secs bytes mbps verified
    metrics=$(grep -m1 '^engine=' "$out")
    secs=$(sed -n 's/.* secs=\([^ ]*\).*/\1/p' <<<"$metrics")
    bytes=$(sed -n 's/.* bytes=\([^ ]*\).*/\1/p' <<<"$metrics")
    mbps=$(sed -n 's/.* MBps=\([^ ]*\).*/\1/p' <<<"$metrics")
    verified=$(sed -n 's/^verified \([0-9]*\)\/.*/\1/p' "$out")
    [ "$role" = producer ] && verified=""
    echo "$role,$g,$host,$rank,$t0,$t1,${secs:-},${bytes:-},${mbps:-},${verified:-},$rc" \
        >"$ROWS/${role}_g$g.csv"
    return "$rc"
}

# Pairs whose producer / consumer runs on this node.
producers=()
consumers=()
for ((k = 0; k < PAIRS; k++)); do
    producers+=($((rank * PAIRS + k)))
    if [ "$PLACEMENT" = cross ]; then
        consumers+=($((((rank - 1 + nnodes) % nnodes) * PAIRS + k)))
    else
        consumers+=($((rank * PAIRS + k)))
    fi
done
echo "producers: ${producers[*]}  consumers: ${consumers[*]}"

run_all() { # run_all <role> <ids...>: concurrently, wait for all
    local role="$1" pids=() ok=0
    shift
    for g in "$@"; do run_proc "$role" "$g" & pids+=($!); done
    for p in "${pids[@]}"; do wait "$p" || ok=1; done
    return "$ok"
}

barrier ready || fail "barrier ready"
status=0
if [ "$MODE" = seq ]; then
    run_all producer "${producers[@]}" || status=1
    barrier produced || fail "barrier produced"
    run_all consumer "${consumers[@]}" || status=1
else
    cpids=()
    for g in "${consumers[@]}"; do run_proc consumer "$g" & cpids+=($!); done
    sleep 1 # the consumers are waiting for files that do not exist yet
    barrier consumers_started || fail "barrier consumers_started"
    run_all producer "${producers[@]}" || status=1
    for p in "${cpids[@]}"; do wait "$p" || status=1; done
fi
barrier done || status=1

if [ "$CONFIG" = capio ]; then
    # CAPIO keeps the files in memory: none may reach the real filesystem.
    ondisk=$(find "$dir" -maxdepth 1 -name 'g*_file_*.dat' 2>/dev/null | wc -l)
    echo "CAPIO files on the real filesystem: $ondisk"
    [ "$rank" = 0 ] && echo "$ondisk" >"$EXP_LOG/files_on_disk"
    barrier checked 60 || true
fi
stop_server
[ "$CONFIG" = fs-tmp ] && rm -rf "$dir"
echo "node $rank done, status $status, $(date -Is)"
exit "$status"
