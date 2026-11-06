# Shared code for the Leonardo job scripts. Source it from a job, then:
#
#   run_init <test name>          results dir, logging, metadata, CSV headers
#   run_exp key=value ...         one experiment (one srun step over all nodes)
#   run_finish                    summary and cleanup
#
# Every run writes to $RESULTS_ROOT/<jobid>_<test>/:
#   job.log          everything the job printed
#   meta.txt         commit, nodes, modules, parameters
#   experiments.csv  one row per experiment (parameters, status, wall time)
#   results.csv      one row per producer/consumer process
#   summary.txt      aggregate bandwidth per experiment (written by run_finish)
#   logs/<exp>/      server, node and program output of each experiment

source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

EXP_COLUMNS="run_id,exp,test,config,engine,nodes,pairs,placement,mode,nfiles,fsize,chunk,qd,pos,rep"

run_init() {
    TEST_NAME="$1"
    RUN_ID="${SLURM_JOB_ID}_${TEST_NAME}"
    RUN_DIR="$RESULTS_ROOT/$RUN_ID"
    DATA_ROOT="$SCRATCH_ROOT/$RUN_ID"
    mkdir -p "$RUN_DIR/logs" "$DATA_ROOT"
    exec > >(tee -a "$RUN_DIR/job.log") 2>&1

    mapfile -t NODES < <(scontrol show hostnames "$SLURM_JOB_NODELIST")
    NNODES=${#NODES[@]}
    EXP_SEQ=0
    CPUS_PER_NODE="${SLURM_CPUS_ON_NODE:-112}"

    {
        echo "run_id:      $RUN_ID"
        echo "test:        $TEST_NAME"
        echo "date:        $(date -Is)"
        echo "commit:      $(git -C "$CAPIO_REPO" rev-parse HEAD) ($(git -C "$CAPIO_REPO" rev-parse --abbrev-ref HEAD))"
        echo "dirty:       $(git -C "$CAPIO_REPO" status --porcelain --untracked-files=no | wc -l) modified files"
        echo "nodes:       $NNODES (${NODES[*]})"
        echo "cpus/node:   $CPUS_PER_NODE"
        echo "partition:   ${SLURM_JOB_PARTITION:-?}  account: ${SLURM_JOB_ACCOUNT:-?}"
        echo "kernel:      $(uname -r)"
        echo "io_uring:    kernel has none (ENOSYS); CAPIO-uring runs without a kernel ring"
        echo "capio:       $CAPIO_SERVER"
        echo "benchmark:   $ONE_TO_ONE"
        echo "data root:   $DATA_ROOT (Lustre)"
        echo "modules:"
        module list 2>&1 | sed 's/^/  /'
    } >"$RUN_DIR/meta.txt"
    cat "$RUN_DIR/meta.txt"

    echo "$EXP_COLUMNS,status,wall_s,files_on_disk" >"$RUN_DIR/experiments.csv"
    echo "$EXP_COLUMNS,role,pair,host,node_rank,t_start,t_end,secs,bytes,MBps,verified,rc" \
        >"$RUN_DIR/results.csv"
    trap run_cleanup EXIT
}

# Kills anything an aborted experiment left behind, on every node.
run_cleanup() {
    srun --overlap -N "$NNODES" --ntasks-per-node=1 bash -c \
        'pkill -u "$USER" -x capio_server; pkill -u "$USER" -x one_to_one; rm -rf /dev/shm/o2o* /dev/shm/sem.o2o* "${TMPDIR:-/tmp}"/capio_bench_*' \
        >/dev/null 2>&1 || true
}

# run_exp config=capio engine=io_uring pairs=1 placement=local mode=seq \
#         nfiles=64 fsize=8M chunk=256K qd=32 pos=0 rep=1 [nodes=N] [test=label]
# Sizes accept K/M/G suffixes (powers of 2). test= labels the rows (default:
# the run's test name), so a job can hold several studies.
run_exp() {
    local config=capio engine=posix pairs=1 placement=local mode=seq nfiles=64
    local fsize=8M chunk=256K qd=32 pos=0 rep=1 nodes="$NNODES" test="$TEST_NAME" kv
    for kv in "$@"; do local "$kv"; done
    local fsize_b chunk_b
    fsize_b=$(numfmt --from=iec "$fsize")
    chunk_b=$(numfmt --from=iec "$chunk")

    EXP_SEQ=$((EXP_SEQ + 1))
    local exp
    exp=$(printf "e%03d_%s_%s_n%s_p%s_%s_%s_c%s_q%s%s_r%s" "$EXP_SEQ" "$config" "$engine" "$nodes" \
        "$pairs" "$placement" "$mode" "$chunk" "$qd" "$([ "$pos" = 1 ] && echo _pos)" "$rep")
    local exp_log="$RUN_DIR/logs/$exp"
    local exp_data="$DATA_ROOT/$exp"
    mkdir -p "$exp_log" "$exp_data/sync" "$exp_data/rows" "$exp_data/data" "$exp_data/tokens"

    local prefix="$RUN_ID,$exp,$test,$config,$engine,$nodes,$pairs,$placement,$mode,$nfiles,$fsize_b,$chunk_b,$qd,$pos,$rep"
    local skip=""
    [ "$config" = fs-tmp ] && [ "$placement" = cross ] && [ "$nodes" -gt 1 ] && skip="fs-tmp is node-local"
    [ "$config" != capio ] && [ "$mode" = conc ] && skip="conc needs CAPIO streaming"
    [ "$config" != capio ] && [ "$engine" = io_uring ] && skip="no io_uring in the kernel"
    if [ -n "$skip" ]; then
        echo "-- $exp: skipped ($skip)"
        echo "$prefix,skipped,," >>"$RUN_DIR/experiments.csv"
        return 0
    fi

    # The workflow name keys CAPIO's /dev/shm objects; a fresh one per experiment.
    local cfg="$exp_data/capio_config.json"
    cat >"$cfg" <<EOF
{
    "name": "o2o${SLURM_JOB_ID}x$EXP_SEQ",
    "IO_Graph": [
        {
            "name": "producer",
            "input_stream": [],
            "output_stream": ["*file_*.dat"],
            "streaming": [
                {"name": ["*file_*.dat"], "committed": "on_close", "mode": "no_update"}
            ]
        },
        {
            "name": "consumer",
            "input_stream": ["*file_*.dat"],
            "output_stream": []
        }
    ]
}
EOF

    echo "-- $exp: start $(date +%T)"
    local t0=$SECONDS status=ok
    EXP="$exp" EXP_LOG="$exp_log" SYNC="$exp_data/sync" ROWS="$exp_data/rows" \
        CONFIG="$config" ENGINE="$engine" PAIRS="$pairs" PLACEMENT="$placement" MODE="$mode" \
        NFILES="$nfiles" FSIZE="$fsize_b" CHUNK="$chunk_b" QD="$qd" \
        POSFLAG="$([ "$pos" = 1 ] && echo -p)" DATA_DIR="$exp_data/data" \
        CAPIO_CFG="$cfg" CAPIO_TOK="$exp_data/tokens" CAPIO_PORT=$((7600 + EXP_SEQ % 1000)) \
        WF="o2o${SLURM_JOB_ID}x$EXP_SEQ" \
        timeout -s TERM "${EXP_TIMEOUT:-1200}" \
        srun --overlap -N "$nodes" --ntasks-per-node=1 -c "$CPUS_PER_NODE" --cpu-bind=none \
        --kill-on-bad-exit=0 bash "$LEO_DIR/lib/node_run.sh" || status=failed
    local wall=$((SECONDS - t0))
    [ -f "$exp_data/sync/failed" ] && status=failed

    local rows=0 f
    for f in "$exp_data"/rows/*.csv; do
        [ -e "$f" ] || continue
        echo "$prefix,$(cat "$f")" >>"$RUN_DIR/results.csv"
        rows=$((rows + 1))
    done
    local expected=$((nodes * pairs * 2))
    [ "$rows" -lt "$expected" ] && status=failed
    echo "$prefix,$status,$wall,$(cat "$exp_log/files_on_disk" 2>/dev/null)" >>"$RUN_DIR/experiments.csv"
    exp_summary "$exp"
    echo "-- $exp: $status in ${wall}s ($rows/$expected processes reported)"
    [ "$status" = ok ] || run_cleanup
    rm -rf "$exp_data/data" # benchmark data; logs and rows stay
    return 0
}

# Aggregate per role: bytes / (last end - first start) over all processes.
exp_summary() {
    awk -F, -v want="$1" '
        NR == 1 { for (i = 1; i <= NF; i++) col[$i] = i; next }
        $col["exp"] != want { next }
        {
            r = $col["role"]; n[r]++; b[r] += $col["bytes"]
            if (!(r in s) || $col["t_start"] < s[r]) s[r] = $col["t_start"]
            if (!(r in e) || $col["t_end"] > e[r]) e[r] = $col["t_end"]
            if ($col["rc"] != 0) bad[r]++
            if (r == "consumer") { v += $col["verified"]; vt += $col["nfiles"] }
        }
        END {
            for (r in n) printf "   %-8s procs=%-3d agg=%9.1f MB/s  span=%7.3fs  failed=%d\n",
                r, n[r], b[r] / 1e6 / (e[r] - s[r]), e[r] - s[r], bad[r] + 0
            if (vt) printf "   verified %d/%d files\n", v, vt
        }' "$RUN_DIR/results.csv"
}

run_finish() {
    local ok failed skipped
    ok=$(grep -c ',ok,' "$RUN_DIR/experiments.csv")
    failed=$(grep -c ',failed,' "$RUN_DIR/experiments.csv")
    skipped=$(grep -c ',skipped,' "$RUN_DIR/experiments.csv")
    if command -v python3 >/dev/null; then
        python3 "$LEO_DIR/analysis/summarize.py" "$RUN_DIR" >"$RUN_DIR/summary.txt" 2>&1 || true
        cat "$RUN_DIR/summary.txt"
    fi
    echo
    echo "== $RUN_ID: $ok ok, $failed failed, $skipped skipped  ($(date -Is))"
    echo "== results in $RUN_DIR"
    rm -rf "$DATA_ROOT"
}
