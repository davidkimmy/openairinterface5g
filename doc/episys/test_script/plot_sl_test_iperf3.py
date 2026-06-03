#!/usr/bin/env python3
# Usage: python3 plot_sl_test_iperf3.py <iperf3_summary.csv> [output.png]
import csv
import sys
import os
import re
from collections import defaultdict

def parse_bw_mbps(bw_str):
    bw_str = bw_str.replace("Mbps", "").strip()
    try:
        return float(bw_str)
    except ValueError:
        return 0.0

def parse_target_mbps(target_str):
    target_str = target_str.strip()
    m = re.match(r'([\d.]+)\s*([KMG]?)', target_str)
    if not m:
        return 0.0
    val = float(m.group(1))
    unit = m.group(2)
    if unit == 'K':
        val /= 1000.0
    elif unit == 'G':
        val *= 1000.0
    return val

def parse_loss(loss_str):
    try:
        return float(loss_str.replace("%", "").strip())
    except ValueError:
        return 0.0

def plot_iperf3(csv_file, output_file=None):
    if not os.path.isfile(csv_file):
        print(f"ERROR: CSV file not found: {csv_file}")
        return

    if output_file is None:
        output_file = csv_file.replace(".csv", ".png")

    # Read CSV: group by MCS
    mcs_data = defaultdict(lambda: {"target": [], "actual": [], "loss": [], "result": []})

    with open(csv_file, "r") as f:
        reader = csv.DictReader(f)
        for row in reader:
            mcs = row.get("MCS", "N/A")
            target = parse_target_mbps(row.get("BW Target", "0"))
            actual = parse_bw_mbps(row.get("BW Actual (Mbps)", "0"))
            loss = parse_loss(row.get("Loss%", "0"))
            result = row.get("Result", "")
            mcs_data[mcs]["target"].append(target)
            mcs_data[mcs]["actual"].append(actual)
            mcs_data[mcs]["loss"].append(loss)
            mcs_data[mcs]["result"].append(result)

    if not mcs_data:
        print("No data found in CSV")
        return

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 8))

    for mcs, data in sorted(mcs_data.items(), key=lambda x: int(x[0]) if x[0].isdigit() else 0):
        label = f"MCS {mcs}"
        ax1.plot(data["target"], data["actual"], "o-", label=label, markersize=5)
        ax2.plot(data["target"], data["loss"], "s-", label=label, markersize=5)

        # Mark saturation/fail points
        for i, res in enumerate(data["result"]):
            if res == "SATURATED":
                ax1.plot(data["target"][i], data["actual"][i], "^", color="orange", markersize=10, zorder=5)
            elif res == "FAIL":
                ax1.plot(data["target"][i], data["actual"][i], "x", color="red", markersize=10, zorder=5)

    # Reference line: ideal (actual = target)
    all_targets = []
    for d in mcs_data.values():
        all_targets.extend(d["target"])
    if all_targets:
        max_target = max(all_targets)
        ax1.plot([0, max_target], [0, max_target], "--", color="gray", alpha=0.5, label="Ideal")

    ax1.set_xlabel("Target Bandwidth (Mbps)")
    ax1.set_ylabel("Actual Bandwidth (Mbps)")
    ax1.set_title("iperf3 Bandwidth Sweep per MCS")
    ax1.legend(loc="upper left", fontsize=8)
    ax1.grid(True, alpha=0.3)

    ax2.set_xlabel("Target Bandwidth (Mbps)")
    ax2.set_ylabel("Packet Loss (%)")
    ax2.set_title("Packet Loss per MCS")
    ax2.legend(loc="upper left", fontsize=8)
    ax2.grid(True, alpha=0.3)
    ax2.axhline(y=20, color="red", linestyle="--", alpha=0.5, label="20% threshold")

    plt.tight_layout()
    plt.savefig(output_file, dpi=150)
    plt.close()
    print(f"iperf3 plot saved to: {output_file}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <iperf3_summary.csv> [output.png]")
        sys.exit(1)
    csv_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else None
    plot_iperf3(csv_file, output_file)
