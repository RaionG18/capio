#!/usr/bin/env bash
# Builds the campaign package: plots, results, traces and README in one tarball.
#
#   leonardo/analysis/make_package.sh [OUT_DIR]     (default: leonardo/package)
#
# PYTHON must have matplotlib (default: python3). Reads leonardo/runs/.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
leo="$(cd "$here/.." && pwd)"
runs="$leo/runs"
out="${1:-$leo/package}"
name="capio_leonardo_campaign_$(date +%F)"
pkg="$out/$name"
py="${PYTHON:-python3}"

campaign=(63952926_single_node 63952927_intranode_scaling 63991233_cross_node_ib
    63952931_scaling_n1 63952933_scaling_n2 63952934_scaling_n4 63952935_scaling_n8)
functional=(63688172_smoke 63688173_smoke2 63706758_smoke2 63706761_stress2
    63715391_stress2 63716295_stress2 63790119_stress2 63791667_preflight 63959342_ib_check)
superseded=(63952928_cross_node)
traces=(63992951_capio_logs)
rq2=(63995995_seek_test)
thesis="${THESIS_REPO:-$(cd "$leo/../.." && pwd)/mhpc_thesis_2026}"

rm -rf "$pkg"
mkdir -p "$pkg"/{plots,campaign,functional_tests,superseded,traces,rq2_seek_test,laptop}
copy() { # copy <dest> <run...>
    local dest="$1" r
    shift
    for r in "$@"; do cp -a "$runs/$r" "$pkg/$dest/"; done
}
copy campaign "${campaign[@]}"
copy functional_tests "${functional[@]}"
copy superseded "${superseded[@]}"
copy traces "${traces[@]}"
copy rq2_seek_test "${rq2[@]}"
cp -a "$runs/slurm" "$pkg/slurm"
cp "$here/package_README.md" "$pkg/README.md"

# Plots and the all-runs summary (from the measurement runs only).
plot_runs=()
for r in "${campaign[@]}"; do plot_runs+=("$pkg/campaign/$r"); done
"$py" "$here/plot_scaling.py" "$pkg/plots" "${plot_runs[@]}"
"$py" "$here/plot_makespan.py" "$pkg/plots" "${plot_runs[@]}"
"$py" "$here/plot_seek.py" "$pkg/plots" "$pkg/rq2_seek_test/${rq2[0]}"
"$py" "$here/summarize.py" --all "$pkg/campaign" >/dev/null
# client -> server round trips per MiB, counted in the CAPIO traces
"$py" "$here/trace_roundtrips.py" "$pkg/traces/${traces[0]}" 2>/dev/null | grep -v Downloading \
    >"$pkg/traces/roundtrips.txt"

# Laptop (kernel with io_uring): level 1 (NOP latency) and level 2 (four
# configurations) from the thesis repository.
cp "$thesis"/spikes/f6-level1/{nop_latency.c,run.sh,results_*.csv} "$pkg/laptop/" 2>/dev/null || true
cp "$thesis"/spikes/f6-evaluation/f6_{raw,summary}_campaign_c*.csv "$pkg/laptop/" 2>/dev/null || true
mv "$pkg/campaign/summary_all.csv" "$pkg/summary_all.csv"

tar -czf "$out/$name.tar.gz" -C "$out" "$name"
echo "package: $out/$name.tar.gz ($(du -h "$out/$name.tar.gz" | cut -f1))"
