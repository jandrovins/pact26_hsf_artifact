#!/usr/bin/env python3
"""
LaTeX-styled benchmark figures for the paper.

Generates six PDFs (and matching PNGs) into ``fig/``:

  * ``benchmark_comparison``        — main 2x5 normalized bar chart
    * ``benchmark_comparison_external_baseline`` — main layout, external 1.0x
  * ``benchmark_speedup_geomean``   — per-benchmark geomean speedup bars
  * ``benchmark_speedup_heatmap``   — speedup heatmap, all configs at once
    * ``benchmark_scaling_curves``    — perf vs. task size, Orig vs. TG
    * ``benchmark_runtime_comparison``— HSF best vs runtime competitors

Data-loading helpers are copied from ``plot_all_benchmarks.py`` so this
script does not depend on it (and the original PNG script keeps working
unchanged as a known-good reference).

Usage:
    python plot_benchmarks_latex.py                 # all figures, with LaTeX
    python plot_benchmarks_latex.py --no-usetex     # skip LaTeX (fast preview)
    python plot_benchmarks_latex.py --figure main   # one figure only
    python plot_benchmarks_latex.py --out fig       # output dir (default: fig)
"""

import argparse
from pathlib import Path

import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch
from matplotlib.lines import Line2D

# ---------------------------------------------------------------------------
# Benchmark configuration (mirrors plot_all_benchmarks.py)
# ---------------------------------------------------------------------------

BASE = Path(__file__).resolve().parent

BENCHMARKS = [
    {
        "name": "Cholesky",
        "orig_csv": BASE / "fox_cholesky_orig/results_final_3bs/summary.csv",
        "tg_csv":   BASE / "fox_cholesky_tg/results_final_3bs/summary.csv",
        "init_csv": BASE / "fox_cholesky_orig/results_final_3bs_init_tasks/summary.csv",
        "metric_col": "mean_gflops",
        "std_col": "std_gflops",
        "size_col": "N",
        "tasksize_col": "TS",
        "small_size": 6144,
        "large_size": 33792,
        "unit": "GFLOPS",
    },
    {
        "name": "Heat",
        "orig_csv": BASE / "fox_heat_orig/results_final_3bs/summary.csv",
        "tg_csv":   BASE / "fox_heat_tg/results_final_3bs/summary.csv",
        "init_csv": BASE / "fox_heat_orig/results_final_3bs_init_tasks/summary.csv",
        "metric_col": "mean_throughput",
        "std_col": None,
        "time_col": "mean_delta_time",
        "std_time_col": "std_delta_time",
        "size_col": "n",
        "tasksize_col": "bs",
        "small_size": 12288,
        "large_size": 49152,
        "unit": "GCells/s",
    },
    {
        "name": "HPCCG",
        "orig_csv": BASE / "fox_hpccg_orig/results_final_3bs/summary.csv",
        "tg_csv":   BASE / "fox_hpccg_tg/results_final_3bs/summary.csv",
        "init_csv": BASE / "fox_hpccg_orig/results_final_3bs_init_tasks/summary.csv",
        "metric_col": "mean_mflops",
        "std_col": "std_mflops",
        "size_col": ["nx", "ny", "nz"],
        "tasksize_col": "ntasks",
        "small_size": (288, 192, 768),
        "large_size": (384, 384, 1536),
        "unit": "MFLOPS",
        "xlabel": "SPMV Tasks per Core",
        # More tasks = smaller block.  Sort descending so x-axis reads
        # Small block → Med block → Large block (left to right).
        "ts_sort_reverse": True,
        "ts_label_map": {
            4608: "24", 1536: "16", 96: "1",   # small problem (192 cores)
            9216: "48", 3072: "16", 192: "1",  # large problem (192 cores)
        },
    },
    {
        "name": "Matmul",
        "orig_csv": BASE / "fox_matmul_orig/results_final_3bs/summary.csv",
        "tg_csv":   BASE / "fox_matmul_tg/results_final_3bs/summary.csv",
        "init_csv": BASE / "fox_matmul_orig/results_final_3bs_init_tasks/summary.csv",
        "metric_col": "mean_gflops",
        "std_col": "std_gflops",
        "size_col": "N",
        "size_col_orig": "nsize",
        "tasksize_col": "TS",
        "tasksize_col_orig": "ts",
        "small_size": 6144,
        "large_size": 49152,
        "unit": "GFLOPS",
    },
    {
        "name": "Multisaxpy",
        # The no-init baseline is too slow to compare; it is not shown as a
        # normal bar.  The parallel-init version serves as the practical
        # baseline (reference line at y=1.0).  A ceiling bar is drawn to
        # communicate that the true no-init baseline exists but is off-scale.
        "orig_csv": None,
        "tg_csv":   BASE / "fox_multisaxpy_tg/results_final_3bs/summary.csv",
        "init_csv": BASE / "fox_multisaxpy_orig/results_final_3bs_init_tasks/summary.csv",
        "noinit_too_slow": True,
        "metric_col": "mean_gflops",
        "std_col": None,
        "time_col": "mean_duration_s",
        "std_time_col": "std_duration_s",
        "size_col": "N",
        "tasksize_col": "TS",
        "small_size": 56524800,
        "large_size": 771740160,
        "unit": "GFLOPS",
        # 2 arrays (x, y) of doubles, TS elements per task -> 16 * TS bytes
        "xlabel": "Memory per Task",
        "ts_label_fn": lambda ts: _fmt_bytes(2 * 8 * int(ts)),
    },
]

COMPETITOR = [
    {
        "name": "Cholesky",
        "comp_csv": BASE / "fox_cholesky_libflame/summary.csv",
        "comp_label": "libFLAME+BLIS",
        "comp_metric_col": "mean_gflops",
        "comp_std_col": "std_gflops",
        "comp_size_col": "nsize",
        "comp_config_cols": ["numa"],
        "tg_idx": 0,
        "small_size": 6144,
        "large_size": 33792,
        "unit": "GFLOPS",
    },
    {
        "name": "Heat",
        "comp_csv": BASE / "fox_heat_omp/summary.csv",
        "comp_label": "OpenMP",
        "comp_metric_col": "mean_throughput",
        "comp_std_col": "std_throughput",
        "comp_size_col": "n",
        "comp_config_cols": ["numa", "procbind"],
        "tg_idx": 1,
        "small_size": 12288,
        "large_size": 49152,
        "unit": "GCells/s",
    },
    {
        "name": "HPCCG",
        "comp_csv": BASE / "fox_hpccg_omp/summary.csv",
        "comp_label": "OpenMP",
        "comp_metric_col": "mean_mflops",
        "comp_std_col": "std_mflops",
        "comp_size_col": ["nx", "ny", "nz"],
        "comp_config_cols": ["ppn"],
        "comp_ref_filter": {"ppn": 1},
        "tg_idx": 2,
        "small_size": (288, 192, 768),
        "large_size": (384, 384, 1536),
        "unit": "MFLOPS",
    },
    {
        "name": "Matmul",
        "comp_csv": BASE / "fox_mt-dgemm_libomp/summary.csv",
        "comp_label": "OpenMP+BLIS",
        "comp_metric_col": "mean_gflops",
        "comp_std_col": "std_gflops",
        "comp_size_col": "nsize",
        "comp_config_cols": ["numa"],
        "tg_idx": 3,
        "small_size": 6144,
        "large_size": 49152,
        "unit": "GFLOPS",
    },
    {
        "name": "Multisaxpy",
        "comp_csv": BASE / "fox_multisaxpy_omp/summary.csv",
        "comp_label": "OpenMP",
        "comp_metric_col": "mean_gflops",
        "comp_std_col": "std_gflops",
        "comp_size_col": "nsize",
        "comp_config_cols": ["numa", "procbind"],
        "tg_idx": 4,
        "small_size": 56524800,
        "large_size": 771740160,
        "unit": "GFLOPS",
    },
]

# Paul Tol's bright qualitative palette — colorblind safe and print friendly.
COLOR_ORIG = "#4477AA"  # blue
COLOR_INIT = "#228833"  # green
COLOR_TG   = "#EE6677"  # red
HATCH_ORIG = ""
HATCH_INIT = "//"
HATCH_TG   = "xx"

LABEL_ORIG = r"OmpSs-2 unmodified"
LABEL_INIT = r"OmpSs-2 + Parallel initialization"
LABEL_TG   = r"HSF (ours)"

# ---------------------------------------------------------------------------
# Style
# ---------------------------------------------------------------------------

def configure_style(use_tex: bool):
    """Apply LLNCS-friendly matplotlib styling."""
    if use_tex:
        matplotlib.rcParams.update({
            "text.usetex": True,
            "font.family": "serif",
            "font.serif":  ["Latin Modern Roman", "Computer Modern Roman"],
            "text.latex.preamble": r"\usepackage{lmodern}",
        })
    else:
        matplotlib.rcParams.update({
            "text.usetex": False,
            "font.family": "serif",
            "mathtext.fontset": "cm",
        })
    matplotlib.rcParams.update({
        "font.size": 8,
        "axes.titlesize": 9,
        "axes.labelsize": 8,
        "xtick.labelsize": 7,
        "ytick.labelsize": 7,
        "legend.fontsize": 7,
        "axes.linewidth": 0.6,
        "xtick.major.width": 0.6,
        "ytick.major.width": 0.6,
        "lines.linewidth": 1.0,
        "patch.linewidth": 0.4,
        "savefig.dpi": 300,
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
    })


# ---------------------------------------------------------------------------
# Data helpers
# ---------------------------------------------------------------------------

def load_csv(path):
    if path is None:
        return None
    p = Path(path)
    if not p.exists():
        print(f"  WARNING: {p} not found")
        return None
    return pd.read_csv(p)


def normalize_columns(df, cfg, source_label):
    if df is None:
        return None
    df = df.copy()

    size_col = cfg.get(f"size_col_{source_label}", cfg["size_col"])
    if isinstance(size_col, list):
        for c in size_col:
            df[c] = df[c].astype(int)
        df["_size"] = list(zip(*(df[c] for c in size_col)))
    else:
        df["_size"] = df[size_col].astype(int)

    ts_col = cfg.get(f"tasksize_col_{source_label}", cfg["tasksize_col"])
    df["_ts"] = df[ts_col].astype(int)
    df["_metric"] = df[cfg["metric_col"]].astype(float)

    std_col = cfg.get("std_col")
    if std_col and std_col in df.columns:
        df["_std"] = df[std_col].astype(float)
    elif cfg.get("time_col") and cfg.get("std_time_col"):
        mean_time = df[cfg["time_col"]].astype(float)
        std_time  = df[cfg["std_time_col"]].astype(float)
        df["_std"] = df["_metric"] * (std_time / mean_time)
    else:
        df["_std"] = 0.0

    return df


def filter_size(df, size_val):
    if df is None:
        return None
    if isinstance(size_val, tuple):
        mask = df["_size"].apply(lambda x: x == size_val)
    else:
        mask = df["_size"] == size_val
    result = df[mask]
    return result if len(result) > 0 else None


def get_intersection_tasksizes(df_orig, df_tg):
    if df_orig is None or df_tg is None:
        return []
    return sorted(set(df_orig["_ts"].unique()) & set(df_tg["_ts"].unique()))


def get_values(df, ts_val):
    if df is None:
        return None, None
    row = df[df["_ts"] == ts_val]
    if len(row) == 0:
        return None, None
    row = row.iloc[0]
    return float(row["_metric"]), float(row["_std"])


def _fmt_bytes(nbytes):
    """Compact human-readable byte size."""
    for unit, denom in (("GiB", 2**30), ("MiB", 2**20), ("KiB", 2**10)):
        if nbytes >= denom:
            v = nbytes / denom
            return f"{v:.0f} {unit}" if v >= 10 else f"{v:.1f} {unit}"
    return f"{int(nbytes)} B"


def _ts_tick_label(cfg, ts):
    """Return the x-axis tick label for a task-size value."""
    fn = cfg.get("ts_label_fn")
    if fn is not None:
        return fn(ts)
    return cfg.get("ts_label_map", {}).get(ts, f"{ts:,}")


def _fmt_baseline(val, unit, label="Baseline", sep=" = "):
    """Absolute reference performance annotation shown on the y=1.0 line."""
    perf = _fmt_abs_with_unit(val, unit)
    if label:
        return f"{label}{sep}{perf}"
    return perf


def _fmt_abs(val, unit):
    """Compact formatter for absolute values inside bars (3 decimal places)."""
    if unit == "GCells/s":
        return f"{val/1e9:.3f}"
    elif unit == "GFLOPS":
        if val >= 1000:
            return f"{val/1000:.3f}T"
        return f"{val:.3f}"
    elif unit == "MFLOPS":
        if val >= 1000:
            return f"{val/1000:.3f}G"
        return f"{val:.3f}"
    return f"{val:.3g}"


def _fmt_abs_with_unit(val, unit):
    """Absolute value formatter with explicit units for text annotations."""
    if unit == "GFLOPS":
        if val >= 1000:
            return f"{val/1000:.3f} TFLOPS"
        return f"{val:.3f} GFLOPS"
    if unit == "GCells/s":
        return f"{val/1e9:.3f} GCells/s"
    if unit == "MFLOPS":
        if val >= 1000:
            return f"{val/1000:.3f} GFLOPS"
        return f"{val:.3f} MFLOPS"
    return f"{val:.3f} {unit}"


def _comp_config_label(row, cols):
    """Short label for a competitor config (for example, N0P1, ×4)."""
    parts = []
    for c in cols:
        val = int(row[c])
        if c == "ppn":
            parts.append(f"×{val}")
        elif c == "numa":
            parts.append(f"N{val}")
        elif c == "procbind":
            parts.append(f"P{val}")
        else:
            parts.append(f"{c}={val}")
    return "".join(parts)


def load_comp_configs(comp_cfg, size_val):
    """Return list of competitor rows: label, metric, std, and config map."""
    df = load_csv(comp_cfg["comp_csv"])
    if df is None:
        return []

    size_col = comp_cfg["comp_size_col"]
    if isinstance(size_col, list):
        for c in size_col:
            df[c] = df[c].astype(int)
        mask = pd.Series(
            [tuple(row) == size_val for row in zip(*(df[c] for c in size_col))],
            index=df.index,
        )
    else:
        mask = df[size_col].astype(int) == size_val

    df_s = df[mask]
    if "numa" in df_s.columns:
        df_s = df_s[df_s["numa"].astype(int) == 1]
    if "procbind" in df_s.columns:
        df_s = df_s[df_s["procbind"].astype(int) == 1]
    if df_s.empty:
        return []

    cfg_cols = comp_cfg.get("comp_config_cols", [])
    metric_col = comp_cfg["comp_metric_col"]
    std_col = comp_cfg.get("comp_std_col")

    result = []
    for _, row in df_s.iterrows():
        lbl = _comp_config_label(row, cfg_cols) if cfg_cols else "OMP"
        metric = float(row[metric_col])
        std = float(row[std_col]) if std_col and std_col in row.index else 0.0
        cfg = {c: int(row[c]) for c in cfg_cols if c in row.index}
        result.append({"label": lbl, "metric": metric, "std": std, "cfg": cfg})
    return result


def get_comp_ref_value(comp_cfg, comp_rows):
    """Reference metric for normalization (default: best competitor)."""
    if not comp_rows:
        return None

    ref_filter = comp_cfg.get("comp_ref_filter")
    if ref_filter:
        for row in comp_rows:
            cfg = row.get("cfg", {})
            if all(cfg.get(k) == v for k, v in ref_filter.items()):
                return float(row["metric"])

    return max(row["metric"] for row in comp_rows)


def get_ref_label(comp_cfg):
    """Short label for the normalization reference shown on each cluster."""
    ref_filter = comp_cfg.get("comp_ref_filter")
    if not ref_filter:
        return "$1.0$"
    parts = []
    if "ppn" in ref_filter:
        parts.append(f"×{int(ref_filter['ppn'])}")
    if "numa" in ref_filter:
        parts.append(f"N{int(ref_filter['numa'])}")
    if "procbind" in ref_filter:
        parts.append(f"P{int(ref_filter['procbind'])}")
    suffix = "".join(parts)
    return f"$1.0$ ({suffix})" if suffix else "$1.0$"


def get_hsf_best(tg_s):
    """Return (best_metric, std_at_best) across all task sizes."""
    if tg_s is None or len(tg_s) == 0:
        return None, None
    best_idx = tg_s["_metric"].idxmax()
    best = tg_s.loc[best_idx]
    return float(best["_metric"]), float(best["_std"])


def size_label(size_val):
    """Return a math-mode-safe label (no surrounding $...$)."""
    if isinstance(size_val, tuple):
        return r"{\times}".join(str(s) for s in size_val)
    return f"{size_val:,}"


def _draw_nodata(ax, title=None):
    ax.text(0.5, 0.5, "n/a", transform=ax.transAxes,
            ha="center", va="center", fontsize=7, color="gray")
    if title:
        ax.set_title(title, fontsize=8)
    ax.set_xticks([])
    ax.set_yticks([])
    for spine in ax.spines.values():
        spine.set_visible(False)


def load_all(cfg):
    """Load orig/tg/init dataframes for one benchmark."""
    return (
        normalize_columns(load_csv(cfg["orig_csv"]), cfg, "orig"),
        normalize_columns(load_csv(cfg["tg_csv"]),   cfg, "tg"),
        normalize_columns(load_csv(cfg["init_csv"]), cfg, "orig"),
    )


def save_fig(fig, out_dir: Path, stem: str):
    out_dir.mkdir(parents=True, exist_ok=True)
    pdf_path = out_dir / f"{stem}.pdf"
    png_path = out_dir / f"{stem}.png"
    fig.savefig(pdf_path, bbox_inches="tight")
    fig.savefig(png_path, bbox_inches="tight", dpi=200)
    plt.close(fig)
    print(f"  -> {pdf_path}")
    print(f"  -> {png_path}")


# ---------------------------------------------------------------------------
# Figure 0 — main 2x5 bar comparison
# ---------------------------------------------------------------------------

def make_main_figure(out_dir: Path):
    print("\n[main] benchmark_comparison")
    fig, axes = plt.subplots(2, 5, figsize=(9.0, 4.0), constrained_layout=True)

    for col_idx, cfg in enumerate(BENCHMARKS):
        df_orig, df_tg, df_init = load_all(cfg)

        for row_idx, (size_val, row_label) in enumerate([
            (cfg["large_size"], "Large"),
            (cfg["small_size"], "Small"),
        ]):
            ax = axes[row_idx, col_idx]

            orig_s = filter_size(df_orig, size_val)
            tg_s   = filter_size(df_tg,   size_val)
            init_s = filter_size(df_init, size_val)

            # For benchmarks where the no-init baseline is too slow, the
            # parallel-init version is the practical reference (y=1.0 line).
            noinit = cfg.get("noinit_too_slow", False)
            ref_s = init_s if noinit else orig_s

            tasksizes = get_intersection_tasksizes(ref_s, tg_s)
            if cfg.get("ts_sort_reverse"):
                tasksizes = list(reversed(tasksizes))

            if not tasksizes:
                ax.text(0.5, 0.5, "no data", transform=ax.transAxes,
                        ha="center", va="center", fontsize=7, color="gray")
                if row_idx == 0:
                    ax.set_title(cfg["name"])
                ax.set_xticks([])
                ax.set_yticks([])
                for spine in ax.spines.values():
                    spine.set_visible(False)
                continue

            n_ts = len(tasksizes)
            x = np.arange(n_ts)

            ref_abs_vals = [get_values(ref_s, ts)[0] for ts in tasksizes]
            valid_ref = [v for v in ref_abs_vals if v is not None and v > 0]
            ref_val = max(valid_ref) if valid_ref else 1.0

            # Bar layout:
            #   normal benchmarks  : orig  [init]  tg
            #   noinit_too_slow    : init  tg   (init is the practical baseline;
            #                                    no-init baseline not shown)
            if noinit:
                bar_keys = ["init", "tg"]
            elif init_s is not None:
                bar_keys = ["orig", "init", "tg"]
            else:
                bar_keys = ["orig", "tg"]

            n_bars = len(bar_keys)
            bar_width = 0.78 / n_bars
            offsets = np.linspace(-(n_bars - 1) / 2, (n_bars - 1) / 2, n_bars) * bar_width

            colors  = {"orig": COLOR_ORIG, "init": COLOR_INIT, "tg": COLOR_TG}
            hatches = {"orig": HATCH_ORIG, "init": HATCH_INIT, "tg": HATCH_TG}
            sources = {"orig": orig_s, "init": init_s, "tg": tg_s}

            max_norm = 1.0
            unit = cfg.get("unit", "")

            # cluster_top[i] = top of tallest bar (+ error) in cluster i
            cluster_top = [0.0] * n_ts
            tg_x_pos   = None
            tg_norm_vals = None
            tg_abs_vals  = None

            for b_idx, key in enumerate(bar_keys):
                x_pos = x + offsets[b_idx]
                df_bar = sources[key]
                norm_vals, err_vals, abs_vals = [], [], []
                for ts in tasksizes:
                    v, s = get_values(df_bar, ts)
                    if v is None:
                        norm_vals.append(0.0)
                        err_vals.append(0.0)
                        abs_vals.append(None)
                    else:
                        norm_vals.append(v / ref_val)
                        err_vals.append((s / ref_val) if s else 0.0)
                        abs_vals.append(v)

                ax.bar(x_pos, norm_vals, bar_width,
                       yerr=err_vals,
                       error_kw=dict(elinewidth=0.5, capsize=1.5, capthick=0.5),
                       color=colors[key], edgecolor="black", linewidth=0.4,
                       hatch=hatches[key], zorder=3)

                for i, (nv, ev) in enumerate(zip(norm_vals, err_vals)):
                    cluster_top[i] = max(cluster_top[i], nv + ev)
                max_norm = max(max_norm, *cluster_top)

                if key == "tg":
                    tg_x_pos     = x_pos
                    tg_norm_vals = norm_vals
                    tg_abs_vals  = abs_vals

            # Speedup labels: HSF speedup relative to best baseline (same as bar normalization),
            # placed above the tallest bar per cluster
            if tg_x_pos is not None:
                for xp, nv, av, ctop in zip(
                        tg_x_pos, tg_norm_vals, tg_abs_vals, cluster_top):
                    if av is None or ref_val <= 0:
                        continue
                    speedup = av / ref_val
                    ax.annotate(rf"${speedup:.2f}\times$",
                                xy=(xp, ctop),
                                xytext=(0, 1.5),
                                textcoords="offset points",
                                ha="center", va="bottom",
                                fontsize=5.5, color="black", zorder=6)

            ymax = max(1.05, max_norm * 1.30)
            ax.set_ylim(0, ymax)

            ax.axhline(y=1.0, color="gray", linestyle="--", linewidth=0.5, zorder=2)

            if row_idx == 0:
                ax.set_title(cfg["name"], fontsize=8)
            ax.set_xticks(x)
            tick_labels = [_ts_tick_label(cfg, ts) for ts in tasksizes]
            ax.set_xticklabels(tick_labels, fontsize=6)
            if row_idx == 1:
                xlabel = cfg.get("xlabel", "block size")
                ax.set_xlabel(xlabel, fontsize=7)

            # --- Top annotation: "1.0 = X.XX unit" just below y-max ---
            line_text = _fmt_baseline(ref_val, unit, label="$1.0$")
            if noinit:
                line_text += "\n(Baseline timed out)"
            ax.text(0.98, 0.97, line_text,
                    transform=ax.transAxes,
                    ha="right", va="top",
                    fontsize=5.5, color="dimgray", style="italic", zorder=9)
            if col_idx == 0:
                ax.set_ylabel(rf"\textbf{{{row_label}}}" if matplotlib.rcParams["text.usetex"] else row_label,
                              fontsize=8)
            ax.tick_params(axis="both", which="major", pad=1.5)
            ax.grid(True, axis="y", alpha=0.25, linewidth=0.4, zorder=0)
            ax.set_axisbelow(True)
            # ylim already set above (needed before drawing ceiling bars)

    # Shared y-label and legend
    fig.supylabel("Speedup", fontsize=8)

    legend_handles = [
        Patch(facecolor=COLOR_ORIG, hatch=HATCH_ORIG, edgecolor="black", linewidth=0.4, label=LABEL_ORIG),
        Patch(facecolor=COLOR_INIT, hatch=HATCH_INIT, edgecolor="black", linewidth=0.4, label=LABEL_INIT),
        Patch(facecolor=COLOR_TG,   hatch=HATCH_TG,   edgecolor="black", linewidth=0.4, label=LABEL_TG),
    ]
    fig.legend(handles=legend_handles, loc="upper center",
               ncol=3, bbox_to_anchor=(0.5, 1.07),
               frameon=False, handlelength=1.6, columnspacing=1.4)

    save_fig(fig, out_dir, "benchmark_comparison")


# ---------------------------------------------------------------------------
# Figure 0B — main 2x5 bar comparison (external runtime baseline)
# ---------------------------------------------------------------------------

def make_main_external_baseline_figure(out_dir: Path):
    print("\n[main-ext] benchmark_comparison_external_baseline")
    fig, axes = plt.subplots(2, 5, figsize=(16.0, 7.5), constrained_layout=True)

    comp_by_tg_idx = {c["tg_idx"]: c for c in COMPETITOR}

    for col_idx, cfg in enumerate(BENCHMARKS):
        comp_cfg = comp_by_tg_idx.get(col_idx)
        df_orig, df_tg, df_init = load_all(cfg)

        for row_idx, (size_val, row_label) in enumerate([
            (cfg["large_size"], "Large"),
            (cfg["small_size"], "Small"),
        ]):
            ax = axes[row_idx, col_idx]

            orig_s = filter_size(df_orig, size_val)
            tg_s   = filter_size(df_tg,   size_val)
            init_s = filter_size(df_init, size_val)

            noinit = cfg.get("noinit_too_slow", False)
            ref_s = init_s if noinit else orig_s

            tasksizes = get_intersection_tasksizes(ref_s, tg_s)
            if cfg.get("ts_sort_reverse"):
                tasksizes = list(reversed(tasksizes))

            if not tasksizes or comp_cfg is None:
                ax.text(0.5, 0.5, "no data", transform=ax.transAxes,
                        ha="center", va="center", fontsize=10, color="gray")
                if row_idx == 0:
                    ax.set_title(cfg["name"])
                ax.set_xticks([])
                ax.set_yticks([])
                for spine in ax.spines.values():
                    spine.set_visible(False)
                continue

            comp_rows = load_comp_configs(comp_cfg, size_val)
            ref_val = get_comp_ref_value(comp_cfg, comp_rows)
            if not comp_rows or ref_val is None or ref_val <= 0:
                ax.text(0.5, 0.5, "no data", transform=ax.transAxes,
                        ha="center", va="center", fontsize=10, color="gray")
                if row_idx == 0:
                    ax.set_title(cfg["name"])
                ax.set_xticks([])
                ax.set_yticks([])
                for spine in ax.spines.values():
                    spine.set_visible(False)
                continue

            n_ts = len(tasksizes)
            x = np.arange(n_ts)

            if noinit:
                bar_keys = ["init", "tg"]
            elif init_s is not None:
                bar_keys = ["orig", "init", "tg"]
            else:
                bar_keys = ["orig", "tg"]

            n_bars = len(bar_keys)
            bar_width = 0.78 / n_bars
            offsets = np.linspace(-(n_bars - 1) / 2, (n_bars - 1) / 2, n_bars) * bar_width

            colors  = {"orig": COLOR_ORIG, "init": COLOR_INIT, "tg": COLOR_TG}
            hatches = {"orig": HATCH_ORIG, "init": HATCH_INIT, "tg": HATCH_TG}
            sources = {"orig": orig_s, "init": init_s, "tg": tg_s}

            max_norm = 1.0
            unit = cfg.get("unit", "")

            cluster_top = [0.0] * n_ts
            tg_x_pos   = None
            tg_norm_vals = None
            tg_abs_vals  = None

            for b_idx, key in enumerate(bar_keys):
                x_pos = x + offsets[b_idx]
                df_bar = sources[key]
                norm_vals, err_vals, abs_vals = [], [], []
                for ts in tasksizes:
                    v, s = get_values(df_bar, ts)
                    if v is None:
                        norm_vals.append(0.0)
                        err_vals.append(0.0)
                        abs_vals.append(None)
                    else:
                        norm_vals.append(v / ref_val)
                        err_vals.append((s / ref_val) if s else 0.0)
                        abs_vals.append(v)

                ax.bar(x_pos, norm_vals, bar_width,
                       yerr=err_vals,
                       error_kw=dict(elinewidth=0.5, capsize=1.5, capthick=0.5),
                       color=colors[key], edgecolor="black", linewidth=0.4,
                       hatch=hatches[key], zorder=3)

                for i, (nv, ev) in enumerate(zip(norm_vals, err_vals)):
                    cluster_top[i] = max(cluster_top[i], nv + ev)
                max_norm = max(max_norm, *cluster_top)

                if key == "tg":
                    tg_x_pos     = x_pos
                    tg_norm_vals = norm_vals
                    tg_abs_vals  = abs_vals

            if tg_x_pos is not None:
                for xp, nv, av, ctop in zip(
                        tg_x_pos, tg_norm_vals, tg_abs_vals, cluster_top):
                    if av is None or ref_val <= 0:
                        continue
                    speedup = av / ref_val
                    ax.annotate(rf"${speedup:.2f}\times$",
                                xy=(xp, ctop),
                                xytext=(0, 2.5),
                                textcoords="offset points",
                                ha="center", va="bottom",
                                fontsize=14, color="black", zorder=6)

            ymax = max(1.05, max_norm * 1.30)
            ax.set_ylim(0, ymax)

            # External-runtime reference (line only, no reference bar).
            ax.axhline(y=1.0, color="gray", linestyle=":", linewidth=1.2, zorder=2)

            if row_idx == 0:
                ax.set_title(cfg["name"], fontsize=16)
            ax.set_xticks(x)
            tick_labels = [_ts_tick_label(cfg, ts) for ts in tasksizes]
            ax.set_xticklabels(tick_labels, fontsize=12)
            if row_idx == 1:
                xlabel = cfg.get("xlabel", "block size")
                ax.set_xlabel(xlabel, fontsize=14)

            line_text = _fmt_baseline(ref_val, unit, label="$1.0$")
            if noinit:
                line_text += "\n(Baseline timed out)"
            ax.text(0.98, 0.97, line_text,
                    transform=ax.transAxes,
                    ha="right", va="top",
                    fontsize=14, color="dimgray", style="italic", zorder=9)

            if col_idx == 0:
                ax.set_ylabel(rf"\textbf{{{row_label}}}" if matplotlib.rcParams["text.usetex"] else row_label,
                              fontsize=16)
            ax.tick_params(axis="both", which="major", pad=3.0, labelsize=12)
            ax.grid(True, axis="y", alpha=0.25, linewidth=0.5, zorder=0)
            ax.set_axisbelow(True)

    fig.supylabel("Speedup", fontsize=16)

    legend_handles = [
        Patch(facecolor=COLOR_ORIG, hatch=HATCH_ORIG, edgecolor="black", linewidth=0.5, label=LABEL_ORIG),
        Patch(facecolor=COLOR_INIT, hatch=HATCH_INIT, edgecolor="black", linewidth=0.5, label=LABEL_INIT),
        Patch(facecolor=COLOR_TG,   hatch=HATCH_TG,   edgecolor="black", linewidth=0.5, label=LABEL_TG),
        Line2D([0], [0], color="gray", linestyle=":", linewidth=1.4,
               label=r"AMD-Math / LLVM OpenMP"),
    ]
    fig.legend(handles=legend_handles, loc="upper center",
               ncol=4, bbox_to_anchor=(0.5, 1.10),
               frameon=False, handlelength=2.2, columnspacing=1.8,
               fontsize=14)

    save_fig(fig, out_dir, "benchmark_comparison_external_baseline")


# ---------------------------------------------------------------------------
# Figure A — geomean speedup bars
# ---------------------------------------------------------------------------

def _geomean(values):
    values = [v for v in values if v is not None and v > 0]
    if not values:
        return None
    return float(np.exp(np.mean(np.log(values))))


def make_geomean_figure(out_dir: Path):
    print("\n[geomean] benchmark_speedup_geomean")
    rows = []
    all_small, all_large = [], []
    for cfg in BENCHMARKS:
        df_orig, df_tg, df_init = load_all(cfg)
        noinit = cfg.get("noinit_too_slow", False)
        df_ref = df_init if noinit else df_orig
        small_speedups = []
        large_speedups = []
        for size_val, bucket in [(cfg["small_size"], small_speedups),
                                 (cfg["large_size"], large_speedups)]:
            ref_s = filter_size(df_ref, size_val)
            tg_s  = filter_size(df_tg,  size_val)
            for ts in get_intersection_tasksizes(ref_s, tg_s):
                rv, _ = get_values(ref_s, ts)
                tv, _ = get_values(tg_s, ts)
                if rv and tv:
                    bucket.append(tv / rv)
        print(f"  [{cfg['name']}] small ({len(small_speedups)} pts): "
              f"{[round(v, 3) for v in small_speedups]}")
        print(f"  [{cfg['name']}] large ({len(large_speedups)} pts): "
              f"{[round(v, 3) for v in large_speedups]}")
        gs = _geomean(small_speedups)
        gl = _geomean(large_speedups)
        print(f"  [{cfg['name']}] geomean -> small={gs}, large={gl}")
        rows.append((cfg["name"], gs, gl))
        all_small += small_speedups
        all_large += large_speedups

    overall_small = _geomean(all_small)
    overall_large = _geomean(all_large)

    names = [r[0] for r in rows] + ["Overall"]
    small_vals = [r[1] for r in rows] + [overall_small]
    large_vals = [r[2] for r in rows] + [overall_large]

    fig, ax = plt.subplots(figsize=(3.5, 2.6), constrained_layout=True)
    y = np.arange(len(names))
    h = 0.38
    ax.barh(y - h/2, [v if v else 0 for v in small_vals], h,
            color="#F8B6BE", edgecolor="black", linewidth=0.4,
            hatch=HATCH_ORIG, label="Small", zorder=3)
    ax.barh(y + h/2, [v if v else 0 for v in large_vals], h,
            color=COLOR_TG, edgecolor="black", linewidth=0.4,
            hatch=HATCH_TG, label="Large", zorder=3)

    for i, v in enumerate(small_vals):
        if v:
            ax.text(v + 0.02, i - h/2, rf"${v:.2f}\times$",
                    va="center", ha="left", fontsize=6)
    for i, v in enumerate(large_vals):
        if v:
            ax.text(v + 0.02, i + h/2, rf"${v:.2f}\times$",
                    va="center", ha="left", fontsize=6)

    ax.axvline(1.0, color="gray", linestyle="--", linewidth=0.5)
    ax.set_yticks(y)
    ax.set_yticklabels(names)
    ax.invert_yaxis()
    ax.set_xlabel(r"Geomean speedup (HSF / Baseline)")
    xmax = max(v for v in small_vals + large_vals if v) * 1.25
    ax.set_xlim(0, max(xmax, 1.4))
    ax.grid(True, axis="x", alpha=0.25, linewidth=0.4, zorder=0)
    ax.set_axisbelow(True)
    ax.legend(loc="lower right", frameon=False, fontsize=7)

    save_fig(fig, out_dir, "benchmark_speedup_geomean")


def make_geomean_init_figure(out_dir: Path):
    """Geomean speedup using OmpSs-2 + parallel-init as the reference for
    every benchmark (falls back to OmpSs-2 unmodified if parallel-init data
    is missing)."""
    print("\n[geomean-init] benchmark_speedup_geomean_init")
    rows = []
    all_small, all_large = [], []
    for cfg in BENCHMARKS:
        df_orig, df_tg, df_init = load_all(cfg)
        df_ref = df_init if df_init is not None else df_orig
        small_speedups, large_speedups = [], []
        for size_val, bucket in [(cfg["small_size"], small_speedups),
                                 (cfg["large_size"], large_speedups)]:
            ref_s = filter_size(df_ref, size_val)
            tg_s  = filter_size(df_tg,  size_val)
            for ts in get_intersection_tasksizes(ref_s, tg_s):
                rv, _ = get_values(ref_s, ts)
                tv, _ = get_values(tg_s, ts)
                if rv and tv:
                    bucket.append(tv / rv)
        print(f"  [{cfg['name']}] small ({len(small_speedups)} pts): "
              f"{[round(v, 3) for v in small_speedups]}")
        print(f"  [{cfg['name']}] large ({len(large_speedups)} pts): "
              f"{[round(v, 3) for v in large_speedups]}")
        gs = _geomean(small_speedups)
        gl = _geomean(large_speedups)
        print(f"  [{cfg['name']}] geomean -> small={gs}, large={gl}")
        rows.append((cfg["name"], gs, gl))
        all_small += small_speedups
        all_large += large_speedups

    overall_small = _geomean(all_small)
    overall_large = _geomean(all_large)

    names = [r[0] for r in rows] + ["Overall"]
    small_vals = [r[1] for r in rows] + [overall_small]
    large_vals = [r[2] for r in rows] + [overall_large]

    fig, ax = plt.subplots(figsize=(3.5, 2.6), constrained_layout=True)
    y = np.arange(len(names))
    h = 0.38
    ax.barh(y - h/2, [v if v else 0 for v in small_vals], h,
            color="#F8B6BE", edgecolor="black", linewidth=0.4,
            hatch=HATCH_ORIG, label="Small", zorder=3)
    ax.barh(y + h/2, [v if v else 0 for v in large_vals], h,
            color=COLOR_TG, edgecolor="black", linewidth=0.4,
            hatch=HATCH_TG, label="Large", zorder=3)

    for i, v in enumerate(small_vals):
        if v:
            ax.text(v + 0.02, i - h/2, rf"${v:.2f}\times$",
                    va="center", ha="left", fontsize=6)
    for i, v in enumerate(large_vals):
        if v:
            ax.text(v + 0.02, i + h/2, rf"${v:.2f}\times$",
                    va="center", ha="left", fontsize=6)

    ax.axvline(1.0, color="gray", linestyle="--", linewidth=0.5)
    ax.set_yticks(y)
    ax.set_yticklabels(names)
    ax.invert_yaxis()
    ax.set_title(r"Geometric Mean over OmpSs-2 with parallel initialization",
                 fontsize=8, fontweight="bold")
    ax.set_xlabel(r"Geomean speedup")
    xmax = max(v for v in small_vals + large_vals if v) * 1.25
    ax.set_xlim(0, max(xmax, 1.4))
    ax.grid(True, axis="x", alpha=0.25, linewidth=0.4, zorder=0)
    ax.set_axisbelow(True)
    ax.legend(loc="lower right", frameon=False, fontsize=7)

    save_fig(fig, out_dir, "benchmark_speedup_geomean_init")


# ---------------------------------------------------------------------------
# Figure B — speedup heatmap
# ---------------------------------------------------------------------------

def make_heatmap_figure(out_dir: Path):
    print("\n[heatmap] benchmark_speedup_heatmap")
    # Build the column index: list of (benchmark, size_label, ts) for every
    # config that exists in either orig or tg.
    bench_columns = []  # one list per benchmark
    bench_data = []     # parallel: dict (size_label, ts) -> speedup or None
    for cfg in BENCHMARKS:
        df_orig, df_tg, df_init = load_all(cfg)
        noinit = cfg.get("noinit_too_slow", False)
        df_ref = df_init if noinit else df_orig
        cols = []
        data = {}
        for size_val, slabel in [(cfg["small_size"], "S"),
                                 (cfg["large_size"], "L")]:
            ref_s = filter_size(df_ref, size_val)
            tg_s  = filter_size(df_tg,  size_val)
            ts_set = set()
            if ref_s is not None:
                ts_set |= set(int(t) for t in ref_s["_ts"].tolist())
            if tg_s is not None:
                ts_set |= set(int(t) for t in tg_s["_ts"].tolist())
            for ts in sorted(ts_set):
                cols.append((slabel, ts))
                rv, _ = get_values(ref_s, ts) if ref_s is not None else (None, None)
                tv, _ = get_values(tg_s,  ts) if tg_s  is not None else (None, None)
                if rv and tv:
                    data[(slabel, ts)] = tv / rv
                else:
                    data[(slabel, ts)] = None
        bench_columns.append(cols)
        bench_data.append(data)

    # Total column count drives figure width.
    total_cols = sum(len(c) for c in bench_columns)
    n_rows = len(BENCHMARKS)

    fig_w = max(6.0, 0.32 * total_cols + 1.5)
    fig, ax = plt.subplots(figsize=(fig_w, 2.6), constrained_layout=True)

    matrix = np.full((n_rows, total_cols), np.nan)
    col_offset = 0
    bench_col_starts = []
    for r, cols in enumerate(bench_columns):
        bench_col_starts.append(col_offset)
        for j, key in enumerate(cols):
            v = bench_data[r][key]
            if v is not None:
                matrix[r, col_offset + j] = v
        col_offset += len(cols)

    cmap = matplotlib.colormaps.get_cmap("RdBu_r").copy()
    cmap.set_bad(color="#dddddd")
    im = ax.imshow(matrix, aspect="auto", cmap=cmap,
                   vmin=0.5, vmax=1.5, interpolation="nearest")

    # Cell text
    for r in range(n_rows):
        for c in range(total_cols):
            v = matrix[r, c]
            if np.isnan(v):
                ax.text(c, r, "--", ha="center", va="center",
                        fontsize=5, color="gray")
            else:
                color = "white" if (v < 0.75 or v > 1.25) else "black"
                ax.text(c, r, rf"${v:.2f}$",
                        ha="center", va="center", fontsize=5.5, color=color)

    # Y axis: benchmark names
    ax.set_yticks(np.arange(n_rows))
    ax.set_yticklabels([cfg["name"] for cfg in BENCHMARKS])

    # X axis: ts labels under each cell
    all_labels = []
    for cols in bench_columns:
        for slabel, ts in cols:
            all_labels.append(f"{slabel}:{ts}")
    ax.set_xticks(np.arange(total_cols))
    ax.set_xticklabels(all_labels, rotation=60, ha="right", fontsize=5.5)

    # Group separators between benchmarks
    for start in bench_col_starts[1:]:
        ax.axvline(start - 0.5, color="black", linewidth=0.6)

    ax.set_xlabel(r"$\langle$size, block size$\rangle$ per benchmark", fontsize=7)
    ax.tick_params(axis="x", which="major", pad=1)

    cbar = fig.colorbar(im, ax=ax, fraction=0.025, pad=0.01)
    cbar.set_label(r"Speedup (HSF / Baseline)", fontsize=7)
    cbar.ax.tick_params(labelsize=6)

    save_fig(fig, out_dir, "benchmark_speedup_heatmap")


# ---------------------------------------------------------------------------
# Figure C — scaling curves
# ---------------------------------------------------------------------------

def make_scaling_figure(out_dir: Path):
    print("\n[scaling] benchmark_scaling_curves")
    fig, axes = plt.subplots(2, 5, figsize=(9.0, 4.0), constrained_layout=True)

    for col_idx, cfg in enumerate(BENCHMARKS):
        df_orig, df_tg, df_init = load_all(cfg)
        noinit = cfg.get("noinit_too_slow", False)
        # For scaling: use init as the "baseline" line when no-init is too slow
        df_base = df_init if noinit else df_orig
        base_label = LABEL_INIT if noinit else LABEL_ORIG

        for row_idx, (size_val, row_label) in enumerate([
            (cfg["large_size"], "Large"),
            (cfg["small_size"], "Small"),
        ]):
            ax = axes[row_idx, col_idx]
            base_s = filter_size(df_base, size_val)
            tg_s   = filter_size(df_tg,   size_val)

            if base_s is None and tg_s is None:
                ax.text(0.5, 0.5, "no data", transform=ax.transAxes,
                        ha="center", va="center", fontsize=7, color="gray")
                if row_idx == 0:
                    ax.set_title(cfg["name"])
                ax.set_xticks([])
                ax.set_yticks([])
                continue

            for df_bar, color, marker, label in [
                (base_s, COLOR_ORIG, "o", base_label),
                (tg_s,   COLOR_TG,   "s", LABEL_TG),
            ]:
                if df_bar is None or len(df_bar) == 0:
                    continue
                d = df_bar.sort_values("_ts")  # type: ignore[call-overload]
                ts_vals = d["_ts"].to_numpy()
                m_vals  = d["_metric"].to_numpy()
                s_vals  = d["_std"].to_numpy()
                ax.plot(ts_vals, m_vals, marker=marker, markersize=3.5,
                        linewidth=1.0, color=color, label=label, zorder=3)
                ax.fill_between(ts_vals,
                                m_vals - s_vals, m_vals + s_vals,
                                color=color, alpha=0.18, linewidth=0, zorder=2)

            unit = cfg.get("unit", "")
            if row_idx == 0:
                ax.set_title(cfg["name"], fontsize=8)
            else:
                xlabel = cfg.get("xlabel", "block size")
                ax.set_xlabel(xlabel, fontsize=7)

            if col_idx == 0:
                ax.set_ylabel(rf"\textbf{{{row_label}}}" if matplotlib.rcParams["text.usetex"] else row_label,
                              fontsize=8)

            # Per-subplot unit annotation, low-right corner so it never collides
            ax.text(0.97, 0.04, unit, transform=ax.transAxes,
                    ha="right", va="bottom", fontsize=5.5, color="gray")

            ax.set_xscale("log")
            ax.tick_params(axis="both", which="major", labelsize=6, pad=1.5)
            ax.tick_params(axis="both", which="minor", labelsize=0)
            ax.grid(True, which="major", alpha=0.25, linewidth=0.4, zorder=0)
            ax.set_axisbelow(True)

    fig.supylabel(r"Performance (benchmark-native units)", fontsize=8)

    legend_handles = [
        Line2D([0], [0], color=COLOR_ORIG, marker="o", markersize=3.5,
               linewidth=1.0, label=LABEL_ORIG),
        Line2D([0], [0], color=COLOR_TG, marker="s", markersize=3.5,
               linewidth=1.0, label=LABEL_TG),
    ]
    fig.legend(handles=legend_handles, loc="upper center",
               ncol=2, bbox_to_anchor=(0.5, 1.04),
               frameon=False, handlelength=1.8, columnspacing=1.6)

    save_fig(fig, out_dir, "benchmark_scaling_curves")


# ---------------------------------------------------------------------------
# Figure D — runtime comparison (HSF best vs. runtime competitors)
# ---------------------------------------------------------------------------

def make_runtime_comparison_figure(out_dir: Path):
    print("\n[runtime] benchmark_runtime_comparison")
    fig, axes = plt.subplots(1, 5, figsize=(7.0, 2.55), constrained_layout=True)

    tg_cache = {}
    for col_idx, ccfg in enumerate(COMPETITOR):
        ax = axes[col_idx]
        tg_idx = ccfg["tg_idx"]
        if tg_idx not in tg_cache:
            tg_cache[tg_idx] = load_all(BENCHMARKS[tg_idx])[1]
        df_tg = tg_cache[tg_idx]
        unit = ccfg["unit"]
        runtime_name = ccfg.get("comp_label", "Runtime").replace("OpenMP", "OMP")

        cluster_specs = [
            ("Small", ccfg["small_size"]),
            ("Large", ccfg["large_size"]),
        ]

        # Build bars for each cluster first to compute consistent subplot bounds.
        clusters = []
        for cluster_label, size_val in cluster_specs:
            if size_val is None:
                continue

            comp_rows = load_comp_configs(ccfg, size_val)
            tg_s = filter_size(df_tg, size_val)
            hsf_val, hsf_std = get_hsf_best(tg_s)
            ref_val = get_comp_ref_value(ccfg, comp_rows)

            if not comp_rows or hsf_val is None or ref_val is None or ref_val <= 0:
                continue

            bars = [
                (row["label"], row["metric"], row["std"], COLOR_ORIG, HATCH_ORIG)
                for row in comp_rows
            ]
            bars.append(("HSF", hsf_val, hsf_std, COLOR_TG, HATCH_TG))
            clusters.append((cluster_label, bars, ref_val))

        if not clusters:
            _draw_nodata(ax, ccfg["name"])
            continue

        is_hpccg = ccfg["name"] == "HPCCG"
        bar_width = 0.72
        cluster_gap = 1.2
        xticks = []
        xticklabels = []
        cluster_centers = []
        cluster_titles = []
        cluster_bounds = []
        x_cursor = 0.0
        ymax_data = 1.05

        for cluster_label, bars, ref_val in clusters:
            n_bars = len(bars)
            x_pos = x_cursor + np.arange(n_bars)
            cluster_center = float(np.mean(x_pos))
            cluster_centers.append(cluster_center)
            cluster_titles.append(cluster_label)
            cluster_bounds.append((float(x_pos[0]), float(x_pos[-1])))

            cluster_top = 0.0
            hsf_x = None
            hsf_norm = None
            hsf_abs = None
            for i, (lbl, metric, std, color, hatch) in enumerate(bars):
                norm_val = metric / ref_val
                err_val = (std / ref_val) if std else 0.0

                ax.bar(
                    x_pos[i],
                    norm_val,
                    bar_width,
                    yerr=err_val,
                    error_kw=dict(elinewidth=0.5, capsize=1.5, capthick=0.5),
                    color=color,
                    edgecolor="black",
                    linewidth=0.4,
                    hatch=hatch,
                    zorder=3,
                )

                cluster_top = max(cluster_top, norm_val + err_val)
                if lbl == "HSF":
                    hsf_x = x_pos[i]
                    hsf_norm = norm_val
                    hsf_abs = metric

                # Blue-bar performance labels (always include units):
                # - Cholesky/Multisaxpy: above bar in black
                # - Others: inside bar in white
                # - HPCCG: only label the blue x1 bar
                is_blue_bar = (lbl != "HSF")
                if is_blue_bar:
                    show_perf_label = True
                    if is_hpccg:
                        show_perf_label = str(lbl).replace("×", "x") == "x1"

                    if show_perf_label:
                        perf_text = _fmt_abs_with_unit(metric, unit)
                        if ccfg["name"] in {"Cholesky", "Multisaxpy"}:
                            ax.annotate(
                                perf_text,
                                xy=(x_pos[i], norm_val + err_val),
                                xytext=(0, 1.2),
                                textcoords="offset points",
                                ha="center",
                                va="bottom",
                                fontsize=3.6,
                                color="black",
                                zorder=7,
                                rotation=90,
                            )
                        else:
                            ax.text(
                                x_pos[i],
                                max(0.03, norm_val * 0.5),
                                perf_text,
                                ha="center",
                                va="center",
                                fontsize=3.6,
                                color="white",
                                rotation=90,
                                zorder=5,
                            )

                xticks.append(float(x_pos[i]))
                if lbl == "HSF":
                    if is_hpccg:
                        xticklabels.append("x1")
                    else:
                        xticklabels.append("HSF")
                elif is_hpccg:
                    xticklabels.append(str(lbl).replace("×", "x"))
                elif lbl in {runtime_name, "OMP"}:
                    xticklabels.append(runtime_name)
                else:
                    xticklabels.append(f"{runtime_name}\n{lbl}")

            if hsf_x is not None and hsf_abs is not None and hsf_norm is not None:
                speedup = hsf_abs / ref_val
                ax.annotate(
                    rf"${speedup:.2f}\times$",
                    xy=(hsf_x, cluster_top),
                    xytext=(0, 1.5),
                    textcoords="offset points",
                    ha="center",
                    va="bottom",
                    fontsize=5.5,
                    color="black",
                    zorder=6,
                )

            ymax_data = max(ymax_data, cluster_top * 1.30)
            x_cursor = x_pos[-1] + 1.0 + cluster_gap

        ax.set_ylim(0, ymax_data)
        ax.axhline(1.0, color="gray", linestyle="--", linewidth=0.5, zorder=2)
        if len(cluster_bounds) >= 2:
            divider_x = 0.5 * (cluster_bounds[0][1] + cluster_bounds[1][0])
            ax.axvline(divider_x, color="gray", linestyle=":", linewidth=0.7, zorder=1)
        ax.set_title(ccfg["name"], fontsize=8, pad=2.0)
        ax.set_xticks(xticks)
        if is_hpccg:
            ax.set_xticklabels(xticklabels, fontsize=6)
            ax.tick_params(axis="x", which="both", length=2.0)
        else:
            ax.set_xticklabels([])
            ax.tick_params(axis="x", which="both", length=0)

        for center, cluster_title in zip(cluster_centers, cluster_titles):
            ax.text(center, -0.12, cluster_title,
                    transform=ax.get_xaxis_transform(),
                    ha="center", va="top",
                    fontsize=6, fontweight="bold")

        if col_idx == 0:
            ax.set_ylabel("Speedup", fontsize=8)

        ax.tick_params(axis="both", which="major", pad=1.5)
        ax.grid(True, axis="y", alpha=0.25, linewidth=0.4, zorder=0)
        ax.set_axisbelow(True)

    legend_handles = [
        Patch(facecolor=COLOR_ORIG, hatch=HATCH_ORIG, edgecolor="black",
              linewidth=0.4, label=r"External runtime (OpenMP / libFLAME+BLIS)"),
        Patch(facecolor=COLOR_TG, hatch=HATCH_TG, edgecolor="black",
              linewidth=0.4, label=LABEL_TG),
    ]
    fig.legend(handles=legend_handles, loc="upper center",
               ncol=2, bbox_to_anchor=(0.5, 1.12),
               frameon=False, handlelength=1.6, columnspacing=1.4)

    save_fig(fig, out_dir, "benchmark_runtime_comparison")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

FIGURES = {
    "main":    make_main_figure,
    "main_ext": make_main_external_baseline_figure,
    "geomean": make_geomean_figure,
    "geomean_init": make_geomean_init_figure,
    "heatmap": make_heatmap_figure,
    "scaling": make_scaling_figure,
    "runtime": make_runtime_comparison_figure,
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--no-usetex", action="store_true",
                        help="Disable LaTeX rendering (faster, no LaTeX install needed).")
    parser.add_argument("--out", default="fig",
                        help="Output directory (default: fig).")
    parser.add_argument("--figure", default="all",
                        choices=["all"] + list(FIGURES.keys()),
                        help="Which figure to render (default: all).")
    args = parser.parse_args()

    configure_style(use_tex=not args.no_usetex)
    out_dir = (BASE / args.out).resolve()

    targets = list(FIGURES.keys()) if args.figure == "all" else [args.figure]
    for name in targets:
        FIGURES[name](out_dir)

    print(f"\nDone. Figures written to {out_dir}")


if __name__ == "__main__":
    main()
