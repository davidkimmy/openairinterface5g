#!/usr/bin/env python3
"""Extract MAC BLER and LDPC stats from UE logs (nearby or syncref)"""

import re
import csv
import sys
from pathlib import Path

def extract_bler_and_ldpc(test_dir, output_bler_csv, output_ldpc_csv, log_type='nearby'):
    """
    Extract both BLER and LDPC from UE logs

    Args:
        test_dir: Directory containing test logs
        output_bler_csv: Output CSV file for BLER data
        output_ldpc_csv: Output CSV file for LDPC data
        log_type: 'nearby' or 'syncref' to select log pattern
    """
    results_path = Path(test_dir).expanduser()

    if not results_path.exists():
        print(f"Error: {test_dir} doesn't exist")
        sys.exit(1)

    # Select log pattern based on type
    # Pattern handles both formats:
    # - Short: *mcs*_noise*_result_nearby.log
    # - Long: rfsim_slmode1_bler_test_on_local_host_mcs*_noise*_*_result_nearby.log
    if log_type == 'nearby':
        log_pattern = "*mcs*_noise*_result_nearby.log"
        ue_name = "nearby"
    elif log_type == 'syncref':
        log_pattern = "*mcs*_noise*_result_nrUE_syncref.log"
        ue_name = "syncref"
    else:
        print(f"Error: Unknown log_type '{log_type}'. Must be 'nearby' or 'syncref'")
        sys.exit(1)

    ue_logs = sorted(results_path.glob(log_pattern))
    print(f"Processing {len(ue_logs)} {ue_name} logs...")

    bler_data = []
    ldpc_data = []

    for log_file in ue_logs:
        filename = log_file.name
        mcs_match = re.search(r'mcs(\d+)_', filename)
        noise_match = re.search(r'noise([-0-9]+)_', filename)

        if not mcs_match or not noise_match:
            continue

        mcs = int(mcs_match.group(1))
        noise = int(noise_match.group(1))
        snr = 20 - 8 - noise

        try:
            with open(log_file, 'r', errors='ignore') as f:
                content = f.read()

            # Extract PC5_RX_SUMMARY for true BLER (includes errors after all HARQ attempts)
            rx_summary_matches = re.findall(
                r'PC5_RX_SUMMARY total=(\d+) errors=(\d+) BLER=([\d.]+)',
                content
            )

            # Extract HARQ stats (cumulative counts) for HARQ distribution
            harq_matches = re.findall(
                r'PC5_HARQ_SUCCESS.*?cumul_r0=(\d+)\s+r1=(\d+)\s+r2=(\d+)\s+r3=(\d+)',
                content
            )

            if rx_summary_matches and harq_matches:
                # Use last summary (most complete)
                last_rx = rx_summary_matches[-1]
                total_blocks = int(last_rx[0])
                errors = int(last_rx[1])
                bler = float(last_rx[2])  # Use directly logged BLER

                last_harq = harq_matches[-1]
                rounds_0 = int(last_harq[0])
                rounds_1 = int(last_harq[1])
                rounds_2 = int(last_harq[2])
                rounds_3 = int(last_harq[3])

                # Adaptive threshold: lower for high SNR (where BLER is expected to be very low)
                # At high SNR, fewer retransmissions occur, so block counts are naturally lower
                threshold = 50 if snr < 15 else 10
                if total_blocks < threshold:
                    print(f"  Skipping {log_file.name}: only {total_blocks} blocks (< {threshold} threshold for SNR={snr}dB)")
                    continue

                bler_data.append({
                    'mcs': mcs,
                    'noise': noise,
                    'snr': snr,
                    'rounds_0': rounds_0,
                    'rounds_1': rounds_1,
                    'rounds_2': rounds_2,
                    'rounds_3': rounds_3,
                    'bler': bler  # True BLER from PC5_RX_SUMMARY
                })

            # Extract LDPC iterations
            ldpc_matches = re.findall(r'PC5_LDPC_ITERATIONS mcs=\d+ iterations=(\d+)', content)
            if ldpc_matches:
                iterations = [int(i) for i in ldpc_matches]
                ldpc_data.append({
                    'mcs': mcs,
                    'noise': noise,
                    'snr': snr,
                    'avg_ldpc_iter': sum(iterations) / len(iterations),
                    'max_ldpc_iter': max(iterations),
                    'num_samples': len(iterations)
                })

        except Exception as e:
            print(f"Error processing {log_file}: {e}")
            continue

    # Write BLER CSV
    with open(output_bler_csv, 'w', newline='') as f:
        fieldnames = ['mcs', 'noise', 'snr', 'rounds_0', 'rounds_1', 'rounds_2', 'rounds_3', 'bler']
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(bler_data)
    print(f"✓ Saved {len(bler_data)} BLER data points to: {output_bler_csv}")

    # Write LDPC CSV
    with open(output_ldpc_csv, 'w', newline='') as f:
        fieldnames = ['mcs', 'noise', 'snr', 'avg_ldpc_iter', 'max_ldpc_iter', 'num_samples']
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(ldpc_data)
    print(f"✓ Saved {len(ldpc_data)} LDPC data points to: {output_ldpc_csv}")

    return len(bler_data), len(ldpc_data)

if __name__ == '__main__':
    if len(sys.argv) != 5:
        print("Usage: extract_bler.py <test_directory> <output_bler_csv> <output_ldpc_csv> <nearby|syncref>")
        print()
        print("Examples:")
        print("  extract_bler.py ~/test_20260518 nearby_bler.csv nearby_ldpc.csv nearby")
        print("  extract_bler.py ~/test_20260518 syncref_bler.csv syncref_ldpc.csv syncref")
        sys.exit(1)

    test_dir = sys.argv[1]
    output_bler_csv = sys.argv[2]
    output_ldpc_csv = sys.argv[3]
    log_type = sys.argv[4]

    extract_bler_and_ldpc(test_dir, output_bler_csv, output_ldpc_csv, log_type)
