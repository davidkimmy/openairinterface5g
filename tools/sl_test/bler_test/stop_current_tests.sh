#!/bin/bash
#############################################################
# Stop All BLER Tests on All Machines
# Run this script from LOCALHOST to stop tests on all configured hosts
# Kills test scripts, softmodems, and cleans up all processes
#############################################################

BLER_SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
SL_TEST_CONFIG_FILE="${BLER_SCRIPT_DIR}/../run_sl_test_config.sh"

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

# Use configured OAI base directory or default
OAI_BASE_DIR="${OAI_BASE_DIR:-$HOME/openairinterface5g}"

echo "=========================================="
echo "Stopping All BLER Tests on All Machines"
echo "=========================================="
echo ""

# Function to stop tests on a remote machine
stop_remote_host() {
    local hostname=$1
    local host_id=$2

    echo "→ Stopping $host_id ($hostname)..."

    # Try SSH with timeout and limited retries
    ssh_success=false
    for attempt in 1 2 3; do
        if timeout 10 ssh -o ConnectTimeout=5 -o ConnectionAttempts=1 -o StrictHostKeyChecking=no $hostname 'bash -s' << 'ENDSSH'; then
            # Kill test script PIDs
            for pid_file in ${HOME}/openairinterface5g/bler_host*.pid; do
                if [[ -f "$pid_file" ]]; then
                    pid=$(cat "$pid_file" 2>/dev/null)
                    if [[ -n "$pid" ]]; then
                        kill -9 $pid 2>/dev/null
                        echo "  ✓ Killed PID $pid from $(basename $pid_file)"
                    fi
                fi
            done

            # Kill run_sl_test.sh scripts
            pkill -9 -f 'run_sl_test.sh' 2>/dev/null && echo "  ✓ Killed run_sl_test.sh processes"

            # Kill all softmodem processes by finding all PIDs
            for proc in nr-softmodem nr-uesoftmodem; do
                pids=$(ps aux | grep "$proc" | grep -v grep | awk '{print $2}')
                if [[ -n "$pids" ]]; then
                    for pid in $pids; do
                        sudo kill -9 $pid 2>/dev/null
                    done
                    echo "  ✓ Killed all $proc processes"
                fi
            done

            # Kill any nohup processes
            pkill -9 -f 'nohup.*run_sl_test' 2>/dev/null

            exit 0
ENDSSH
            if [[ $? -eq 0 ]]; then
                ssh_success=true
                break
            fi
        else
            echo "  ⚠ SSH attempt $attempt/3 failed (exit code: $?)"
            [[ $attempt -lt 3 ]] && sleep 2
        fi
    done

    if [[ "$ssh_success" == "false" ]]; then
        echo "  ✗ Failed to connect to $hostname after 3 attempts, skipping..."
        echo ""
        return
    fi

    echo "  ✓ $host_id stopped"
    echo ""
}

# Function to stop tests on localhost
stop_localhost() {
    local host_id=$1

    echo "→ Stopping $host_id (localhost)..."

    # Kill test script PIDs
    for pid_file in ${OAI_BASE_DIR}/bler_host*.pid; do
        if [[ -f "$pid_file" ]]; then
            pid=$(cat "$pid_file" 2>/dev/null)
            if [[ -n "$pid" ]]; then
                kill -9 $pid 2>/dev/null
                echo "  ✓ Killed PID $pid from $(basename $pid_file)"
            fi
        fi
    done

    # Kill run_sl_test.sh scripts
    pkill -9 -f "run_sl_test.sh" 2>/dev/null && echo "  ✓ Killed run_sl_test.sh processes"

    # Kill all softmodems
    for proc in nr-softmodem nr-uesoftmodem; do
        pids=$(ps aux | grep "$proc" | grep -v grep | awk '{print $2}')
        if [[ -n "$pids" ]]; then
            for pid in $pids; do
                sudo kill -9 $pid 2>/dev/null
            done
            echo "  ✓ Killed all $proc processes"
        fi
    done

    # Kill any nohup processes
    pkill -9 -f "nohup.*run_sl_test" 2>/dev/null

    echo "  ✓ localhost stopped"
    echo ""
}

# Stop all machines
host_idx=0
for hostname in "${bler_hosts[@]}"; do
    host_idx=$((host_idx + 1))
    host_id="host${host_idx}"

    if [[ "$hostname" == "localhost" || "$hostname" == "local" ]]; then
        stop_localhost "$host_id"
    else
        stop_remote_host "$hostname" "$host_id"
    fi
done

echo "=========================================="
echo "✓ All Tests Stopped on All Machines"
echo "=========================================="
echo ""
echo "Verifying..."
for hostname in "${bler_hosts[@]}"; do
    if [[ "$hostname" == "localhost" || "$hostname" == "local" ]]; then
        count=$(ps aux | grep -E '(nr-softmodem|nr-uesoftmodem|run_sl_test)' | grep -v grep | wc -l)
        echo "  localhost: $count processes remaining"
    else
        count=$(timeout 5 ssh -o ConnectTimeout=3 -o StrictHostKeyChecking=no $hostname "ps aux | grep -E '(nr-softmodem|nr-uesoftmodem|run_sl_test)' | grep -v grep | wc -l" 2>/dev/null || echo "?")
        echo "  $hostname: $count processes remaining"
    fi
done
echo ""
echo "To verify status:"
echo "  $BLER_SCRIPT_DIR/check_test_status.sh"
echo "=========================================="
