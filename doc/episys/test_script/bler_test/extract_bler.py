#!/usr/bin/env python3
"""
Unified BLER extraction - Single standalone file.

Supports PC5 sidelink and Uu interface extraction with all logic embedded.
No external dependencies - completely self-contained.

Usage: extract_bler.py <test_dir> <output_bler_csv> <output_ldpc_csv> <log_type>

Log types:
  nearby, syncref     - PC5 sidelink (MAX strategy)
  uu_dl_gnb          - Uu DL from gNB RX
  uu_dl_relay        - Uu DL from Relay UE RX
  uu_ul              - Uu UL from gNB RX
"""
import re, csv, sys, os, tempfile
from pathlib import Path

# Bilateral extraction for PC5
def parse_log_filename(filename, filepath):
    pattern = r'.*mcs(\d+)_noise(-?\d+)_.*result_.*\.log'
    match = re.search(pattern, filename)
    if not match: return None
    mcs, noise = int(match.group(1)), int(match.group(2))
    try: timestamp = os.path.getmtime(filepath)
    except: timestamp = 0.0
    role = 'nearby' if 'nearby' in filename else ('syncref' if 'syncref' in filename or 'nrUE' in filename else None)
    return {'mcs': mcs, 'noise': noise, 'timestamp': timestamp, 'role': role} if role else None

def find_log_pairs(test_dir):
    groups = {}
    for log_file in Path(test_dir).expanduser().glob("*mcs*_noise*_result*.log"):
        info = parse_log_filename(log_file.name, str(log_file))
        if not info: continue
        key = (info['mcs'], info['noise'])
        if key not in groups: groups[key] = {'nearby': [], 'syncref': []}
        groups[key][info['role']].append((log_file, info['timestamp']))
    pairs = []
    for (mcs, noise), roles in groups.items():
        if roles['nearby'] and roles['syncref']:
            nearby_log = max(roles['nearby'], key=lambda x: x[1])[0]
            syncref_log = max(roles['syncref'], key=lambda x: x[1])[0]
            pairs.append({'mcs': mcs, 'noise': noise, 'nearby_log': str(nearby_log), 'syncref_log': str(syncref_log)})
    return pairs

def extract_pssch_stats(log_file):
    try:
        with open(log_file, 'r', errors='ignore') as f: content = f.read()
    except: return (0, 0)
    matches = re.findall(r'PSSCH Stats:.*?TX (\d+), RX ok (\d+)', content)
    return (int(matches[-1][0]), int(matches[-1][1])) if matches else (0, 0)

def compute_bilateral_bler(pair, ue_role):
    tx_syncref, rx_ok_syncref = extract_pssch_stats(pair['syncref_log'])
    tx_nearby, rx_ok_nearby = extract_pssch_stats(pair['nearby_log'])
    tx_peer = tx_syncref if ue_role == 'nearby' else tx_nearby
    rx_ok_self = rx_ok_nearby if ue_role == 'nearby' else rx_ok_syncref
    if tx_peer < 50 or rx_ok_self > tx_peer: return None
    return {'mcs': pair['mcs'], 'noise': pair['noise'], 'snr': 20-8-pair['noise'], 'rounds_0': rx_ok_self, 'rounds_1': 0, 'rounds_2': 0, 'rounds_3': 0, 'bler': (tx_peer - rx_ok_self) / tx_peer}

def extract_bilateral_bler(test_dir, output_bler_csv, output_ldpc_csv, ue_role):
    pairs = find_log_pairs(test_dir)
    bler_data = [result for pair in pairs if (result := compute_bilateral_bler(pair, ue_role))]
    with open(output_bler_csv, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=['mcs', 'noise', 'snr', 'rounds_0', 'rounds_1', 'rounds_2', 'rounds_3', 'bler'])
        writer.writeheader(); writer.writerows(bler_data)
    with open(output_ldpc_csv, 'w', newline='') as f:
        csv.DictWriter(f, fieldnames=['mcs', 'noise', 'snr', 'avg_ldpc_iter', 'max_ldpc_iter', 'num_samples']).writeheader()
    return len(bler_data), 0

# RX method extraction for PC5 and Uu
def extract_bler_rx_method(test_dir, output_bler_csv, output_ldpc_csv, log_type):
    results_path = Path(test_dir).expanduser()
    if not results_path.exists(): print(f"Error: {test_dir} doesn't exist"); sys.exit(1)

    # Config for each log type
    configs = {
        'nearby': ("*mcs*_noise*_result_nearby.log", "nearby", "PC5_RX_SUMMARY", "PC5_HARQ_SUCCESS", "PC5_LDPC_ITERATIONS"),
        'syncref': ("*mcs*_noise*_result_nrUE_syncref.log", "syncref", "PC5_RX_SUMMARY", "PC5_HARQ_SUCCESS", "PC5_LDPC_ITERATIONS"),
        'uu_dl': ("*mcs*_noise*_result_gNB.log", "Uu DL", "GNB_DL_SUMMARY", "GNB_DL_HARQ", None),
        'uu_dl_gnb': ("*mcs*_noise*_result_gNB.log", "Uu DL (gNB RX)", "GNB_DL_SUMMARY", "GNB_DL_HARQ", None),
        'uu_dl_relay': ("*mcs*_noise*_result_nrUE_syncref.log", "Uu DL (Relay UE RX)", "UU_RX_SUMMARY", "UU_HARQ_SUCCESS", "UU_LDPC_ITERATIONS"),
        'uu_ul': ("*mcs*_noise*_result_gNB.log", "Uu UL", None, "GNB_UL_HARQ", None)
    }
    if log_type not in configs: print(f"Error: Unknown log_type '{log_type}'"); sys.exit(1)

    log_pattern, ue_name, rx_tag, harq_tag, ldpc_tag = configs[log_type]
    ue_logs = sorted(results_path.glob(log_pattern))
    print(f"Processing {len(ue_logs)} {ue_name} logs...")

    bler_data, ldpc_data = [], []
    for log_file in ue_logs:
        mcs_match = re.search(r'mcs(\d+)_', log_file.name)
        noise_match = re.search(r'noise([-0-9]+)_', log_file.name)
        if not mcs_match or not noise_match: continue
        mcs, noise, snr = int(mcs_match.group(1)), int(noise_match.group(1)), 20 - 8 - int(noise_match.group(1))

        try:
            with open(log_file, 'r', errors='ignore') as f: content = f.read()

            # Extract based on type
            if log_type in ['uu_ul', 'uu_dl', 'uu_dl_gnb']:
                ack_matches = re.findall(r'GNB_UL_HARQ_ACK.*?round=(\d+)' if log_type == 'uu_ul' else r'GNB_DL_HARQ_ACK.*?round=(\d+)', content)
                nack_matches = re.findall(r'GNB_UL_HARQ_NACK.*?round=(\d+)' if log_type == 'uu_ul' else r'GNB_DL_HARQ_NACK.*?round=(\d+)', content)
                if ack_matches or nack_matches:
                    rounds = [sum(1 for r in ack_matches if int(r) == i) for i in range(4)]
                    nack_round_3 = sum(1 for r in nack_matches if int(r) == 3)
                    total_blocks, errors, bler = len(ack_matches) + nack_round_3, nack_round_3, (nack_round_3 / (len(ack_matches) + nack_round_3) if len(ack_matches) + nack_round_3 > 0 else 0.0)
                    rx_summary_matches, harq_matches = [(total_blocks, errors, bler)], [tuple(rounds)]
                else: rx_summary_matches, harq_matches = [], []
            else:
                rx_summary_matches = re.findall(rf'{rx_tag}(?:\s+rnti=\w+)?(?:\s+mcs=\d+)?\s+total=(\d+)\s+errors=(\d+)\s+BLER=([\d.]+)', content)
                if log_type in ['uu_dl', 'uu_dl_gnb']:
                    harq_matches = [(len(re.findall(r'GNB_DL_HARQ_ACK.*?round=0', content)), len(re.findall(r'GNB_DL_HARQ_ACK.*?round=1', content)), len(re.findall(r'GNB_DL_HARQ_ACK.*?round=2', content)), len(re.findall(r'GNB_DL_HARQ_ACK.*?round=3', content)))]
                elif log_type == 'uu_dl_relay':
                    harq_matches = [(len(re.findall(r'UU_HARQ_SUCCESS.*?round=0', content)), len(re.findall(r'UU_HARQ_SUCCESS.*?round=1', content)), len(re.findall(r'UU_HARQ_SUCCESS.*?round=2', content)), len(re.findall(r'UU_HARQ_SUCCESS.*?round=3', content)))]
                else:
                    harq_matches = re.findall(rf'{harq_tag}.*?cumul_r0=(\d+)\s+r1=(\d+)\s+r2=(\d+)\s+r3=(\d+)', content)

            if rx_summary_matches and harq_matches:
                if log_type in ['uu_dl', 'uu_dl_gnb']:
                    entries_1000 = [(int(t), int(e), float(b)) for t, e, b in rx_summary_matches if int(t) == 1000]
                    if entries_1000:
                        total_blocks, errors = sum(t for t, e, b in entries_1000), sum(e for t, e, b in entries_1000)
                        bler = errors / total_blocks if total_blocks > 0 else 0.0
                    else:
                        total_blocks, errors, bler = int(rx_summary_matches[-1][0]), int(rx_summary_matches[-1][1]), float(rx_summary_matches[-1][2])
                else:
                    total_blocks, errors, bler = int(rx_summary_matches[-1][0]), int(rx_summary_matches[-1][1]), float(rx_summary_matches[-1][2])

                rounds_0, rounds_1, rounds_2, rounds_3 = [int(harq_matches[-1][i]) for i in range(4)]
                threshold = 5 if log_type in ['uu_dl', 'uu_dl_gnb', 'uu_dl_relay', 'uu_ul'] else (50 if snr < 15 else 10)
                if total_blocks >= threshold:
                    bler_data.append({'mcs': mcs, 'noise': noise, 'snr': snr, 'rounds_0': rounds_0, 'rounds_1': rounds_1, 'rounds_2': rounds_2, 'rounds_3': rounds_3, 'bler': bler})

            if ldpc_tag:
                ldpc_matches = re.findall(rf'{ldpc_tag} mcs=\d+ iterations=(\d+)', content)
                if ldpc_matches:
                    iterations = [int(i) for i in ldpc_matches]
                    ldpc_data.append({'mcs': mcs, 'noise': noise, 'snr': snr, 'avg_ldpc_iter': sum(iterations) / len(iterations), 'max_ldpc_iter': max(iterations), 'num_samples': len(iterations)})
        except Exception as e: print(f"Error processing {log_file}: {e}"); continue

    with open(output_bler_csv, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=['mcs', 'noise', 'snr', 'rounds_0', 'rounds_1', 'rounds_2', 'rounds_3', 'bler'])
        writer.writeheader(); writer.writerows(bler_data)
    print(f"✓ Saved {len(bler_data)} BLER data points")

    with open(output_ldpc_csv, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=['mcs', 'noise', 'snr', 'avg_ldpc_iter', 'max_ldpc_iter', 'num_samples'])
        writer.writeheader(); writer.writerows(ldpc_data)
    print(f"✓ Saved {len(ldpc_data)} LDPC data points")

    return len(bler_data), len(ldpc_data)

# MAX BLER merge for PC5 (pure Python, no pandas)
def merge_max_bler(rx_csv, bilateral_csv, output_csv):
    # Read RX data
    rx_data = {}
    if Path(rx_csv).exists() and os.path.getsize(rx_csv) > 0:
        with open(rx_csv, 'r') as f:
            reader = csv.DictReader(f)
            for row in reader:
                key = (int(row['mcs']), int(row['noise']))
                rx_data[key] = row

    # Read bilateral data
    bilateral_data = {}
    if Path(bilateral_csv).exists() and os.path.getsize(bilateral_csv) > 0:
        with open(bilateral_csv, 'r') as f:
            reader = csv.DictReader(f)
            for row in reader:
                key = (int(row['mcs']), int(row['noise']))
                bilateral_data[key] = row

    # Merge using MAX strategy
    all_keys = set(rx_data.keys()) | set(bilateral_data.keys())
    merged_data = []

    for key in sorted(all_keys):
        mcs, noise = key
        rx_row = rx_data.get(key)
        bilateral_row = bilateral_data.get(key)

        if rx_row and bilateral_row:
            # Both exist: use MAX BLER, keep RX HARQ data
            result_row = rx_row.copy()
            if float(bilateral_row['bler']) > float(rx_row['bler']):
                result_row['bler'] = bilateral_row['bler']
        elif rx_row:
            result_row = rx_row.copy()
        else:
            result_row = bilateral_row.copy()

        merged_data.append(result_row)

    # Write merged data
    if merged_data:
        with open(output_csv, 'w', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=merged_data[0].keys())
            writer.writeheader()
            writer.writerows(merged_data)

    return len(merged_data)

# Main function
def extract_bler_unified(test_dir, output_bler_csv, output_ldpc_csv, log_type):
    valid_types = ['nearby', 'syncref', 'uu_dl', 'uu_dl_gnb', 'uu_dl_relay', 'uu_ul']
    if log_type not in valid_types: print(f"Error: Invalid log_type. Valid: {', '.join(valid_types)}"); sys.exit(1)

    if log_type in ['uu_dl', 'uu_dl_gnb', 'uu_dl_relay', 'uu_ul']:
        print(f"Using RX method for {log_type}")
        return extract_bler_rx_method(test_dir, output_bler_csv, output_ldpc_csv, log_type)

    print(f"Using MAX BLER strategy for {log_type}")
    temp_dir_obj = tempfile.TemporaryDirectory()
    temp_dir = temp_dir_obj.name
    rx_bler_csv = Path(temp_dir) / "rx_bler.csv"
    rx_ldpc_csv = Path(temp_dir) / "rx_ldpc.csv"
    bilateral_bler_csv = Path(temp_dir) / "bilateral_bler.csv"
    bilateral_ldpc_csv = Path(temp_dir) / "bilateral_ldpc.csv"

    print("Step 1: RX method...")
    rx_bler_count, rx_ldpc_count = extract_bler_rx_method(test_dir, str(rx_bler_csv), str(rx_ldpc_csv), log_type)
    print(f"  RX: {rx_bler_count} BLER rows")

    print("Step 2: Bilateral method...")
    bilateral_bler_count, _ = extract_bilateral_bler(test_dir, str(bilateral_bler_csv), str(bilateral_ldpc_csv), log_type)
    print(f"  Bilateral: {bilateral_bler_count} BLER rows")

    print("Step 3: Merging with MAX...")
    merged_count = merge_max_bler(str(rx_bler_csv), str(bilateral_bler_csv), output_bler_csv)
    print(f"  Merged: {merged_count} BLER rows")

    if rx_ldpc_csv.exists():
        import shutil
        shutil.copy(str(rx_ldpc_csv), output_ldpc_csv)

    temp_dir_obj.cleanup()
    return merged_count, rx_ldpc_count

if __name__ == '__main__':
    if len(sys.argv) != 5:
        print("Usage: extract_bler.py <test_dir> <output_bler_csv> <output_ldpc_csv> <log_type>")
        print("Log types: nearby, syncref, uu_dl_gnb, uu_dl_relay, uu_ul")
        sys.exit(1)
    extract_bler_unified(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4])
