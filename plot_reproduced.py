#!/usr/bin/env python3
"""
LaTeX-styled benchmark figures for reproduced results.

Generates PDFs (and matching PNGs) into ``fig/``:

  * ``benchmark_comparison``        — main 2x5 normalized bar chart
  * ``benchmark_comparison_external_baseline`` — 2x5 normalized to the external
                                      (OpenMP/AMD-Math) baseline at y=1.0
  * ``benchmark_speedup_geomean``   — per-benchmark geomean speedup bars
  * ``benchmark_speedup_geomean_init`` — geomean speedup using OmpSs-2 with parallel-init

Usage:
    python plot_reproduced.py                 # all figures, with LaTeX
    python plot_reproduced.py --no-usetex     # skip LaTeX (fast preview)
    python plot_reproduced.py --figure main   # one figure only
    python plot_reproduced.py --out fig       # output dir (default: fig)
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

BASE = Path(__file__).resolve().parent / "reproduced_results"

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

# External (non-OmpSs-2) baseline used as the y=1.0 reference: the pure
# OpenMP/MPI runtimes reproduced under baseline/ (collected into
# reproduced_results/fox_<bench>_<variant>/summary.csv).
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
# External-baseline (competitor) helpers
# ---------------------------------------------------------------------------

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


# ---------------------------------------------------------------------------
# Figure — main 2x5 normalized to the external (OpenMP/AMD-Math) baseline
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
# Entry point
# ---------------------------------------------------------------------------

FIGURES = {
    "main":    make_main_figure,
    "main_ext": make_main_external_baseline_figure,
    "geomean": make_geomean_figure,
    "geomean_init": make_geomean_init_figure,
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
    out_dir = (Path(__file__).resolve().parent / args.out).resolve()

    targets = list(FIGURES.keys()) if args.figure == "all" else [args.figure]
    for name in targets:
        FIGURES[name](out_dir)

    print(f"\nDone. Figures written to {out_dir}")


if __name__ == "__main__":
    main()
