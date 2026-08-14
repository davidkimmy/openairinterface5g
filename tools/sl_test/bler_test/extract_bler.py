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

# SNR (dB) is derived from the swept noise power as: SNR = TX_POWER_DBM - PLOSS_DB - noise.
# The TX reference and ploss come from the config (run_sl_test_config.sh) via env vars set by
# process_and_fetch_results.sh; defaults preserve the previous 20/8 behaviour. The equation is
# the same for both backends, only the TX reference differs:
#   - RFSim : tx_power_dbm (nominal TX power in dBm)
#   - vrtsim: vrtsim_tx_power_dbfs (TX signal level in dBFS; vrtsim's noise_power_dB is a
#             dBFS noise floor, so SNR = signal_dBFS - ploss - noise_power_dB).
TX_POWER_DBM = float(os.environ.get("BLER_TX_POWER_DBM", 20))
PLOSS_DB = float(os.environ.get("BLER_PLOSS_DB", 8))

# How PC5 (nearby/syncref) BLER is combined (Uu always uses "rx"). Select via BLER_PC5_METHOD
# (config: bler_pc5_method); default "rx":
#   "rx"           - RX-summary method only, like Uu (per-iteration rows, plot averages). DEFAULT.
#   "rx_preferred" - RX-summary, bilateral only as a fallback (iteration-mean per method).
#   "max"          - max(RX, bilateral) per point (iteration-mean per method); conservative.
PC5_METHOD = os.environ.get("BLER_PC5_METHOD", "rx").lower()

# Optional log-filename prefix filter. When several backends' logs share one test dir (e.g. a
# serial rfsim+vrtsim BLER run), BLER_LOG_PREFIX restricts extraction to one backend's logs
# (their filenames start with the test name, e.g. "rfsim"/"vrtsim"). Empty = no filter.
LOG_PREFIX = os.environ.get("BLER_LOG_PREFIX", "")

def _snr_from_noise(noise):
    return TX_POWER_DBM - PLOSS_DB - noise

# The test_dir argument may be a single directory OR a ':'-separated list of directories
# (used by merge_runs.sh to re-process several runs TOGETHER without copying/renaming any
# logs). We simply glob across all of them; extract emits one row per log file, so the plot
# averages the runs by (mcs, snr).
def _split_dirs(test_dir):
    return [Path(d).expanduser() for d in str(test_dir).split(':') if d]

def _glob_dirs(dirs, pattern):
    files = []
    for d in dirs:
        files.extend(d.glob(f"{LOG_PREFIX}{pattern}"))
    return sorted(files)

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
    for log_file in _glob_dirs(_split_dirs(test_dir), "*mcs*_noise*_result*.log"):
        info = parse_log_filename(log_file.name, str(log_file))
        if not info: continue
        key = (info['mcs'], info['noise'])
        if key not in groups: groups[key] = {'nearby': [], 'syncref': []}
        groups[key][info['role']].append((log_file, info['timestamp']))
    pairs = []
    for (mcs, noise), roles in groups.items():
        if roles['nearby'] and roles['syncref']:
            # Pair per ITERATION: sort both roles by timestamp and zip them, so each
            # num_repeat iteration's nearby+syncref logs form one pair (instead of only the
            # latest). This lets the iterations be averaged downstream. Any extra unmatched
            # log (unequal counts) is dropped by zip().
            nearby_sorted = [f for f, _ in sorted(roles['nearby'], key=lambda x: x[1])]
            syncref_sorted = [f for f, _ in sorted(roles['syncref'], key=lambda x: x[1])]
            for nearby_log, syncref_log in zip(nearby_sorted, syncref_sorted):
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
    return {'mcs': pair['mcs'], 'noise': pair['noise'], 'snr': _snr_from_noise(pair['noise']), 'rounds_0': rx_ok_self, 'rounds_1': 0, 'rounds_2': 0, 'rounds_3': 0, 'bler': (tx_peer - rx_ok_self) / tx_peer}

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
    dirs = _split_dirs(test_dir)
    missing = [str(d) for d in dirs if not d.exists()]
    if missing: print(f"Error: {', '.join(missing)} doesn't exist"); sys.exit(1)

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
    ue_logs = _glob_dirs(dirs, log_pattern)
    print(f"Processing {len(ue_logs)} {ue_name} logs...")

    bler_data, ldpc_data = [], []
    for log_file in ue_logs:
        mcs_match = re.search(r'mcs(\d+)_', log_file.name)
        noise_match = re.search(r'noise([-0-9]+)_', log_file.name)
        if not mcs_match or not noise_match: continue
        mcs, noise, snr = int(mcs_match.group(1)), int(noise_match.group(1)), _snr_from_noise(int(noise_match.group(1)))

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

# PC5 BLER combine (pure Python, no pandas): iteration-mean per method, then combine the two
# methods per `combine`: "rx_preferred" (RX, bilateral only as fallback) or "max" (larger of
# the two). (name kept as merge_max_bler for call-site stability.)
def merge_max_bler(rx_csv, bilateral_csv, output_csv, combine="rx_preferred"):
    # Aggregate a per-point CSV ACROSS ITERATIONS: mean BLER + mean HARQ rounds per
    # (mcs, noise). Previously this did a dict last-row-wins overwrite, so num_repeat
    # iterations on one host were discarded (only the last survived). Now each iteration
    # contributes to the mean.
    def load_mean(path):
        acc = {}
        if Path(path).exists() and os.path.getsize(path) > 0:
            with open(path, 'r') as f:
                for row in csv.DictReader(f):
                    acc.setdefault((int(row['mcs']), int(row['noise'])), []).append(row)
        agg = {}
        for key, rows in acc.items():
            n = len(rows)
            agg[key] = {
                'mcs': key[0], 'noise': key[1], 'snr': rows[0]['snr'],
                'rounds_0': round(sum(int(r['rounds_0']) for r in rows) / n),
                'rounds_1': round(sum(int(r['rounds_1']) for r in rows) / n),
                'rounds_2': round(sum(int(r['rounds_2']) for r in rows) / n),
                'rounds_3': round(sum(int(r['rounds_3']) for r in rows) / n),
                'bler': sum(float(r['bler']) for r in rows) / n,
            }
        return agg

    rx_data = load_mean(rx_csv)
    bilateral_data = load_mean(bilateral_csv)

    # One row per (mcs, noise). combine="rx_preferred": use the RX-summary iteration-mean (the
    # PHY's own direct error count), bilateral only where RX is absent. combine="max": take the
    # larger of the two iteration-means (conservative). RX HARQ rounds are kept in both cases.
    all_keys = set(rx_data.keys()) | set(bilateral_data.keys())
    merged_data = []
    for key in sorted(all_keys):
        rx_row = rx_data.get(key)
        bilateral_row = bilateral_data.get(key)
        if rx_row and bilateral_row:
            result_row = dict(rx_row)
            if combine == "max" and bilateral_row['bler'] > rx_row['bler']:
                result_row['bler'] = bilateral_row['bler']
        else:
            result_row = dict(rx_row or bilateral_row)
        merged_data.append(result_row)

    if merged_data:
        with open(output_csv, 'w', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=['mcs', 'noise', 'snr', 'rounds_0', 'rounds_1', 'rounds_2', 'rounds_3', 'bler'])
            writer.writeheader()
            writer.writerows(merged_data)

    return len(merged_data)

# Main function
def extract_bler_unified(test_dir, output_bler_csv, output_ldpc_csv, log_type):
    valid_types = ['nearby', 'syncref', 'uu_dl', 'uu_dl_gnb', 'uu_dl_relay', 'uu_ul']
    if log_type not in valid_types: print(f"Error: Invalid log_type. Valid: {', '.join(valid_types)}"); sys.exit(1)

    # Uu ALWAYS uses the RX method. PC5 (nearby/syncref) uses the RX method by DEFAULT
    # (BLER_PC5_METHOD="rx"), same as Uu: one row per log file (per num_repeat iteration), the
    # plot averages them. BLER_PC5_METHOD can instead select a bilateral-augmented mode
    # ("rx_preferred" or "max"); the RX-summary is the PHY's own direct error count, while the
    # bilateral (two-sided counter-differencing) is artifact-prone, hence "rx" is the default.
    if log_type in ['uu_dl', 'uu_dl_gnb', 'uu_dl_relay', 'uu_ul'] or PC5_METHOD == 'rx':
        print(f"Using RX method for {log_type}")
        return extract_bler_rx_method(test_dir, output_bler_csv, output_ldpc_csv, log_type)

    # PC5 with the bilateral estimator (BLER_PC5_METHOD = "rx_preferred" or "max").
    print(f"Using PC5 method '{PC5_METHOD}' (RX + bilateral) for {log_type}")
    temp_dir_obj = tempfile.TemporaryDirectory()
    temp_dir = temp_dir_obj.name
    rx_bler_csv, rx_ldpc_csv = Path(temp_dir) / "rx_bler.csv", Path(temp_dir) / "rx_ldpc.csv"
    bil_bler_csv, bil_ldpc_csv = Path(temp_dir) / "bil_bler.csv", Path(temp_dir) / "bil_ldpc.csv"
    _, rx_ldpc_count = extract_bler_rx_method(test_dir, str(rx_bler_csv), str(rx_ldpc_csv), log_type)
    extract_bilateral_bler(test_dir, str(bil_bler_csv), str(bil_ldpc_csv), log_type)
    combine = 'max' if PC5_METHOD == 'max' else 'rx_preferred'
    merged_count = merge_max_bler(str(rx_bler_csv), str(bil_bler_csv), output_bler_csv, combine=combine)
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
