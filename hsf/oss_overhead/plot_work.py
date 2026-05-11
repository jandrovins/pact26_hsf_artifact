#!/usr/bin/env python3
import sys
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt

csv = sys.argv[1] if len(sys.argv) > 1 else "work_sweep.csv"
df = pd.read_csv(csv)

fig, ax = plt.subplots()

ax.plot(df["niters"], df["mean_us"],  marker="o", label="Mean")
ax.plot(df["niters"], df["p50_us"],   marker="s", label="Median (p50)")

def niters_label(ni):
    if ni >= 1_000_000:
        return f"{ni/1e6:.1f}M"
    if ni >= 1_000:
        return f"{ni/1e3:.0f}k"
    return str(ni)

for _, row in df.iterrows():
    ni  = int(row["niters"])
    t   = row["mean_us"]
    lbl_ni = niters_label(ni)
    lbl_t  = f"{t:.1f}µs"
    # niters label — lower right
    ax.annotate(lbl_ni, (row["niters"], t),
                textcoords="offset points", xytext=(6, -10), fontsize=7,
                ha="left", va="top")
    # time label — upper left
    ax.annotate(lbl_t, (row["niters"], t),
                textcoords="offset points", xytext=(-6, 10), fontsize=7,
                ha="right", va="bottom")

# Power-law fit in log-log space.  A slope ≈ 1 and R² ≈ 1 confirm that the
# per-task work is strictly linear in niters, which is the calibration claim
# we need.  This is a much stronger reference than anchoring a line through
# the smallest, noisiest data point.
log_n = np.log(np.asarray(df["niters"], dtype=float))
log_t = np.log(np.asarray(df["mean_us"], dtype=float))
slope, log_intercept = np.polyfit(log_n, log_t, 1)
intercept = float(np.exp(log_intercept))
residuals = log_t - (slope * log_n + log_intercept)
r2 = 1 - float(np.sum(residuals ** 2) / np.sum((log_t - log_t.mean()) ** 2))
x = np.asarray(df["niters"], dtype=float)
ax.plot(x, intercept * x ** slope, linestyle="--", color="gray",
        label=f"power-law fit: slope={slope:.3f}, $R^2$={r2:.4f}")

ax.set_xscale("log")
ax.set_yscale("log")
ax.set_xlabel("niters")
ax.set_ylabel("Time (µs)")
ax.set_title("Work granularity vs execution time")
ax.legend()
ax.grid(True, which="both", linestyle=":", alpha=0.5)

out = csv.replace(".csv", ".pdf")
fig.savefig(out)
print(f"Saved {out}")
