#!/bin/bash
#############################################################
# Process BLER Test Results
# Collects, combines, and plots BLER data from all 4 hosts
# Uses iteration-based distribution with full MCS range per host
#############################################################

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PARENT_DIR="$(dirname "$SCRIPT_DIR")"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# Source config first to get OAI_BASE_DIR
SL_TEST_CONFIG_FILE="$PARENT_DIR/run_sl_test_config.sh"
if [[ -f "$SL_TEST_CONFIG_FILE" ]]; then
    source "$SL_TEST_CONFIG_FILE" > /dev/null 2>&1
fi

# Use configured paths or defaults
OAI_BASE_DIR="${OAI_BASE_DIR:-$HOME/openairinterface5g}"
LOCAL_RESULTS="${BLER_RESULTS_DIR}_${TIMESTAMP}"

mkdir -p $LOCAL_RESULTS

echo "=========================================="
echo "BLER Results - Process & Plot"
echo "=========================================="
echo ""

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
        test_dir=$(ls -dt ${OAI_BASE_DIR}/test_2026* 2>/dev/null | head -1)
        if [[ -z "$test_dir" ]]; then
            echo "  ⚠ No test directory found"
            continue
        fi

        echo "  Test directory: $test_dir"
        echo "  Processing logs..."

        cd $SCRIPT_DIR
        python3 process_bler_local.py \
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
                # Find most recent test directory
                test_dir=\$(ls -dt ~/openairinterface5g/test_2026* 2>/dev/null | head -1)
                if [[ -z \"\$test_dir\" ]]; then
                    echo '  ⚠ No test directory found'
                    exit 1
                fi

                echo \"  Test directory: \$test_dir\"
                echo \"  Processing logs...\"

                cd ~/ci_script/bler_test
                python3 process_bler_local.py \
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
cd $SCRIPT_DIR

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
        test_dir=$(ls -dt ${OAI_BASE_DIR}/test_2026* 2>/dev/null | head -1)
        if [[ -z "$test_dir" ]]; then
            echo "  ⚠ No test directory found"
            continue
        fi
        echo "  Test directory: $test_dir"

        python3 $SCRIPT_DIR/extract_bler.py \
            "$test_dir" \
            $LOCAL_RESULTS/nearby_bler_${host_id}.csv \
            $LOCAL_RESULTS/nearby_ldpc_${host_id}.csv \
            nearby
    else
        # Try to find test directory with timeout
        test_dir=$(timeout 10 ssh -o ConnectTimeout=5 -o ConnectionAttempts=1 $hostname "ls -dt ~/openairinterface5g/test_2026* 2>/dev/null | head -1" 2>/dev/null)
        if [[ -z "$test_dir" ]]; then
            echo "  ⚠ No test directory found or connection failed, skipping..."
            continue
        fi
        echo "  Test directory: $test_dir"

        # Try SSH with timeout (increased for bilateral processing)
        if ! timeout 180 ssh -o ConnectTimeout=5 -o ConnectionAttempts=1 $hostname "python3 ~/ci_script/bler_test/extract_bler.py \
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
        test_dir=$(ls -dt ${OAI_BASE_DIR}/test_2026* 2>/dev/null | head -1)
        if [[ -z "$test_dir" ]]; then
            echo "  ⚠ No test directory found"
            continue
        fi
        echo "  Test directory: $test_dir"

        python3 $SCRIPT_DIR/extract_bler.py \
            "$test_dir" \
            $LOCAL_RESULTS/syncref_rx_bler_${host_id}.csv \
            $LOCAL_RESULTS/syncref_rx_ldpc_${host_id}.csv \
            syncref
    else
        # Try to find test directory with timeout
        test_dir=$(timeout 10 ssh -o ConnectTimeout=5 -o ConnectionAttempts=1 $hostname "ls -dt ~/openairinterface5g/test_2026* 2>/dev/null | head -1" 2>/dev/null)
        if [[ -z "$test_dir" ]]; then
            echo "  ⚠ No test directory found or connection failed, skipping..."
            continue
        fi
        echo "  Test directory: $test_dir"

        # Try SSH with timeout (increased for bilateral processing)
        if ! timeout 180 ssh -o ConnectTimeout=5 -o ConnectionAttempts=1 $hostname "python3 ~/ci_script/bler_test/extract_bler.py \
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
        test_dir=$(ls -dt ${OAI_BASE_DIR}/test_2026* 2>/dev/null | head -1)
        if [[ -z "$test_dir" ]]; then
            echo "  ⚠ No test directory found"
            continue
        fi
        echo "  Test directory: $test_dir"

        python3 $SCRIPT_DIR/extract_bler.py \
            "$test_dir" \
            $LOCAL_RESULTS/uu_dl_bler_${host_id}.csv \
            $LOCAL_RESULTS/uu_dl_ldpc_${host_id}.csv \
            uu_dl
    else
        test_dir=$(timeout 10 ssh -o ConnectTimeout=5 -o ConnectionAttempts=1 $hostname "ls -dt ~/openairinterface5g/test_2026* 2>/dev/null | head -1" 2>/dev/null)
        if [[ -z "$test_dir" ]]; then
            echo "  ⚠ No test directory found or connection failed, skipping..."
            continue
        fi
        echo "  Test directory: $test_dir"

        if ! timeout 180 ssh -o ConnectTimeout=5 -o ConnectionAttempts=1 $hostname "python3 ~/ci_script/bler_test/extract_bler.py \
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
        test_dir=$(ls -dt ${OAI_BASE_DIR}/test_2026* 2>/dev/null | head -1)
        if [[ -z "$test_dir" ]]; then
            echo "  ⚠ No test directory found"
            continue
        fi
        echo "  Test directory: $test_dir"

        python3 $SCRIPT_DIR/extract_bler.py \
            "$test_dir" \
            $LOCAL_RESULTS/uu_ul_bler_${host_id}.csv \
            $LOCAL_RESULTS/uu_ul_ldpc_${host_id}.csv \
            uu_ul
    else
        test_dir=$(timeout 10 ssh -o ConnectTimeout=5 -o ConnectionAttempts=1 $hostname "ls -dt ~/openairinterface5g/test_2026* 2>/dev/null | head -1" 2>/dev/null)
        if [[ -z "$test_dir" ]]; then
            echo "  ⚠ No test directory found or connection failed, skipping..."
            continue
        fi
        echo "  Test directory: $test_dir"

        if ! timeout 180 ssh -o ConnectTimeout=5 -o ConnectionAttempts=1 $hostname "python3 ~/ci_script/bler_test/extract_bler.py \
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
if python3 $SCRIPT_DIR/plot_results.py $LOCAL_RESULTS nearby; then
    echo "  ✓ nearby_bler_4panel.png"
else
    echo "  ✗ Failed to generate nearby plot (see error above)"
fi

# Generate syncref RX 4-panel plot
echo "→ Syncref RX 4-panel plot..."
if python3 $SCRIPT_DIR/plot_results.py $LOCAL_RESULTS syncref_rx; then
    echo "  ✓ syncref_rx_bler_4panel.png"
else
    echo "  ✗ Failed to generate syncref RX plot (see error above)"
fi

# Generate Uu DL 4-panel plot
echo "→ Uu DL (gNB→RelayUE) 4-panel plot..."
if python3 $SCRIPT_DIR/plot_results.py $LOCAL_RESULTS uu_dl; then
    echo "  ✓ uu_dl_bler_4panel.png"
else
    echo "  ✗ Failed to generate Uu DL plot (see error above)"
fi

# Generate Uu UL 4-panel plot (only BLER + HARQ, no LDPC)
echo "→ Uu UL (RelayUE→gNB) 2-panel plot..."
if python3 $SCRIPT_DIR/plot_results.py $LOCAL_RESULTS uu_ul; then
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
