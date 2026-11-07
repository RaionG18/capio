#!/bin/bash
#SBATCH --job-name=capio_preflight
#SBATCH --time=00:20:00
#SBATCH --nodes=8
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --gres=tmpfs:20g
#
# Functional test: a shared slice of each node (8 cores, 32 GB), not whole nodes.
# Before the campaign: every shape the campaign uses that no earlier test ran,
# with small files (8 x 4 MiB per pair), both engines.
#   8 and 4 nodes      full mesh of servers, cross-node streaming (job 30)
#   2 nodes, 8 pairs   cross and local streaming with 8 pairs per node (job 20)
#   1 node, 32 pairs   intra-node scaling (job 11)
#   1 node             chunk 4K, queue depth 1 and 64, -p, conc (job 10)
# The walltime bounds the cost, so failures do not abort this job: we want to
# see all of them.
set -uo pipefail
export MAX_CONSECUTIVE_FAILS=1000
source "${CAPIO_REPO:-$SLURM_SUBMIT_DIR}/leonardo/lib/common.sh"
run_init preflight

small=(nfiles=8 fsize=4M)

echo "==== 8 and 4 nodes"
for engine in io_uring posix; do
    run_exp config=capio engine=$engine placement=cross mode=conc pairs=4 nodes=8 "${small[@]}"
done
run_exp config=capio engine=io_uring placement=cross mode=seq pairs=4 nodes=8 "${small[@]}"
run_exp config=fs-lustre engine=posix placement=cross mode=seq pairs=4 nodes=8 "${small[@]}"
for engine in io_uring posix; do
    run_exp config=capio engine=$engine placement=cross mode=conc pairs=4 nodes=4 "${small[@]}"
done

echo "==== 2 nodes, 8 pairs"
for engine in io_uring posix; do
    run_exp config=capio engine=$engine placement=cross mode=conc pairs=8 nodes=2 "${small[@]}"
done
run_exp config=capio engine=io_uring placement=local mode=conc pairs=8 nodes=2 "${small[@]}"

echo "==== 1 node, 32 pairs"
for engine in io_uring posix; do
    run_exp config=capio engine=$engine mode=seq pairs=32 nodes=1 "${small[@]}"
done
run_exp config=fs-tmp engine=posix mode=seq pairs=32 nodes=1 "${small[@]}"
run_exp config=fs-lustre engine=posix mode=seq pairs=32 nodes=1 "${small[@]}"

echo "==== 1 node: chunk, queue depth, -p, conc"
for engine in io_uring posix; do
    run_exp config=capio engine=$engine chunk=4K nodes=1 "${small[@]}"
done
for qd in 1 64; do
    run_exp config=capio engine=io_uring qd=$qd nodes=1 "${small[@]}"
done
run_exp config=capio engine=io_uring pos=1 nodes=1 "${small[@]}"
run_exp config=capio engine=io_uring mode=conc nodes=1 "${small[@]}"

run_finish
