#!/bin/bash
#
# run_experiment.sh — PCIe Topology Tax Experiment (ConnectX-7 + NVIDIA L4)
#
# Measures RDMA WRITE latency into 3 buffer targets:
#   cpu   — server CPU memory (baseline)
#   gpu0  — GPU0, NODE distance to NIC (same NUMA, optimal)
#   gpu2  — GPU2, SYS  distance to NIC (cross NUMA, suboptimal)
#
# Run this on the CLIENT machine.
# Start the correct bench_server on the server before each run.
#
# Usage:
#   ./scripts/run_experiment.sh <server-ip> <client-ip> <config>
#
#   config = cpu | gpu0 | gpu2
#
# Example (run 3 times, once per config):
#   ./scripts/run_experiment.sh 10.0.2.2 10.0.2.1 cpu
#   ./scripts/run_experiment.sh 10.0.2.2 10.0.2.1 gpu0
#   ./scripts/run_experiment.sh 10.0.2.2 10.0.2.1 gpu2
#
# Server commands (run on server before each client run):
#   cpu:  ./bench_server 10.0.2.2
#   gpu0: ./bench_server_gpu --gpu 0 10.0.2.2
#   gpu2: ./bench_server_gpu --gpu 2 10.0.2.2
#

set -euo pipefail

# ── Arguments ─────────────────────────────────────────────────────────────────
if [ $# -lt 3 ]; then
    echo "Usage: $0 <server-ip> <client-ip> <config>"
    echo "  config: cpu | gpu0 | gpu2"
    echo ""
    echo "Example:"
    echo "  $0 10.0.2.2 10.0.2.1 cpu"
    exit 1
fi

SERVER_IP=$1
CLIENT_IP=$2
CONFIG=$3

if [[ "$CONFIG" != "cpu" && "$CONFIG" != "gpu0" && "$CONFIG" != "gpu2" ]]; then
    echo "ERROR: config must be cpu, gpu0, or gpu2"
    exit 1
fi

# ── Configuration ─────────────────────────────────────────────────────────────
BENCH_CLIENT="./bench_client"
RESULTS_DIR="results"
ITERATIONS=10000

# Message sizes: 64B → 4MB
MSG_SIZES=(64 256 1024 4096 65536 262144 1048576 4194304)

# ── Setup ─────────────────────────────────────────────────────────────────────
mkdir -p "$RESULTS_DIR"
CSV="$RESULTS_DIR/results.csv"

if [ ! -f "$BENCH_CLIENT" ]; then
    echo "ERROR: $BENCH_CLIENT not found. Run 'make bench_client' first."
    exit 1
fi

# Create CSV with header if it doesn't exist yet
if [ ! -f "$CSV" ]; then
    echo "config,msg_size_bytes,throughput_gbps,p50_us,p99_us,p999_us,p9999_us,cnp_count,ack_timeouts,rx_errors" > "$CSV"
    echo "Created: $CSV"
fi

# ── Remind user which server to start ─────────────────────────────────────────
echo "========================================"
echo "  PCIe Topology Tax Experiment"
echo "  Config: $CONFIG"
echo "========================================"
echo ""
case "$CONFIG" in
    cpu)
        echo "  Server should be running:"
        echo "    ./bench_server 10.0.2.2"
        echo "  (CPU memory — baseline)"
        ;;
    gpu0)
        echo "  Server should be running:"
        echo "    ./bench_server_gpu --gpu 0 10.0.2.2"
        echo "  (GPU0 — NODE distance, optimal path)"
        ;;
    gpu2)
        echo "  Server should be running:"
        echo "    ./bench_server_gpu --gpu 2 10.0.2.2"
        echo "  (GPU2 — SYS distance, cross-NUMA path)"
        ;;
esac
echo ""
read -rp "  Press Enter when server is ready..." _

echo ""
echo "  Server:     $SERVER_IP"
echo "  Client:     $CLIENT_IP"
echo "  Iterations: $ITERATIONS"
echo "  Results:    $CSV"
echo ""

# ── Main sweep ────────────────────────────────────────────────────────────────
total_runs=${#MSG_SIZES[@]}
run=0

for msg_size in "${MSG_SIZES[@]}"; do
    run=$(( run + 1 ))

    if   [ "$msg_size" -ge 1048576 ]; then label="$((msg_size/1048576))MB"
    elif [ "$msg_size" -ge 1024 ];    then label="$((msg_size/1024))KB"
    else                                   label="${msg_size}B"
    fi

    printf "  [%d/%d] config=%-5s msg=%-8s ... " \
           "$run" "$total_runs" "$CONFIG" "$label"

    result=$("$BENCH_CLIENT" "$SERVER_IP" "$msg_size" "$ITERATIONS" \
                             "$CLIENT_IP" 2>/dev/null)

    echo "${CONFIG},${msg_size},${result}" >> "$CSV"

    throughput=$(echo "$result" | cut -d',' -f1)
    p99=$(echo "$result"        | cut -d',' -f3)
    printf "BW=%-10s Gbps  p99=%-8s us\n" "$throughput" "$p99"

    sleep 0.3
done

echo ""
echo "========================================"
echo "  Config '$CONFIG' complete ($total_runs runs)"
echo "  Results appended to: $CSV"
echo "========================================"
echo ""

# Check if all 3 configs are done
cpu_rows=$(grep -c "^cpu,"   "$CSV" 2>/dev/null || echo 0)
gpu0_rows=$(grep -c "^gpu0," "$CSV" 2>/dev/null || echo 0)
gpu2_rows=$(grep -c "^gpu2," "$CSV" 2>/dev/null || echo 0)

echo "  Progress:"
printf "    cpu:  %2d/%d rows\n" "$cpu_rows"  "$total_runs"
printf "    gpu0: %2d/%d rows\n" "$gpu0_rows" "$total_runs"
printf "    gpu2: %2d/%d rows\n" "$gpu2_rows" "$total_runs"
echo ""

if [ "$cpu_rows" -ge "$total_runs" ] && \
   [ "$gpu0_rows" -ge "$total_runs" ] && \
   [ "$gpu2_rows" -ge "$total_runs" ]; then
    echo "  All 3 configs done! Run:"
    echo "    python3 scripts/plot_results.py"
else
    echo "  Next: run the other configs on the server, then re-run this script."
fi
