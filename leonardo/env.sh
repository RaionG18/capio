# Environment for building and running CAPIO on Leonardo (DCGP partition).
# Sourced by build.sh, submit.sh and every job script.

module purge >/dev/null 2>&1
module load gcc/12.2.0 cmake/3.27.9

export CAPIO_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export LEO_DIR="$CAPIO_REPO/leonardo"

# Slurm account and partition.
export LEO_ACCOUNT="${LEO_ACCOUNT:-ict26_mhpc}"
export LEO_PARTITION="${LEO_PARTITION:-dcgp_usr_prod}"

# Build products (not in git). Each path can be overridden from the environment.
export LEO_DEPS="${LEO_DEPS:-$LEO_DIR/.deps}"
export LIBURING_PREFIX="${LIBURING_PREFIX:-$LEO_DEPS/liburing}"
export CAPIO_BUILD="${CAPIO_BUILD:-$CAPIO_REPO/build-leonardo}"
export CAPIO_SERVER="${CAPIO_SERVER:-$CAPIO_BUILD/capio/server/capio_server}"
export CAPIO_LIB="${CAPIO_LIB:-$CAPIO_BUILD/capio/posix/libcapio_posix.so}"
export ONE_TO_ONE="${ONE_TO_ONE:-$LEO_DEPS/bin/one_to_one}"

export PKG_CONFIG_PATH="$LIBURING_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$LIBURING_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Results: one directory per run, <jobid>_<test>, kept in the repo clone.
export RESULTS_ROOT="${RESULTS_ROOT:-$LEO_DIR/results}"
# Benchmark data and CAPIO_DIR: Lustre scratch, shared by all nodes.
export SCRATCH_ROOT="${SCRATCH_ROOT:-$CINECA_SCRATCH/capio_runs}"
