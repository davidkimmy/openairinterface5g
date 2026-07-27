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
import os
import subprocess
import tempfile
from pathlib import Path
from collections import defaultdict

# SNR (dB) = TX_POWER_DBM - PLOSS_DB - noise, from the config via env (BLER_TX_POWER_DBM /
# BLER_PLOSS_DB, set/forwarded by process_and_fetch_results.sh). Matches extract_bler.py so
# vrtsim (and Uu) BLER use the same config-driven SNR mapping instead of a hardcoded 20 - 8.
TX_POWER_DBM = float(os.environ.get("BLER_TX_POWER_DBM", 20))
PLOSS_DB = float(os.environ.get("BLER_PLOSS_DB", 8))

# Optional log-filename prefix filter (see extract_bler.py). When several backends share one
# test dir (serial rfsim+vrtsim run), BLER_LOG_PREFIX="rfsim"/"vrtsim" restricts to one backend.
LOG_PREFIX = os.environ.get("BLER_LOG_PREFIX", "")

# test_dir may be a single directory OR a ':'-separated list of directories (merge_runs.sh
# re-processes several runs TOGETHER without copying/renaming logs). Glob across all of them;
# the averaging by (mcs, noise, interface) below then averages the runs.
def _split_dirs(test_dir):
    return [Path(d).expanduser() for d in str(test_dir).split(':') if d]

def _glob_all(paths, pattern):
    files = []
    for p in paths:
        files.extend(p.glob(f"{LOG_PREFIX}{pattern}"))
    return sorted(files)

def extract_pc5_tx_from_log(log_file, interface_type):
    """Extract PC5_TX (scheduler-side) BLER from syncref logs"""
    filename = Path(log_file).name
    mcs_match = re.search(r'mcs(\d+)_', filename)
    noise_match = re.search(r'noise([-0-9]+)_', filename)

    if not mcs_match or not noise_match:
        return None

    mcs = int(mcs_match.group(1))
    noise = int(noise_match.group(1))
    snr = TX_POWER_DBM - PLOSS_DB - noise

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
    snr = TX_POWER_DBM - PLOSS_DB - noise

    try:
        with open(log_file, 'r', errors='ignore') as f:
            content = f.read()

        # Extract PC5_RX_SUMMARY: total=X errors=Y BLER=Z
        # Pattern handles both formats:
        # - With MCS: PC5_RX_SUMMARY mcs=9 total=100 errors=0 BLER=0.0000
        # - Without MCS: PC5_RX_SUMMARY total=100 errors=0 BLER=0.0000
        rx_summary_matches = re.findall(r'PC5_RX_SUMMARY(?:\s+mcs=\d+)?\s+total=(\d+)\s+errors=(\d+)\s+BLER=([\d.]+)', content)
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
    snr = TX_POWER_DBM - PLOSS_DB - noise

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

def convert_bilateral_row_to_pc5_rx_format(row):
    """Convert bilateral extraction CSV row to PC5 RX data format"""
    method = row.get('method', 'unknown')

    # Determine interface name based on method
    if method == 'bilateral':
        interface = 'pc5_bilateral_' + ('syncref' if 'syncref' in row.get('role', '') else 'nearby')
    elif method == 'rx':
        interface = 'pc5_rx_' + ('syncref' if 'syncref' in row.get('role', '') else 'remote')
    else:
        interface = f'pc5_{method}_' + ('syncref' if 'syncref' in row.get('role', '') else 'remote')

    rounds_0 = int(row.get('rounds_0', 0))
    bler = float(row['bler'])

    # Calculate total and errors based on method
    if method == 'bilateral':
        # For bilateral: rounds_0 = successful, need to calculate total and errors
        total = int(rounds_0 / (1 - bler)) if bler < 1 else rounds_0
        errors = total - rounds_0
    else:
        # For rx method: total = all rounds, errors = bler * total
        rounds_1 = int(row.get('rounds_1', 0))
        rounds_2 = int(row.get('rounds_2', 0))
        rounds_3 = int(row.get('rounds_3', 0))
        total = rounds_0 + rounds_1 + rounds_2 + rounds_3
        errors = int(bler * total)

    return {
        'mcs': int(row['mcs']),
        'noise': int(row['noise']),
        'snr': float(row['snr']),  # config-driven SNR is a float (e.g. 17.0); int() would raise
        'total': total,
        'errors': errors,
        'bler': bler,
        'interface': interface
    }

def extract_bilateral_bler_for_ue(test_dir, ue_type, temp_dir_path, script_dir):
    """
    Extract bilateral BLER data for a specific UE type (syncref or nearby)

    Args:
        test_dir: Directory containing test logs
        ue_type: 'syncref' or 'nearby'
        temp_dir_path: Temporary directory for intermediate files
        script_dir: Directory containing extract_bler.py script

    Returns:
        list: Extracted PC5 RX data entries
    """
    print(f"  Attempting bilateral extraction for {ue_type}...")

    bilateral_bler_csv = Path(temp_dir_path) / f"{ue_type}_bilateral_bler.csv"
    bilateral_ldpc_csv = Path(temp_dir_path) / f"{ue_type}_bilateral_ldpc.csv"
    extract_bler_script = script_dir / "extract_bler.py"

    pc5_rx_data = []

    try:
        result = subprocess.run(
            ["python3", str(extract_bler_script), test_dir,
             str(bilateral_bler_csv), str(bilateral_ldpc_csv), ue_type],
            capture_output=True,
            text=True,
            check=True
        )

        # Read and convert bilateral data
        if bilateral_bler_csv.exists():
            with open(bilateral_bler_csv, 'r') as f:
                reader = csv.DictReader(f)
                for row in reader:
                    row['role'] = ue_type  # Add role for interface naming
                    pc5_rx_data.append(convert_bilateral_row_to_pc5_rx_format(row))

            print(f"    ✓ Extracted bilateral data for {ue_type}")

    except subprocess.CalledProcessError as e:
        print(f"    ⚠ Warning: Bilateral extraction for {ue_type} failed: {e}")
        print(f"      stderr: {e.stderr}")
    except Exception as e:
        print(f"    ⚠ Warning: Bilateral extraction for {ue_type} error: {e}")

    return pc5_rx_data

def extract_bilateral_bler_data(test_dir, script_dir):
    """
    Extract bilateral BLER data for both syncref and nearby UEs

    Returns:
        list: Combined PC5 RX data from both UEs
    """
    all_pc5_rx_data = []

    # Create temporary directory for bilateral extraction
    temp_dir_obj = tempfile.TemporaryDirectory()
    temp_dir_path = temp_dir_obj.name

    # Extract for both UE types
    for ue_type in ['syncref', 'nearby']:
        pc5_rx_data = extract_bilateral_bler_for_ue(
            test_dir, ue_type, temp_dir_path, script_dir
        )
        all_pc5_rx_data.extend(pc5_rx_data)

    # Cleanup temp directory
    temp_dir_obj.cleanup()

    return all_pc5_rx_data

def process_relay_ue_logs(results_paths):
    """
    Process Relay UE (syncref) logs for PC5 interface (across one or more test dirs)

    Returns:
        tuple: (harq_data, pc5_rx_data)
    """
    harq_data = []
    pc5_rx_data = []

    relay_logs = _glob_all(results_paths, "*mcs*_noise*_result_nrUE_syncref.log")
    print(f"  Processing {len(relay_logs)} Relay UE logs...")

    for log in relay_logs:
        # HARQ-based BLER
        data = extract_mac_bler_from_log(log, 'pc5_relay')
        if data:
            harq_data.append(data)

        # PC5_RX_SUMMARY based BLER (syncref receives PSFCH feedback)
        rx_data = extract_pc5_rx_summary_from_log(log, 'pc5_rx_syncref')
        if rx_data:
            pc5_rx_data.append(rx_data)

        # PC5_TX (scheduler-side) BLER - tracks all transmissions
        tx_data = extract_pc5_tx_from_log(log, 'pc5_tx_syncref')
        if tx_data:
            pc5_rx_data.append(tx_data)

    return harq_data, pc5_rx_data

def process_remote_ue_logs(results_paths):
    """
    Process Remote UE (nearby) logs for PC5 interface (across one or more test dirs)

    Returns:
        tuple: (harq_data, pc5_rx_data)
    """
    harq_data = []
    pc5_rx_data = []

    remote_logs = _glob_all(results_paths, "*mcs*_noise*_result_nearby.log")
    print(f"  Processing {len(remote_logs)} Remote UE logs...")

    for log in remote_logs:
        # HARQ-based BLER
        data = extract_mac_bler_from_log(log, 'pc5_remote')
        if data:
            harq_data.append(data)

        # PC5_RX_SUMMARY based BLER
        rx_data = extract_pc5_rx_summary_from_log(log, 'pc5_rx_remote')
        if rx_data:
            pc5_rx_data.append(rx_data)

    return harq_data, pc5_rx_data

def process_gnb_logs(results_paths):
    """
    Process gNB logs for Uu interface (DL and UL) (across one or more test dirs)

    Returns:
        list: HARQ data for both DL and UL
    """
    harq_data = []

    gnb_logs = _glob_all(results_paths, "*mcs*_noise*_result_gNB.log")
    print(f"  Processing {len(gnb_logs)} gNB logs...")

    for log in gnb_logs:
        # Uu DL
        data_dl = extract_mac_bler_from_log(log, 'uu_dl')
        if data_dl:
            harq_data.append(data_dl)

        # Uu UL
        data_ul = extract_mac_bler_from_log(log, 'uu_ul')
        if data_ul:
            harq_data.append(data_ul)

    return harq_data

def average_harq_data(all_data):
    """
    Average HARQ data across iterations grouped by (mcs, noise, interface)

    Returns:
        list: Averaged BLER data points
    """
    grouped = defaultdict(lambda: {
        'count': 0, 'sum_bler': 0.0,
        'sum_r0': 0, 'sum_r1': 0, 'sum_r2': 0, 'sum_r3': 0,
        'snr': 0, 'sum_avg_ldpc': 0.0, 'sum_max_ldpc': 0.0
    })

    for item in all_data:
        key = (item['mcs'], item['noise'], item['interface'])
        grouped[key]['count'] += 1
        grouped[key]['sum_bler'] += item['bler']
        grouped[key]['sum_r0'] += item['rounds_0']
        grouped[key]['sum_r1'] += item['rounds_1']
        grouped[key]['sum_r2'] += item['rounds_2']
        grouped[key]['sum_r3'] += item['rounds_3']
        grouped[key]['snr'] = item['snr']
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

    return averaged

def average_pc5_rx_data(all_pc5_rx_data):
    """
    Average PC5 RX data across iterations grouped by (mcs, noise, interface)

    Returns:
        list: Averaged PC5 RX BLER data points
    """
    rx_grouped = defaultdict(lambda: {
        'count': 0, 'sum_bler': 0.0,
        'sum_total': 0, 'sum_errors': 0, 'snr': 0
    })

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

    return rx_averaged

def save_harq_csv(averaged_data, output_csv):
    """Save averaged HARQ BLER data to CSV"""
    with open(output_csv, 'w', newline='') as f:
        fieldnames = ['mcs', 'noise', 'snr', 'interface',
                     'rounds_0', 'rounds_1', 'rounds_2', 'rounds_3',
                     'bler', 'avg_ldpc_iter', 'max_ldpc_iter']
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(averaged_data)

    print(f"  ✓ Saved {len(averaged_data)} averaged BLER points to: {output_csv}")

    # Print summary
    if averaged_data:
        mcs_values = [item['mcs'] for item in averaged_data]
        noise_values = [item['noise'] for item in averaged_data]
        print(f"  MCS range: {min(mcs_values)}-{max(mcs_values)}")
        print(f"  Noise range: {min(noise_values)}-{max(noise_values)}")

def save_pc5_rx_csv(rx_averaged_data, output_pc5_rx_csv):
    """Save averaged PC5 RX BLER data to CSV"""
    with open(output_pc5_rx_csv, 'w', newline='') as f:
        fieldnames = ['mcs', 'noise', 'snr', 'interface', 'total', 'errors', 'bler']
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rx_averaged_data)

    print(f"  ✓ Saved {len(rx_averaged_data)} PC5_RX_SUMMARY BLER points to: {output_pc5_rx_csv}")

def process_local_logs(test_dir, output_csv, output_pc5_rx_csv):
    """
    Process logs in test directory and save to CSV

    Main orchestrator function that coordinates all extraction and processing steps
    """
    results_paths = _split_dirs(test_dir)
    script_dir = Path(__file__).parent

    print(f"Processing logs in: {test_dir}")

    # Step 1: Extract bilateral BLER data using extract_bler.py (test_dir may be a ':'-list;
    # extract_bler.py globs across all of them too).
    all_pc5_rx_data = extract_bilateral_bler_data(test_dir, script_dir)

    # Step 2: Process logs for HARQ-based BLER
    all_harq_data = []

    # PC5 Relay UE (syncref)
    relay_harq, relay_pc5_rx = process_relay_ue_logs(results_paths)
    all_harq_data.extend(relay_harq)
    all_pc5_rx_data.extend(relay_pc5_rx)

    # PC5 Remote UE (nearby)
    remote_harq, remote_pc5_rx = process_remote_ue_logs(results_paths)
    all_harq_data.extend(remote_harq)
    all_pc5_rx_data.extend(remote_pc5_rx)

    # Uu from gNB logs
    gnb_harq = process_gnb_logs(results_paths)
    all_harq_data.extend(gnb_harq)

    # Step 3: Validate data
    if not all_harq_data:
        print(f"  Warning: No BLER data extracted!")
        return False

    # Step 4: Average and save HARQ data
    averaged_harq = average_harq_data(all_harq_data)
    save_harq_csv(averaged_harq, output_csv)

    # Step 5: Average and save PC5 RX data
    if all_pc5_rx_data:
        rx_averaged = average_pc5_rx_data(all_pc5_rx_data)
        save_pc5_rx_csv(rx_averaged, output_pc5_rx_csv)
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

    missing = [str(d) for d in _split_dirs(test_dir) if not d.exists()]
    if missing:
        print(f"Error: Test directory not found: {', '.join(missing)}")
        sys.exit(1)

    success = process_local_logs(test_dir, output_csv, output_pc5_rx_csv)

    if not success:
        sys.exit(1)

if __name__ == '__main__':
    main()
