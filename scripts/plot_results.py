#!/usr/bin/env python3
"""
plot_results.py — Generate paper figures from experiment CSV

Usage:
    python3 scripts/plot_results.py [results/results.csv]

Produces:
    results/fig1_throughput_vs_loss.pdf
    results/fig2_latency_vs_loss.pdf
    results/fig3_throughput_vs_msgsize.pdf
    results/fig4_nic_counters.pdf
"""

import sys
import os
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np

# ── Style — IEEE paper friendly ───────────────────────────────────────────────
plt.rcParams.update({
    "font.family":     "serif",
    "font.size":       11,
    "axes.labelsize":  12,
    "legend.fontsize": 10,
    "xtick.labelsize": 10,
    "ytick.labelsize": 10,
    "figure.dpi":      150,
    "lines.linewidth": 1.8,
    "lines.markersize": 6,
})

MARKERS = ['o', 's', '^', 'D', 'v', 'P', 'X']
COLORS  = ['#1f77b4', '#ff7f0e', '#2ca02c', '#d62728',
           '#9467bd', '#8c564b', '#e377c2']

# ── Load data ─────────────────────────────────────────────────────────────────
csv_path = sys.argv[1] if len(sys.argv) > 1 else "results/results.csv"
if not os.path.exists(csv_path):
    print(f"ERROR: {csv_path} not found. Run run_experiment.sh first.")
    sys.exit(1)

df = pd.read_csv(csv_path)
print(f"Loaded {len(df)} rows from {csv_path}")
print(df.head())

out_dir = os.path.dirname(csv_path)
os.makedirs(out_dir, exist_ok=True)

# Replace loss=0 with a small value for log-scale x-axis
ZERO_LOSS_LABEL = 0.001
df_plot = df.copy()
df_plot["loss_x"] = df_plot["loss_pct"].replace(0, ZERO_LOSS_LABEL)

loss_rates = sorted(df["loss_pct"].unique())
msg_sizes  = sorted(df["msg_size_bytes"].unique())

def size_label(b):
    if b >= 1048576: return f"{b//1048576}MB"
    if b >= 1024:    return f"{b//1024}KB"
    return f"{b}B"

# ── Figure 1: Throughput vs Loss Rate ────────────────────────────────────────
key_sizes = [s for s in msg_sizes if s in [4096, 65536, 1048576, 4194304]]

fig, ax = plt.subplots(figsize=(6, 4))
for i, sz in enumerate(key_sizes):
    d = df_plot[df_plot["msg_size_bytes"] == sz].sort_values("loss_x")
    ax.plot(d["loss_x"], d["throughput_gbps"],
            marker=MARKERS[i], color=COLORS[i],
            label=size_label(sz))

ax.set_xscale("log")
ax.set_xlabel("Packet Loss Rate (%)")
ax.set_ylabel("Throughput (Gbps)")
ax.set_title("RDMA WRITE Throughput vs Loss Rate\n(PFC Disabled, ConnectX-7)")
ax.legend(title="Msg Size", loc="upper right")
ax.grid(True, which="both", alpha=0.3, linestyle="--")

# Custom x-axis labels
xticks = [ZERO_LOSS_LABEL, 0.01, 0.05, 0.1, 0.5, 1.0, 2.0, 5.0]
xlabels = ["0", "0.01", "0.05", "0.1", "0.5", "1", "2", "5"]
ax.set_xticks(xticks)
ax.set_xticklabels(xlabels)

plt.tight_layout()
out = f"{out_dir}/fig1_throughput_vs_loss.pdf"
plt.savefig(out, bbox_inches="tight")
print(f"Saved: {out}")
plt.close()

# ── Figure 2: Tail Latency vs Loss Rate ──────────────────────────────────────
focus_size = 4096  # 4KB — representative for latency study
d = df_plot[df_plot["msg_size_bytes"] == focus_size].sort_values("loss_x")

fig, ax = plt.subplots(figsize=(6, 4))
ax.plot(d["loss_x"], d["p50_us"],   marker="o", color=COLORS[0], label="p50")
ax.plot(d["loss_x"], d["p99_us"],   marker="s", color=COLORS[1], label="p99")
ax.plot(d["loss_x"], d["p999_us"],  marker="^", color=COLORS[2], label="p999")
ax.plot(d["loss_x"], d["p9999_us"], marker="D", color=COLORS[3], label="p9999")

ax.set_xscale("log")
ax.set_yscale("log")
ax.set_xlabel("Packet Loss Rate (%)")
ax.set_ylabel("Latency (μs)")
ax.set_title(f"RDMA WRITE Tail Latency vs Loss Rate\n({size_label(focus_size)} msg, PFC Disabled, ConnectX-7)")
ax.legend(loc="upper left")
ax.grid(True, which="both", alpha=0.3, linestyle="--")
ax.set_xticks(xticks)
ax.set_xticklabels(xlabels)

plt.tight_layout()
out = f"{out_dir}/fig2_latency_vs_loss.pdf"
plt.savefig(out, bbox_inches="tight")
print(f"Saved: {out}")
plt.close()

# ── Figure 3: Throughput vs Message Size (at different loss rates) ────────────
key_losses = [l for l in loss_rates if l in [0, 0.1, 1.0, 5.0]]

fig, ax = plt.subplots(figsize=(6, 4))
for i, loss in enumerate(key_losses):
    d = df[df["loss_pct"] == loss].sort_values("msg_size_bytes")
    label = "0% (baseline)" if loss == 0 else f"{loss}% loss"
    ax.plot(d["msg_size_bytes"], d["throughput_gbps"],
            marker=MARKERS[i], color=COLORS[i], label=label)

ax.set_xscale("log")
ax.set_xlabel("Message Size (bytes)")
ax.set_ylabel("Throughput (Gbps)")
ax.set_title("RDMA WRITE Throughput vs Message Size\n(PFC Disabled, ConnectX-7)")
ax.legend(title="Loss Rate", loc="upper left")
ax.grid(True, which="both", alpha=0.3, linestyle="--")

# Custom x-axis labels
msg_ticks  = msg_sizes
msg_labels = [size_label(s) for s in msg_sizes]
ax.set_xticks(msg_ticks)
ax.set_xticklabels(msg_labels, rotation=30)

plt.tight_layout()
out = f"{out_dir}/fig3_throughput_vs_msgsize.pdf"
plt.savefig(out, bbox_inches="tight")
print(f"Saved: {out}")
plt.close()

# ── Figure 4: NIC Counters vs Loss Rate ──────────────────────────────────────
# Shows hardware retransmissions increasing with loss — key paper evidence
focus_size = 4096
d = df_plot[df_plot["msg_size_bytes"] == focus_size].sort_values("loss_x")

fig, axes = plt.subplots(1, 2, figsize=(10, 4))

axes[0].bar(range(len(d)), d["ack_timeouts"],
            color=COLORS[0], alpha=0.8)
axes[0].set_xticks(range(len(d)))
axes[0].set_xticklabels([str(l) for l in d["loss_pct"]], rotation=30)
axes[0].set_xlabel("Packet Loss Rate (%)")
axes[0].set_ylabel("ACK Timeout Count")
axes[0].set_title("Hardware Retransmissions\n(ACK Timeouts)")
axes[0].grid(axis="y", alpha=0.3)

axes[1].bar(range(len(d)), d["cnp_count"],
            color=COLORS[1], alpha=0.8)
axes[1].set_xticks(range(len(d)))
axes[1].set_xticklabels([str(l) for l in d["loss_pct"]], rotation=30)
axes[1].set_xlabel("Packet Loss Rate (%)")
axes[1].set_ylabel("CNP Handled Count")
axes[1].set_title("Congestion Notifications\n(CNP Handled)")
axes[1].grid(axis="y", alpha=0.3)

fig.suptitle(f"NIC Counters vs Loss Rate  ({size_label(focus_size)} msg, PFC Disabled, ConnectX-7)",
             fontsize=12)
plt.tight_layout()
out = f"{out_dir}/fig4_nic_counters.pdf"
plt.savefig(out, bbox_inches="tight")
print(f"Saved: {out}")
plt.close()

# ── Summary table ─────────────────────────────────────────────────────────────
print("\n── Summary: 4KB msg, Throughput & p99 latency ──────────────────")
t = df[df["msg_size_bytes"] == 4096][
    ["loss_pct", "throughput_gbps", "p50_us", "p99_us", "p999_us", "ack_timeouts"]
].sort_values("loss_pct")
print(t.to_string(index=False))
print("\nAll figures saved to:", out_dir)
