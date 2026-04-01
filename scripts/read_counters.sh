#!/bin/bash
#
# read_counters.sh — Snapshot NIC counters for ConnectX-7
#
# Usage:
#   ./scripts/read_counters.sh [ib-device] [port]
#
# Example:
#   ./scripts/read_counters.sh mlx5_0 1
#

DEV=${1:-mlx5_0}
PORT=${2:-1}
PORT_PATH="/sys/class/infiniband/$DEV/ports/$PORT"

if [ ! -d "$PORT_PATH" ]; then
    echo "ERROR: $PORT_PATH not found."
    echo "Available devices:"
    ibv_devices
    exit 1
fi

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  NIC Counters: $DEV port $PORT"
echo "  $(date)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

echo ""
echo "── Port Counters ────────────────────────"
for counter in port_rcv_packets port_xmit_packets \
               port_rcv_data port_xmit_data \
               port_rcv_errors port_xmit_discards \
               local_ack_timeout_err; do
    path="$PORT_PATH/counters/$counter"
    val=$(cat "$path" 2>/dev/null || echo "n/a")
    printf "  %-30s %s\n" "$counter" "$val"
done

echo ""
echo "── HW Counters (DCQCN / ECN) ────────────"
HW="$PORT_PATH/hw_counters"
if [ -d "$HW" ]; then
    for counter in np_cnp_sent rp_cnp_handled \
                   np_ecn_marked_roce_packets \
                   out_of_buffer out_of_sequence \
                   packet_seq_err req_cqe_error; do
        path="$HW/$counter"
        val=$(cat "$path" 2>/dev/null || echo "n/a")
        printf "  %-30s %s\n" "$counter" "$val"
    done
else
    echo "  hw_counters not available"
fi

echo ""
echo "── Link State ───────────────────────────"
printf "  %-30s %s\n" "state" "$(cat $PORT_PATH/state 2>/dev/null || echo n/a)"
printf "  %-30s %s\n" "rate"  "$(cat $PORT_PATH/rate  2>/dev/null || echo n/a)"
printf "  %-30s %s\n" "lid"   "$(cat $PORT_PATH/lid   2>/dev/null || echo n/a)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
