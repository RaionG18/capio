#!/usr/bin/env bash
# One fake node (run by repro.sh inside unshare -Urm --uts --pid):
#   node.sh NAME PORT PTAG CTAG   producer of PTAG and consumer of CTAG ("-" = none)
set -u
NAME=$1 PORT=$2 PTAG=$3 CTAG=$4
hostname "$NAME"; mount -t tmpfs tmpfs /dev/shm; mount --bind "$R/hosts" /etc/hosts
log=$R/logs/$NAME; mkdir -p "$log"
(cd "$R/capio_dir" && CAPIO_DIR=$R/capio_dir CAPIO_WORKFLOW_NAME=rep exec ${SRVWRAP:-} ${GDB:+gdb -q -batch -ex run -ex "thread apply all bt" -ex kill --args} "$B/capio/server/capio_server" -c "$R/cfg.json" -r "$R/capio_dir" -b mtcl --discovery fs --token-directory "$R/tok" --backend-options "TCP:$PORT@100000") >"$log/server.log" 2>&1 &
sp=$!
for i in $(seq 100); do grep -q "Connected to" "$log/server.log" && break; sleep 0.1; done
grep -q "Connected to" "$log/server.log" || echo "$NAME: not connected"
touch "$R/sync/up.$NAME"; until [ -e "$R/sync/up.nodeA" ] && [ -e "$R/sync/up.nodeB" ]; do sleep 0.1; done
run() { local tag=$2
    if [ -n "${GDB:-}" ] && [ "$1" = consumer ]; then
        CAPIO_DIR=$R/capio_dir CAPIO_WORKFLOW_NAME=rep CAPIO_APP_NAME=$1 INTERCEPT_ALL_OBJS=1 timeout -s KILL 40 gdb -q -batch -ex "set exec-wrapper env LD_PRELOAD=$B/capio/posix/libcapio_posix.so ${WRAP:-}" -ex run -ex "thread apply all bt" -ex kill --args "$O" -r $1 -n ${NF:-2} -f ${FS:-1048576} -c ${CH:-262144} -q 32 $POS -e $ENGINE -d "$R/capio_dir" -t ${tag}_ >"$log/$1.out" 2>&1; echo "$NAME $1 $tag rc=$?"; return
    fi
    CAPIO_DIR=$R/capio_dir CAPIO_WORKFLOW_NAME=rep CAPIO_APP_NAME=$1 INTERCEPT_ALL_OBJS=1 LD_PRELOAD=$B/capio/posix/libcapio_posix.so ${WRAP:-} timeout -s KILL 20 "$O" -r $1 -n ${NF:-2} -f ${FS:-1048576} -c ${CH:-262144} -q 32 $POS -e $ENGINE -d "$R/capio_dir" -t ${tag}_ >"$log/$1.out" 2>&1; echo "$NAME $1 $tag rc=$?"; }
pids=()
if [ "$MODE" = conc ]; then
    [ "$CTAG" != - ] && { run consumer $CTAG & pids+=($!); }
    sleep 1; touch "$R/sync/go.$NAME"; until [ -e "$R/sync/go.nodeA" ] && [ -e "$R/sync/go.nodeB" ]; do sleep 0.1; done
    [ "$PTAG" != - ] && { run producer $PTAG & pids+=($!); }
    wait "${pids[@]}"
else
    [ "$PTAG" != - ] && run producer $PTAG
    touch "$R/sync/prod.$NAME"; until [ -e "$R/sync/prod.nodeA" ] && [ -e "$R/sync/prod.nodeB" ]; do sleep 0.1; done
    [ "$CTAG" != - ] && run consumer $CTAG
fi
touch "$R/sync/done.$NAME"; until [ -e "$R/sync/done.nodeA" ] && [ -e "$R/sync/done.nodeB" ]; do sleep 0.1; done
kill -0 $sp 2>/dev/null && echo "$NAME server alive at end" || echo "$NAME server DEAD at end"
kill $sp 2>/dev/null; sleep 1; kill -9 $sp 2>/dev/null; wait $sp 2>/dev/null
