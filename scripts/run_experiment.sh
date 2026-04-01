#!/bin/bash
#
# run_experiment.sh — Automated sweep for PFC-free RoCE study (ConnectX-7)
#
# Run this on the CLIENT machine.
# bench_server must already be running on the SERVER machine.
#
# Usage:
#   ./scripts/run_experiment.sh <server-ip> <iface> [client-ip]
#
# Example:
#   ./scripts/run_experiment.sh 192.168.1.2 eth0
#   ./scripts/run_experiment.sh 192.168.1.2 eth0 192.168.1.1
#

set -euo pipefail

# ── Arguments ────────────────────────────────────────────────────────────────
if [ $# -lt 2 ]; then
    echo "Usage: $0 <server-ip> <iface> [client-ip]"
    echo "Example: $0 192.168.1.2 eth0"
    exit 1
fi

SERVER_IP=$1
IFACE=$2
CLIENT_IP=${3:-""}

# ── Configuration ─────────────────────────────────────────────────────────────
BENCH_CLIENT="./bench_client"
RESULTS_DIR="results"
ITERATIONS=10000

# Message sizes in bytes: 64B, 256B, 1KB, 4KB, 64KB, 256KB, 1MB, 4MB
MSG_SIZES=(64 256 1024 4096 65536 262144 1048576 4194304)

# Loss rates in percent (0 = no netem, clean baseline)
LOSS_RATES=(0 0.01 0.05 0.1 0.5 1.0 2.0 5.0)

# ── Setup ─────────────────────────────────────────────────────────────────────
mkdir -p "$RESULTS_DIR"
CSV="$RESULTS_DIR/results.csv"

if [ ! -f "$BENCH_CLIENT" ]; then
    echo "ERROR: $BENCH_CLIENT not found. Run 'make' first."
    exit 1
fi

# CSV header
echo "msg_size_bytes,loss_pct,throughput_gbps,p50_us,p99_us,p999_us,p9999_us,cnp_count,ack_timeouts,rx_errors" > "$CSV"

echo "========================================"
echo "  PFC-free RoCE Benchmark — ConnectX-7"
echo "========================================"
echo "Server:     $SERVER_IP"
echo "Interface:  $IFACE"
echo "Iterations: $ITERATIONS"
echo "Results:    $CSV"
echo ""

# ── Cleanup function ──────────────────────────────────────────────────────────
cleanup_netem() {
    sudo tc qdisc del dev "$IFACE" root 2>/dev/null || true
}
trap cleanup_netem EXIT

# ── Helper: set loss rate ─────────────────────────────────────────────────────
set_loss() {
    local loss=$1
    cleanup_netem
    if [ "$loss" != "0" ]; then
        sudo tc qdisc add dev "$IFACE" root netem loss "${loss}%"
    fi
}

# ── Helper: verify netem is set ───────────────────────────────────────────────
show_netem() {
    local loss=$1
    if [ "$loss" = "0" ]; then
        echo "  [netem] disabled (clean baseline)"
    else
        echo "  [netem] loss=${loss}%  ($(tc qdisc show dev "$IFACE" | grep netem || echo 'check tc'))"
    fi
}

# ── Main sweep ────────────────────────────────────────────────────────────────
total_runs=$(( ${#LOSS_RATES[@]} * ${#MSG_SIZES[@]} ))
run=0

for loss in "${LOSS_RATES[@]}"; do
    echo ""
    echo "━━━  Loss rate: ${loss}%  ━━━━━━━━━━━━━━━━━━━━━━━━━"
    set_loss "$loss"
    show_netem "$loss"
    echo ""

    for msg_size in "${MSG_SIZES[@]}"; do
        run=$(( run + 1 ))

        # Human-readable size label
        if   [ "$msg_size" -ge 1048576 ]; then label="$((msg_size/1048576))MB"
        elif [ "$msg_size" -ge 1024 ];    then label="$((msg_size/1024))KB"
        else                                   label="${msg_size}B"
        fi

        printf "  [%2d/%2d] msg=%-8s ... " "$run" "$total_runs" "$label"

        # Run benchmark — stdout = CSV fields, stderr = human log
        if [ -n "$CLIENT_IP" ]; then
            result=$("$BENCH_CLIENT" "$SERVER_IP" "$msg_size" "$ITERATIONS" "$CLIENT_IP" 2>/dev/null)
        else
            result=$("$BENCH_CLIENT" "$SERVER_IP" "$msg_size" "$ITERATIONS" 2>/dev/null)
        fi

        # Append to CSV
        echo "${msg_size},${loss},${result}" >> "$CSV"

        # Print summary from the CSV fields
        throughput=$(echo "$result" | cut -d',' -f1)
        p99=$(echo "$result" | cut -d',' -f3)
        printf "BW=%-8s Gbps  p99=%-8s us\n" "$throughput" "$p99"

        # Small pause between runs to let server reset
        sleep 0.5
    done
done

echo ""
echo "========================================"
echo "  All $total_runs runs complete"
echo "  Results: $CSV"
echo "========================================"
echo ""
echo "Next step: python3 scripts/plot_results.py"
