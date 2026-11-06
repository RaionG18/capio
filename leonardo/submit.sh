#!/usr/bin/env bash
# Submits a job script with its Slurm output under leonardo/results/slurm/.
#
#   ./leonardo/submit.sh leonardo/jobs/00_smoke.slurm
#   ./leonardo/submit.sh leonardo/jobs/30_scaling.slurm --nodes=4
#
# Extra arguments go to sbatch (they override the script's #SBATCH lines).
# Each run writes its results to leonardo/results/<jobid>_<test>/.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

[ $# -ge 1 ] || { sed -n '2,8p' "$0"; exit 2; }
job="$(realpath "$1")"
shift
[ -x "$CAPIO_SERVER" ] && [ -x "$ONE_TO_ONE" ] || { echo "Build first: ./leonardo/build.sh"; exit 1; }

mkdir -p "$RESULTS_ROOT/slurm"
cd "$CAPIO_REPO"
sbatch --parsable --account="$LEO_ACCOUNT" --partition="$LEO_PARTITION" \
    --output="$RESULTS_ROOT/slurm/%j_%x.out" "$@" "$job"
