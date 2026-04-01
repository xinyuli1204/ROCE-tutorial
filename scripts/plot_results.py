#!/usr/bin/env python3
"""
plot_results.py — PCIe Topology Tax figures for IEEE Access paper

Usage:
    python3 scripts/plot_results.py [results/results.csv]

Expects CSV columns:
    config, msg_size_bytes, throughput_gbps,
    p50_us, p99_us, p999_us, p9999_us,
    cnp_count, ack_timeouts, rx_errors

Produces:
    results/fig1_throughput_vs_msgsize.pdf   — throughput: cpu vs gpu0 vs gpu2
    results/fig2_p99_vs_msgsize.pdf          — p99 latency: the topology tax
    results/fig3_tail_latency_4kb.pdf        — p50/p99/p999/p9999 at 4KB
    results/fig4_nic_counters.pdf            — ack_timeouts + cnp_count
    results/fig5_topology_tax_pct.pdf        — % penalty of gpu0 and gpu2 vs cpu
"""

import sys
import os
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np

# ── Style — IEEE paper ────────────────────────────────────────────────────────
plt.rcParams.update({
    "font.family":      "serif",
    "font.size":        11,
    "axes.labelsize":   12,
    "legend.fontsize":  10,
    "xtick.labelsize":  10,
    "ytick.labelsize":  10,
    "figure.dpi":       150,
    "lines.linewidth":  1.8,
    "lines.markersize": 6,
})

# Config display names, colors, markers
CONFIG_STYLE = {
    "cpu":  {"label": "CPU memory (baseline)",      "color": "#1f77b4", "marker": "o", "ls": "-"},
    "gpu0": {"label": "GPU0 — NODE (same NUMA)",    "color": "#2ca02c", "marker": "s", "ls": "--"},
    "gpu2": {"label": "GPU2 — SYS (cross NUMA)",    "color": "#d62728", "marker": "^", "ls": ":"},
}
CONFIG_ORDER = ["cpu", "gpu0", "gpu2"]

# ── Load data ─────────────────────────────────────────────────────────────────
csv_path = sys.argv[1] if len(sys.argv) > 1 else "results/results.csv"
if not os.path.exists(csv_path):
    print(f"ERROR: {csv_path} not found. Run run_experiment.sh first.")
    sys.exit(1)

df = pd.read_csv(csv_path)
print(f"Loaded {len(df)} rows from {csv_path}")
print(df.groupby("config").size().to_string())

out_dir = os.path.dirname(csv_path) or "results"
os.makedirs(out_dir, exist_ok=True)

msg_sizes = sorted(df["msg_size_bytes"].unique())
configs   = [c for c in CONFIG_ORDER if c in df["config"].unique()]

def size_label(b):
    if b >= 1048576: return f"{b//1048576}MB"
    if b >= 1024:    return f"{b//1024}KB"
    return f"{b}B"

msg_labels = [size_label(s) for s in msg_sizes]

# ── Figure 1: Throughput vs Message Size ─────────────────────────────────────
fig, ax = plt.subplots(figsize=(6, 4))
for cfg in configs:
    s = CONFIG_STYLE[cfg]
    d = df[df["config"] == cfg].sort_values("msg_size_bytes")
    ax.plot(d["msg_size_bytes"], d["throughput_gbps"],
            label=s["label"], color=s["color"],
            marker=s["marker"], linestyle=s["ls"])

ax.set_xscale("log")
ax.set_xlabel("Message Size")
ax.set_ylabel("Throughput (Gbps)")
ax.set_title("RDMA WRITE Throughput vs Message Size\n(PFC Disabled, ConnectX-7 → L4)")
ax.legend(loc="upper left", fontsize=9)
ax.grid(True, which="both", alpha=0.3, linestyle="--")
ax.set_xticks(msg_sizes)
ax.set_xticklabels(msg_labels, rotation=30)
plt.tight_layout()
out = f"{out_dir}/fig1_throughput_vs_msgsize.pdf"
plt.savefig(out, bbox_inches="tight")
print(f"Saved: {out}")
plt.close()

# ── Figure 2: p99 Latency vs Message Size — the topology tax ──────────────────
fig, ax = plt.subplots(figsize=(6, 4))
for cfg in configs:
    s = CONFIG_STYLE[cfg]
    d = df[df["config"] == cfg].sort_values("msg_size_bytes")
    ax.plot(d["msg_size_bytes"], d["p99_us"],
            label=s["label"], color=s["color"],
            marker=s["marker"], linestyle=s["ls"])

ax.set_xscale("log")
ax.set_yscale("log")
ax.set_xlabel("Message Size")
ax.set_ylabel("p99 Latency (μs)")
ax.set_title("p99 Tail Latency vs Message Size\n(PCIe Topology Tax, ConnectX-7 → L4)")
ax.legend(loc="upper left", fontsize=9)
ax.grid(True, which="both", alpha=0.3, linestyle="--")
ax.set_xticks(msg_sizes)
ax.set_xticklabels(msg_labels, rotation=30)
plt.tight_layout()
out = f"{out_dir}/fig2_p99_vs_msgsize.pdf"
plt.savefig(out, bbox_inches="tight")
print(f"Saved: {out}")
plt.close()

# ── Figure 3: Tail latency distribution at 4KB (key inference message size) ──
focus_size = 4096
if focus_size not in msg_sizes:
    focus_size = msg_sizes[0]

fig, axes = plt.subplots(1, len(configs), figsize=(4 * len(configs), 4), sharey=True)
if len(configs) == 1:
    axes = [axes]

percentiles = ["p50_us", "p99_us", "p999_us", "p9999_us"]
pct_labels  = ["p50", "p99", "p999", "p9999"]
x = np.arange(len(percentiles))

for ax, cfg in zip(axes, configs):
    s  = CONFIG_STYLE[cfg]
    d  = df[(df["config"] == cfg) & (df["msg_size_bytes"] == focus_size)]
    if d.empty:
        ax.set_title(f"{cfg}\n(no data)")
        continue
    vals = [float(d[p].iloc[0]) for p in percentiles]
    bars = ax.bar(x, vals, color=s["color"], alpha=0.8)
    ax.set_xticks(x)
    ax.set_xticklabels(pct_labels)
    ax.set_title(s["label"], fontsize=9)
    ax.set_ylabel("Latency (μs)" if cfg == configs[0] else "")
    ax.grid(axis="y", alpha=0.3)
    for bar, val in zip(bars, vals):
        ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height() * 1.02,
                f"{val:.1f}", ha="center", va="bottom", fontsize=8)

fig.suptitle(f"Tail Latency Distribution at {size_label(focus_size)}\n"
             f"(RDMA WRITE, PFC Disabled, ConnectX-7 → L4)", fontsize=11)
plt.tight_layout()
out = f"{out_dir}/fig3_tail_latency_{size_label(focus_size)}.pdf"
plt.savefig(out, bbox_inches="tight")
print(f"Saved: {out}")
plt.close()

# ── Figure 4: NIC counters (ack_timeouts + cnp_count) at 4KB ─────────────────
fig, axes = plt.subplots(1, 2, figsize=(10, 4))
bar_x = np.arange(len(configs))
bar_w = 0.5

for ax_idx, (col, ylabel, title) in enumerate([
    ("ack_timeouts", "ACK Timeout Count",  "Hardware Retransmissions (ACK Timeouts)"),
    ("cnp_count",    "CNP Handled Count",  "Congestion Notifications (CNP Handled)"),
]):
    ax = axes[ax_idx]
    vals   = []
    colors = []
    for cfg in configs:
        d = df[(df["config"] == cfg) & (df["msg_size_bytes"] == focus_size)]
        vals.append(float(d[col].iloc[0]) if not d.empty else 0)
        colors.append(CONFIG_STYLE[cfg]["color"])

    bars = ax.bar(bar_x, vals, width=bar_w, color=colors, alpha=0.8)
    ax.set_xticks(bar_x)
    ax.set_xticklabels([CONFIG_STYLE[c]["label"] for c in configs],
                       rotation=15, ha="right", fontsize=8)
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.grid(axis="y", alpha=0.3)

fig.suptitle(f"NIC Counters at {size_label(focus_size)} "
             f"(ConnectX-7 → L4, PFC Disabled)", fontsize=11)
plt.tight_layout()
out = f"{out_dir}/fig4_nic_counters.pdf"
plt.savefig(out, bbox_inches="tight")
print(f"Saved: {out}")
plt.close()

# ── Figure 5: Topology tax % penalty vs CPU baseline ─────────────────────────
if "cpu" in configs and len(configs) > 1:
    fig, axes = plt.subplots(1, 2, figsize=(10, 4))

    for ax, (metric, ylabel) in zip(axes, [
        ("throughput_gbps", "Throughput change vs CPU (%)"),
        ("p99_us",          "p99 latency overhead vs CPU (%)")
    ]):
        cpu_d = df[df["config"] == "cpu"].sort_values("msg_size_bytes")

        for cfg in [c for c in configs if c != "cpu"]:
            s   = CONFIG_STYLE[cfg]
            cfg_d = df[df["config"] == cfg].sort_values("msg_size_bytes")

            pct = []
            for sz in msg_sizes:
                cpu_val = float(cpu_d[cpu_d["msg_size_bytes"] == sz][metric].iloc[0]) \
                          if not cpu_d[cpu_d["msg_size_bytes"] == sz].empty else None
                cfg_val = float(cfg_d[cfg_d["msg_size_bytes"] == sz][metric].iloc[0]) \
                          if not cfg_d[cfg_d["msg_size_bytes"] == sz].empty else None
                if cpu_val and cfg_val and cpu_val != 0:
                    pct.append((cfg_val - cpu_val) / cpu_val * 100)
                else:
                    pct.append(0.0)

            ax.plot(msg_sizes, pct,
                    label=s["label"], color=s["color"],
                    marker=s["marker"], linestyle=s["ls"])

        ax.axhline(0, color="gray", linewidth=1, linestyle="--")
        ax.set_xscale("log")
        ax.set_xlabel("Message Size")
        ax.set_ylabel(ylabel)
        ax.legend(fontsize=9)
        ax.grid(True, which="both", alpha=0.3, linestyle="--")
        ax.set_xticks(msg_sizes)
        ax.set_xticklabels(msg_labels, rotation=30)

    fig.suptitle("PCIe Topology Tax: % overhead vs CPU baseline\n"
                 "(ConnectX-7 → L4, PFC Disabled)", fontsize=11)
    plt.tight_layout()
    out = f"{out_dir}/fig5_topology_tax_pct.pdf"
    plt.savefig(out, bbox_inches="tight")
    print(f"Saved: {out}")
    plt.close()

# ── Summary table ─────────────────────────────────────────────────────────────
print(f"\n── Summary at {size_label(focus_size)} ──────────────────────────────")
cols = ["config", "throughput_gbps", "p50_us", "p99_us", "p999_us", "ack_timeouts"]
t = df[df["msg_size_bytes"] == focus_size][cols].sort_values("config")
print(t.to_string(index=False))
print("\nAll figures saved to:", out_dir)
