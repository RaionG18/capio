#!/usr/bin/env bash
# Submits the measurement campaign as a chain: each job starts only after the
# previous one completed successfully (afterok), so the campaign holds at most
# one allocation at a time (8 nodes at most) and never floods the shared
# account's queue. A job that aborts (2 failed experiments in a row, see
# lib/common.sh) or hits its walltime stops the chain: the later jobs stay
# pending with reason DependencyNeverSatisfied until cancelled (scancel).
#
#   ./leonardo/campaign.sh              all measurement jobs
#   ./leonardo/campaign.sh --dry-run    print the sbatch commands only
#
# Budget (account ict26_mhpc is shared): measurement jobs take whole nodes
# (112 cores) so other users' jobs do not disturb the timings. Billing is by
# allocated core-hours of the time actually used; walltimes are upper bounds.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
dry=0
[ "${1:-}" = "--dry-run" ] && dry=1

jobs=(
    "jobs/10_single_node.slurm"
    "jobs/11_intranode_scaling.slurm"
    "jobs/20_cross_node.slurm"
    "jobs/30_scaling.slurm --nodes=1"
    "jobs/30_scaling.slurm --nodes=2"
    "jobs/30_scaling.slurm --nodes=4"
    "jobs/30_scaling.slurm --nodes=8"
)

prev=""
for entry in "${jobs[@]}"; do
    read -r script args <<<"$entry"
    dep=()
    [ -n "$prev" ] && dep=(--dependency="afterok:$prev")
    if [ "$dry" = 1 ]; then
        echo "submit.sh $here/$script ${dep[*]} ${args:-}"
        prev="PREV"
        continue
    fi
    # shellcheck disable=SC2086
    prev=$("$here/submit.sh" "$here/$script" "${dep[@]}" ${args:-})
    echo "$prev  $script ${args:-}"
done
