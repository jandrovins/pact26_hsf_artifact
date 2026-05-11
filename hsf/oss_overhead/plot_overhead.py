#!/usr/bin/env python3
"""
Plot runtime overhead comparison from CSV files in the consolidated/ directory.

Usage:
    python plot_overhead.py <consolidated_dir> [ntasks_filter]

    e.g.  python plot_overhead.py results 307200

Each CSV file is named <runtime>_ntasks<N>_niters<M>_<TIMESTAMP>.csv and contains
one row per run with columns: run,wall_s,ntasks,niters,mean_us,std_us,min_us,p25_us,
p50_us,p75_us,max_us

Produces a single overhead_comparison.pdf with three stacked panels sharing a
continuous log x-axis (median task time in µs):
  1. Single Creator   — wall time for non-taskloop runtimes
  2. Parallel Creators — wall time for taskloop runtimes
  3. Benchmark strip  — IQR of dominant task duration per HSF benchmark
"""

import sys
import re
import argparse
from pathlib import Path

import pandas as pd
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
import matplotlib.lines as mlines

# ---------------------------------------------------------------------------
# LaTeX-compatible style
# ---------------------------------------------------------------------------

matplotlib.rcParams.update({
    "text.usetex": False,
    "font.family": "serif",
    "mathtext.fontset": "cm",
    "font.size": 8,
    "axes.titlesize": 14,
    "axes.labelsize": 12,
    "xtick.labelsize": 12,
    "ytick.labelsize": 12,
    "legend.fontsize": 12,
    "axes.linewidth": 0.6,
    "xtick.major.width": 0.6,
    "ytick.major.width": 0.6,
    "lines.linewidth": 1.2,
    "patch.linewidth": 0.4,
    "savefig.dpi": 300,
    "pdf.fonttype": 42,
    "ps.fonttype": 42,
})

# ---------------------------------------------------------------------------
# CLI arguments
# ---------------------------------------------------------------------------

_parser = argparse.ArgumentParser(description=__doc__)
_parser.add_argument("consolidated_dir", nargs="?", default="consolidated",
                     help="directory with CSV files (default: consolidated)")
_parser.add_argument("ntasks_filter", nargs="?", type=int, default=None,
                     help="restrict to this ntasks value (e.g. 307200)")
_parser.add_argument("--calibration-bands", action="store_true",
                     help="show per-runtime min--max shaded bands on the bottom "
                          "subplot of the calibration figure (off by default)")
_args = _parser.parse_args()

consolidated_dir = Path(_args.consolidated_dir)
ntasks_filter    = _args.ntasks_filter
NCORES = 192

# ---------------------------------------------------------------------------
# Load data
# ---------------------------------------------------------------------------

pattern = re.compile(r"^(?P<runtime>\w+)_ntasks(?P<ntasks>\d+)_niters(?P<niters>\d+)_(?P<timestamp>\d+)")

RUNTIME_RENAME = {
    "oss_tggrp_cs_cachingtrue_":          "oss_tggrp_cs_caching",
    "oss_tggrp_taskloop_cs_cachingtrue_": "oss_tggrp_taskloop_cs_caching",
}

frames = []
for csv_file in sorted(consolidated_dir.glob("*.csv")):
    m = pattern.match(csv_file.name)
    if not m:
        continue
    df = pd.read_csv(csv_file)
    df["runtime"]    = RUNTIME_RENAME.get(m.group("runtime"), m.group("runtime"))
    df["ntasks_key"] = int(m.group("ntasks"))
    df["timestamp"]  = m.group("timestamp")
    frames.append(df)

if not frames:
    print(f"No CSV files found in {consolidated_dir}", file=sys.stderr)
    sys.exit(1)

data = pd.concat(frames, ignore_index=True)

# ---------------------------------------------------------------------------
# Filter by ntasks if requested
# ---------------------------------------------------------------------------

if ntasks_filter is not None:
    data = data[data["ntasks_key"] == ntasks_filter]
    if data.empty:
        print(f"No data found for ntasks={ntasks_filter}", file=sys.stderr)
        sys.exit(1)
    print(f"Filtering to ntasks={ntasks_filter}")

# ---------------------------------------------------------------------------
# Warn on duplicate configurations
# ---------------------------------------------------------------------------

file_counts = (
    data.groupby(["runtime", "ntasks_key", "niters"])["timestamp"]
    .nunique()
)
dupes = file_counts[file_counts > 1]
if not dupes.empty:
    print("WARNING: multiple timestamps for same configuration — runs pooled:")
    for (rt, nt_k, ni), count in dupes.items():
        print(f"  {rt} ntasks={nt_k} niters={ni}: {count} timestamps")

# ---------------------------------------------------------------------------
# Print found configurations
# ---------------------------------------------------------------------------

print(f"Configurations found in {consolidated_dir}:")
for rt, grp in data.groupby("runtime"):
    niters_vals    = sorted(grp["niters"].unique())
    ntasks_vals    = sorted(grp["ntasks_key"].unique())
    runs_per_combo = grp.groupby("niters")["run"].count().median()
    print(f"  {rt}: ntasks={ntasks_vals}, niters={niters_vals}, ~{int(runs_per_combo)} runs/combo")

# ---------------------------------------------------------------------------
# Aggregate: per (runtime, ntasks_key, niters) stats across all runs
# ---------------------------------------------------------------------------

def _p25(x): return np.percentile(x, 25)
def _p75(x): return np.percentile(x, 75)

agg = (
    data.groupby(["runtime", "ntasks_key", "niters"])
    .agg(
        nruns        =("run",    "count"),
        # Task time: mean of per-run medians (Option A) is the headline central
        # tendency.  Each per-run p50_us is itself a robust within-run estimator
        # (median of ntasks samples), so averaging N of them gives a tight
        # estimate; SE = std / sqrt(N) quantifies cross-run uncertainty.
        mean_p50     =("p50_us", "mean"),
        sem_p50      =("p50_us", "sem"),
        std_p50      =("p50_us", "std"),
        # Per-run p50 distribution kept for diagnostics; these are min/max OF
        # per-run medians, NOT min/max task durations.
        median_p50_across_runs=("p50_us", "median"),
        p25_p50_across_runs   =("p50_us", _p25),
        p75_p50_across_runs   =("p50_us", _p75),
        min_p50_across_runs   =("p50_us", "min"),
        max_p50_across_runs   =("p50_us", "max"),
        # Wall time: median + IQR (used for error bars in figure).
        median_wall  =("wall_s", "median"),
        p25_wall     =("wall_s", _p25),
        p75_wall     =("wall_s", _p75),
        wall_mean_s  =("wall_s", "mean"),
        wall_std_s   =("wall_s", "std"),
        wall_min_s   =("wall_s", "min"),
        wall_max_s   =("wall_s", "max"),
    )
    .reset_index()
    .sort_values("niters")
)

# Warn if run counts differ across configurations (sampling-error asymmetry).
_nruns_by_rt = agg.groupby("runtime")["nruns"].agg(["min", "max"])
if (_nruns_by_rt["min"] != _nruns_by_rt["max"]).any() or _nruns_by_rt["min"].nunique() > 1:
    print("WARNING: run counts differ across runtimes — sampling error is uneven:")
    for rt, row in _nruns_by_rt.iterrows():
        marker = "  " if row["min"] == row["max"] else " *"
        print(f"  {marker}{rt}: {row['min']}–{row['max']} runs/config")

runtimes         = sorted(agg["runtime"].unique())
ntasks_all       = sorted(agg["ntasks_key"].unique())
non_taskloop_rts = [rt for rt in runtimes if "taskloop" not in rt]
taskloop_rts     = [rt for rt in runtimes if "taskloop" in rt]

def niters_label(ni):
    if ni >= 1_000_000:
        return f"{ni/1e6:.1f}M"
    if ni >= 1_000:
        return f"{ni/1e3:.1f}k"
    return str(ni)

# Work baseline from work_sweep.csv
work_sweep_path = consolidated_dir / "work_sweep.csv"
if not work_sweep_path.exists():
    print(f"WARNING: {work_sweep_path} not found", file=sys.stderr)
    _has_baseline = False
    agg["work_baseline"] = np.nan
else:
    _has_baseline = True
    work_sweep = pd.read_csv(work_sweep_path)
    baseline = work_sweep[["niters", "p50_us"]].rename(columns={"p50_us": "work_baseline"})
    agg = agg.merge(baseline, on="niters", how="left")
    agg["ideal_wall"] = agg["work_baseline"] * agg["ntasks_key"] / (NCORES * 1e6)

# ---------------------------------------------------------------------------
# Export summary statistics CSV
# ---------------------------------------------------------------------------

stats_full = agg.rename(columns={
    "mean_p50": "task_mean_p50_us", "sem_p50": "task_sem_p50_us", "std_p50": "task_std_p50_us",
    "p25_wall": "wall_p25_s", "median_wall": "wall_p50_s", "p75_wall": "wall_p75_s",
}).copy()
stats_full["task_cv"] = stats_full["task_std_p50_us"] / stats_full["task_mean_p50_us"]
stats_full["wall_cv"] = stats_full["wall_std_s"]  / stats_full["wall_mean_s"]

if _has_baseline:
    stats_full["overhead_abs_us"]         = (stats_full["wall_p50_s"] - stats_full["ideal_wall"]) * 1e6
    stats_full["overhead_wall_pct"]       = (stats_full["wall_p50_s"] / stats_full["ideal_wall"] - 1) * 100
    stats_full["slowdown_factor"]         = stats_full["wall_p50_s"] / stats_full["ideal_wall"]
    stats_full["overhead_share_wall_pct"] = (
        (stats_full["wall_p50_s"] - stats_full["ideal_wall"]) / stats_full["wall_p50_s"] * 100
    )
    # Hardware-/granularity-independent overhead per task: how many µs the
    # runtime adds on top of the pure arithmetic work for each task it
    # processes.  Derived from wall-time difference vs. the work-only lower
    # bound, divided by tasks-per-core.
    stats_full["overhead_per_task_us"] = (
        (stats_full["wall_p50_s"] - stats_full["ideal_wall"]) * NCORES
        / stats_full["ntasks_key"] * 1e6
    )

stats_csv_path = consolidated_dir / "summary_stats.csv"
stats_full.sort_values(["ntasks_key", "niters", "runtime"]).rename(columns={
    "ntasks_key":      "ntasks",
    "task_cv":         "task_cv_ratio",
    "wall_cv":         "wall_cv_ratio",
    "work_baseline":   "work_baseline_us",
    "ideal_wall":      "ideal_wall_s",
    "slowdown_factor": "slowdown_factor_x",
}).to_csv(stats_csv_path, index=False, float_format="%.4f")
print(f"Saved {stats_csv_path}")

# ---------------------------------------------------------------------------
# Cross-runtime task-time consistency stats + LaTeX table
# ---------------------------------------------------------------------------

# Cross-runtime grand mean is weighted by per-runtime nruns so that
# configurations with fewer runs (e.g. GCC libgomp at 10) are not
# overrepresented relative to fully-sampled ones at 30.  The min/max remain
# unweighted, since they describe the realised range across runtimes.
def _per_niters_stats(grp):
    vals    = grp["mean_p50"].values
    weights = grp["nruns"].values
    return pd.Series({
        "grand_mean": float(np.average(vals, weights=weights)),
        "rt_min":     float(vals.min()),
        "rt_max":     float(vals.max()),
        "rt_std":     float(vals.std(ddof=1)) if len(vals) > 1 else 0.0,
        "n_rts":      int(len(vals)),
    })

rt_stats = (
    agg.groupby(["ntasks_key", "niters"])
    .apply(_per_niters_stats, include_groups=False)
    .reset_index()
)
rt_stats["rt_cv_pct"] = rt_stats["rt_std"] / rt_stats["grand_mean"] * 100
max_cv_pct = rt_stats["rt_cv_pct"].max()

def _us_label(v):
    if v >= 1_000:
        k = v / 1_000
        return f"{k:.0f}k" if k == int(k) else f"{k:.1f}k"
    return f"{v:.0f}" if v == int(v) else f"{v:.1f}"

# Always emit one table per ntasks (no silent "collapse" heuristic) — the
# caption reports the actual maximum cross-runtime CV so the reader can judge
# the calibration claim from the data instead of an arbitrary threshold.
ntasks_groups = [
    (nt_k, rt_stats[rt_stats["ntasks_key"] == nt_k])
    for nt_k in sorted(rt_stats["ntasks_key"].unique())
]
n_rts_total = int(rt_stats["n_rts"].max())

lines = [
    r"\begin{table}[htbp]",
    r"  \centering",
    r"  \caption{Per-task execution time as a function of workload size (\texttt{niters}).",
    r"    For each \texttt{niters} value the table reports the cross-runtime mean of per-runtime",
    r"    mean-of-medians task time, together with the minimum, maximum, and coefficient of",
    rf"    variation (CV) across all {n_rts_total} runtime configurations. The maximum CV across",
    rf"    the entire sweep is {max_cv_pct:.2f}\,\%, confirming that task execution time is",
    r"    determined solely by the workload and is independent of the runtime overhead mechanism",
    r"    under study.}",
    r"  \label{tab:task_time}",
]
col_spec = r"rrrrc"
header   = (r"    \texttt{niters} & Mean ($\mu$s) & Min ($\mu$s) & "
            r"Max ($\mu$s) & CV (\%) \\")

for ntasks_label, sub in ntasks_groups:
    lines.append(rf"  % ntasks = {ntasks_label}")
    lines += [
        r"  \begin{tabular}{" + col_spec + r"}",
        r"    \toprule",
        header,
        r"    \midrule",
    ]
    for _, row in sub.sort_values("niters").iterrows():
        ni  = niters_label(int(row["niters"]))
        med = _us_label(row["grand_mean"])
        mn  = _us_label(row["rt_min"])
        mx  = _us_label(row["rt_max"])
        cv  = f"{row['rt_cv_pct']:.2f}"
        lines.append(f"    {ni} & {med} & {mn} & {mx} & {cv} \\\\")
    lines += [r"    \bottomrule", r"  \end{tabular}"]

lines.append(r"\end{table}")
tex = "\n".join(lines)
tex_path = consolidated_dir / "task_time_table.tex"
tex_path.write_text(tex)
print(f"Saved {tex_path}")

# ---------------------------------------------------------------------------
# Colour / marker / label dictionaries
# ---------------------------------------------------------------------------

COLOR_ORIG         = "#CCBB44"  # yellow — omp  (LLVM Libomp)
COLOR_OMP_GCC      = "#AA3377"  # purple — omp_gcc (GCC libgomp)
COLOR_INIT         = "#4477AA"  # blue   — oss  (nOS-V Orig)
COLOR_TG_GRP       = "#EE6677"  # red    — oss_tggrp / oss_tggrp_cs
COLOR_TG_GRP_CACHE = "#EE6677"  # orange — oss_tggrp + caching
COLOR_TG_3L        = "#CCBB44"  # yellow — oss_tg
COLOR_TG_3L_CACHE  = "#AA3377"  # purple — oss_tg + caching
COLOR_GREY         = "#BBBBBB"  # grey

colors = {
    "omp":                               COLOR_ORIG,
    "omp_taskloop":                      COLOR_ORIG,
    "omp_gcc":                           COLOR_OMP_GCC,
    "omp_gcc_taskloop":                  COLOR_OMP_GCC,
    "oss":                               COLOR_INIT,
    "oss_taskloop":                      COLOR_INIT,
    "oss_tggrp":                         COLOR_TG_GRP,
    "oss_tggrp_taskloop":                COLOR_TG_GRP,
    "oss_tggrp_rand1":                   COLOR_TG_GRP,
    "oss_tggrp_taskloop_rand1":          COLOR_TG_GRP,
    "oss_tggrp_caching":                 COLOR_TG_GRP_CACHE,
    "oss_tggrp_taskloop_caching":        COLOR_TG_GRP_CACHE,
    "oss_tggrp_caching_rand1":           COLOR_TG_GRP_CACHE,
    "oss_tggrp_taskloop_caching_rand1":  COLOR_TG_GRP_CACHE,
    "oss_tggrp_cachingfalse":            COLOR_TG_GRP,
    "oss_tggrp_taskloop_cachingfalse":   COLOR_TG_GRP,
    "oss_tggrp_cachingfalse_rand1":      COLOR_GREY,
    "oss_tggrp_taskloop_cachingfalse_rand1": COLOR_GREY,
    "oss_tggrp_cs":                      COLOR_TG_GRP,
    "oss_tggrp_cs_caching":              COLOR_TG_GRP_CACHE,
    "oss_tggrp_taskloop_cs_caching":     COLOR_TG_GRP_CACHE,
    "oss_tg":                            COLOR_TG_3L,
    "oss_tg_taskloop":                   COLOR_TG_3L,
    "oss_tg_rand1":                      COLOR_TG_3L,
    "oss_tg_taskloop_rand1":             COLOR_TG_3L,
    "oss_tg_caching":                    COLOR_TG_3L_CACHE,
    "oss_tg_taskloop_caching":           COLOR_TG_3L_CACHE,
    "oss_tg_caching_rand1":              COLOR_TG_3L_CACHE,
    "oss_tg_taskloop_caching_rand1":     COLOR_TG_3L_CACHE,
    "oss_tg_cachingfalse":               COLOR_TG_3L,
    "oss_tg_taskloop_cachingfalse":      COLOR_TG_3L,
    "oss_tg_cachingfalse_rand1":         COLOR_GREY,
    "oss_tg_taskloop_cachingfalse_rand1": COLOR_GREY,
}

markers = {
    "omp":                               "o",
    "omp_taskloop":                      "o",
    "omp_gcc":                           "o",
    "omp_gcc_taskloop":                  "o",
    "oss":                               "s",
    "oss_taskloop":                      "s",
    "oss_tggrp":                         "^",
    "oss_tggrp_taskloop":                "^",
    "oss_tggrp_rand1":                   "^",
    "oss_tggrp_taskloop_rand1":          "^",
    "oss_tggrp_caching":                 "D",
    "oss_tggrp_taskloop_caching":        "D",
    "oss_tggrp_caching_rand1":           "D",
    "oss_tggrp_taskloop_caching_rand1":  "D",
    "oss_tggrp_cachingfalse":            "X",
    "oss_tggrp_taskloop_cachingfalse":   "X",
    "oss_tggrp_cachingfalse_rand1":      "h",
    "oss_tggrp_taskloop_cachingfalse_rand1": "h",
    "oss_tggrp_cs":                      "^",
    "oss_tggrp_cs_caching":              "D",
    "oss_tggrp_taskloop_cs_caching":     "D",
    "oss_tg":                            "v",
    "oss_tg_taskloop":                   "v",
    "oss_tg_rand1":                      "v",
    "oss_tg_taskloop_rand1":             "v",
    "oss_tg_caching":                    "P",
    "oss_tg_taskloop_caching":           "P",
    "oss_tg_caching_rand1":              "P",
    "oss_tg_taskloop_caching_rand1":     "P",
    "oss_tg_cachingfalse":               "p",
    "oss_tg_taskloop_cachingfalse":      "p",
    "oss_tg_cachingfalse_rand1":         "H",
    "oss_tg_taskloop_cachingfalse_rand1": "H",
}

RT_LABEL = {
    "omp":                             "LLVM Libomp",
    "omp_taskloop":                    "LLVM Libomp",
    "omp_gcc":                         "GCC libgomp",
    "omp_gcc_taskloop":                "GCC libgomp",
    "oss":                             "nOS-V Orig",
    "oss_taskloop":                    "nOS-V Orig",
    "oss_tggrp":                       "OSS+TG-GRP",
    "oss_tggrp_taskloop":              "OSS+TG-GRP taskloop",
    "oss_tggrp_caching":               "OSS+TG-GRP caching",
    "oss_tggrp_taskloop_caching":      "OSS+TG-GRP caching taskloop",
    "oss_tggrp_cachingfalse":          "OSS+TG-GRP no-cache",
    "oss_tggrp_taskloop_cachingfalse": "OSS+TG-GRP no-cache taskloop",
    "oss_tggrp_cs":                    "HSF",
    "oss_tggrp_cs_caching":            "HSF",
    "oss_tggrp_taskloop_cs_caching":   "HSF",
    "oss_tg":                          "OSS+TG",
    "oss_tg_taskloop":                 "OSS+TG taskloop",
    "oss_tg_caching":                  "OSS+TG caching",
    "oss_tg_taskloop_caching":         "OSS+TG caching taskloop",
    "oss_tg_cachingfalse":             "OSS+TG no-cache",
    "oss_tg_taskloop_cachingfalse":    "OSS+TG no-cache taskloop",
}

# ---------------------------------------------------------------------------
# Select ntasks for the figure
# ---------------------------------------------------------------------------

plot_nt = ntasks_filter if ntasks_filter is not None else (
    307200 if 307200 in ntasks_all else ntasks_all[-1]
)

cluster_mean_p50 = (
    agg.groupby(["ntasks_key", "niters"])["mean_p50"]
    .mean()
    .rename("cluster_mean_p50")
    .reset_index()
)

sub_nt = agg[agg["ntasks_key"] == plot_nt]
cm_nt  = cluster_mean_p50[cluster_mean_p50["ntasks_key"] == plot_nt].set_index("niters")

def _rt_xy_wall(rt):
    """Return (task_time_us, median_wall_s, p25_wall_s, p75_wall_s) arrays
    for one runtime at plot_nt.  IQR bands quantify run-to-run variability."""
    sub = sub_nt[sub_nt["runtime"] == rt].sort_values("niters")
    xs, ys, lo, hi = [], [], [], []
    for _, row in sub.iterrows():
        ni = row["niters"]
        if ni in cm_nt.index:
            xs.append(cm_nt.loc[ni, "cluster_mean_p50"])
            ys.append(row["median_wall"])
            lo.append(row["p25_wall"])
            hi.append(row["p75_wall"])
    return np.array(xs), np.array(ys), np.array(lo), np.array(hi)

# ---------------------------------------------------------------------------
# HSF task duration data
# ---------------------------------------------------------------------------

_hsf_csv = Path(__file__).parent / "HSF_task_durations.csv"
if not _hsf_csv.exists():
    _hsf_csv = consolidated_dir.parent / "HSF_task_durations.csv"

_has_hsf    = False
_hsf_dom    = None
_bench_color = {}

if _hsf_csv.exists():
    _hsf_raw = pd.read_csv(_hsf_csv)
    _hsf = _hsf_raw[
        (_hsf_raw["n"] > 1) &
        (_hsf_raw["p50"] > 1_000) &    # > 1 µs
        (_hsf_raw["p50"] < 1e11)        # < 100 s (excludes whole-program entries)
    ].copy()
    _hsf["p50_us"]   = _hsf["p50"] / 1e3
    _hsf["p25_us"]   = _hsf["p25"] / 1e3
    _hsf["p75_us"]   = _hsf["p75"] / 1e3
    _hsf["total_ns"] = _hsf["n"] * _hsf["p50"]

    _hsf_dom = (
        _hsf.loc[_hsf.groupby("app")["total_ns"].idxmax()]
        .copy().reset_index(drop=True)
    )

    def _app_label(app):
        parts = app.split("_")
        return f"{parts[0]} {parts[1]}" if len(parts) > 1 else parts[0]

    _hsf_dom["label"] = _hsf_dom["app"].apply(_app_label)
    _bench_names = sorted(_hsf_dom["label"].unique())
    _bench_cmap  = plt.cm.get_cmap("tab10", len(_bench_names))
    _bench_color = {b: _bench_cmap(i) for i, b in enumerate(_bench_names)}
    _has_hsf = True

    print("\nHSF dominant task types (most total runtime):")
    for _, r in _hsf_dom.iterrows():
        print(f"  {r['label']:20s}  {r['tasktype']:30s}  p50={_us_label(r['p50_us'])}µs  "
              f"[{_us_label(r['p25_us'])}, {_us_label(r['p75_us'])}]µs")
else:
    print(f"WARNING: HSF_task_durations.csv not found — benchmark strip omitted",
          file=sys.stderr)

# ---------------------------------------------------------------------------
# Figure: task-time calibration  →  p50_task_time.pdf
# Top:    cross-runtime median task time vs niters (calibration curve)
# Bottom: per-runtime % deviation from that median
# ---------------------------------------------------------------------------

rt_nt       = rt_stats[rt_stats["ntasks_key"] == plot_nt].sort_values("niters")
niters_vals = sorted(sub_nt["niters"].unique())
tick_pos    = np.arange(len(niters_vals))
tick_lbl    = [niters_label(ni) for ni in niters_vals]
niters_idx  = {ni: i for i, ni in enumerate(niters_vals)}

fig0, (ax_top, ax_bot) = plt.subplots(
    2, 1, figsize=(6, 5),
    gridspec_kw={"height_ratios": [1.2, 1]},
)
fig0.suptitle("Task Duration", fontsize=16, fontweight="bold")

# — Top: calibration curve —
xi = [niters_idx[ni] for ni in rt_nt["niters"].values]
ax_top.fill_between(xi, rt_nt["rt_min"].values, rt_nt["rt_max"].values,
                    alpha=0.25, color="steelblue", label="min–max range")
ax_top.plot(xi, rt_nt["grand_mean"].values, color="steelblue",
            linewidth=1.5, marker="o", markersize=5, label="cross-runtime mean")
for x_pos, y_val in zip(xi, rt_nt["grand_mean"].values):
    ax_top.annotate(_us_label(y_val), xy=(x_pos, y_val), xytext=(0, 5),
                    textcoords="offset points", ha="center", va="bottom", fontsize=9.5)
ax_top.set_yscale("log")
ax_top.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _: _us_label(v)))
ax_top.set_ylabel(f"ntasks = {plot_nt:,}\nTask time (µs)")
ax_top.set_xticks(tick_pos)
ax_top.set_xticklabels([])
ax_top.legend(loc="upper left", fontsize=9, frameon=True)
ax_top.grid(True, which="both", linestyle=":", alpha=0.5)

# — Bottom: per-runtime % deviation from cross-runtime mean.  Solid line is
# the per-runtime mean-of-medians; shaded band is the min--max range of the
# per-run medians (cross-run variability).  Both are expressed as signed
# percentages of the cross-runtime weighted mean shown as the dashed zero line.
ax_bot.axhline(0, color="black", linewidth=0.8, linestyle="--")
for rt in runtimes:
    sub = sub_nt[sub_nt["runtime"] == rt].sort_values("niters")
    if sub.empty:
        continue
    merged  = sub.merge(rt_nt[["niters", "grand_mean"]], on="niters", how="left")
    xi_dev  = [niters_idx[ni] for ni in merged["niters"].values if ni in niters_idx]
    gm      = merged["grand_mean"].values
    dev     = (merged["mean_p50"].values             - gm) / gm * 100
    dev_lo  = (merged["min_p50_across_runs"].values  - gm) / gm * 100
    dev_hi  = (merged["max_p50_across_runs"].values  - gm) / gm * 100
    ls = ":" if "taskloop" in rt else "-"
    if _args.calibration_bands:
        ax_bot.fill_between(xi_dev, dev_lo, dev_hi,
                            color=colors.get(rt), alpha=0.15, linewidth=0)
    ax_bot.plot(xi_dev, dev, color=colors.get(rt), linestyle=ls,
                marker=markers.get(rt, "o"), markersize=3.5, linewidth=1.2,
                label=RT_LABEL.get(rt, rt))

ax_bot.set_ylim(bottom=-5)
ax_bot.set_ylabel("Deviation (%)")
ax_bot.set_xticks(tick_pos)
ax_bot.set_xticklabels(tick_lbl, rotation=45, ha="right")
ax_bot.set_xlabel("niters (work per task)")
ax_bot.grid(True, which="both", linestyle=":", alpha=0.5)

# Legend: deduplicate by label; note taskloop = dashed line
_handles0, _labels0, _seen0 = [], [], set()
for h, l in zip(*ax_bot.get_legend_handles_labels()):
    if l not in _seen0:
        _handles0.append(h)
        _labels0.append(l)
        _seen0.add(l)
_handles0.append(mlines.Line2D([], [], color="gray", linestyle=":",
                                linewidth=1.2, label="taskloop variant"))
_labels0.append("taskloop variant")
fig0.tight_layout()
fig0.subplots_adjust(bottom=0.30)
fig0.legend(_handles0, _labels0, loc="lower center", bbox_to_anchor=(0.5, 0.01),
            ncol=min(len(_handles0), 4), frameon=True, fontsize=9)

out0 = consolidated_dir / "p50_task_time.pdf"

# ---------------------------------------------------------------------------
# Figure: "Runtime Overhead"
# 3 rows sharing a continuous log x-axis (median task time µs):
#   0 — Single Creator   (non-taskloop wall time)
#   1 — Parallel Creators (taskloop wall time)
#   2 — Benchmark task granularity strip
# ---------------------------------------------------------------------------

fig, (ax_single, ax_parallel, ax_bench) = plt.subplots(
    3, 1,
    figsize=(7, 7.5),
    sharex=True,
    gridspec_kw={"height_ratios": [2.5, 2.5, 2.2]},
)
fig.suptitle("Runtime Overhead", fontsize=16, fontweight="bold")
fig.subplots_adjust(left=0.13, right=0.97, top=0.94, bottom=0.10, hspace=0.13)

def _plot_wall_panel(ax, group_rts, subtitle, show_legend=True):
    seen_labels = set()
    for rt in group_rts:
        xs, ys, lo, hi = _rt_xy_wall(rt)
        if len(xs) == 0:
            continue
        lbl = RT_LABEL.get(rt, rt)
        ax.errorbar(xs, ys, yerr=[ys - lo, hi - ys],
                    color=colors.get(rt),
                    marker=markers.get(rt, "o"),
                    markersize=3.5, linewidth=1.2,
                    elinewidth=0.8, capsize=2,
                    label=lbl if lbl not in seen_labels else "_nolegend_")
        seen_labels.add(lbl)
    if _has_hsf:
        for _, hrow in _hsf_dom.iterrows():
            ax.axvline(hrow["p50_us"],
                       color=_bench_color[hrow["label"]],
                       linestyle="--", linewidth=0.9, alpha=0.75)
    ax.set_yscale("log")
    ax.set_ylabel("Wall time (s)")
    ax.set_title(subtitle, loc="left", fontsize=12, fontweight="bold", pad=3)
    if show_legend:
        ax.legend(loc="upper left", fontsize=9, frameon=True, ncol=2)
    ax.grid(True, which="both", linestyle=":", alpha=0.4)

_plot_wall_panel(ax_single,   non_taskloop_rts, "Single Creator", show_legend=False)
_plot_wall_panel(ax_parallel, taskloop_rts,     "Parallel Creators", show_legend=True)

# --- x-axis range: cover measured task times + benchmark durations ---
all_task_us = sorted(
    cm_nt.loc[ni, "cluster_mean_p50"]
    for ni in sub_nt["niters"].unique() if ni in cm_nt.index
)
x_lo = min(all_task_us[0],
           _hsf_dom["p25_us"].min() if _has_hsf else all_task_us[0]) * 0.6
x_hi = 15_000  # µs — a bit past the 10k mark

# --- Benchmark granularity strip ---
if _has_hsf:
    for i, (_, hrow) in enumerate(_hsf_dom.iterrows()):
        bc = _bench_color[hrow["label"]]
        ax_bench.barh(i, hrow["p75_us"] - hrow["p25_us"],
                      left=hrow["p25_us"], height=0.55,
                      color=bc, alpha=0.55, linewidth=0)
        ax_bench.plot(hrow["p50_us"], i,
                      marker="|", markersize=10, color=bc, linewidth=2)
        if hrow["label"].startswith(("hpccg", "cholesky")):
            ax_bench.text(hrow["p25_us"] * 0.96, i,
                          f"{hrow['label']} ({hrow['tasktype'][:18]})",
                          va="center", ha="right", fontsize=8.5, color=bc)
        else:
            ax_bench.text(hrow["p75_us"] * 1.04, i,
                          f"{hrow['label']} ({hrow['tasktype'][:18]})",
                          va="center", ha="left", fontsize=8.5, color=bc)
    ax_bench.set_ylim(-0.6, len(_hsf_dom) - 0.4)

ax_bench.set_xscale("log")
ax_bench.set_xlim(x_lo, x_hi)
ax_bench.set_yticks([])
ax_bench.set_xlabel("Median task time (µs)")
ax_bench.set_title("Benchmarks dominant task granularity (bar=IQR,  | = p50)",
                    loc="left", fontsize=12, fontweight="bold", pad=3)

# Force xticks at the actual measured task durations (one per niters value)
# and disable the auto-generated log-scale minor/major ticks. The same set of
# vertical guides (very fine) is overlaid on every subplot via axvline so the
# reader can trace each niters point through all three panels.
_tick_positions = list(all_task_us)
_tick_labels    = [_us_label(v) for v in _tick_positions]

for _ax in (ax_single, ax_parallel, ax_bench):
    _ax.grid(False)
    _ax.set_xticks(_tick_positions, minor=False)
    _ax.set_xticks([], minor=True)
    for _x in _tick_positions:
        _ax.axvline(_x, color="black", linewidth=0.25, alpha=0.10,
                    linestyle=":", zorder=0)
ax_bench.set_xticklabels(_tick_labels, rotation=45, ha="right")
ax_bench.xaxis.set_major_formatter(
    mticker.FuncFormatter(lambda v, _: _us_label(v))
)

# ---------------------------------------------------------------------------
# Save
# ---------------------------------------------------------------------------

out = consolidated_dir / "overhead_comparison.pdf"
for _fig, _out in [(fig0, out0), (fig, out)]:
    _fig.savefig(_out)
    print(f"Saved {_out}")
