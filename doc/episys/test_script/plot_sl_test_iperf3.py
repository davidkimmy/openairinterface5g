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

def parse_transfer_mb(transfer_str):
    transfer_str = transfer_str.replace("MB", "").strip()
    try:
        return float(transfer_str)
    except ValueError:
        return 0.0

def plot_iperf3(csv_file, output_file=None):
    if not os.path.isfile(csv_file):
        print(f"ERROR: CSV file not found: {csv_file}")
        return

    if output_file is None:
        output_file = csv_file.replace(".csv", ".png")

    # Read CSV: group by MCS
    mcs_data = defaultdict(lambda: {"target": [], "actual": [], "transfer": [], "loss": [], "result": []})

    with open(csv_file, "r") as f:
        reader = csv.DictReader(f)
        for row in reader:
            mcs = row.get("MCS", "N/A")
            target = parse_target_mbps(row.get("BW Target", "0"))
            actual = parse_bw_mbps(row.get("BW Actual (Mbps)", "0"))
            transfer = parse_transfer_mb(row.get("Transfer (MB)", "0"))
            loss = parse_loss(row.get("Loss%", "0"))
            result = row.get("Result", "")
            mcs_data[mcs]["target"].append(target)
            mcs_data[mcs]["actual"].append(actual)
            mcs_data[mcs]["transfer"].append(transfer)
            mcs_data[mcs]["loss"].append(loss)
            mcs_data[mcs]["result"].append(result)

    if not mcs_data:
        print("No data found in CSV")
        return

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax1 = plt.subplots(figsize=(10, 6))
    ax2 = ax1.twinx()

    for idx, (mcs, data) in enumerate(sorted(mcs_data.items(), key=lambda x: int(x[0]) if x[0].isdigit() else 0)):
        label = f"MCS {mcs}"
        ax1.plot(data["target"], data["actual"], "o-", color="green", label=f"{label} Bitrate (Mbps)", markersize=5)
        ax1.plot(data["target"], data["transfer"], "D-", color="blue", label=f"{label} Transfer (MB)", markersize=4)
        ax2.plot(data["target"], data["loss"], "s-", color="red", label=f"{label} Loss (%)", markersize=4)

        for i, res in enumerate(data["result"]):
            if res == "SATURATED":
                ax1.plot(data["target"][i], data["actual"][i], "^", color="orange", markersize=10, zorder=5)
            elif res == "FAIL":
                ax1.plot(data["target"][i], data["actual"][i], "x", color="darkred", markersize=10, zorder=5)

    ax1.set_xlabel("Target Bandwidth (Mbps)")
    ax1.set_ylabel("KPI Performance (MB, Mbps)")
    ax1.set_title("5G Sidelink KPI performance per target bit rate")
    ax1.grid(True, alpha=0.3)

    ax2.set_ylabel("Packet Loss (%)", color="red")
    ax2.tick_params(axis="y", labelcolor="red")
    ax2.axhline(y=20, color="red", linestyle="--", alpha=0.3, label="20% loss threshold")

    lines1, labels1 = ax1.get_legend_handles_labels()
    lines2, labels2 = ax2.get_legend_handles_labels()
    ax1.legend(lines1 + lines2, labels1 + labels2, loc="upper left", fontsize=7)

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
