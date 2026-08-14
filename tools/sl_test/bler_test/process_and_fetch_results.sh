#!/bin/bash
#############################################################
# Process BLER Test Results
# Collects, combines, and plots BLER data from all 4 hosts
# Uses iteration-based distribution with full MCS range per host
#############################################################

BLER_SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
# Home-relative path of this bler_test dir, used for REMOTE (ssh) hosts so they resolve the
# scripts under their own $HOME (works even if the remote user id differs).
BLER_SCRIPT_DIR_REL="${BLER_SCRIPT_DIR#$HOME/}"
PARENT_DIR="$(dirname "$BLER_SCRIPT_DIR")"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# Source config first to get OAI_BASE_DIR
SL_TEST_CONFIG_FILE="$PARENT_DIR/run_sl_test_config.sh"
if [[ -f "$SL_TEST_CONFIG_FILE" ]]; then
    source "$SL_TEST_CONFIG_FILE" > /dev/null 2>&1
fi

# Pass the SNR-mapping constants to extract_bler.py, which computes
#   SNR = BLER_TX_POWER_DBM - BLER_PLOSS_DB - noise.
# The TX reference is backend-specific: RFSim uses tx_power_dbm (dBm); vrtsim uses
# vrtsim_tx_power_dbfs (TX signal level in dBFS), since vrtsim's noise_power_dB is a dBFS
# noise floor. Detect the backend from the enabled BLER test name.
# Which BLER backends are enabled (independent checks -- both may be listed for a serial run).
has_rfsim=false; has_vrtsim=false
printf '%s\n' "${enabled_tests[@]}" 2>/dev/null | grep -q '^[[:space:]]*rfsim_.*bler' && has_rfsim=true
printf '%s\n' "${enabled_tests[@]}" 2>/dev/null | grep -q '^[[:space:]]*vrtsim_.*bler' && has_vrtsim=true

# Select the backend for the TX reference. A per-backend sub-invocation forces it via
# BLER_FORCE_BACKEND; otherwise auto-detect (vrtsim wins if present).
if [[ -n "$BLER_FORCE_BACKEND" ]]; then
    BLER_BACKEND="$BLER_FORCE_BACKEND"
elif [[ "$has_vrtsim" == true ]]; then
    BLER_BACKEND="vrtsim"
else
    BLER_BACKEND="rfsim"
fi
# Export so plot_results.py can see it (e.g. to drop the modulation-order annotations for vrtsim,
# whose SNR axis uses a dBFS reference).
export BLER_BACKEND
if [[ "$BLER_BACKEND" == "vrtsim" ]]; then
    export BLER_TX_POWER_DBM="${vrtsim_tx_power_dbfs:--30}"
else
    export BLER_TX_POWER_DBM="${tx_power_dbm:-20}"
fi
export BLER_PLOSS_DB="${ploss_db:-8}"
# PC5 BLER combine method for extract_bler.py: rx (default) | rx_preferred | max
export BLER_PC5_METHOD="${bler_pc5_method:-rx}"
# Log-filename prefix filter (empty = all). Set to "rfsim"/"vrtsim" in a per-backend
# sub-invocation so the extractors only pick that backend's logs. Local python calls inherit
# it from the environment; the remote ssh calls pass it explicitly.
export BLER_LOG_PREFIX="${BLER_LOG_PREFIX:-}"
echo "SNR mapping: backend=$BLER_BACKEND  TX_ref=${BLER_TX_POWER_DBM}  ploss=${BLER_PLOSS_DB}  (SNR = TX_ref - ploss - noise)${BLER_LOG_PREFIX:+  [logs: ${BLER_LOG_PREFIX}*]}"

# Use configured paths or defaults
OAI_BASE_DIR="${OAI_BASE_DIR:-$HOME/openairinterface5g}"

# Merge mode (set by merge_runs.sh): BLER_MERGE_DIRS is a ':'-separated list of existing
# test_<ts> dirs to re-process TOGETHER (averaged). We read those dirs directly -- no logs are
# copied, renamed, or symlinked, and 'latest' is not touched. Merging is inherently a single
# host operation, so force localhost-only regardless of the configured bler_hosts.
if [[ -n "$BLER_MERGE_DIRS" ]]; then
    bler_hosts=(localhost)
fi

# Serial rfsim + vrtsim run: both backends' logs share one test_<ts> dir but need DIFFERENT SNR
# references (rfsim=dBm, vrtsim=dBFS), so they cannot be plotted together. When both are enabled
# (and we are not already in a per-backend sub-invocation or a merge), re-run this script once
# per backend -- each pass filters logs by prefix and writes its own bler_results_<ts>_<backend>.
if [[ "$has_rfsim" == true && "$has_vrtsim" == true && -z "$BLER_LOG_PREFIX" && -z "$BLER_MERGE_DIRS" ]]; then
    echo "Both rfsim and vrtsim BLER tests enabled -> post-processing each backend separately"
    for be in rfsim vrtsim; do
        echo ""
        echo "########## Backend: $be ##########"
        BLER_LOG_PREFIX="$be" BLER_FORCE_BACKEND="$be" BLER_RESULTS_SUFFIX="_$be" bash "$0"
    done
    exit 0
fi

# Resolve the local test dir(s): the merge list if merging, else the 'latest' symlink, else the
# newest test_2026* by mtime.
resolve_local_test_dir() {
    if [[ -n "$BLER_MERGE_DIRS" ]]; then
        echo "$BLER_MERGE_DIRS"
    elif [[ -d "${OAI_BASE_DIR}/latest" ]]; then
        readlink -f "${OAI_BASE_DIR}/latest"
    else
        ls -dt ${OAI_BASE_DIR}/test_2026* 2>/dev/null | head -1
    fi
}

# Name the results folder after the DATA's timestamp -- the test_<timestamp> dir that the
# 'latest' symlink points to -- instead of the moment this script runs. This makes the
# output folder match the test run it was derived from, and re-processing the same run
# writes to the same bler_results_<timestamp> folder. Falls back to the current time if
# there is no 'latest' symlink (e.g. a hand-made test dir).
DATA_TIMESTAMP=""
if [[ -d "${OAI_BASE_DIR}/latest" ]]; then
    DATA_TIMESTAMP=$(basename "$(readlink -f "${OAI_BASE_DIR}/latest")" | grep -oE '[0-9]{8}_[0-9]{6}' | head -1)
fi
RESULTS_TIMESTAMP="${DATA_TIMESTAMP:-$TIMESTAMP}"

# BLER_RESULTS_SUFFIX (e.g. "_rfsim"/"_vrtsim") is set by the per-backend split above so each
# backend gets its own results folder; empty for a normal single-backend run.
if [[ -n "$BLER_MERGE_DIRS" ]]; then
    # Merge output goes NEXT TO the input run dirs: parent dir defaults to OAI_BASE_DIR (where
    # the test_<ts> dirs live) and is overridable via merge_runs.sh -d <parent>.
    LOCAL_RESULTS="${BLER_MERGE_OUT_PARENT:-$OAI_BASE_DIR}/bler_results_merged_${TIMESTAMP}${BLER_RESULTS_SUFFIX}"
else
    LOCAL_RESULTS="${BLER_RESULTS_DIR}_${RESULTS_TIMESTAMP}${BLER_RESULTS_SUFFIX}"
fi

mkdir -p $LOCAL_RESULTS

echo "=========================================="
echo "BLER Results - Process & Plot"
echo "=========================================="
echo ""

# Verify Python and the plotting dependencies (see requirements.txt in this folder).
# We only check here (no auto-install) so nothing is fetched from the network without
# the user's action. plot_results.py needs matplotlib/numpy/pandas/scipy; the extract
# scripts are stdlib-only.
if ! command -v python3 >/dev/null 2>&1; then
    echo "ERROR: python3 not found. Install Python 3 to post-process BLER results."
    exit 1
fi
MISSING_PY=$(python3 - <<'PY'
import importlib.util
mods = ["matplotlib", "numpy", "pandas", "scipy"]
print(" ".join(m for m in mods if importlib.util.find_spec(m) is None))
PY
)
if [[ -n "$MISSING_PY" ]]; then
    echo "ERROR: missing Python packages for BLER post-processing: $MISSING_PY"
    echo "Install them with:"
    echo "  pip3 install -r \"$BLER_SCRIPT_DIR/requirements.txt\""
    exit 1
fi

# Check if bler_hosts array is defined (already sourced above)
if [[ -z "${bler_hosts[@]}" ]]; then
    echo "ERROR: bler_hosts array not defined in config file: $SL_TEST_CONFIG_FILE"
    echo "Add to the config file:"
    echo "  bler_hosts=(l3 l4 l5 localhost)"
    exit 1
fi

num_hosts=${#bler_hosts[@]}
echo "Detected $num_hosts hosts from config"
echo ""

# Build array of host IDs (host1, host2, host3, ...)
host_ids=()
for ((i=1; i<=num_hosts; i++)); do
    host_ids+=("host${i}")
done

# Note: Test configuration (MCS/noise values) will be shown after processing logs
echo ""

# Display host configuration
echo "Machine Configuration:"
host_idx=0
for hostname in "${bler_hosts[@]}"; do
    host_idx=$((host_idx + 1))
    host_id="host${host_idx}"

    # Worker config file names match what was created by orchestration
    if [[ "$hostname" == "localhost" || "$hostname" == "local" ]]; then
        sl_test_config_file="$PARENT_DIR/run_sl_test_config_worker_local.sh"
    else
        sl_test_config_file="$PARENT_DIR/run_sl_test_config_worker_${hostname}.sh"
    fi

    if [[ -f "$sl_test_config_file" ]]; then
        iter_start=$(grep "^iteration_start=" "$sl_test_config_file" | cut -d'=' -f2)
        iter_end=$(grep "^iteration_end=" "$sl_test_config_file" | cut -d'=' -f2)
        echo "  ${host_id} (${hostname}): Iterations ${iter_start}-${iter_end}"
    else
        echo "  ${host_id} (${hostname}): Config not found (expected: $(basename $sl_test_config_file))"
    fi
done

echo ""
echo "Results will be saved to: $LOCAL_RESULTS"
echo ""

# Process each host
host_idx=0
for hostname in "${bler_hosts[@]}"; do
    host_idx=$((host_idx + 1))
    host_id="host${host_idx}"

    echo "=========================================="
    echo "Processing $host_id ($hostname)"
    echo "=========================================="

    if [[ "$hostname" == "localhost" ]]; then
        # Process locally
        test_dir=$(resolve_local_test_dir)
        if [[ -z "$test_dir" ]]; then
            echo "  ⚠ No test directory found"
            continue
        fi

        echo "  Test directory: $test_dir"
        echo "  Processing logs..."

        cd $BLER_SCRIPT_DIR
        BLER_TX_POWER_DBM=$BLER_TX_POWER_DBM BLER_PLOSS_DB=$BLER_PLOSS_DB BLER_LOG_PREFIX=$BLER_LOG_PREFIX python3 process_bler_local.py \
            $test_dir \
            $LOCAL_RESULTS/bler_${host_id}.csv \
            $LOCAL_RESULTS/bler_${host_id}_pc5_rx.csv

        if [[ -f $LOCAL_RESULTS/bler_${host_id}.csv ]]; then
            lines=$(wc -l < $LOCAL_RESULTS/bler_${host_id}.csv)
            echo "  ✓ Saved: bler_${host_id}.csv ($lines lines)"
        fi

        if [[ -f $LOCAL_RESULTS/bler_${host_id}_pc5_rx.csv ]]; then
            lines=$(wc -l < $LOCAL_RESULTS/bler_${host_id}_pc5_rx.csv)
            echo "  ✓ Saved: bler_${host_id}_pc5_rx.csv ($lines lines)"
        fi
    else
        # Process on remote host
        echo "  → SSHing to $hostname to process logs..."

        # Try SSH with timeout and limited retries
        ssh_success=false
        for attempt in 1 2 3; do
            if timeout 300 ssh -o ConnectTimeout=5 -o ConnectionAttempts=1 $hostname "
                # Prefer the 'latest' symlink (deterministic); fall back to newest test_* by mtime
                test_dir=\$(if [ -d ~/openairinterface5g/latest ]; then readlink -f ~/openairinterface5g/latest; else ls -dt ~/openairinterface5g/test_2026* 2>/dev/null | head -1; fi)
                if [[ -z \"\$test_dir\" ]]; then
                    echo '  ⚠ No test directory found'
                    exit 1
                fi

                echo \"  Test directory: \$test_dir\"
                echo \"  Processing logs...\"

                cd ~/$BLER_SCRIPT_DIR_REL
                BLER_TX_POWER_DBM=$BLER_TX_POWER_DBM BLER_PLOSS_DB=$BLER_PLOSS_DB BLER_LOG_PREFIX=$BLER_LOG_PREFIX python3 process_bler_local.py \
                    \$test_dir \
                    /tmp/bler_${host_id}.csv \
                    /tmp/bler_${host_id}_pc5_rx.csv

                if [[ -f /tmp/bler_${host_id}.csv ]]; then
                    lines=\$(wc -l < /tmp/bler_${host_id}.csv)
                    echo \"  ✓ Generated: bler_${host_id}.csv (\$lines lines)\"
                fi

                if [[ -f /tmp/bler_${host_id}_pc5_rx.csv ]]; then
                    lines=\$(wc -l < /tmp/bler_${host_id}_pc5_rx.csv)
                    echo \"  ✓ Generated: bler_${host_id}_pc5_rx.csv (\$lines lines)\"
                fi
            "; then
                ssh_success=true
                break
            else
                echo "  ⚠ SSH attempt $attempt/3 failed"
                [[ $attempt -lt 3 ]] && sleep 2
            fi
        done

        if [[ "$ssh_success" == "false" ]]; then
            echo "  ✗ Failed to connect to $hostname after 3 attempts, skipping..."
            continue
        fi

        # Fetch CSV files only (tiny compared to logs!)
        echo "  Fetching CSVs..."
        timeout 10 scp -q -o ConnectTimeout=5 $hostname:/tmp/bler_${host_id}.csv $LOCAL_RESULTS/ 2>/dev/null && echo "  ✓ Fetched: bler_${host_id}.csv"
        timeout 10 scp -q -o ConnectTimeout=5 $hostname:/tmp/bler_${host_id}_pc5_rx.csv $LOCAL_RESULTS/ 2>/dev/null && echo "  ✓ Fetched: bler_${host_id}_pc5_rx.csv"
    fi
    echo ""
done

echo "=========================================="
echo "Combining Results from All Machines"
echo "=========================================="

# Combine all host CSVs
cd $BLER_SCRIPT_DIR

# Create combined CSV by concatenating all host results
echo "Combining MAC BLER results..."
header_written=false
for host_id in "${host_ids[@]}"; do
    if [[ -f $LOCAL_RESULTS/bler_${host_id}.csv ]]; then
        if [[ "$header_written" == "false" ]]; then
            head -1 $LOCAL_RESULTS/bler_${host_id}.csv > $LOCAL_RESULTS/bler_combined.csv
            header_written=true
        fi
        tail -n +2 $LOCAL_RESULTS/bler_${host_id}.csv >> $LOCAL_RESULTS/bler_combined.csv 2>/dev/null
    fi
done
if [[ -f $LOCAL_RESULTS/bler_combined.csv ]]; then
    lines=$(wc -l < $LOCAL_RESULTS/bler_combined.csv)
    echo "✓ Combined MAC BLER: $((lines - 1)) data points"
else
    echo "✗ No MAC BLER data available"
fi

echo "Combining PC5_RX BLER results..."
header_written=false
for host_id in "${host_ids[@]}"; do
    if [[ -f $LOCAL_RESULTS/bler_${host_id}_pc5_rx.csv ]]; then
        if [[ "$header_written" == "false" ]]; then
            head -1 $LOCAL_RESULTS/bler_${host_id}_pc5_rx.csv > $LOCAL_RESULTS/bler_combined_pc5_rx.csv
            header_written=true
        fi
        tail -n +2 $LOCAL_RESULTS/bler_${host_id}_pc5_rx.csv >> $LOCAL_RESULTS/bler_combined_pc5_rx.csv 2>/dev/null
    fi
done
if [[ -f $LOCAL_RESULTS/bler_combined_pc5_rx.csv ]]; then
    lines=$(wc -l < $LOCAL_RESULTS/bler_combined_pc5_rx.csv)
    echo "✓ Combined PC5_RX BLER: $((lines - 1)) data points"
else
    echo "✗ No PC5_RX BLER data available"
fi

echo ""
# Skipping comprehensive 9-panel plot (not needed)
echo ""
echo "=========================================="
echo "Extracting Nearby (RX) BLER Data"
echo "=========================================="

# Extract nearby BLER and LDPC from all hosts
host_idx=0
for hostname in "${bler_hosts[@]}"; do
    host_idx=$((host_idx + 1))
    host_id="host${host_idx}"

    echo "→ Processing $host_id ($hostname)..."

    if [[ "$hostname" == "localhost" ]]; then
        test_dir=$(resolve_local_test_dir)
        if [[ -z "$test_dir" ]]; then
            echo "  ⚠ No test directory found"
            continue
        fi
        echo "  Test directory: $test_dir"

        python3 $BLER_SCRIPT_DIR/extract_bler.py \
            "$test_dir" \
            $LOCAL_RESULTS/nearby_bler_${host_id}.csv \
            $LOCAL_RESULTS/nearby_ldpc_${host_id}.csv \
            nearby
    else
        # Try to find test directory with timeout
        test_dir=$(timeout 10 ssh -o ConnectTimeout=5 -o ConnectionAttempts=1 $hostname "if [ -d ~/openairinterface5g/latest ]; then readlink -f ~/openairinterface5g/latest; else ls -dt ~/openairinterface5g/test_2026* 2>/dev/null | head -1; fi" 2>/dev/null)
        if [[ -z "$test_dir" ]]; then
            echo "  ⚠ No test directory found or connection failed, skipping..."
            continue
        fi
        echo "  Test directory: $test_dir"

        # Try SSH with timeout (increased for bilateral processing)
        if ! timeout 180 ssh -o ConnectTimeout=5 -o ConnectionAttempts=1 $hostname "BLER_TX_POWER_DBM=$BLER_TX_POWER_DBM BLER_PLOSS_DB=$BLER_PLOSS_DB BLER_PC5_METHOD=$BLER_PC5_METHOD BLER_LOG_PREFIX=$BLER_LOG_PREFIX python3 ~/$BLER_SCRIPT_DIR_REL/extract_bler.py \
            $test_dir \
            /tmp/nearby_bler_${host_id}.csv \
            /tmp/nearby_ldpc_${host_id}.csv \
            nearby"; then
            echo "  ✗ Failed to extract data from $hostname, skipping..."
            continue
        fi

        timeout 10 scp -q -o ConnectTimeout=5 $hostname:/tmp/nearby_bler_${host_id}.csv $LOCAL_RESULTS/ 2>/dev/null
        timeout 10 scp -q -o ConnectTimeout=5 $hostname:/tmp/nearby_ldpc_${host_id}.csv $LOCAL_RESULTS/ 2>/dev/null
    fi
done

# Combine nearby BLER
echo "Combining nearby BLER..."
# Find first available file for header
header_written=false
for host_id in "${host_ids[@]}"; do
    if [[ -f $LOCAL_RESULTS/nearby_bler_${host_id}.csv ]]; then
        if [[ "$header_written" == "false" ]]; then
            head -1 $LOCAL_RESULTS/nearby_bler_${host_id}.csv > $LOCAL_RESULTS/nearby_bler_combined.csv
            header_written=true
        fi
        tail -n +2 $LOCAL_RESULTS/nearby_bler_${host_id}.csv >> $LOCAL_RESULTS/nearby_bler_combined.csv 2>/dev/null
    fi
done
if [[ -f $LOCAL_RESULTS/nearby_bler_combined.csv ]]; then
    lines=$(wc -l < $LOCAL_RESULTS/nearby_bler_combined.csv)
    echo "✓ Combined nearby BLER: $((lines - 1)) data points"
else
    echo "✗ No nearby BLER data available"
fi

# Combine nearby LDPC
echo "Combining nearby LDPC..."
header_written=false
for host_id in "${host_ids[@]}"; do
    if [[ -f $LOCAL_RESULTS/nearby_ldpc_${host_id}.csv ]]; then
        if [[ "$header_written" == "false" ]]; then
            head -1 $LOCAL_RESULTS/nearby_ldpc_${host_id}.csv > $LOCAL_RESULTS/nearby_ldpc_combined.csv
            header_written=true
        fi
        tail -n +2 $LOCAL_RESULTS/nearby_ldpc_${host_id}.csv >> $LOCAL_RESULTS/nearby_ldpc_combined.csv 2>/dev/null
    fi
done
if [[ -f $LOCAL_RESULTS/nearby_ldpc_combined.csv ]]; then
    lines=$(wc -l < $LOCAL_RESULTS/nearby_ldpc_combined.csv)
    echo "✓ Combined nearby LDPC: $((lines - 1)) data points"
else
    echo "✗ No nearby LDPC data available"
fi

echo ""
# Skipping Syncref TX - not needed for analysis
echo "=========================================="
echo "Skipping Syncref (TX) BLER Data"
echo "=========================================="
echo ""
echo "=========================================="
echo "Extracting Syncref RX BLER Data"
echo "=========================================="

# Extract syncref RX BLER and LDPC from all hosts
host_idx=0
for hostname in "${bler_hosts[@]}"; do
    host_idx=$((host_idx + 1))
    host_id="host${host_idx}"

    echo "→ Processing $host_id ($hostname)..."

    if [[ "$hostname" == "localhost" ]]; then
        test_dir=$(resolve_local_test_dir)
        if [[ -z "$test_dir" ]]; then
            echo "  ⚠ No test directory found"
            continue
        fi
        echo "  Test directory: $test_dir"

        python3 $BLER_SCRIPT_DIR/extract_bler.py \
            "$test_dir" \
            $LOCAL_RESULTS/syncref_rx_bler_${host_id}.csv \
            $LOCAL_RESULTS/syncref_rx_ldpc_${host_id}.csv \
            syncref
    else
        # Try to find test directory with timeout
        test_dir=$(timeout 10 ssh -o ConnectTimeout=5 -o ConnectionAttempts=1 $hostname "if [ -d ~/openairinterface5g/latest ]; then readlink -f ~/openairinterface5g/latest; else ls -dt ~/openairinterface5g/test_2026* 2>/dev/null | head -1; fi" 2>/dev/null)
        if [[ -z "$test_dir" ]]; then
            echo "  ⚠ No test directory found or connection failed, skipping..."
            continue
        fi
        echo "  Test directory: $test_dir"

        # Try SSH with timeout (increased for bilateral processing)
        if ! timeout 180 ssh -o ConnectTimeout=5 -o ConnectionAttempts=1 $hostname "BLER_TX_POWER_DBM=$BLER_TX_POWER_DBM BLER_PLOSS_DB=$BLER_PLOSS_DB BLER_PC5_METHOD=$BLER_PC5_METHOD BLER_LOG_PREFIX=$BLER_LOG_PREFIX python3 ~/$BLER_SCRIPT_DIR_REL/extract_bler.py \
            $test_dir \
            /tmp/syncref_rx_bler_${host_id}.csv \
            /tmp/syncref_rx_ldpc_${host_id}.csv \
            syncref"; then
            echo "  ✗ Failed to extract data from $hostname, skipping..."
            continue
        fi

        timeout 10 scp -q -o ConnectTimeout=5 $hostname:/tmp/syncref_rx_bler_${host_id}.csv $LOCAL_RESULTS/ 2>/dev/null
        timeout 10 scp -q -o ConnectTimeout=5 $hostname:/tmp/syncref_rx_ldpc_${host_id}.csv $LOCAL_RESULTS/ 2>/dev/null
    fi
done

# Combine syncref RX BLER
echo "Combining syncref RX BLER..."
header_written=false
for host_id in "${host_ids[@]}"; do
    if [[ -f $LOCAL_RESULTS/syncref_rx_bler_${host_id}.csv ]]; then
        if [[ "$header_written" == "false" ]]; then
            head -1 $LOCAL_RESULTS/syncref_rx_bler_${host_id}.csv > $LOCAL_RESULTS/syncref_rx_bler_combined.csv
            header_written=true
        fi
        tail -n +2 $LOCAL_RESULTS/syncref_rx_bler_${host_id}.csv >> $LOCAL_RESULTS/syncref_rx_bler_combined.csv 2>/dev/null
    fi
done
if [[ -f $LOCAL_RESULTS/syncref_rx_bler_combined.csv ]]; then
    lines=$(wc -l < $LOCAL_RESULTS/syncref_rx_bler_combined.csv)
    echo "✓ Combined syncref RX BLER: $((lines - 1)) data points"
else
    echo "✗ No syncref RX BLER data available"
fi

# Combine syncref RX LDPC
echo "Combining syncref RX LDPC..."
header_written=false
for host_id in "${host_ids[@]}"; do
    if [[ -f $LOCAL_RESULTS/syncref_rx_ldpc_${host_id}.csv ]]; then
        if [[ "$header_written" == "false" ]]; then
            head -1 $LOCAL_RESULTS/syncref_rx_ldpc_${host_id}.csv > $LOCAL_RESULTS/syncref_rx_ldpc_combined.csv
            header_written=true
        fi
        tail -n +2 $LOCAL_RESULTS/syncref_rx_ldpc_${host_id}.csv >> $LOCAL_RESULTS/syncref_rx_ldpc_combined.csv 2>/dev/null
    fi
done
if [[ -f $LOCAL_RESULTS/syncref_rx_ldpc_combined.csv ]]; then
    lines=$(wc -l < $LOCAL_RESULTS/syncref_rx_ldpc_combined.csv)
    echo "✓ Combined syncref RX LDPC: $((lines - 1)) data points"
else
    echo "✗ No syncref RX LDPC data available"
fi

echo ""
echo "=========================================="
echo "Extracting Uu DL BLER Data (gNB → Relay UE)"
echo "=========================================="

# Extract Uu DL BLER and LDPC from all hosts
host_idx=0
for hostname in "${bler_hosts[@]}"; do
    host_idx=$((host_idx + 1))
    host_id="host${host_idx}"

    echo "→ Processing $host_id ($hostname)..."

    if [[ "$hostname" == "localhost" ]]; then
        test_dir=$(resolve_local_test_dir)
        if [[ -z "$test_dir" ]]; then
            echo "  ⚠ No test directory found"
            continue
        fi
        echo "  Test directory: $test_dir"

        python3 $BLER_SCRIPT_DIR/extract_bler.py \
            "$test_dir" \
            $LOCAL_RESULTS/uu_dl_bler_${host_id}.csv \
            $LOCAL_RESULTS/uu_dl_ldpc_${host_id}.csv \
            uu_dl
    else
        test_dir=$(timeout 10 ssh -o ConnectTimeout=5 -o ConnectionAttempts=1 $hostname "if [ -d ~/openairinterface5g/latest ]; then readlink -f ~/openairinterface5g/latest; else ls -dt ~/openairinterface5g/test_2026* 2>/dev/null | head -1; fi" 2>/dev/null)
        if [[ -z "$test_dir" ]]; then
            echo "  ⚠ No test directory found or connection failed, skipping..."
            continue
        fi
        echo "  Test directory: $test_dir"

        if ! timeout 180 ssh -o ConnectTimeout=5 -o ConnectionAttempts=1 $hostname "BLER_TX_POWER_DBM=$BLER_TX_POWER_DBM BLER_PLOSS_DB=$BLER_PLOSS_DB BLER_PC5_METHOD=$BLER_PC5_METHOD BLER_LOG_PREFIX=$BLER_LOG_PREFIX python3 ~/$BLER_SCRIPT_DIR_REL/extract_bler.py \
            $test_dir \
            /tmp/uu_dl_bler_${host_id}.csv \
            /tmp/uu_dl_ldpc_${host_id}.csv \
            uu_dl"; then
            echo "  ✗ Failed to extract data from $hostname, skipping..."
            continue
        fi

        timeout 10 scp -q -o ConnectTimeout=5 $hostname:/tmp/uu_dl_bler_${host_id}.csv $LOCAL_RESULTS/ 2>/dev/null
        timeout 10 scp -q -o ConnectTimeout=5 $hostname:/tmp/uu_dl_ldpc_${host_id}.csv $LOCAL_RESULTS/ 2>/dev/null
    fi
done

# Combine Uu DL BLER
echo "Combining Uu DL BLER..."
header_written=false
for host_id in "${host_ids[@]}"; do
    if [[ -f $LOCAL_RESULTS/uu_dl_bler_${host_id}.csv ]]; then
        if [[ "$header_written" == "false" ]]; then
            head -1 $LOCAL_RESULTS/uu_dl_bler_${host_id}.csv > $LOCAL_RESULTS/uu_dl_bler_combined.csv
            header_written=true
        fi
        tail -n +2 $LOCAL_RESULTS/uu_dl_bler_${host_id}.csv >> $LOCAL_RESULTS/uu_dl_bler_combined.csv 2>/dev/null
    fi
done
if [[ -f $LOCAL_RESULTS/uu_dl_bler_combined.csv ]]; then
    lines=$(wc -l < $LOCAL_RESULTS/uu_dl_bler_combined.csv)
    echo "✓ Combined Uu DL BLER: $((lines - 1)) data points"
else
    echo "✗ No Uu DL BLER data available"
fi

# Combine Uu DL LDPC
echo "Combining Uu DL LDPC..."
header_written=false
for host_id in "${host_ids[@]}"; do
    if [[ -f $LOCAL_RESULTS/uu_dl_ldpc_${host_id}.csv ]]; then
        if [[ "$header_written" == "false" ]]; then
            head -1 $LOCAL_RESULTS/uu_dl_ldpc_${host_id}.csv > $LOCAL_RESULTS/uu_dl_ldpc_combined.csv
            header_written=true
        fi
        tail -n +2 $LOCAL_RESULTS/uu_dl_ldpc_${host_id}.csv >> $LOCAL_RESULTS/uu_dl_ldpc_combined.csv 2>/dev/null
    fi
done
if [[ -f $LOCAL_RESULTS/uu_dl_ldpc_combined.csv ]]; then
    lines=$(wc -l < $LOCAL_RESULTS/uu_dl_ldpc_combined.csv)
    echo "✓ Combined Uu DL LDPC: $((lines - 1)) data points"
else
    echo "✗ No Uu DL LDPC data available"
fi

echo ""
echo "=========================================="
echo "Extracting Uu UL BLER Data (Relay UE → gNB)"
echo "=========================================="

# Extract Uu UL BLER from all hosts (gNB logs)
host_idx=0
for hostname in "${bler_hosts[@]}"; do
    host_idx=$((host_idx + 1))
    host_id="host${host_idx}"

    echo "→ Processing $host_id ($hostname)..."

    if [[ "$hostname" == "localhost" ]]; then
        test_dir=$(resolve_local_test_dir)
        if [[ -z "$test_dir" ]]; then
            echo "  ⚠ No test directory found"
            continue
        fi
        echo "  Test directory: $test_dir"

        python3 $BLER_SCRIPT_DIR/extract_bler.py \
            "$test_dir" \
            $LOCAL_RESULTS/uu_ul_bler_${host_id}.csv \
            $LOCAL_RESULTS/uu_ul_ldpc_${host_id}.csv \
            uu_ul
    else
        test_dir=$(timeout 10 ssh -o ConnectTimeout=5 -o ConnectionAttempts=1 $hostname "if [ -d ~/openairinterface5g/latest ]; then readlink -f ~/openairinterface5g/latest; else ls -dt ~/openairinterface5g/test_2026* 2>/dev/null | head -1; fi" 2>/dev/null)
        if [[ -z "$test_dir" ]]; then
            echo "  ⚠ No test directory found or connection failed, skipping..."
            continue
        fi
        echo "  Test directory: $test_dir"

        if ! timeout 180 ssh -o ConnectTimeout=5 -o ConnectionAttempts=1 $hostname "BLER_TX_POWER_DBM=$BLER_TX_POWER_DBM BLER_PLOSS_DB=$BLER_PLOSS_DB BLER_PC5_METHOD=$BLER_PC5_METHOD BLER_LOG_PREFIX=$BLER_LOG_PREFIX python3 ~/$BLER_SCRIPT_DIR_REL/extract_bler.py \
            $test_dir \
            /tmp/uu_ul_bler_${host_id}.csv \
            /tmp/uu_ul_ldpc_${host_id}.csv \
            uu_ul"; then
            echo "  ✗ Failed to extract data from $hostname, skipping..."
            continue
        fi

        timeout 10 scp -q -o ConnectTimeout=5 $hostname:/tmp/uu_ul_bler_${host_id}.csv $LOCAL_RESULTS/ 2>/dev/null
        timeout 10 scp -q -o ConnectTimeout=5 $hostname:/tmp/uu_ul_ldpc_${host_id}.csv $LOCAL_RESULTS/ 2>/dev/null
    fi
done

# Combine Uu UL BLER
echo "Combining Uu UL BLER..."
header_written=false
for host_id in "${host_ids[@]}"; do
    if [[ -f $LOCAL_RESULTS/uu_ul_bler_${host_id}.csv ]]; then
        if [[ "$header_written" == "false" ]]; then
            head -1 $LOCAL_RESULTS/uu_ul_bler_${host_id}.csv > $LOCAL_RESULTS/uu_ul_bler_combined.csv
            header_written=true
        fi
        tail -n +2 $LOCAL_RESULTS/uu_ul_bler_${host_id}.csv >> $LOCAL_RESULTS/uu_ul_bler_combined.csv 2>/dev/null
    fi
done
if [[ -f $LOCAL_RESULTS/uu_ul_bler_combined.csv ]]; then
    lines=$(wc -l < $LOCAL_RESULTS/uu_ul_bler_combined.csv)
    echo "✓ Combined Uu UL BLER: $((lines - 1)) data points"
else
    echo "✗ No Uu UL BLER data available"
fi

echo ""
echo "=========================================="
echo "Generating Focused Plots"
echo "=========================================="

# Generate nearby 4-panel plot
echo "→ Nearby (UE Rx) 4-panel plot..."
if python3 $BLER_SCRIPT_DIR/plot_results.py $LOCAL_RESULTS nearby; then
    echo "  ✓ nearby_bler_4panel.png"
else
    echo "  ✗ Failed to generate nearby plot (see error above)"
fi

# Generate syncref RX 4-panel plot
echo "→ Syncref RX 4-panel plot..."
if python3 $BLER_SCRIPT_DIR/plot_results.py $LOCAL_RESULTS syncref_rx; then
    echo "  ✓ syncref_rx_bler_4panel.png"
else
    echo "  ✗ Failed to generate syncref RX plot (see error above)"
fi

# Generate Uu DL 4-panel plot
echo "→ Uu DL (gNB→RelayUE) 4-panel plot..."
if python3 $BLER_SCRIPT_DIR/plot_results.py $LOCAL_RESULTS uu_dl; then
    echo "  ✓ uu_dl_bler_4panel.png"
else
    echo "  ✗ Failed to generate Uu DL plot (see error above)"
fi

# Generate Uu UL 4-panel plot (only BLER + HARQ, no LDPC)
echo "→ Uu UL (RelayUE→gNB) 2-panel plot..."
if python3 $BLER_SCRIPT_DIR/plot_results.py $LOCAL_RESULTS uu_ul; then
    echo "  ✓ uu_ul_bler_2panel.png"
else
    echo "  ✗ Failed to generate Uu UL plot (see error above)"
fi

echo ""
echo "=========================================="
echo "✓ Complete!"
echo "=========================================="
echo "Location: $LOCAL_RESULTS"
echo ""
echo "Generated plots:"
num_plots=$(ls -1 $LOCAL_RESULTS/*.png 2>/dev/null | wc -l)
if [[ $num_plots -gt 0 ]]; then
    ls -lh $LOCAL_RESULTS/*.png
else
    echo "  ⚠ No plots generated (check for errors above)"
fi
echo ""
echo "CSV files:"
num_csvs=$(ls -1 $LOCAL_RESULTS/*.csv 2>/dev/null | wc -l)
echo "  $num_csvs CSV files generated"
echo ""

# Open plots if GUI available
if [[ $num_plots -gt 0 ]] && command -v eog &> /dev/null && [[ -n "$DISPLAY" ]]; then
    echo "Opening plots in image viewer..."
    existing_plots=()
    for plot in nearby_bler_4panel syncref_rx_bler_4panel uu_dl_bler_4panel uu_ul_bler_2panel; do
        if [[ -f "$LOCAL_RESULTS/${plot}.png" ]]; then
            existing_plots+=("$LOCAL_RESULTS/${plot}.png")
        fi
    done
    if [[ ${#existing_plots[@]} -gt 0 ]]; then
        eog "${existing_plots[@]}" 2>/dev/null &
    fi
elif [[ $num_plots -gt 0 ]]; then
    echo "Note: GUI not available. View plots manually at: $LOCAL_RESULTS"
fi

echo "✓ Done!"
