#!/bin/bash
# Quick status check for full BLER tests on all machines
# Reads bler_hosts from config file

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
SL_TEST_CONFIG_FILE="${SCRIPT_DIR}/../run_sl_test_config.sh"

# Source config to get bler_hosts array
if [[ -f "$SL_TEST_CONFIG_FILE" ]]; then
    source "$SL_TEST_CONFIG_FILE" 2>/dev/null
else
    echo "ERROR: Config file not found: $SL_TEST_CONFIG_FILE"
    exit 1
fi

# Check if bler_hosts is defined
if [[ -z "${bler_hosts[@]}" ]]; then
    echo "ERROR: bler_hosts array not defined in config"
    echo "Define it in $SL_TEST_CONFIG_FILE:"
    echo "  bler_hosts=(host1 host2 host3 localhost)"
    exit 1
fi

echo "=========================================="
echo "Full BLER Test Status"
echo "=========================================="
echo "Checking all machines..."
echo ""

# Function to extract detailed status
get_detailed_status() {
    local log_file=$1
    local is_remote=$2
    local host=$3

    if [[ "$is_remote" == "true" ]]; then
        # Remote execution
        progress=$(ssh $host "tail -10000 $log_file 2>/dev/null | grep 'PROGRESS:' | tail -1" 2>/dev/null)
        elapsed=$(ssh $host "tail -10000 $log_file 2>/dev/null | grep 'Elapsed:' | tail -1" 2>/dev/null)
        remaining=$(ssh $host "tail -10000 $log_file 2>/dev/null | grep 'Est. Remaining:' | tail -1" 2>/dev/null)
        noise=$(ssh $host "tail -10000 $log_file 2>/dev/null | grep '   Noise Power:' | tail -1" 2>/dev/null)
        mcs=$(ssh $host "tail -10000 $log_file 2>/dev/null | grep '   MCS:' | tail -1" 2>/dev/null)
        harq=$(ssh $host "tail -20 $log_file 2>/dev/null | grep 'HARQ_STATS' | tail -1" 2>/dev/null)
    else
        # Local execution
        progress=$(tail -10000 $log_file 2>/dev/null | grep "PROGRESS:" | tail -1)
        elapsed=$(tail -10000 $log_file 2>/dev/null | grep "Elapsed:" | tail -1)
        remaining=$(tail -10000 $log_file 2>/dev/null | grep "Est. Remaining:" | tail -1)
        noise=$(tail -10000 $log_file 2>/dev/null | grep "   Noise Power:" | grep -v "Expected SINR" | tail -1)
        mcs=$(tail -10000 $log_file 2>/dev/null | grep "   MCS:" | tail -1)
        harq=$(tail -20 $log_file 2>/dev/null | grep "HARQ_STATS" | tail -1)
    fi

    if [[ -n "$progress" ]]; then
        echo "  ✓ Running: $progress"
        [[ -n "$noise" ]] && echo "     $noise"
        [[ -n "$mcs" ]] && echo "     $mcs"
        [[ -n "$elapsed" ]] && echo "     $elapsed"
        [[ -n "$remaining" ]] && echo "     $remaining"
    else
        echo "  ✓ Running (no progress logged yet)"
    fi

    if [[ -n "$harq" ]]; then
        echo "  📊 Latest: $harq"
    fi
}

# Calculate iterations per host
num_hosts=${#bler_hosts[@]}
iterations_per_host=$((${num_repeat:-12} / num_hosts))

# Check status for each host
host_idx=0
for hostname in "${bler_hosts[@]}"; do
    host_idx=$((host_idx + 1))
    host_id="host${host_idx}"

    # Calculate iteration range
    start=$(( (host_idx - 1) * iterations_per_host + 1 ))
    end=$(( host_idx * iterations_per_host ))
    if [[ $host_idx -eq $num_hosts ]]; then
        # Give remainder to last host
        remainder=$((${num_repeat:-12} % num_hosts))
        end=$((end + remainder))
    fi

    echo "→ ${host_id} (${hostname}) [Iterations ${start}-${end} | MCS 0-28]:"

    if [[ "$hostname" == "localhost" || "$hostname" == "local" ]]; then
        # Check localhost
        if ps aux | grep "bash run_sl_test.sh" | grep -v grep > /dev/null 2>&1; then
            get_detailed_status "$HOME/openairinterface5g/bler_${host_id}.log" "false" ""
        else
            echo "  ✗ Not running"
        fi
    else
        # Check remote host
        if ssh $hostname "ps aux | grep 'bash run_sl_test.sh' | grep -v grep" > /dev/null 2>&1; then
            get_detailed_status "~/openairinterface5g/bler_${host_id}.log" "true" "$hostname"
        else
            echo "  ✗ Not running"
        fi
    fi
    echo ""
done

echo "=========================================="
echo "Test Distribution (Iteration-Based):"
host_idx=0
for hostname in "${bler_hosts[@]}"; do
    host_idx=$((host_idx + 1))
    start=$(( (host_idx - 1) * iterations_per_host + 1 ))
    end=$(( host_idx * iterations_per_host ))
    if [[ $host_idx -eq $num_hosts ]]; then
        remainder=$((${num_repeat:-12} % num_hosts))
        end=$((end + remainder))
    fi
    printf "  host%d (%s): Iterations %d-%d | MCS 0-28\n" $host_idx "$hostname" $start $end
done
echo ""
echo "Test Parameters:"
echo "  • Tests per host: $((${#mcs_array[@]:-29} * ${#noise_power_array[@]:-17} * iterations_per_host))"
echo "  • ${duration:-85} seconds per test"
echo ""
echo "Monitor: watch -n 30 '$SCRIPT_DIR/check_test_status.sh'"
echo "=========================================="
