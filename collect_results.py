#!/usr/bin/env python3
"""
collect_results.py — Parse SLURM output files from reproduced benchmarks
and generate results.csv + summary.csv matching the original results/ format.

Usage:
    python collect_results.py [--results-dir reproduced_results]
"""
import argparse
import csv
import os
import re
import sys
from collections import defaultdict
from pathlib import Path
import json
import math

RESULTS_DIR = "reproduced_results"


# ---------------------------------------------------------------------------
# Output parsers — one per benchmark variant
# ---------------------------------------------------------------------------

def parse_cholesky(line):
    """Parse: Printing result:  <duration> <gflops> <N> <TS>"""
    m = re.search(r"Printing result:\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+(\d+)\s+(\d+)", line)
    if m:
        return {
            "duration_s": float(m.group(1)),
            "gflops": float(m.group(2)),
            "N": int(m.group(3)),
            "TS": int(m.group(4)),
        }
    return None


def parse_heat_hsf(line):
    """Parse: <execTime> fake_exec: <delta_time> <throughput> <0> <rows> <cols> <rbs> <cbs> <niter> resultstring"""
    m = re.search(
        r"([\d.eE+-]+)\s+fake_exec:\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+[\d.eE+-]+\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+resultstring",
        line,
    )
    if m:
        return {
            "delta_time": float(m.group(2)),
            "throughput": float(m.group(3)),
            "rows": int(m.group(4)),
            "cols": int(m.group(5)),
            "rbs": int(m.group(6)),
            "cbs": int(m.group(7)),
            "niter": int(m.group(8)),
        }
    return None


def parse_heat_nosvorig(line):
    """Parse: <delta_time> <throughput> <0> <rows> <cols> <rbs> <cbs> <niter> heat_result"""
    m = re.search(
        r"([\d.eE+-]+)\s+([\d.eE+-]+)\s+[\d.eE+-]+\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+heat_result",
        line,
    )
    if m:
        return {
            "delta_time": float(m.group(1)),
            "throughput": float(m.group(2)),
            "rows": int(m.group(3)),
            "cols": int(m.group(4)),
            "rbs": int(m.group(5)),
            "cbs": int(m.group(6)),
            "niter": int(m.group(7)),
        }
    return None


def parse_hpccg(line):
    """Parse: time,<t>,nx,<nx>,ny,<ny>,nz,<nz>,ranks,<r>,threads,<t>,ntasks,<n>,iterations,<i>,flops,<f>,mflops,<m>,total_mem(bytes),<b>"""
    m = re.search(
        r"time,([\d.eE+-]+),nx,(\d+),ny,(\d+),nz,(\d+),ranks,(\d+),threads,(\d+),ntasks,(\d+),iterations,(\d+),flops,([\d.eE+-]+),mflops,([\d.eE+-]+),total_mem\(bytes\),(\d+)",
        line,
    )
    if m:
        return {
            "time_s": float(m.group(1)),
            "nx": int(m.group(2)),
            "ny": int(m.group(3)),
            "nz": int(m.group(4)),
            "ranks": int(m.group(5)),
            "threads": int(m.group(6)),
            "ntasks": int(m.group(7)),
            "iterations": int(m.group(8)),
            "flops": float(m.group(9)),
            "mflops": float(m.group(10)),
        }
    return None


def parse_matmul(line):
    """Parse: Printing result <duration> <N> <M> <its> <TS> <gflops>"""
    m = re.search(
        r"Printing result\s+([\d.eE+-]+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+([\d.eE+-]+)",
        line,
    )
    if m:
        return {
            "duration_s": float(m.group(1)),
            "N": int(m.group(2)),
            "M": int(m.group(3)),
            "its": int(m.group(4)),
            "TS": int(m.group(5)),
            "gflops": float(m.group(6)),
        }
    return None


def saxpy_gflops(n, its, duration_s):
    """Multisaxpy GFLOP/s from the measured time, 2 FLOPs (mul + add) per element.

    Used for every Multisaxpy variant instead of the printed value: older
    OmpSs-2/OpenMP binaries counted 1 FLOP per element while HSF counted 2,
    which doubled every HSF/baseline Multisaxpy ratio.
    """
    return 2.0 * n * its / duration_s / 1e9


def parse_multisaxpy_hsf(line):
    """Parse: Printing result <duration> <N> <TS> <its> <gflops>"""
    m = re.search(
        r"Printing result\s+([\d.eE+-]+)\s+(\d+)\s+(\d+)\s+(\d+)\s+([\d.eE+-]+)",
        line,
    )
    if m:
        duration, n, its = float(m.group(1)), int(m.group(2)), int(m.group(4))
        return {
            "duration_s": duration,
            "N": n,
            "TS": int(m.group(3)),
            "iterations": its,
            "gflops": saxpy_gflops(n, its, duration),
        }
    return None


def parse_multisaxpy_nosvorig(line):
    """Parse: <duration> <gflops> <N> <TS> <its> result_multisaxpy"""
    m = re.search(
        r"([\d.eE+-]+)\s+([\d.eE+-]+)\s+(\d+)\s+(\d+)\s+(\d+)\s+result_multisaxpy",
        line,
    )
    if m:
        duration, n, its = float(m.group(1)), int(m.group(3)), int(m.group(5))
        return {
            "duration_s": duration,
            "gflops": saxpy_gflops(n, its, duration),
            "N": n,
            "TS": int(m.group(4)),
            "iterations": its,
        }
    return None


# ---------------------------------------------------------------------------
# Baseline (external, non-OmpSs-2) parsers — one result per job*.out (whole file)
#
# These binaries run once per SLURM array task, so each job*.out holds exactly
# one measurement. Parse the whole file (the metric may span multiple lines).
# ---------------------------------------------------------------------------

def baseline_cholesky(text):
    """Cholesky libFLAME: 'Printing result:  <time> <gflops> <N>'."""
    m = re.search(r"Printing result:\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+\d+", text)
    if m:
        return {"duration_s": float(m.group(1)), "gflops": float(m.group(2))}
    return None


def baseline_heat(text):
    """Heat OpenMP: '<time> <throughput> <0> <r> <c> <rbs> <cbs> <its> heat_omp'."""
    m = re.search(
        r"^\s*([\d.eE+-]+)\s+([\d.eE+-]+)\s+[\d.eE+-]+\s+\d+\s+\d+\s+\d+\s+\d+\s+\d+\s+heat_omp",
        text, re.MULTILINE,
    )
    if m:
        return {"delta_time": float(m.group(1)), "throughput": float(m.group(2))}
    return None


def baseline_hpccg(text):
    """HPCCG: 'Time Summary:' Total (s) and 'MFLOPS Summary:' Total."""
    mt = re.search(r"Time Summary:\s*\n\s*Total\s*:\s*([\d.eE+-]+)", text)
    mm = re.search(r"MFLOPS Summary:\s*\n\s*Total\s*:\s*([\d.eE+-]+)", text)
    if mt and mm:
        return {"time_s": float(mt.group(1)), "mflops": float(mm.group(1))}
    return None


def baseline_matmul(text):
    """mt-dgemm: 'Multiply time:  <t> seconds' and 'GFLOP/s rate:  <g> GF/s'."""
    mt = re.search(r"Multiply time:\s+([\d.eE+-]+)\s+seconds", text)
    mg = re.search(r"GFLOP/s rate:\s+([\d.eE+-]+)\s+GF/s", text)
    if mt and mg:
        return {"duration_s": float(mt.group(1)), "gflops": float(mg.group(1))}
    return None


def baseline_multisaxpy(text):
    """multisaxpy_smp: '<time> <gflops> <N> NaN <its> multisaxpy_smp'."""
    m = re.search(
        r"^\s*([\d.eE+-]+)\s+([\d.eE+-]+)\s+(\d+)\s+NaN\s+(\d+)\s+multisaxpy_smp",
        text, re.MULTILINE,
    )
    if m:
        duration = float(m.group(1))
        return {"duration_s": duration,
                "gflops": saxpy_gflops(int(m.group(3)), int(m.group(4)), duration)}
    return None


# ---------------------------------------------------------------------------
# Metadata parser — extract config from experiment string in filename
# ---------------------------------------------------------------------------

def parse_exp_str(exp_str):
    """Parse key=value pairs from experiment string like 'n6144_ts128_imm_true_mmap1'."""
    parts = {}
    for token in exp_str.split("_"):
        # Try key-value split on common patterns
        for prefix in ["imm", "ppn", "mmap", "prio", "numa", "tgenabled", "lower",
                        "upper", "affflex", "flex", "bsf", "forcenb", "l3", "hier",
                        "hwc", "policy", "label", "diags", "bf", "tge", "useprio",
                        "gemmtpb", "cpus", "threads", "maxit", "its", "iterations",
                        "warmup"]:
            if token.startswith(prefix) and len(token) > len(prefix):
                parts[prefix] = token[len(prefix):]
                break
    return parts


# ---------------------------------------------------------------------------
# SLURM output scanner
# ---------------------------------------------------------------------------

def scan_raw_dir(raw_dir, parser):
    """Scan all job*.out files in raw_dir, parse benchmark output lines."""
    results = []
    raw_path = Path(raw_dir)
    if not raw_path.exists():
        return results

    for exp_dir in sorted(raw_path.iterdir()):
        if not exp_dir.is_dir():
            continue
        exp_str = exp_dir.name

        # Read metadata from exp_meta.json if present
        meta_file = exp_dir / "exp_meta.json"
        if meta_file.exists():
            with open(meta_file) as f:
                meta = json.load(f)
        else:
            meta = {"experiment": exp_str}

        for out_file in sorted(exp_dir.glob("job*.out")):
            with open(out_file) as f:
                for line in f:
                    parsed = parser(line)
                    if parsed:
                        row = dict(meta)
                        row.update(parsed)
                        results.append(row)
    return results


def scan_baseline(raw_dir, file_parser):
    """One result per job*.out (whole-file parse). Returns {exp: (meta, [runs])}."""
    out = {}
    raw_path = Path(raw_dir)
    if not raw_path.exists():
        return out
    for exp_dir in sorted(raw_path.iterdir()):
        if not exp_dir.is_dir():
            continue
        meta_file = exp_dir / "exp_meta.json"
        if meta_file.exists():
            with open(meta_file) as f:
                meta = json.load(f)
        else:
            meta = {"experiment": exp_dir.name}
        runs = []
        for out_file in sorted(exp_dir.glob("job*.out")):
            parsed = file_parser(out_file.read_text())
            if parsed:
                runs.append(parsed)
        if runs:
            out[exp_dir.name] = (meta, runs)
    return out


# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------

def compute_stats(values):
    n = len(values)
    if n == 0:
        return 0, 0.0, 0.0, 0.0, 0.0, 0.0
    mean = sum(values) / n
    median = sorted(values)[n // 2]
    mn = min(values)
    mx = max(values)
    if n > 1:
        variance = sum((v - mean) ** 2 for v in values) / (n - 1)
        std = math.sqrt(variance)
    else:
        std = 0.0
    return n, mean, median, mn, mx, std


# ---------------------------------------------------------------------------
# Per-benchmark collectors
# ---------------------------------------------------------------------------

def collect_cholesky(variant, subdir, results_dir):
    """Collect cholesky results for orig/init/tg."""
    if variant == "tg":
        base = Path(results_dir) / "fox_cholesky_tg" / "results_final_3bs"
    elif variant == "init":
        base = Path(results_dir) / "fox_cholesky_orig" / "results_final_3bs_init_tasks"
    else:
        base = Path(results_dir) / "fox_cholesky_orig" / "results_final_3bs"

    raw = base / "raw"
    runs = scan_raw_dir(raw, parse_cholesky)
    if not runs:
        print(f"  No data for cholesky {variant}")
        return

    # Group by (N, TS)
    groups = defaultdict(list)
    for r in runs:
        key = (r["N"], r["TS"])
        groups[key].append(r)

    rows = []
    for (N, TS), run_list in sorted(groups.items()):
        gflops_vals = [r["gflops"] for r in run_list]
        dur_vals = [r["duration_s"] for r in run_list]
        nruns, mean_g, med_g, min_g, max_g, std_g = compute_stats(gflops_vals)
        _, mean_d, *_ = compute_stats(dur_vals)

        meta = run_list[0].get("experiment", "")
        exp_name = meta if meta else f"N{N}_TS{TS}"

        if variant == "tg":
            row = {
                "experiment": exp_name,
                "N": N, "TS": TS,
                "lower": run_list[0].get("lower", ""),
                "upper": run_list[0].get("upper", ""),
                "affflex": run_list[0].get("affflex", ""),
                "useprio": run_list[0].get("useprio", ""),
                "gemmtpb": run_list[0].get("gemmtpb", ""),
                "tgenabled": run_list[0].get("tgenabled", ""),
                "imm": run_list[0].get("imm", ""),
                "ppn": run_list[0].get("ppn", ""),
                "mmap": run_list[0].get("mmap", ""),
                "nruns": nruns,
                "mean_gflops": mean_g, "median_gflops": med_g,
                "min_gflops": min_g, "max_gflops": max_g,
                "std_gflops": std_g, "mean_duration_s": mean_d,
            }
        else:
            row = {
                "experiment": exp_name,
                "N": N, "TS": TS,
                "lower": run_list[0].get("lower", "node"),
                "upper": run_list[0].get("upper", "node"),
                "affflex": run_list[0].get("affflex", 0),
                "useprio": run_list[0].get("useprio", 0),
                "gemmtpb": run_list[0].get("gemmtpb", 48),
                "tgenabled": run_list[0].get("tgenabled", 0),
                "imm": run_list[0].get("imm", "true"),
                "ppn": run_list[0].get("ppn", 1),
                "mmap": run_list[0].get("mmap", 1),
                "nruns": nruns,
                "mean_gflops": mean_g, "median_gflops": med_g,
                "min_gflops": min_g, "max_gflops": max_g,
                "std_gflops": std_g, "mean_duration_s": mean_d,
            }
        rows.append(row)

    if rows:
        header = list(rows[0].keys())
        write_csv(base / "summary.csv", header, rows)
        print(f"  Wrote {base / 'summary.csv'} ({len(rows)} configs)")


def collect_heat(variant, subdir, results_dir):
    if variant == "tg":
        base = Path(results_dir) / "fox_heat_tg" / "results_final_3bs"
        parser = parse_heat_hsf
    elif variant == "init":
        base = Path(results_dir) / "fox_heat_orig" / "results_final_3bs_init_tasks"
        parser = parse_heat_nosvorig
    else:
        base = Path(results_dir) / "fox_heat_orig" / "results_final_3bs"
        parser = parse_heat_nosvorig

    raw = base / "raw"
    runs = scan_raw_dir(raw, parser)
    if not runs:
        print(f"  No data for heat {variant}")
        return

    groups = defaultdict(list)
    for r in runs:
        key = (r["rows"], r["rbs"])
        groups[key].append(r)

    rows = []
    for (n, bs), run_list in sorted(groups.items()):
        dt_vals = [r["delta_time"] for r in run_list]
        tp_vals = [r["throughput"] for r in run_list]
        nruns, mean_dt, med_dt, min_dt, max_dt, std_dt = compute_stats(dt_vals)
        _, mean_tp, *_ = compute_stats(tp_vals)

        meta = run_list[0].get("experiment", "")
        exp_name = meta if meta else f"n{n}_bs{bs}"

        if variant == "tg":
            row = {
                "experiment": exp_name,
                "n": n, "bs": bs,
                "its": run_list[0].get("its", ""),
                "diags": run_list[0].get("diags", ""),
                "prio": run_list[0].get("prio", ""),
                "bf": run_list[0].get("bf", ""),
                "tge": run_list[0].get("tge", ""),
                "lower": run_list[0].get("lower", ""),
                "upper": run_list[0].get("upper", ""),
                "flex": run_list[0].get("flex", ""),
                "policy": run_list[0].get("policy", ""),
                "nruns": nruns,
                "mean_delta_time": mean_dt, "median_delta_time": med_dt,
                "min_delta_time": min_dt, "max_delta_time": max_dt,
                "std_delta_time": std_dt, "mean_throughput": mean_tp,
            }
        else:
            row = {
                "experiment": exp_name,
                "n": n, "bs": bs,
                "its": run_list[0].get("its", ""),
                "imm": run_list[0].get("imm", "true"),
                "mmap": run_list[0].get("mmap", 1),
                "prio": run_list[0].get("prio", 1),
                "numa": run_list[0].get("numa", ""),
                "nruns": nruns,
                "mean_delta_time": mean_dt, "median_delta_time": med_dt,
                "min_delta_time": min_dt, "max_delta_time": max_dt,
                "std_delta_time": std_dt, "mean_throughput": mean_tp,
            }
        rows.append(row)

    if rows:
        header = list(rows[0].keys())
        write_csv(base / "summary.csv", header, rows)
        print(f"  Wrote {base / 'summary.csv'} ({len(rows)} configs)")


def collect_hpccg(variant, subdir, results_dir):
    if variant == "tg":
        base = Path(results_dir) / "fox_hpccg_tg" / "results_final_3bs"
    elif variant == "init":
        base = Path(results_dir) / "fox_hpccg_orig" / "results_final_3bs_init_tasks"
    else:
        base = Path(results_dir) / "fox_hpccg_orig" / "results_final_3bs"

    raw = base / "raw"
    runs = scan_raw_dir(raw, parse_hpccg)
    if not runs:
        print(f"  No data for hpccg {variant}")
        return

    groups = defaultdict(list)
    for r in runs:
        key = (r["nx"], r["ny"], r["nz"], r["ntasks"])
        groups[key].append(r)

    rows = []
    for (nx, ny, nz, ntasks), run_list in sorted(groups.items()):
        mf_vals = [r["mflops"] for r in run_list]
        t_vals = [r["time_s"] for r in run_list]
        nruns, mean_mf, med_mf, min_mf, max_mf, std_mf = compute_stats(mf_vals)
        _, mean_t, *_ = compute_stats(t_vals)

        meta = run_list[0].get("experiment", "")
        exp_name = meta if meta else f"nx{nx}_ny{ny}_nz{nz}_nt{ntasks}"

        if variant == "tg":
            row = {
                "experiment": exp_name,
                "nx": nx, "ny": ny, "nz": nz,
                "ntasks": ntasks,
                "maxit": run_list[0].get("maxit", ""),
                "tgenabled": run_list[0].get("tgenabled", 1),
                "lower": run_list[0].get("lower", ""),
                "upper": run_list[0].get("upper", ""),
                "flex": run_list[0].get("flex", ""),
                "imm": run_list[0].get("imm", "false"),
                "ppn": run_list[0].get("ppn", 1),
                "threads": run_list[0].get("threads", ""),
                "bsf": run_list[0].get("bsf", ""),
                "nruns": nruns,
                "mean_mflops": mean_mf, "median_mflops": med_mf,
                "min_mflops": min_mf, "max_mflops": max_mf,
                "std_mflops": std_mf, "mean_time_s": mean_t,
            }
        else:
            row = {
                "experiment": exp_name,
                "nx": nx, "ny": ny, "nz": nz,
                "ntasks": ntasks,
                "maxit": run_list[0].get("maxit", ""),
                "tgenabled": run_list[0].get("tgenabled", 0),
                "lower": run_list[0].get("lower", "node"),
                "upper": run_list[0].get("upper", "node"),
                "flex": run_list[0].get("flex", 0),
                "imm": run_list[0].get("imm", "true"),
                "ppn": run_list[0].get("ppn", 1),
                "cpuspertask": run_list[0].get("cpuspertask", ""),
                "threads": run_list[0].get("threads", ""),
                "bsf": run_list[0].get("bsf", ""),
                "mmap": run_list[0].get("mmap", 1),
                "numainterleaved": run_list[0].get("numainterleaved", ""),
                "nruns": nruns,
                "mean_mflops": mean_mf, "median_mflops": med_mf,
                "min_mflops": min_mf, "max_mflops": max_mf,
                "std_mflops": std_mf, "mean_time_s": mean_t,
            }
        rows.append(row)

    if rows:
        header = list(rows[0].keys())
        write_csv(base / "summary.csv", header, rows)
        print(f"  Wrote {base / 'summary.csv'} ({len(rows)} configs)")


def collect_matmul(variant, subdir, results_dir):
    if variant == "tg":
        base = Path(results_dir) / "fox_matmul_tg" / "results_final_3bs"
    elif variant == "init":
        base = Path(results_dir) / "fox_matmul_orig" / "results_final_3bs_init_tasks"
    else:
        base = Path(results_dir) / "fox_matmul_orig" / "results_final_3bs"

    raw = base / "raw"
    runs = scan_raw_dir(raw, parse_matmul)
    if not runs:
        print(f"  No data for matmul {variant}")
        return

    groups = defaultdict(list)
    for r in runs:
        key = (r["N"], r["M"], r["TS"])
        groups[key].append(r)

    rows = []
    for (N, M, TS), run_list in sorted(groups.items()):
        gf_vals = [r["gflops"] for r in run_list]
        t_vals = [r["duration_s"] for r in run_list]
        nruns, mean_g, med_g, min_g, max_g, std_g = compute_stats(gf_vals)
        _, mean_t, med_t, min_t, max_t, std_t = compute_stats(t_vals)

        meta = run_list[0].get("experiment", "")
        exp_name = meta if meta else f"N{N}_M{M}_TS{TS}"

        if variant == "tg":
            row = {
                "experiment": exp_name,
                "N": N, "M": M, "TS": TS,
                "tgenabled": run_list[0].get("tgenabled", 1),
                "hier": run_list[0].get("hier", 0),
                "lower": run_list[0].get("lower", ""),
                "upper": run_list[0].get("upper", ""),
                "affflex": run_list[0].get("affflex", ""),
                "imm": run_list[0].get("imm", "false"),
                "ppn": run_list[0].get("ppn", 1),
                "mmap": run_list[0].get("mmap", 1),
                "forcenb": run_list[0].get("forcenb", 1),
                "hwc": run_list[0].get("hwc", "none"),
                "l3": run_list[0].get("l3", 96),
                "nruns": nruns,
                "mean_gflops": mean_g, "median_gflops": med_g,
                "min_gflops": min_g, "max_gflops": max_g,
                "std_gflops": std_g, "mean_duration_s": mean_t,
            }
        else:
            row = {
                "experiment": exp_name,
                "imm": run_list[0].get("imm", "true"),
                "ppn": run_list[0].get("ppn", 1),
                "cpuspertask": run_list[0].get("cpuspertask", ""),
                "nsize": N, "msize": M, "ts": TS,
                "its": run_list[0].get("its", ""),
                "mmap": run_list[0].get("mmap", 1),
                "numa": run_list[0].get("numa", 0),
                "nruns": nruns,
                "mean_time": mean_t, "median_time": med_t,
                "min_time": min_t, "max_time": max_t,
                "std_time": std_t,
                "mean_gflops": mean_g, "std_gflops": std_g,
            }
        rows.append(row)

    if rows:
        header = list(rows[0].keys())
        write_csv(base / "summary.csv", header, rows)
        print(f"  Wrote {base / 'summary.csv'} ({len(rows)} configs)")


def collect_multisaxpy(variant, subdir, results_dir):
    if variant == "tg":
        base = Path(results_dir) / "fox_multisaxpy_tg" / "results_final_3bs"
        parser = parse_multisaxpy_hsf
    else:
        base = Path(results_dir) / "fox_multisaxpy_orig" / "results_final_3bs_init_tasks"
        parser = parse_multisaxpy_nosvorig

    raw = base / "raw"
    runs = scan_raw_dir(raw, parser)
    if not runs:
        print(f"  No data for multisaxpy {variant}")
        return

    groups = defaultdict(list)
    for r in runs:
        key = (r["N"], r["TS"])
        groups[key].append(r)

    rows = []
    for (N, TS), run_list in sorted(groups.items()):
        dur_vals = [r["duration_s"] for r in run_list]
        gf_vals = [r["gflops"] for r in run_list]
        nruns, mean_d, med_d, min_d, max_d, std_d = compute_stats(dur_vals)
        _, mean_g, med_g, *_ = compute_stats(gf_vals)

        meta = run_list[0].get("experiment", "")
        exp_name = meta if meta else f"N{N}_TS{TS}"

        if variant == "tg":
            row = {
                "experiment": exp_name,
                "label": run_list[0].get("label", ""),
                "lower": run_list[0].get("lower", ""),
                "policy": run_list[0].get("policy", ""),
                "N": N, "TS": TS,
                "iterations": run_list[0].get("iterations", ""),
                "cpus": run_list[0].get("cpus", ""),
                "ppn": run_list[0].get("ppn", 1),
                "nruns": nruns,
                "mean_duration_s": mean_d, "median_duration_s": med_d,
                "min_duration_s": min_d, "max_duration_s": max_d,
                "std_duration_s": std_d,
                "mean_gflops": mean_g, "median_gflops": med_g,
            }
        else:
            row = {
                "experiment": exp_name,
                "imm": run_list[0].get("imm", ""),
                "mmap": run_list[0].get("mmap", ""),
                "prio": run_list[0].get("prio", ""),
                "N": N, "TS": TS,
                "iterations": run_list[0].get("iterations", ""),
                "cpus": run_list[0].get("cpus", ""),
                "ppn": run_list[0].get("ppn", 1),
                "nruns": nruns,
                "mean_duration_s": mean_d, "median_duration_s": med_d,
                "min_duration_s": min_d, "max_duration_s": max_d,
                "std_duration_s": std_d,
                "mean_gflops": mean_g, "median_gflops": med_g,
            }
        rows.append(row)

    if rows:
        header = list(rows[0].keys())
        write_csv(base / "summary.csv", header, rows)
        print(f"  Wrote {base / 'summary.csv'} ({len(rows)} configs)")


# ---------------------------------------------------------------------------
# Baseline (external OpenMP/MPI) collectors — summary.csv columns match the
# competitor summaries in results/fox_<bench>_<variant>/summary.csv
# ---------------------------------------------------------------------------

def collect_cholesky_baseline(results_dir):
    base = Path(results_dir) / "fox_cholesky_libflame"
    data = scan_baseline(base / "raw", baseline_cholesky)
    if not data:
        print("  No data for cholesky baseline (libflame)")
        return
    rows = []
    for exp_name, (meta, runs) in sorted(data.items()):
        nruns, mean_t, med_t, min_t, max_t, std_t = compute_stats([r["duration_s"] for r in runs])
        _, mean_g, _, _, _, std_g = compute_stats([r["gflops"] for r in runs])
        rows.append({
            "experiment": exp_name,
            "ppn": meta.get("ppn", 1), "cpuspertask": meta.get("cpuspertask", ""),
            "nsize": meta.get("nsize", ""), "numa": meta.get("numa", ""),
            "nruns": nruns,
            "mean_time": mean_t, "median_time": med_t, "min_time": min_t,
            "max_time": max_t, "std_time": std_t,
            "mean_gflops": mean_g, "std_gflops": std_g,
        })
    write_csv(base / "summary.csv", list(rows[0].keys()), rows)
    print(f"  Wrote {base / 'summary.csv'} ({len(rows)} configs)")


def collect_heat_baseline(results_dir):
    base = Path(results_dir) / "fox_heat_omp"
    data = scan_baseline(base / "raw", baseline_heat)
    if not data:
        print("  No data for heat baseline (omp)")
        return
    rows = []
    for exp_name, (meta, runs) in sorted(data.items()):
        nruns, mean_t, med_t, min_t, max_t, std_t = compute_stats([r["delta_time"] for r in runs])
        _, mean_tp, _, _, _, std_tp = compute_stats([r["throughput"] for r in runs])
        rows.append({
            "experiment": exp_name,
            "ppn": meta.get("ppn", 1), "cpuspertask": meta.get("cpuspertask", ""),
            "n": meta.get("n", ""), "bs": meta.get("bs", ""), "its": meta.get("its", ""),
            "numa": meta.get("numa", ""), "procbind": meta.get("procbind", ""),
            "nruns": nruns,
            "mean_time": mean_t, "median_time": med_t, "min_time": min_t,
            "max_time": max_t, "std_time": std_t,
            "mean_throughput": mean_tp, "std_throughput": std_tp,
        })
    write_csv(base / "summary.csv", list(rows[0].keys()), rows)
    print(f"  Wrote {base / 'summary.csv'} ({len(rows)} configs)")


def collect_hpccg_baseline(results_dir):
    base = Path(results_dir) / "fox_hpccg_omp"
    data = scan_baseline(base / "raw", baseline_hpccg)
    if not data:
        print("  No data for hpccg baseline (omp)")
        return
    rows = []
    for exp_name, (meta, runs) in sorted(data.items()):
        nruns, mean_t, med_t, min_t, max_t, std_t = compute_stats([r["time_s"] for r in runs])
        _, mean_mf, _, _, _, std_mf = compute_stats([r["mflops"] for r in runs])
        rows.append({
            "experiment": exp_name,
            "ppn": meta.get("ppn", 1), "cpuspertask": meta.get("cpuspertask", ""),
            "nx": meta.get("nx", ""), "ny": meta.get("ny", ""), "nz": meta.get("nz", ""),
            "maxit": meta.get("maxit", ""), "nzlocal": meta.get("nzlocal", ""),
            "numa": meta.get("numa", ""),
            "nruns": nruns,
            "mean_time": mean_t, "median_time": med_t, "min_time": min_t,
            "max_time": max_t, "std_time": std_t,
            "mean_mflops": mean_mf, "std_mflops": std_mf,
        })
    write_csv(base / "summary.csv", list(rows[0].keys()), rows)
    print(f"  Wrote {base / 'summary.csv'} ({len(rows)} configs)")


def collect_matmul_baseline(results_dir):
    base = Path(results_dir) / "fox_mt-dgemm_libomp"
    data = scan_baseline(base / "raw", baseline_matmul)
    if not data:
        print("  No data for matmul baseline (mt-dgemm)")
        return
    rows = []
    for exp_name, (meta, runs) in sorted(data.items()):
        nruns, mean_t, med_t, min_t, max_t, std_t = compute_stats([r["duration_s"] for r in runs])
        _, mean_g, _, _, _, std_g = compute_stats([r["gflops"] for r in runs])
        rows.append({
            "experiment": exp_name,
            "ppn": meta.get("ppn", 1), "cpuspertask": meta.get("cpuspertask", ""),
            "nsize": meta.get("nsize", ""), "msize": meta.get("msize", ""),
            "its": meta.get("its", ""), "numa": meta.get("numa", ""),
            "nruns": nruns,
            "mean_time": mean_t, "median_time": med_t, "min_time": min_t,
            "max_time": max_t, "std_time": std_t,
            "mean_gflops": mean_g, "std_gflops": std_g,
        })
    write_csv(base / "summary.csv", list(rows[0].keys()), rows)
    print(f"  Wrote {base / 'summary.csv'} ({len(rows)} configs)")


def collect_multisaxpy_baseline(results_dir):
    base = Path(results_dir) / "fox_multisaxpy_omp"
    data = scan_baseline(base / "raw", baseline_multisaxpy)
    if not data:
        print("  No data for multisaxpy baseline (omp)")
        return
    rows = []
    for exp_name, (meta, runs) in sorted(data.items()):
        nruns, mean_t, med_t, min_t, max_t, std_t = compute_stats([r["duration_s"] for r in runs])
        _, mean_g, _, _, _, std_g = compute_stats([r["gflops"] for r in runs])
        rows.append({
            "experiment": exp_name,
            "ppn": meta.get("ppn", 1), "cpuspertask": meta.get("cpuspertask", ""),
            "nsize": meta.get("nsize", ""), "iters": meta.get("iters", ""),
            "numa": meta.get("numa", ""), "procbind": meta.get("procbind", ""),
            "nruns": nruns,
            "mean_time": mean_t, "median_time": med_t, "min_time": min_t,
            "max_time": max_t, "std_time": std_t,
            "mean_gflops": mean_g, "std_gflops": std_g,
        })
    write_csv(base / "summary.csv", list(rows[0].keys()), rows)
    print(f"  Wrote {base / 'summary.csv'} ({len(rows)} configs)")


# ---------------------------------------------------------------------------
# CSV writer
# ---------------------------------------------------------------------------

def write_csv(path, header, rows):
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=header)
        writer.writeheader()
        writer.writerows(rows)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Collect reproduced benchmark results into CSV")
    parser.add_argument("--results-dir", default=RESULTS_DIR, help="Path to reproduced_results/")
    args = parser.parse_args()
    rd = args.results_dir

    print("Collecting results from:", rd)

    print("\n=== Cholesky ===")
    collect_cholesky("orig", "results_final_3bs", rd)
    collect_cholesky("init", "results_final_3bs_init_tasks", rd)
    collect_cholesky("tg", "results_final_3bs", rd)

    print("\n=== Heat ===")
    collect_heat("orig", "results_final_3bs", rd)
    collect_heat("init", "results_final_3bs_init_tasks", rd)
    collect_heat("tg", "results_final_3bs", rd)

    print("\n=== HPCCG ===")
    collect_hpccg("orig", "results_final_3bs", rd)
    collect_hpccg("init", "results_final_3bs_init_tasks", rd)
    collect_hpccg("tg", "results_final_3bs", rd)

    print("\n=== Matmul ===")
    collect_matmul("orig", "results_final_3bs", rd)
    collect_matmul("init", "results_final_3bs_init_tasks", rd)
    collect_matmul("tg", "results_final_3bs", rd)

    print("\n=== Multisaxpy ===")
    collect_multisaxpy("init", "results_final_3bs_init_tasks", rd)
    collect_multisaxpy("tg", "results_final_3bs", rd)

    print("\n=== Baseline (external OpenMP/MPI) ===")
    collect_cholesky_baseline(rd)
    collect_heat_baseline(rd)
    collect_hpccg_baseline(rd)
    collect_matmul_baseline(rd)
    collect_multisaxpy_baseline(rd)

    print("\nDone.")


if __name__ == "__main__":
    main()
