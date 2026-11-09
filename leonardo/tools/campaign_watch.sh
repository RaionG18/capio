#!/usr/bin/env bash
# Watches a submitted campaign from the laptop (ssh alias leo) and exits on the
# first event worth a look, printing it:
#   LIMIT      campaign core-hours above LIMIT_COREH: every job cancelled
#   JOBFAIL    a job ended not COMPLETED/0:0: the pending rest cancelled
#   EXPFAIL    a running job recorded a failed experiment (no action)
#   DONE <id>  a job completed (time to check its results)
#   FINISHED   no campaign job left in the queue
#   campaign_watch.sh LIMIT_COREH JOBID...   (state of reported jobs in $STATE)
set -u
limit=$1; shift; ids=("$@"); list=$(IFS=,; echo "${ids[*]}")
STATE=${STATE:-$HOME/.cache/campaign_watch.state}; mkdir -p "$(dirname "$STATE")"; touch "$STATE"
while true; do
    out=$(ssh -o LogLevel=ERROR -o ConnectTimeout=30 leo "
        sacct -j $list -X -n -P -o JobID,State,ExitCode,CPUTimeRAW 2>/dev/null
        echo ---
        squeue -j $list -h -o '%i %T %r' 2>/dev/null
        echo ---
        for d in ~/capio/leonardo/results/{$list}_*; do [ -f \$d/experiments.csv ] && echo \"\$(basename \$d) \$(grep -c ',failed,' \$d/experiments.csv) \$(grep -c ',ok,' \$d/experiments.csv)\"; done 2>/dev/null; true" 2>/dev/null) || { sleep 300; continue; }
    acct=$(sed -n '1,/^---$/p' <<<"$out" | grep -v '^---$')
    queue=$(sed -n '/^---$/,/^---$/p' <<<"$out" | sed -n '2,$p' | grep -v '^---$')
    exps=$(awk '/^---$/{f++; next} f==2' <<<"$out")
    coreh=$(awk -F'|' '{s+=$4} END{printf "%.0f", s/3600}' <<<"$acct")
    if [ "$coreh" -gt "$limit" ]; then
        ssh -o LogLevel=ERROR leo "scancel $(echo ${ids[*]})"
        echo "LIMIT: campaign at ${coreh} core-h > ${limit}: cancelled every job"; echo "$acct"; exit 0
    fi
    while IFS='|' read -r id st ec cpu; do
        [ -z "$id" ] && continue
        case "$st" in PENDING|RUNNING|REQUEUED|CONFIGURING|COMPLETING) continue;; esac
        grep -qx "$id" "$STATE" && continue
        echo "$id" >>"$STATE"
        if [ "$st" = COMPLETED ] && [ "$ec" = 0:0 ]; then
            echo "DONE $id ($((cpu / 3600)) core-h; campaign ${coreh} core-h)"; echo "$exps"; exit 0
        fi
        pend=$(awk '$2=="PENDING"{print $1}' <<<"$queue" | tr '\n' ' ')
        [ -n "$pend" ] && ssh -o LogLevel=ERROR leo "scancel $pend"
        echo "JOBFAIL $id state=$st exit=$ec: cancelled pending [$pend]; campaign ${coreh} core-h"; echo "$exps"; exit 0
    done <<<"$acct"
    while read -r run failed ok; do
        [ -z "$run" ] && continue
        key="exp:$run:$failed"
        [ "$failed" -gt 0 ] && ! grep -qx "$key" "$STATE" && { echo "$key" >>"$STATE"; echo "EXPFAIL $run: $failed failed, $ok ok so far; campaign ${coreh} core-h"; exit 0; }
    done <<<"$exps"
    [ -z "$queue" ] && { echo "FINISHED: campaign ${coreh} core-h"; echo "$exps"; exit 0; }
    sleep 300
done
