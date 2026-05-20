#!/bin/bash
#############################################################
# Process BLER Test Results
# Collects, combines, and plots BLER data from all 4 hosts
# Uses iteration-based distribution with full MCS range per host
#############################################################

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PARENT_DIR="$(dirname "$SCRIPT_DIR")"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOCAL_RESULTS=~/openairinterface5g/bler_results_${TIMESTAMP}

mkdir -p $LOCAL_RESULTS

echo "=========================================="
echo "BLER Results - Process & Plot"
echo "=========================================="
echo ""

# Source config to get bler_hosts array
SL_TEST_CONFIG_FILE="$PARENT_DIR/run_sl_test_config.sh"
if [[ ! -f "$SL_TEST_CONFIG_FILE" ]]; then
    echo "ERROR: Config file not found at $SL_TEST_CONFIG_FILE"
    exit 1
fi

# Source config (suppress output)
source "$SL_TEST_CONFIG_FILE" > /dev/null 2>&1

# Check if bler_hosts array is defined
if [[ -z "${bler_hosts[@]}" ]]; then
    echo "ERROR: bler_hosts array not defined in config"
    echo "Add to $SL_TEST_CONFIG_FILE:"
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

# Display host configuration
echo "Machine Configuration:"
host_idx=0
for hostname in "${bler_hosts[@]}"; do
    host_idx=$((host_idx + 1))
    host_id="M${host_idx}"

    sl_test_config_file="$PARENT_DIR/run_sl_test_config_${host_id}.sh"
    if [[ -f "$sl_test_config_file" ]]; then
        iter_start=$(grep "^iteration_start=" "$sl_test_config_file" | cut -d'=' -f2)
        iter_end=$(grep "^iteration_end=" "$sl_test_config_file" | cut -d'=' -f2)
        echo "  ${host_id} (${hostname}): Iterations ${iter_start}-${iter_end} | MCS 0-28"
    else
        echo "  ${host_id} (${hostname}): Config not found"
    fi
done

echo ""
echo "Results will be saved to: $LOCAL_RESULTS"
echo ""

# Process each host
host_idx=0
for hostname in "${bler_hosts[@]}"; do
    host_idx=$((host_idx + 1))
    host_id="M${host_idx}"

    # Calculate MCS range for display (distribute 0-28 across hosts)
    total_mcs=29
    mcs_per_machine=$((total_mcs / num_hosts))
    mcs_start=$(((host_idx - 1) * mcs_per_machine))
    mcs_end=$((host_idx * mcs_per_machine - 1))
    if [[ $host_idx -eq $num_hosts ]]; then
        mcs_end=28  # Last host gets remainder
    fi
    mcs_str="${mcs_start},${mcs_end}"

    echo "=========================================="
    echo "Processing $host_id ($hostname) - MCS $mcs_str"
    echo "=========================================="

    if [[ "$hostname" == "localhost" ]]; then
        # Process locally
        test_dir=$(ls -dt ~/openairinterface5g/test_2026* 2>/dev/null | head -1)
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
            if timeout 10 ssh -o ConnectTimeout=5 -o ConnectionAttempts=1 $hostname "
                # Find most recent test directory
                test_dir=\$(ls -dt ~/openairinterface5g/test_2026* 2>/dev/null | head -1)
                if [[ -z \"\$test_dir\" ]]; then
                    echo '  ⚠ No test directory found'
                    exit 1
                fi

                echo \"  Test directory: \$test_dir\"
                echo \"  Processing logs...\"

                cd ~/ci_script
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
            " 2>/dev/null; then
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
    if [[ -f $LOCAL_RESULTS/bler_${machine}.csv ]]; then
        if [[ "$header_written" == "false" ]]; then
            head -1 $LOCAL_RESULTS/bler_${machine}.csv > $LOCAL_RESULTS/bler_combined.csv
            header_written=true
        fi
        tail -n +2 $LOCAL_RESULTS/bler_${machine}.csv >> $LOCAL_RESULTS/bler_combined.csv 2>/dev/null
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
    if [[ -f $LOCAL_RESULTS/bler_${machine}_pc5_rx.csv ]]; then
        if [[ "$header_written" == "false" ]]; then
            head -1 $LOCAL_RESULTS/bler_${machine}_pc5_rx.csv > $LOCAL_RESULTS/bler_combined_pc5_rx.csv
            header_written=true
        fi
        tail -n +2 $LOCAL_RESULTS/bler_${machine}_pc5_rx.csv >> $LOCAL_RESULTS/bler_combined_pc5_rx.csv 2>/dev/null
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
        test_dir=$(ls -dt ~/openairinterface5g/test_2026* 2>/dev/null | head -1)
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

        # Try SSH with timeout
        if ! timeout 30 ssh -o ConnectTimeout=5 -o ConnectionAttempts=1 $hostname "python3 ~/ci_script/extract_bler.py \
            $test_dir \
            /tmp/nearby_bler_${host_id}.csv \
            /tmp/nearby_ldpc_${host_id}.csv \
            nearby" 2>/dev/null; then
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
    if [[ -f $LOCAL_RESULTS/nearby_bler_${machine}.csv ]]; then
        if [[ "$header_written" == "false" ]]; then
            head -1 $LOCAL_RESULTS/nearby_bler_${machine}.csv > $LOCAL_RESULTS/nearby_bler_combined.csv
            header_written=true
        fi
        tail -n +2 $LOCAL_RESULTS/nearby_bler_${machine}.csv >> $LOCAL_RESULTS/nearby_bler_combined.csv 2>/dev/null
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
    if [[ -f $LOCAL_RESULTS/nearby_ldpc_${machine}.csv ]]; then
        if [[ "$header_written" == "false" ]]; then
            head -1 $LOCAL_RESULTS/nearby_ldpc_${machine}.csv > $LOCAL_RESULTS/nearby_ldpc_combined.csv
            header_written=true
        fi
        tail -n +2 $LOCAL_RESULTS/nearby_ldpc_${machine}.csv >> $LOCAL_RESULTS/nearby_ldpc_combined.csv 2>/dev/null
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
        test_dir=$(ls -dt ~/openairinterface5g/test_2026* 2>/dev/null | head -1)
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

        # Try SSH with timeout
        if ! timeout 30 ssh -o ConnectTimeout=5 -o ConnectionAttempts=1 $hostname "python3 ~/ci_script/extract_bler.py \
            $test_dir \
            /tmp/syncref_rx_bler_${host_id}.csv \
            /tmp/syncref_rx_ldpc_${host_id}.csv \
            syncref" 2>/dev/null; then
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
    if [[ -f $LOCAL_RESULTS/syncref_rx_bler_${machine}.csv ]]; then
        if [[ "$header_written" == "false" ]]; then
            head -1 $LOCAL_RESULTS/syncref_rx_bler_${machine}.csv > $LOCAL_RESULTS/syncref_rx_bler_combined.csv
            header_written=true
        fi
        tail -n +2 $LOCAL_RESULTS/syncref_rx_bler_${machine}.csv >> $LOCAL_RESULTS/syncref_rx_bler_combined.csv 2>/dev/null
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
    if [[ -f $LOCAL_RESULTS/syncref_rx_ldpc_${machine}.csv ]]; then
        if [[ "$header_written" == "false" ]]; then
            head -1 $LOCAL_RESULTS/syncref_rx_ldpc_${machine}.csv > $LOCAL_RESULTS/syncref_rx_ldpc_combined.csv
            header_written=true
        fi
        tail -n +2 $LOCAL_RESULTS/syncref_rx_ldpc_${machine}.csv >> $LOCAL_RESULTS/syncref_rx_ldpc_combined.csv 2>/dev/null
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
echo "Generating Focused Plots"
echo "=========================================="

# Generate nearby 4-panel plot
echo "→ Nearby (UE Rx) 4-panel plot..."
if python3 $SCRIPT_DIR/plot_results.py $LOCAL_RESULTS nearby 2>/dev/null; then
    echo "  ✓ nearby_bler_4panel.png"
else
    echo "  ✗ Failed to generate nearby plot"
fi

# Generate syncref RX 4-panel plot
echo "→ Syncref RX 4-panel plot..."
if python3 $SCRIPT_DIR/plot_results.py $LOCAL_RESULTS syncref_rx 2>/dev/null; then
    echo "  ✓ syncref_rx_bler_4panel.png"
else
    echo "  ✗ Failed to generate syncref RX plot"
fi

echo ""
echo "=========================================="
echo "✓ Complete!"
echo "=========================================="
echo "Location: $LOCAL_RESULTS"
echo ""
echo "Generated plots:"
ls -lh $LOCAL_RESULTS/*.png 2>/dev/null
echo ""
echo "CSV files:"
ls -lh $LOCAL_RESULTS/*.csv 2>/dev/null | wc -l
echo " CSV files generated"
echo ""

# Open plots if GUI available
if command -v eog &> /dev/null; then
    echo "Opening plots..."
    eog $LOCAL_RESULTS/nearby_bler_4panel.png \
        $LOCAL_RESULTS/syncref_rx_bler_4panel.png &
fi

echo "✓ Done!"
