#!/usr/bin/env python3
"""
Process BLER data locally on a machine
Extracts BLER statistics from logs and saves to CSV
Run this ON THE MACHINE where logs are located
NO EXTERNAL DEPENDENCIES - uses only standard library
"""

import re
import sys
import csv
from pathlib import Path
from collections import defaultdict

def extract_pc5_tx_from_log(log_file, interface_type):
    """Extract PC5_TX (scheduler-side) BLER from syncref logs"""
    filename = Path(log_file).name
    mcs_match = re.search(r'mcs(\d+)_', filename)
    noise_match = re.search(r'noise([-0-9]+)_', filename)

    if not mcs_match or not noise_match:
        return None

    mcs = int(mcs_match.group(1))
    noise = int(noise_match.group(1))
    ploss_db = 8
    snr = 20 - ploss_db - noise

    try:
        with open(log_file, 'r', errors='ignore') as f:
            content = f.read()

        # Try PC5_TX_SCHED first (new log format - counts all TX)
        tx_matches = re.findall(r'PC5_TX_SCHED total=(\d+) retrans=(\d+) BLER=([\d.]+)', content)
        if tx_matches:
            # Use the last summary (most complete data)
            last_summary = tx_matches[-1]
            total = int(last_summary[0])
            retrans = int(last_summary[1])
            bler = float(last_summary[2])

            if total > 0:
                return {
                    'mcs': mcs,
                    'noise': noise,
                    'snr': snr,
                    'total': total,
                    'retrans': retrans,
                    'errors': retrans,  # For consistency with RX format
                    'bler': bler,
                    'interface': interface_type
                }

        # Fallback to PC5_SCHED_BLER (old log format - requires PSFCH feedback)
        sched_matches = re.findall(r'PC5_SCHED_BLER total=(\d+) retrans=(\d+) BLER=([\d.]+)', content)
        if sched_matches:
            # Use the last summary (most complete data)
            last_summary = sched_matches[-1]
            total = int(last_summary[0])
            retrans = int(last_summary[1])
            bler = float(last_summary[2])

            if total > 0:
                return {
                    'mcs': mcs,
                    'noise': noise,
                    'snr': snr,
                    'total': total,
                    'retrans': retrans,
                    'errors': retrans,  # For consistency with RX format
                    'bler': bler,
                    'interface': interface_type
                }

    except Exception as e:
        print(f"Error parsing PC5 TX BLER from {log_file}: {e}", file=sys.stderr)

    return None

def extract_pc5_rx_summary_from_log(log_file, interface_type):
    """Extract PC5_RX_SUMMARY based BLER from logs"""
    filename = Path(log_file).name
    mcs_match = re.search(r'mcs(\d+)_', filename)
    noise_match = re.search(r'noise([-0-9]+)_', filename)

    if not mcs_match or not noise_match:
        return None

    mcs = int(mcs_match.group(1))
    noise = int(noise_match.group(1))
    ploss_db = 8
    snr = 20 - ploss_db - noise

    try:
        with open(log_file, 'r', errors='ignore') as f:
            content = f.read()

        # Extract PC5_RX_SUMMARY: total=X errors=Y BLER=Z
        rx_summary_matches = re.findall(r'PC5_RX_SUMMARY total=(\d+) errors=(\d+) BLER=([\d.]+)', content)
        if rx_summary_matches:
            # Use the last summary (most complete data)
            last_summary = rx_summary_matches[-1]
            total = int(last_summary[0])
            errors = int(last_summary[1])
            bler = float(last_summary[2])

            if total > 0:
                return {
                    'mcs': mcs,
                    'noise': noise,
                    'snr': snr,
                    'total': total,
                    'errors': errors,
                    'bler': bler,
                    'interface': interface_type
                }

    except Exception as e:
        print(f"Error parsing PC5_RX_SUMMARY from {log_file}: {e}", file=sys.stderr)

    return None

def extract_mac_bler_from_log(log_file, interface_type):
    """Extract MAC BLER from HARQ round stats"""
    filename = Path(log_file).name
    mcs_match = re.search(r'mcs(\d+)_', filename)
    noise_match = re.search(r'noise([-0-9]+)_', filename)

    if not mcs_match or not noise_match:
        return None

    mcs = int(mcs_match.group(1))
    noise = int(noise_match.group(1))
    ploss_db = 8
    snr = 20 - ploss_db - noise

    try:
        with open(log_file, 'r', errors='ignore') as f:
            content = f.read()

        if interface_type.startswith('pc5'):
            harq_matches = re.findall(r'Harq round stats.*?(\d+)/(\d+)/(\d+)(?:/(\d+))?', content)
            if harq_matches:
                last_stats = harq_matches[-1]
                rounds_0 = int(last_stats[0])
                rounds_1 = int(last_stats[1])
                rounds_2 = int(last_stats[2])
                rounds_3 = int(last_stats[3]) if last_stats[3] else 0

                total_transmissions = rounds_0 + rounds_1 + rounds_2 + rounds_3
                if total_transmissions > 0:
                    total_retrans = rounds_1 + rounds_2 + rounds_3
                    mac_bler = total_retrans / total_transmissions

                    # Extract LDPC iteration statistics
                    ldpc_iterations = []
                    ldpc_matches = re.findall(r'PC5_LDPC_ITERATIONS mcs=\d+ iterations=(\d+)', content)
                    if ldpc_matches:
                        ldpc_iterations = [int(i) for i in ldpc_matches]
                        avg_ldpc_iterations = sum(ldpc_iterations) / len(ldpc_iterations)
                        max_ldpc_iterations = max(ldpc_iterations)
                    else:
                        avg_ldpc_iterations = 0
                        max_ldpc_iterations = 0

                    return {
                        'mcs': mcs,
                        'noise': noise,
                        'snr': snr,
                        'rounds_0': rounds_0,
                        'rounds_1': rounds_1,
                        'rounds_2': rounds_2,
                        'rounds_3': rounds_3,
                        'bler': mac_bler,
                        'interface': interface_type,
                        'avg_ldpc_iter': avg_ldpc_iterations,
                        'max_ldpc_iter': max_ldpc_iterations
                    }

        elif interface_type == 'uu_dl':
            harq_matches = re.findall(r'dlsch_rounds\s+(\d+)/(\d+)/(\d+)/(\d+)', content)
            if harq_matches:
                last_stats = harq_matches[-1]
                rounds_0 = int(last_stats[0])
                rounds_1 = int(last_stats[1])
                rounds_2 = int(last_stats[2])
                rounds_3 = int(last_stats[3])

                total_transmissions = rounds_0 + rounds_1 + rounds_2 + rounds_3
                if total_transmissions > 0:
                    total_retrans = rounds_1 + rounds_2 + rounds_3
                    mac_bler = total_retrans / total_transmissions

                    return {
                        'mcs': mcs,
                        'noise': noise,
                        'snr': snr,
                        'rounds_0': rounds_0,
                        'rounds_1': rounds_1,
                        'rounds_2': rounds_2,
                        'rounds_3': rounds_3,
                        'bler': mac_bler,
                        'interface': interface_type
                    }

        elif interface_type == 'uu_ul':
            harq_matches = re.findall(r'ulsch_rounds\s+(\d+)/(\d+)/(\d+)/(\d+)', content)
            if harq_matches:
                last_stats = harq_matches[-1]
                rounds_0 = int(last_stats[0])
                rounds_1 = int(last_stats[1])
                rounds_2 = int(last_stats[2])
                rounds_3 = int(last_stats[3])

                total_transmissions = rounds_0 + rounds_1 + rounds_2 + rounds_3
                if total_transmissions > 0:
                    total_retrans = rounds_1 + rounds_2 + rounds_3
                    mac_bler = total_retrans / total_transmissions

                    return {
                        'mcs': mcs,
                        'noise': noise,
                        'snr': snr,
                        'rounds_0': rounds_0,
                        'rounds_1': rounds_1,
                        'rounds_2': rounds_2,
                        'rounds_3': rounds_3,
                        'bler': mac_bler,
                        'interface': interface_type
                    }

    except Exception as e:
        print(f"Error parsing {log_file}: {e}", file=sys.stderr)

    return None

def process_local_logs(test_dir, output_csv, output_pc5_rx_csv):
    """Process logs in test directory and save to CSV"""
    results_path = Path(test_dir)
    all_data = []
    all_pc5_rx_data = []

    print(f"Processing logs in: {test_dir}")

    # PC5 Relay UE (syncref)
    relay_logs = sorted(results_path.glob("*mcs*_noise*_result_nrUE_syncref.log"))
    print(f"  Processing {len(relay_logs)} Relay UE logs...")
    for log in relay_logs:
        # HARQ-based BLER
        data = extract_mac_bler_from_log(log, 'pc5_relay')
        if data:
            all_data.append(data)

        # PC5_RX_SUMMARY based BLER (syncref receives PSFCH feedback)
        rx_data = extract_pc5_rx_summary_from_log(log, 'pc5_rx_syncref')
        if rx_data:
            all_pc5_rx_data.append(rx_data)

        # PC5_TX (scheduler-side) BLER - tracks all transmissions
        tx_data = extract_pc5_tx_from_log(log, 'pc5_tx_syncref')
        if tx_data:
            all_pc5_rx_data.append(tx_data)  # Add to same dataset for comparison

    # PC5 Remote UE (nearby)
    remote_logs = sorted(results_path.glob("*mcs*_noise*_result_nearby.log"))
    print(f"  Processing {len(remote_logs)} Remote UE logs...")
    for log in remote_logs:
        # HARQ-based BLER
        data = extract_mac_bler_from_log(log, 'pc5_remote')
        if data:
            all_data.append(data)

        # PC5_RX_SUMMARY based BLER
        rx_data = extract_pc5_rx_summary_from_log(log, 'pc5_rx_remote')
        if rx_data:
            all_pc5_rx_data.append(rx_data)

    # Uu from gNB logs
    gnb_logs = sorted(results_path.glob("*mcs*_noise*_result_gNB.log"))
    print(f"  Processing {len(gnb_logs)} gNB logs...")
    for log in gnb_logs:
        data_dl = extract_mac_bler_from_log(log, 'uu_dl')
        if data_dl:
            all_data.append(data_dl)

        data_ul = extract_mac_bler_from_log(log, 'uu_ul')
        if data_ul:
            all_data.append(data_ul)

    if not all_data:
        print(f"  Warning: No BLER data extracted!")
        return False

    # Average across iterations (group by mcs, noise, interface)
    # Use dictionary to group data
    grouped = defaultdict(lambda: {'count': 0, 'sum_bler': 0.0, 'sum_r0': 0, 'sum_r1': 0, 'sum_r2': 0, 'sum_r3': 0, 'snr': 0, 'sum_avg_ldpc': 0.0, 'sum_max_ldpc': 0.0})

    for item in all_data:
        key = (item['mcs'], item['noise'], item['interface'])
        grouped[key]['count'] += 1
        grouped[key]['sum_bler'] += item['bler']
        grouped[key]['sum_r0'] += item['rounds_0']
        grouped[key]['sum_r1'] += item['rounds_1']
        grouped[key]['sum_r2'] += item['rounds_2']
        grouped[key]['sum_r3'] += item['rounds_3']
        grouped[key]['snr'] = item['snr']  # Same for all in group
        grouped[key]['sum_avg_ldpc'] += item.get('avg_ldpc_iter', 0)
        grouped[key]['sum_max_ldpc'] += item.get('max_ldpc_iter', 0)

    # Calculate averages
    averaged = []
    for (mcs, noise, interface), values in grouped.items():
        avg_bler = values['sum_bler'] / values['count']
        avg_ldpc = values['sum_avg_ldpc'] / values['count']
        max_ldpc = values['sum_max_ldpc'] / values['count']
        averaged.append({
            'mcs': mcs,
            'noise': noise,
            'snr': values['snr'],
            'interface': interface,
            'rounds_0': values['sum_r0'],
            'rounds_1': values['sum_r1'],
            'rounds_2': values['sum_r2'],
            'rounds_3': values['sum_r3'],
            'bler': avg_bler,
            'avg_ldpc_iter': avg_ldpc,
            'max_ldpc_iter': max_ldpc
        })

    # Sort by interface, noise, mcs
    averaged.sort(key=lambda x: (x['interface'], x['noise'], x['mcs']))

    # Write to CSV
    with open(output_csv, 'w', newline='') as f:
        fieldnames = ['mcs', 'noise', 'snr', 'interface', 'rounds_0', 'rounds_1', 'rounds_2', 'rounds_3', 'bler', 'avg_ldpc_iter', 'max_ldpc_iter']
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(averaged)

    print(f"  ✓ Saved {len(averaged)} averaged BLER points to: {output_csv}")

    # Print summary
    mcs_values = [item['mcs'] for item in averaged]
    noise_values = [item['noise'] for item in averaged]
    if mcs_values and noise_values:
        print(f"  MCS range: {min(mcs_values)}-{max(mcs_values)}")
        print(f"  Noise range: {min(noise_values)}-{max(noise_values)}")

    # Process PC5_RX_SUMMARY data
    if all_pc5_rx_data:
        # Average across iterations
        rx_grouped = defaultdict(lambda: {'count': 0, 'sum_bler': 0.0, 'sum_total': 0, 'sum_errors': 0, 'snr': 0})

        for item in all_pc5_rx_data:
            key = (item['mcs'], item['noise'], item['interface'])
            rx_grouped[key]['count'] += 1
            rx_grouped[key]['sum_bler'] += item['bler']
            rx_grouped[key]['sum_total'] += item['total']
            rx_grouped[key]['sum_errors'] += item['errors']
            rx_grouped[key]['snr'] = item['snr']

        rx_averaged = []
        for (mcs, noise, interface), values in rx_grouped.items():
            avg_bler = values['sum_bler'] / values['count']
            rx_averaged.append({
                'mcs': mcs,
                'noise': noise,
                'snr': values['snr'],
                'interface': interface,
                'total': values['sum_total'],
                'errors': values['sum_errors'],
                'bler': avg_bler
            })

        # Sort by interface, noise, mcs
        rx_averaged.sort(key=lambda x: (x['interface'], x['noise'], x['mcs']))

        # Write PC5_RX_SUMMARY CSV
        with open(output_pc5_rx_csv, 'w', newline='') as f:
            fieldnames = ['mcs', 'noise', 'snr', 'interface', 'total', 'errors', 'bler']
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(rx_averaged)

        print(f"  ✓ Saved {len(rx_averaged)} PC5_RX_SUMMARY BLER points to: {output_pc5_rx_csv}")
    else:
        print(f"  ⚠ No PC5_RX_SUMMARY data found")

    return True

def main():
    if len(sys.argv) < 3:
        print("Usage: python3 process_bler_local.py <test_directory> <output_csv> [output_pc5_rx_csv]")
        sys.exit(1)

    test_dir = sys.argv[1]
    output_csv = sys.argv[2]
    output_pc5_rx_csv = sys.argv[3] if len(sys.argv) > 3 else output_csv.replace('.csv', '_pc5_rx.csv')

    if not Path(test_dir).exists():
        print(f"Error: Test directory not found: {test_dir}")
        sys.exit(1)

    success = process_local_logs(test_dir, output_csv, output_pc5_rx_csv)

    if not success:
        sys.exit(1)

if __name__ == '__main__':
    main()
