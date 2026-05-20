#!/bin/bash
#############################################################
# Standalone shell script
# Usage:
#   Shell> ./run_sl_test.sh [-d <base_dir>] [-g <0|1>]
#############################################################

timestamp=$(date +"%Y%m%d_%H%M%S")
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")

# Defaults
base_dir="$SCRIPT_DIR"
USE_GNOME=0
sa_flag=""
ext_clock_flag=""
ensure_ping_test_time=0  # Default: 0 (strict duration)

# Safe SSH wrapper - only SSH if host is not local
safe_ssh() {
    local host=$1
    shift
    local cmd="$@"

    if [[ "$host" == "local" ]] || [[ "$host" == "" ]] || [[ "$host" == "localhost" ]]; then
        # Execute locally
        bash -c "$cmd"
    else
        # Execute remotely
        ssh "$host" "$cmd"
    fi
}

# Override from config file
# Support custom config file via BLER_CONFIG_FILE env variable (for distributed testing)
if [[ -n "$BLER_CONFIG_FILE" ]]; then
    # If BLER_CONFIG_FILE is absolute path, use it directly; otherwise prepend SCRIPT_DIR
    if [[ "$BLER_CONFIG_FILE" == /* ]] || [[ "$BLER_CONFIG_FILE" == ~* ]]; then
        SL_TEST_CONFIG_FILE="${BLER_CONFIG_FILE/#\~/$HOME}"
    else
        SL_TEST_CONFIG_FILE="$SCRIPT_DIR/$BLER_CONFIG_FILE"
    fi
else
    SL_TEST_CONFIG_FILE="$SCRIPT_DIR/run_sl_test_config.sh"
fi

echo "DEBUG: Sourcing config from: $SL_TEST_CONFIG_FILE"
if [[ ! -f "$SL_TEST_CONFIG_FILE" ]]; then
    echo "ERROR: Config file not found: $SL_TEST_CONFIG_FILE"
    exit 1
fi
source "$SL_TEST_CONFIG_FILE"
echo "DEBUG: After sourcing config - enabled_tests: ${enabled_tests[@]}"
echo "DEBUG: After sourcing config - test_profile: $test_profile"
[[ -n "$base_log_dir" ]] && base_dir="${base_log_dir/#\~/$HOME}"
[[ "$use_external_clock" == "1" ]] && ext_clock_flag=" --clock-source 1 --time-source 1"
[[ -n "$use_gnome" ]] && USE_GNOME="$use_gnome"
[[ "$use_sa" == "1" ]] && sa_flag="--sa"

# Initialize default sleep timings (can be overridden by config)
declare -A sleep_timing

# Apply extended delays for slower systems or specific environments
# Only called if use_extended_delays=1 in config file
apply_extended_delays() {
    sleep_timing["tun_wait_1st"]=3
    sleep_timing["tun_wait_2nd"]=3
    sleep_timing["sync_stab_30s"]=30
    sleep_timing["sync_stab_45s_v1"]=45
    sleep_timing["sync_stab_45s_v2"]=45
}

# Apply extended delays if enabled in config
if [[ "$use_extended_delays" == "1" ]]; then
    apply_extended_delays
fi

# Parallel mode: auto-generate host-specific configs and launch
if [[ "$parallel_mode" == "true" ]]; then
    echo "=========================================="
    echo "Parallel Mode Detected"
    echo "=========================================="

    # Check if bler_hosts array is defined in config
    if [[ -z "${bler_hosts[@]}" ]]; then
        echo "ERROR: bler_hosts array not defined in config file"
        echo "Add to your config file:"
        echo "  bler_hosts=(l3 l4 l5 localhost)"
        exit 1
    fi

    num_hosts=${#bler_hosts[@]}
    echo "Found $num_hosts hosts in bler_hosts array"
    echo ""

    # Calculate iterations per host
    total_iterations=${num_repeat:-10}
    iterations_per_host=$((total_iterations / num_hosts))
    remainder=$((total_iterations % num_hosts))

    echo "Generating configs and launching tests..."
    echo "Total iterations: $total_iterations"
    echo "Per host: $iterations_per_host"
    echo ""

    # Generate config and launch for each host
    host_idx=0
    for hostname in "${bler_hosts[@]}"; do
        host_idx=$((host_idx + 1))
        host_id="host${host_idx}"

        sl_test_config_file="$SCRIPT_DIR/run_sl_test_config_${host_id}.sh"

        # Calculate iteration range for this host
        start=$(( (host_idx - 1) * iterations_per_host + 1 ))
        end=$(( host_idx * iterations_per_host ))

        # Give remainder iterations to last host
        if [[ $host_idx -eq $num_hosts ]]; then
            end=$((end + remainder))
        fi

        echo "→ ${host_id} (${hostname}): iterations ${start}-${end}"

        # Generate config file
        cat > "$sl_test_config_file" << EOF
#!/bin/bash
# ${host_id} Config - Auto-generated from ${SL_TEST_CONFIG_FILE}
# Generated: $(date)
# Host: ${hostname}

enabled_tests=(
    rfsim_slmode1_bler_test_on_local_host
)

base_log_dir="~/openairinterface5g"
use_gnome=0

# ${host_id}: Iterations ${start}-${end}
num_repeat=$((end - start + 1))
iteration_start=${start}
iteration_end=${end}

# Full MCS range: 0-28 (${#mcs_array[@]} values)
mcs_array=(${mcs_array[@]})
duration=${duration}

# Noise power array: ${#noise_power_array[@]} values
noise_power_array=(${noise_power_array[@]})

ploss_db=${ploss_db}
csi_acquisition=${csi_acquisition:-0}
psfch_period=${psfch_period:-2}

ping_count=${ping_count:-975}
ping_interval=${ping_interval:-0.0667}

# Softmodem log files
softmodem_log_files=(
    result_gNB.log
    result_nrUE.log
    result_syncref.log
    result_nearby.log
    result_nrUE_syncref.log
)

echo "=========================================="
echo "${host_id} Config (${hostname})"
echo "=========================================="
echo "Iterations: \${iteration_start} to \${iteration_end} (\${num_repeat} iterations)"
echo "MCS Array: \${#mcs_array[@]} values (0-28, full range)"
echo "Noise Powers: \${#noise_power_array[@]} values"
echo "Duration: \${duration}s per test"
echo "Total tests: \$(((\${#mcs_array[@]}) * (\${#noise_power_array[@]}) * num_repeat))"
echo "=========================================="
EOF
        chmod +x "$sl_test_config_file"

        # Distribute config to remote host if not localhost
        if [[ "$hostname" != "localhost" && "$hostname" != "local" ]]; then
            echo "  → Copying config to ${hostname}..."
            scp -q "$sl_test_config_file" "${hostname}:~/ci_script/" 2>/dev/null || echo "  ⚠ Failed to copy config to ${hostname}"
        fi
    done

    echo ""
    echo "=========================================="
    echo "Launching Tests on All Machines"
    echo "=========================================="

    # Launch on each host
    host_idx=0
    for hostname in "${bler_hosts[@]}"; do
        host_idx=$((host_idx + 1))
        host_id="host${host_idx}"
        config_name="run_sl_test_config_${host_id}.sh"
        log_file="~/openairinterface5g/bler_${host_id}.log"
        pid_file="~/openairinterface5g/bler_${host_id}.pid"

        echo ""
        echo "→ ${host_id} (${hostname})"

        if [[ "$hostname" == "localhost" || "$hostname" == "local" ]]; then
            # Launch locally
            cd ~/ci_script
            BLER_CONFIG_FILE="$config_name" nohup bash run_sl_test.sh > "${log_file/#\~/$HOME}" 2>&1 &
            echo $! > "${pid_file/#\~/$HOME}"
            echo "  ✓ Started locally (PID: $!)"
        else
            # Launch remotely via SSH
            ssh -n -f "$hostname" "cd ~/ci_script && BLER_CONFIG_FILE=$config_name nohup bash run_sl_test.sh > $log_file 2>&1 & echo \$! > $pid_file" 2>/dev/null
            if [[ $? -eq 0 ]]; then
                echo "  ✓ Started on ${hostname}"
            else
                echo "  ✗ Failed to start on ${hostname}"
            fi
            sleep 2  # Configurable: default 2s - wait for SSH remote process to start
        fi
    done

    echo ""
    echo "=========================================="
    echo "✓ All Tests Launched!"
    echo "=========================================="
    echo ""
    echo "Monitor progress:"
    echo "  bash ~/ci_script/check_test_status.sh"
    echo ""
    echo "Logs:"
    host_idx=0
    for hostname in "${bler_hosts[@]}"; do
        host_idx=$((host_idx + 1))
        host_id="host${host_idx}"
        echo "  ${host_id} (${hostname}): ~/openairinterface5g/bler_${host_id}.log"
    done
    echo ""
    echo "Expected completion: ~58 hours (all machines in parallel)"
    echo "=========================================="
    exit 0
fi

# Override from command line (highest priority)
while getopts "d:g:" opt; do
    case $opt in
        d) base_dir="$OPTARG" ;;
        g) USE_GNOME="$OPTARG" ;;
        *) echo "Usage: $0 [-d <base_dir>] [-g <0|1>]"; exit 1 ;;
    esac
done
shift $((OPTIND - 1))

log_dir="$base_dir/test_${timestamp}"
mkdir -p "$log_dir"
ln -sfn "test_${timestamp}" "$base_dir/latest"
echo "Log files will be saved at $log_dir"

test_summary_file="$log_dir/test_summary_${timestamp}.csv"

CONF_PATH=$HOME/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF

# Check if any BLER tests are enabled (they run locally, skip SSH lookups)
is_bler_test=false
for test_entry in "${enabled_tests[@]}"; do
    # Extract test name (before any ':' separator for CSI/PSFCH params)
    test_name="${test_entry%%:*}"
    if [[ "$test_name" == *"bler"* ]]; then
        is_bler_test=true
        break
    fi
done

if [[ "$is_bler_test" == "false" ]]; then
    # Standard tests - perform SSH config lookups
    REMOTE_UE_HOST="remote_ue" # host name in the ~/.ssh/config
    REMOTE_HOST_IP=$(ssh -G $REMOTE_UE_HOST 2>/dev/null | awk '/^hostname / {print $2}')
    echo "Remote Host IP address = " $REMOTE_HOST_IP

    LOCAL_HOST="local" # host name in the ~/.ssh/config
    LOCAL_HOST_IP=$(ssh -G $LOCAL_HOST 2>/dev/null | awk '/^hostname / {print $2}') #  #LOCAL_HOST_IP=$(ip route get 1.2.3.4 | awk '{print $7}')
    echo "Local Host IP address = " $LOCAL_HOST_IP

    RELAY_UE_HOST="relay_ue" # host name in the ~/.ssh/config
    RELAY_UE_HOST_IP=$(ssh -G $RELAY_UE_HOST 2>/dev/null | awk '/^hostname / {print $2}')
    echo "Relay UE Host IP address = " $RELAY_UE_HOST_IP

    NR_UE_HOST="nr_ue" # host name in the ~/.ssh/config
    NR_UE_HOST_IP=$(ssh -G $NR_UE_HOST 2>/dev/null | awk '/^hostname / {print $2}')
    echo "nrUE Host IP address = " $NR_UE_HOST_IP

    GNB_HOST="gNB" # host name in the ~/.ssh/config
    GNB_HOST_IP=$(ssh -G $GNB_HOST 2>/dev/null | awk '/^hostname / {print $2}')
    echo "gNB Host IP address = " $GNB_HOST_IP
else
    # BLER tests run locally - set to "local" to bypass SSH
    REMOTE_UE_HOST="local"
    REMOTE_HOST_IP="127.0.0.1"
    LOCAL_HOST="local"
    LOCAL_HOST_IP="127.0.0.1"
    RELAY_UE_HOST="local"
    RELAY_UE_HOST_IP="127.0.0.1"
    NR_UE_HOST="local"
    NR_UE_HOST_IP="127.0.0.1"
    GNB_HOST="local"
    GNB_HOST_IP="127.0.0.1"
    echo "BLER test mode: skipping SSH config lookups (local execution)"
fi

# Read default values from config files (before any tests modify them)
DEFAULT_CSI_ACQ=$(grep "sl_CSI_Acquisition" $CONF_PATH/sl_sync_ref.conf | grep -oP '\d+' | head -1)
DEFAULT_PSFCH_PERIOD=$(grep "sl_PSFCH_Period" $CONF_PATH/sl_sync_ref.conf | grep -oP '\d+' | head -1)
echo "Default CSI Acquisition = " $DEFAULT_CSI_ACQ
echo "Default PSFCH Period = " $DEFAULT_PSFCH_PERIOD

TX_GAIN=0      # Default: 0
RX_GAIN=110    # Default: 110

#############################################################
### Basic functions ###
#############################################################
set_atten() {
    curl http://169.254.10.10/:CHAN:1:2:3:4:SETATT:$@
    sleep 1;
    curl http://169.254.10.10/:ATT?
}

check_same_str() {
    [[ $1 == $2 ]] && echo "Result:  Pass" || echo "Result:  Fail"
}

check_same_val() {
    [[ $1 -eq $2 ]] && echo "Result:  Pass" || echo "Result:  Fail"
}

is_numeric() {
    [[ $1 =~ ^[+-]?[0-9]+([.][0-9]+)?$ ]] && return 0 || return 1
}

is_same() {
    is_numeric $1 && is_numeric $2 && check_same_val $1 $2 || check_same_str $1 $2
}

sleep_time() {
    [[ $mectric == "retx" ]] && sleep 35 || [[ $mectric == "bler" ]] && sleep 170 || sleep 5
}

check_process() {
    pid=$(pgrep $1)
    [ -z "$pid" ] && echo "Program not running." || echo "Program running with $pid"
}

kill_process() {
    printf "Removing %s\n" "$*"
    for process_name in "$@"; do
        sudo killall -KILL "$process_name" 2>/dev/null
        sleep 2
        pids=$(pgrep "$process_name")
        if [ -n "$pids" ]; then
            for pid in $pids; do
                sudo kill -9 "$pid" 2>/dev/null
            done
        fi
    done
}

kill_all() {
    [[ $# -ge 1 ]] && host_name=$1
    [[ $# -ge 2 ]] && target=$2
    if [[ $host_name == 'local' ]]; then
        kill_process $target
    else
        ssh $host_name "$(declare -f kill_process); kill_process ping $target" ; sleep 3
    fi
}

restart_core_network() {
    local cn_dir="${CN_DIR:-$HOME/oai-cn5g}"

    echo "Restarting core network..."

    # Check if CN directory exists
    if [[ ! -d "$cn_dir" ]]; then
        echo "ERROR: Core network directory not found at $cn_dir"
        echo "Please set CN_DIR in config or ensure ~/oai-cn5g exists"
        return 1
    fi

    # Check if docker-compose file exists (support both .yml and .yaml)
    local compose_file=""
    if [[ -f "$cn_dir/docker-compose.yml" ]]; then
        compose_file="docker-compose.yml"
    elif [[ -f "$cn_dir/docker-compose.yaml" ]]; then
        compose_file="docker-compose.yaml"
    else
        echo "ERROR: docker-compose.yml or docker-compose.yaml not found in $cn_dir"
        return 1
    fi

    # Stop existing containers
    echo "  Stopping existing containers..."
    cd "$cn_dir" && docker compose down
    if [[ $? -ne 0 ]]; then
        echo "WARNING: Failed to stop containers (may not be running)"
    fi
    sleep 3  # Wait for clean shutdown

    # Ensure docker service is running (try with sudo if needed)
    if ! systemctl is-active --quiet docker.service; then
        echo "  Docker service not running, attempting to start..."
        if sudo systemctl start docker.service 2>/dev/null; then
            echo "  Docker service started"
            sleep 1
        else
            echo "  WARNING: Could not start docker service (may already be running or need manual start)"
        fi
    fi

    # Start containers
    echo "  Starting containers..."
    cd "$cn_dir" && docker compose up -d
    if [[ $? -ne 0 ]]; then
        echo "ERROR: Failed to start core network containers"
        return 1
    fi

    sleep 3  # Wait for initialization

    # Verify containers are running
    local running_containers=$(cd "$cn_dir" && docker compose ps --status running | grep -c "Up")
    if [[ $running_containers -eq 0 ]]; then
        echo "ERROR: No containers are running after startup"
        echo "Check logs with: cd $cn_dir && docker compose logs"
        return 1
    fi

    echo "Core network restarted successfully ($running_containers containers running)"
    return 0
}

block_comment() {
: <<'END_COMMENT'
END_COMMENT
}
#############################################################
### Library functions ###
#############################################################
wait_for_tun_interface() {
    local iface=$1
    local host=$2
    local timeout=$3

    echo "Waiting for interface $iface on $host (timeout: ${timeout}s)..."
    local elapsed=$((3 + ${sleep_timing[tun_wait_1st]}))
    sleep $elapsed # Configurable: default 5s - initial wait for TUN

    while [ $elapsed -lt $timeout ]; do
        echo "Checking for $iface... (${elapsed}s elapsed)"
        local interface_exists=0
        if [[ "$host" == "local" ]] || [[ "$host" == "" ]] || [[ "$host" == "localhost" ]]; then
            if ifconfig | grep -q "$iface"; then
                interface_exists=1
            fi
        else
            if safe_ssh "$host" "ifconfig | grep -q $iface"; then
                interface_exists=1
            fi
        fi
        if [ $interface_exists -eq 1 ]; then
            echo "Tunnel interface $iface detected on $host, waiting for interface sync..."
            sleep $((2 + ${sleep_timing[tun_wait_2nd]}))  # Configurable: default 5s - wait for interface sync
            return 0
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    echo "ERROR: $iface not available on $host after ${timeout}s"
    return 1
}

wait_for_pc5_sync() {
    local log_file="$HOME/result_nearby.log"
    local timeout=${1:-60}

    echo "Waiting for PC5 sync (timeout: ${timeout}s)..."
    local elapsed=0
    while [ $elapsed -lt $timeout ]; do
        if [ -f "$log_file" ] && grep -q "RX SLSS REQ" "$log_file"; then
            echo "PC5 Sync Achieved (${elapsed}s elapsed)"
            return 0
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    echo "WARNING: PC5 sync not detected after ${timeout}s"
    return 1
}

validate_test_type_for_local_host() {
    local test_type=$1
    local test_name=$2

    if [[ $test_type == "usrp" ]]; then
        echo "ERROR: ${test_name} cannot be used with test_type='usrp'"
        echo "       USRP tests require two separate machines with USRP hardware"
        echo "       Use a test function with two_hosts instead"
        return 1
    fi
    return 0
}

print_test_summary_header() {
    if [ ! -f "$test_summary_file" ]; then
        echo "Test Name,Itrn,Num Hosts,MCS,Runtime,Ping Rate,PSSCH Rate1,PSSCH Rate2,PSSCH Total,Result" \
            | tee -a "$test_summary_file"
    fi
}

print_test_summary() {
    local test_name=$1
    local iteration=$2
    local num_hosts=$3
    local mcs=$4
    local runtime=$5
    local tx_packets=$6
    local rx_packets=$7
    local test_result=$8  # "PASS" or "FAIL"

    # Calculate ping pass rate
    if [ "$tx_packets" -gt 0 ]; then
        local ping_rate=$((rx_packets * 100 / tx_packets))
        local ping_rate_str="${ping_rate}%"
    else
        local ping_rate_str="N/A"
    fi

    # Calculate PSSCH pass rates (RX/TX format for success rate)
    # Rate1: syncref TX -> nearby RX
    if [ "$LAST_PSSCH_TX_SYNCREF" -gt 0 ]; then
        local pssch_rate1=$((LAST_PSSCH_RX_NEARBY * 100 / LAST_PSSCH_TX_SYNCREF))
        local pssch_rate1_str="${LAST_PSSCH_RX_NEARBY}/${LAST_PSSCH_TX_SYNCREF} (${pssch_rate1}%)"
    else
        local pssch_rate1_str="N/A"
    fi

    # Rate2: nearby TX -> syncref RX
    if [ "$LAST_PSSCH_TX_NEARBY" -gt 0 ]; then
        local pssch_rate2=$((LAST_PSSCH_RX_SYNCREF * 100 / LAST_PSSCH_TX_NEARBY))
        local pssch_rate2_str="${LAST_PSSCH_RX_SYNCREF}/${LAST_PSSCH_TX_NEARBY} (${pssch_rate2}%)"
    else
        local pssch_rate2_str="N/A"
    fi

    # Calculate total PSSCH rate (aggregated both directions)
    # For single-host tests, only count syncref; for multi-host, aggregate both
    if [ "$num_hosts" -eq 1 ]; then
        # Single host: only syncref direction exists
        if [ "$LAST_PSSCH_TX_SYNCREF" -gt 0 ]; then
            local pssch_total=$((LAST_PSSCH_RX_NEARBY * 100 / LAST_PSSCH_TX_SYNCREF))
            local pssch_total_str="${pssch_total}%"
        else
            local pssch_total_str="N/A"
        fi
    else
        # Multi-host: aggregate both directions
        local total_tx=$((LAST_PSSCH_TX_SYNCREF + LAST_PSSCH_TX_NEARBY))
        local total_rx=$((LAST_PSSCH_RX_SYNCREF + LAST_PSSCH_RX_NEARBY))
        if [ "$total_tx" -gt 0 ]; then
            local pssch_total=$((total_rx * 100 / total_tx))
            local pssch_total_str="${pssch_total}%"
        else
            local pssch_total_str="N/A"
        fi
    fi

    # Print header if this is the first test
    print_test_summary_header

    # Print one row for this test
    echo "$test_name,$iteration,$num_hosts,${mcs:-N/A},${runtime}s,$ping_rate_str,$pssch_rate1_str,$pssch_rate2_str,$pssch_total_str,$test_result" \
        | tee -a "$test_summary_file"
}

print_runtime() {
    local start_time=$1
    local end_time=$2
    local elapsed=$((end_time - start_time))
    local minutes=$((elapsed / 60))
    local seconds=$((elapsed % 60))
    printf "Elapsed Time: %d seconds (%dm %ds)\n" $elapsed $minutes $seconds
}

get_ping_stats_tuple() {
    local ping_output_file=$1
    local transmitted=$(grep -oP '\d+(?= packets transmitted)' "$ping_output_file" | head -1)
    local received=$(grep -oP '\d+(?= received)' "$ping_output_file" | head -1)
    local loss=$(grep -oP '\d+(?=% packet loss)' "$ping_output_file" | head -1)

    # Return both transmitted and received as "transmitted received" tuple to stdout
    echo "${transmitted:-0} ${received:-0}"
}

get_pssch_stats() {
    local log_file=$1
    local ue_name=$2  # "syncref" or "nearby"

    if [ ! -f "$log_file" ]; then
        echo "0 0"
        return
    fi

    # Get the last PSSCH Stats line from the log
    # Format: [UE0] 512:19 PSSCH Stats: TX 26, RX ok 17, RX not ok (0/0/0/0)
    local pssch_line=$(grep "PSSCH Stats:" "$log_file" | tail -1)

    if [ -z "$pssch_line" ]; then
        echo "0 0"
        return
    fi

    # Extract TX and RX ok values
    local tx=$(echo "$pssch_line" | grep -oP 'TX \K\d+')
    local rx_ok=$(echo "$pssch_line" | grep -oP 'RX ok \K\d+')

    # Return as tuple: tx rx_ok
    echo "${tx:-0} ${rx_ok:-0}"
}


check_ping_result() {
    local transmitted=$1
    local received=$2
    local threshold_percent=${3:-60}  # Default 60% threshold

    if [ "$transmitted" -eq 0 ]; then
        return 1
    fi

    # Calculate success percentage
    local success_percent=$((received * 100 / transmitted))

    if [ "$success_percent" -ge "$threshold_percent" ]; then
        return 0
    else
        return 1
    fi
}

check_and_restore_default() {
    local config_file=$1
    local param_name=$2
    local default_value=$3

    # Extract current value from config file
    local current_value=$(grep "$param_name" "$config_file" | grep -oP '\d+' | head -1)

    if [ "$current_value" != "$default_value" ]; then
        echo "Restoring $config_file: $param_name from $current_value to $default_value"
        sed -i "s/$param_name *= $current_value/$param_name = $default_value/g" "$config_file"
    fi
}

check_and_restore_default_remote() {
    local remote_host=$1
    local config_file=$2
    local param_name=$3
    local default_value=$4

    # Extract current value from remote config file
    local current_value=$(safe_ssh "$remote_host" "grep '$param_name' '$config_file' | grep -oP '\d+' | head -1")

    if [ "$current_value" != "$default_value" ]; then
        echo "Restoring remote $config_file: $param_name from $current_value to $default_value"
        safe_ssh "$remote_host" "sed -i 's/$param_name *= $current_value/$param_name = $default_value/g' '$config_file'"
    fi
}

find_user_name() {
    local host=$1
    if [[ $host == "local" ]]; then
        whoami
    else
        ssh -G "$host" 2>/dev/null | awk '/^user / {print $2}'
    fi
}

sync_default_config_params() {
    # Sync sl_PSFCH_Period and sl_CSI_Acquisition across all config files
    # Args: csi_acq psfch_period
    local csi_acq=${1:-0}
    local psfch_period=${2:-2}

    echo "Syncing default config params: CSI=$csi_acq, PSFCH=$psfch_period"

    # Local sidelink configs
    sed -i "s/\(sl_CSI_Acquisition[[:space:]]*=[[:space:]]*\)[0-9]\+/\1${csi_acq}/g" "$CONF_PATH/sl_sync_ref.conf"
    sed -i "s/\(sl_PSFCH_Period[[:space:]]*=[[:space:]]*\)[0-9]\+/\1${psfch_period}/g" "$CONF_PATH/sl_sync_ref.conf"
    sed -i "s/\(sl_CSI_Acquisition[[:space:]]*=[[:space:]]*\)[0-9]\+/\1${csi_acq}/g" "$CONF_PATH/sl_ue1.conf"
    sed -i "s/\(sl_PSFCH_Period[[:space:]]*=[[:space:]]*\)[0-9]\+/\1${psfch_period}/g" "$CONF_PATH/sl_ue1.conf"

    # Local gNB relay config
    local gnb_conf="$HOME/openairinterface5g/targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band78.fr1.106PRB.usrpb210_relay_ue.conf"
    if [[ -f "$gnb_conf" ]]; then
        sed -i "s/\(sl_CSI_Acquisition[[:space:]]*=[[:space:]]*\)[0-9]\+/\1${csi_acq}/g" "$gnb_conf"
        sed -i "s/\(sl_PSFCH_Period[[:space:]]*=[[:space:]]*\)[0-9]\+/\1${psfch_period}/g" "$gnb_conf"
    fi

    # Remote config if REMOTE_UE_HOST is set
    if [[ -n "$REMOTE_UE_HOST" ]] && [[ "$REMOTE_UE_HOST" != "local" ]]; then
        local remote_user=$(find_user_name "$REMOTE_UE_HOST")
        local remote_conf="/home/$remote_user/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf"
        safe_ssh "$REMOTE_UE_HOST" "sed -i 's/\(sl_CSI_Acquisition[[:space:]]*=[[:space:]]*\)[0-9]\+/\1${csi_acq}/g' $remote_conf" 2>/dev/null
        safe_ssh "$REMOTE_UE_HOST" "sed -i 's/\(sl_PSFCH_Period[[:space:]]*=[[:space:]]*\)[0-9]\+/\1${psfch_period}/g' $remote_conf" 2>/dev/null
    fi

    echo "  ✓ Config params synced"
}

sync_config_files() {
    local remote_host=$1
    local config_files=("sl_sync_ref.conf" "sl_ue1.conf")

    if [[ $remote_host == "local" ]] || [[ -z "$remote_host" ]]; then
        echo "No remote host specified, skipping config sync"
        return 0
    fi

    local remote_user=$(find_user_name "$remote_host")
    local remote_conf_path="/home/$remote_user/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF"

    echo "Syncing configuration files to $remote_host..."
    for conf_file in "${config_files[@]}"; do
        local local_file="$CONF_PATH/$conf_file"
        local remote_file="$remote_conf_path/$conf_file"

        if [[ -f "$local_file" ]]; then
            echo "  Copying $conf_file to $remote_host"
            scp "$local_file" "$remote_host:$remote_file" 2>/dev/null
            if [ $? -eq 0 ]; then
                echo "  ✓ $conf_file synced successfully"
            else
                echo "  ✗ Failed to sync $conf_file"
            fi
        else
            echo "  ✗ Local file not found: $local_file"
        fi
    done
    echo "Configuration sync complete"
}

cleanup_old_logs() {
    for f in "${softmodem_log_files[@]}"; do
        rm -f "$HOME/$f"
    done
    GNOME_WIN_IDX=0
}

save_softmodem_logs() {
    local test_name=$1
    local ts=$(date +"%Y%m%d_%H%M%S")
    for f in "${softmodem_log_files[@]}"; do
        if [[ -f "$HOME/$f" ]]; then
            local basename=$(basename "$f" .log)
            mv "$HOME/$f" "$log_dir/${basename}_${test_name}_${ts}.log"
        fi
    done
}

GNOME_WIN_IDX=0
GNOME_WIN_POS=("80x20+0+0" "80x20+960+0" "80x20+0+540" "80x20+960+540" "80x10+480+780")

run_cmd() {
    [[ $# -ge 1 ]] && host_name=$1
    [[ $# -ge 2 ]] && cmd=$2
    [[ $# -ge 3 ]] && log_file=$3

    local geom="${GNOME_WIN_POS[$((GNOME_WIN_IDX % ${#GNOME_WIN_POS[@]}))]}"
    GNOME_WIN_IDX=$((GNOME_WIN_IDX + 1))

    if [[ $host_name == "local" ]] || [[ $host_name == "" ]] ; then
        if [ $USE_GNOME -ge 1 ]; then
            gnome-terminal --geometry=$geom -- bash -c "source ~/.bashrc 2>/dev/null; $cmd 2>&1 | tee $log_file" &
        else
            bash -c "source ~/.bashrc 2>/dev/null; $cmd" 2>&1 | tee $log_file &
        fi
    else
        if [ $USE_GNOME -ge 1 ]; then
            gnome-terminal --geometry=$geom -- bash -c "ssh $host_name '$cmd' 2>&1 | tee $log_file" &
        else
            bash -c "ssh $host_name '$cmd'" 2>&1 | tee $log_file &
        fi
    fi
}

evaluate_ping_test() {
    [[ $# -ge 1 ]] && host_name=$1
    [[ $# -ge 2 ]] && src_if=$2
    [[ $# -ge 3 ]] && dest_ip=$3
    [[ $# -ge 4 ]] && local sl_mode=$4
    [[ $# -ge 5 ]] && local test_name=$5

    local user_name
    user_name=$(find_user_name "$host_name")
    # Generate unique filename with timestamp
    local timestamp=$(date +%Y%m%d_%H%M%S)

    if [[ $host_name == "local" ]]; then
        # Run ping locally
        ping_output="$log_dir/ping_result_${test_name}_${timestamp}.txt"
        cmd="ping -c 15 -I $src_if $dest_ip"
        echo "Ping command: $cmd"

        # Save command to commands.txt
        echo "=== Ping Command (host: $host_name) ===" >> "$log_dir/commands.txt"
        echo "$cmd" >> "$log_dir/commands.txt"
        echo "" >> "$log_dir/commands.txt"

        run_cmd $host_name "$cmd" $ping_output
    else
        # Run ping on remote host
        remote_log_dir="/home/$user_name/test_${timestamp}"
        ping_output="$remote_log_dir/ping_result_${test_name}_${timestamp}.txt"
        echo "Ping command (remote): ping -c 15 -I $src_if $dest_ip on $host_name"
        local safe_filename="$log_dir/ping_result_${test_name}_${timestamp}.txt"

        # Build remote command with proper variable expansion
        local cmd="source /home/$user_name/.bashrc 2>/dev/null; mkdir -p $remote_log_dir && cd $remote_log_dir && ping -c 15 -I $src_if $dest_ip"

        # Save command to commands.txt
        echo "=== Ping Command (host: $host_name) ===" >> "$log_dir/commands.txt"
        echo "ping -c 15 -I $src_if $dest_ip" >> "$log_dir/commands.txt"
        echo "" >> "$log_dir/commands.txt"

        run_cmd $host_name "$cmd" "$safe_filename"
    fi

    sleep $duration;

    # Give extra time for ping to complete and file to be written
    sleep 3

    kill_all $host_name "ping"
    kill_all $host_name "nr-uesoftmodem"

    # Extract ping statistics
    if [[ $host_name == "local" ]]; then
        # Read local ping output
        if [ -f "$ping_output" ]; then
            ping_stats=$(get_ping_stats_tuple "$ping_output")
        else
            echo "ERROR: Ping output file not found at $ping_output"
            ping_stats="0 0"
        fi
    else
        local local_ping_output="$log_dir/ping_result_${test_name}_${timestamp}.txt"
        if [ -f "$local_ping_output" ]; then
            ping_stats=$(get_ping_stats_tuple "$local_ping_output")
        else
            echo "ERROR: Remote ping output file not found at $ping_output"
            ping_stats="0 0"
        fi
    fi

    # Set global variables for summary
    LAST_TX_PACKETS=$(echo $ping_stats | awk '{print $1}')
    LAST_RX_PACKETS=$(echo $ping_stats | awk '{print $2}')

    # Get PSSCH statistics from logs
    # sl_mode 1 uses result_nrUE_syncref.log, sl_mode 2 uses result_syncref.log
    # For PC5 two-host tests: syncref runs locally, nearby runs remotely
    if [[ $sl_mode -eq 0 ]]; then
        local syncref_log=""
    elif [[ $sl_mode -eq 1 ]]; then
        # sl_mode 1: check local first, then remote
        if [ -f "$HOME/result_nrUE_syncref.log" ]; then
            local syncref_log="$HOME/result_nrUE_syncref.log"
        elif [ -f "$log_dir/result_nrUE_syncref.log" ]; then
            local syncref_log="$log_dir/result_nrUE_syncref.log"
        else
            local syncref_log=""
        fi
    elif [[ $sl_mode -eq 2 ]]; then
        # sl_mode 2: check local first, then remote
        if [ -f "$HOME/result_syncref.log" ]; then
            local syncref_log="$HOME/result_syncref.log"
        elif [ -f "$log_dir/result_syncref.log" ]; then
            local syncref_log="$log_dir/result_syncref.log"
        else
            local syncref_log=""
        fi
    fi

    if [[ $sl_mode -eq 0 ]]; then
        if [[ $host_name == "local" ]]; then
            local nrue_log="$HOME/result_nrUE.log"
        else
            local nrue_log="$log_dir/result_nrUE.log"
        fi
        local nrue_log=""
    else
        # For sidelink: check local first, then remote
        if [ -f "$HOME/result_nearby.log" ]; then
            local nearby_log="$HOME/result_nearby.log"
        elif [ -f "$log_dir/result_nearby.log" ]; then
            local nearby_log="$log_dir/result_nearby.log"
        else
            local nearby_log=""
        fi
    fi

    if [ -f "$syncref_log" ]; then
        local pssch_syncref=$(get_pssch_stats "$syncref_log" "syncref")
        LAST_PSSCH_TX_SYNCREF=$(echo $pssch_syncref | awk '{print $1}')
        LAST_PSSCH_RX_SYNCREF=$(echo $pssch_syncref | awk '{print $2}')
    else
        LAST_PSSCH_TX_SYNCREF=0
        LAST_PSSCH_RX_SYNCREF=0
    fi

    if [ -f "$nearby_log" ]; then
        local pssch_nearby=$(get_pssch_stats "$nearby_log" "nearby")
        LAST_PSSCH_TX_NEARBY=$(echo $pssch_nearby | awk '{print $1}')
        LAST_PSSCH_RX_NEARBY=$(echo $pssch_nearby | awk '{print $2}')
    else
        LAST_PSSCH_TX_NEARBY=0
        LAST_PSSCH_RX_NEARBY=0
    fi

    # Check result and set global result variable
    check_ping_result $ping_stats 60  # Expect at least 60% success rate
    if [ $? -eq 0 ]; then
        LAST_TEST_RESULT="PASS"
    else
        LAST_TEST_RESULT="FAIL"
    fi
}

evaluate_ping_and_rsrp_test() {
    # Argumemt(s): duration, test_type, mcs, iteration, host_name, test_name
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && test_type=$2
    [[ $# -ge 3 ]] && mcs=$3
    [[ $# -ge 4 ]] && iteration=$4
    [[ $# -ge 5 ]] && host_name=$5
    [[ $# -ge 6 ]] && num_hosts=$6
    [[ $# -ge 7 ]] && test_name=$7 || test_name="${FUNCNAME[0]}"

    local sl_mode=2
    local src_if="oaitun_ue1"
    local dest_ip="10.0.0.100"

    evaluate_ping_test $nearby_host_name $src_if $dest_ip $sl_mode "${test_name}"

    src_file="~/result_syncref.log"; str_to_find='TotalTx 30'; dst_file="~/result_summary.txt"
    test_result=$(tail -n 100 $src_file | grep -m 1 $str_to_find >> $dst_file)
}

run_gNB_cmd() {
    [[ $# -ge 1 ]] && test_type=$1
    [[ $# -ge 2 ]] && sl_mode=$2
    [[ $# -ge 3 ]] && host_name=$3

    # Get user name based on host
    local user_name=$(find_user_name "$host_name")

    [[ $sl_mode -eq 1 ]] && sl_relay_tag="--relay-type 1 --remote-ue-id 1 --ip-demo 1 --sl-mode 1" || sl_relay_tag=""
    [[ $sl_mode -eq 1 ]] && conf_tag="_relay_ue" || conf_tag=""

    if [[ $test_type == "rfsim" ]]; then
        if [[ $host_name == 'local' ]]; then
            gNB_cmd="cd $HOME/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=$HOME/openairinterface5g/cmake_targets/ran_build/build ./nr-softmodem \
                    -O $HOME/openairinterface5g/targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band78.fr1.106PRB.usrpb210$conf_tag.conf --gNBs.[0].min_rxtxtime 6 \
                    --rfsimulator.serveraddr server --rfsimulator.serverport 4048 --rfsim $sa_flag --log_config.global_log_level info $sl_relay_tag"
        else
            gNB_cmd="LD_LIBRARY_PATH=/home/$user_name/openairinterface5g/cmake_targets/ran_build/build:$LD_LIBRARY_PATH \
                    sudo -E /home/$user_name/openairinterface5g/cmake_targets/ran_build/build/nr-softmodem \
                    -O /home/$user_name/openairinterface5g/targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band78.fr1.106PRB.usrpb210$conf_tag.conf --gNBs.[0].min_rxtxtime 6 \
                    --rfsimulator.serveraddr server --rfsimulator.serverport 4048 --rfsim $sa_flag --log_config.global_log_level info $sl_relay_tag"
        fi
    elif [[ $test_type == "usrp" ]]; then
        gNB_cmd="cd $HOME/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=$HOME/openairinterface5g/cmake_targets/ran_build/build ./nr-softmodem \
                -O ../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band78.fr1.106PRB.usrpb210$conf_tag.conf --gNBs.[0].min_rxtxtime 6 \
                -E $sa_flag --max-ldpc-iterations ${max_ldpc_iterations} --ue-txgain ${TX_GAIN} --ue-rxgain ${RX_GAIN} --device.name oai_usrpdevif $sl_relay_tag"
    fi
    log_file="$HOME/result_gNB.log"
    echo $gNB_cmd; echo

    # Save command to commands.txt
    echo "=== gNB Command (host: $host_name) ===" >> "$log_dir/commands.txt"
    echo "$gNB_cmd" >> "$log_dir/commands.txt"
    echo "" >> "$log_dir/commands.txt"

    run_cmd $host_name "$gNB_cmd" $log_file
}

run_nrUE_cmd() {
    # Argumemt(s): duration, test_type, mcs, iteration, host_name, test_name
    [[ $# -ge 1 ]] && test_type=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && sl_mode=$3
    [[ $# -ge 4 ]] && host_name=$4

    # Get user name based on host
    local user_name=$(find_user_name "$host_name")

    if [[ $test_type == "rfsim" ]]; then
        if [[ $host_name == 'local' ]]; then
            nrUE_cmd="cd $HOME/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=$HOME/openairinterface5g/cmake_targets/ran_build/build \
                    ./nr-uesoftmodem \
                    -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 \
                    --rfsimulator.serveraddr 127.0.0.1 --rfsimulator.serverport 4048 --rfsim $sa_flag \
                    --log_config.global_log_level info"
        else
            nrUE_cmd="LD_LIBRARY_PATH=/home/$user_name/openairinterface5g/cmake_targets/ran_build/build:$LD_LIBRARY_PATH \
                    sudo -E /home/$user_name/openairinterface5g/cmake_targets/ran_build/build/nr-uesoftmodem \
                    -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 \
                    --rfsimulator.serveraddr $LOCAL_HOST_IP --rfsimulator.serverport 4048 --rfsim $sa_flag \
                    --log_config.global_log_level info"
        fi
    elif [[ $test_type == "usrp" ]]; then
        nrUE_cmd="LD_LIBRARY_PATH=/home/$user_name/openairinterface5g/cmake_targets/ran_build/build \
                    sudo -E /home/$user_name/openairinterface5g/cmake_targets/ran_build/build/nr-uesoftmodem \
                    -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 \
                    -E $sa_flag --ue-txgain ${TX_GAIN} --ue-rxgain ${RX_GAIN} --thread-pool -1,-1 --device.name oai_usrpdevif \
                    --max-ldpc-iterations ${max_ldpc_iterations} --log_config.global_log_level info"
    fi
    log_file="$HOME/result_nrUE.log"
    echo $nrUE_cmd; echo

    # Save command to commands.txt
    echo "=== nrUE Command (host: $host_name) ===" >> "$log_dir/commands.txt"
    echo "$nrUE_cmd" >> "$log_dir/commands.txt"
    echo "" >> "$log_dir/commands.txt"

    run_cmd $host_name "$nrUE_cmd" $log_file
}

run_syncref_cmd() {
    [[ $# -ge 1 ]] && test_type=$1
    [[ $# -ge 2 ]] && local mcs=" --mcs $2" || local mcs=""
    [[ $# -ge 3 ]] && sl_mode=$3
    [[ $# -ge 4 ]] && host_name=$4

    # Get user name based on host
    local user_name=$(find_user_name "$host_name")

    if [[ $sl_mode -eq 1 ]]; then
        if [[ $test_type == "rfsim" ]]; then
            if [[ $host_name == 'local' ]]; then
                syncref_cmd="cd $HOME/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=$HOME/openairinterface5g/cmake_targets/ran_build/build ./nr-uesoftmodem \
                            -O $HOME/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf \
                            -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 \
                            --rfsim $sa_flag --sync-ref --sl-mode 1 \
                            --rfsimulator.serveraddr 127.0.0.1 --rfsimulator.serverport 4048 \
                            --rfsimulator.serveraddrsl 127.0.0.1 --rfsimulator.serverportsl 4148 \
                            --log_config.global_log_level info --relay-type 1 --is-relay-ue 1  $mcs"
            else
                syncref_cmd="LD_LIBRARY_PATH=/home/$user_name/openairinterface5g/cmake_targets/ran_build/build:$LD_LIBRARY_PATH \
                            sudo -E /home/$user_name/openairinterface5g/cmake_targets/ran_build/build/nr-uesoftmodem \
                            -O /home/$user_name/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf \
                            -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 \
                            --rfsim $sa_flag --sync-ref --sl-mode 1 --relay-type 1 --is-relay-ue 1 \
                            --rfsimulator.serveraddr $GNB_HOST_IP  --rfsimulator.serverport 4048 \
                            --rfsimulator.serveraddrsl $REMOTE_HOST_IP  --rfsimulator.serverportsl 4148 \
                            --log_config.global_log_level info $mcs"
            fi
        elif [[ $test_type == "usrp" ]]; then
            syncref_cmd="cd $HOME/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=$HOME/openairinterface5g/cmake_targets/ran_build/build ./nr-uesoftmodem \
                        -O $HOME/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf \
                        -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 \
                        -E $sa_flag --sl-mode 1 --sync-ref --node-number 2 --ip-demo 1 --relay-type 1 --is-relay-ue 1 \
                        --usrp-args 'serial=$RELAY_UE_USRP_SN_FOR_UU,type=b200' --usrp-args-sl 'serial=$RELAY_UE_USRP_SN_FOR_SL,type=b200' \
                        $ext_clock_flag \
                        --max-ldpc-iterations ${max_ldpc_iterations} --ue-txgain ${TX_GAIN} --ue-rxgain ${RX_GAIN} --device.name oai_usrpdevif $mcs"
        fi
        log_file="$HOME/result_nrUE_syncref.log"
    elif [[ $sl_mode -eq 2 ]]; then
        if [[ $test_type == "rfsim" ]]; then
            syncref_cmd="cd $HOME/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=$HOME/openairinterface5g/cmake_targets/ran_build/build \
                         $HOME/openairinterface5g/cmake_targets/ran_build/build/nr-uesoftmodem \
                        -O $HOME/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf --sync-ref --sl-mode 2 --rfsim $sa_flag \
                        --rfsimulator.serveraddrsl server --rfsimulator.serverportsl 4148 --log_config.global_log_level info  $mcs"
        elif [[ $test_type == "usrp" ]]; then
            syncref_cmd="cd $HOME/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=$HOME/openairinterface5g/cmake_targets/ran_build/build \
                        $HOME/openairinterface5g/cmake_targets/ran_build/build/nr-uesoftmodem \
                        -O $HOME/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf -E $sa_flag --sl-mode 2 --sync-ref \
                        $ext_clock_flag \
                        --max-ldpc-iterations ${max_ldpc_iterations} --ue-txgain ${TX_GAIN} --ue-rxgain ${RX_GAIN} --thread-pool -1,-1 --device.name oai_usrpdevif $mcs"
        fi
        log_file="$HOME/result_syncref.log"
    fi

    echo $syncref_cmd; echo;

    # Save command to commands.txt
    echo "=== Syncref UE Command (host: $host_name) ===" >> "$log_dir/commands.txt"
    echo "$syncref_cmd" >> "$log_dir/commands.txt"
    echo "" >> "$log_dir/commands.txt"

    run_cmd $host_name "$syncref_cmd" $log_file
}

run_nearby_cmd() {
    [[ $# -ge 1 ]] && test_type=$1
    [[ $# -ge 2 ]] && local mcs=" --mcs $2" || local mcs=""
    [[ $# -ge 3 ]] && sl_mode=$3
    [[ $# -ge 4 ]] && host_name=$4

    # Get user name based on host
    local user_name=$(find_user_name "$host_name")

    if [[ $sl_mode -eq 1 ]]; then
        if [[ $test_type == "rfsim" ]]; then
            if [[ $host_name == 'local' ]]; then
                nearby_cmd="cd $HOME/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=$HOME/openairinterface5g/cmake_targets/ran_build/build ./nr-uesoftmodem \
                            -O $HOME/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf --rfsim $sa_flag --sl-mode 2 $mcs \
                            --rfsimulator.serveraddrsl server --rfsimulator.serverportsl 4148 \
                            --log_config.global_log_level info --relay-type 1"
            else
                nearby_cmd="LD_LIBRARY_PATH=/home/$user_name/openairinterface5g/cmake_targets/ran_build/build:$LD_LIBRARY_PATH \
                            sudo -E /home/$user_name/openairinterface5g/cmake_targets/ran_build/build/nr-uesoftmodem \
                            -O /home/$user_name/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf --rfsim $sa_flag --sl-mode 2 $mcs \
                            --rfsimulator.serveraddrsl server --rfsimulator.serverportsl 4148 \
                            --log_config.global_log_level info --relay-type 1"
            fi
        elif [[ $test_type == "usrp" ]]; then
            nearby_cmd="LD_LIBRARY_PATH=/home/$user_name/openairinterface5g/cmake_targets/ran_build/build \
                        sudo -E /home/$user_name/openairinterface5g/cmake_targets/ran_build/build/nr-uesoftmodem \
                        -O /home/$user_name/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf -E $sa_flag --sl-mode 2 --relay-type 1 \
                        $ext_clock_flag \
                        --max-ldpc-iterations ${max_ldpc_iterations} --ue-txgain ${TX_GAIN} --ue-rxgain ${RX_GAIN} --thread-pool -1,-1 --device.name oai_usrpdevif $mcs"
        fi
    elif [[ $sl_mode -eq 2 ]]; then
        if [[ $test_type == "rfsim" ]]; then
            if [[ $host_name == 'local' ]]; then
                nearby_cmd="cd $HOME/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=$HOME/openairinterface5g/cmake_targets/ran_build/build ./nr-uesoftmodem \
                        -O $HOME/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf --rfsim $sa_flag --sl-mode 2 $mcs \
                        --rfsimulator.serveraddrsl 127.0.0.1 --rfsimulator.serverportsl 4148 --log_config.global_log_level info"
            else
                nearby_cmd="LD_LIBRARY_PATH=/home/$user_name/openairinterface5g/cmake_targets/ran_build/build:$LD_LIBRARY_PATH \
                        sudo -E /home/$user_name/openairinterface5g/cmake_targets/ran_build/build/nr-uesoftmodem \
                        -O /home/$user_name/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf --rfsim $sa_flag --sl-mode 2 $mcs \
                        --rfsimulator.serveraddrsl $LOCAL_HOST_IP --rfsimulator.serverportsl 4148 --log_config.global_log_level info"
            fi
        elif [[ $test_type == "usrp" ]]; then
            nearby_cmd="LD_LIBRARY_PATH=/home/$user_name/openairinterface5g/cmake_targets/ran_build/build \
                        sudo -E /home/$user_name/openairinterface5g/cmake_targets/ran_build/build/nr-uesoftmodem \
                        -O /home/$user_name/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf -E $sa_flag --sl-mode 2 \
                        $ext_clock_flag \
                        --max-ldpc-iterations ${max_ldpc_iterations} --ue-txgain ${TX_GAIN} --ue-rxgain ${RX_GAIN} --thread-pool -1,-1 --device.name oai_usrpdevif $mcs"
        fi
    fi
    log_file="$HOME/result_nearby.log"
    echo $nearby_cmd; echo

    # Save command to commands.txt
    echo "=== Nearby UE Command (host: $host_name) ===" >> "$log_dir/commands.txt"
    echo "$nearby_cmd" >> "$log_dir/commands.txt"
    echo "" >> "$log_dir/commands.txt"

    run_cmd $host_name "$nearby_cmd" $log_file
}

#############################################################
####################### Test cases ##########################
#############################################################

slmode1_srap_ping_test() {
    # Argumemt: duration, test_type, mcs
    [[ $# -ge 1 ]] && duration=$1 || duration=15
    [[ $# -ge 2 ]] && test_type=$2
    [[ $# -ge 3 ]] && mcs=$3
    [[ $# -ge 4 ]] && iteration=$4
    [[ $# -ge 5 ]] && gnb_host_name=$5
    [[ $# -ge 6 ]] && syncref_host_name=$6
    [[ $# -ge 7 ]] && nearby_host_name=$7
    [[ $# -ge 8 ]] && num_hosts=$8
    [[ $# -ge 9 ]] && test_name=$9 || test_name="${FUNCNAME[0]}"

    echo "====================  Testing ${test_name}  ===================="

    # Validate test type for local host execution
    if [[ $num_hosts -eq 1 ]]; then
        validate_test_type_for_local_host $test_type $test_name || return 1
    fi
    cleanup_old_logs

    # Sync default configuration parameters across all files
    sync_default_config_params 0 2

    local start_time=$(date +%s)
    local sl_mode=1
    local src_if="oaitun_ue2"
    local dest_ip="8.8.8.8"

    # Sync configuration files to remote hosts if needed
    if [[ $syncref_host_name != "local" ]]; then
        sync_config_files $syncref_host_name
    fi
    if [[ $nearby_host_name != "local" ]]; then
        sync_config_files $nearby_host_name
    fi

    run_gNB_cmd $test_type $sl_mode $gnb_host_name
    run_nearby_cmd  $test_type $mcs $sl_mode $nearby_host_name
    run_syncref_cmd $test_type $mcs $sl_mode $syncref_host_name

    local wait_start=$(date +%s)
    wait_for_tun_interface $src_if $nearby_host_name $duration
    local remaining=$(( duration - $(date +%s) + wait_start ))
    if [[ "$ensure_ping_test_time" == "1" ]]; then
        [[ $remaining -lt 5 ]] && remaining=5
        wait_for_pc5_sync $remaining
        remaining=$(( duration - $(date +%s) + wait_start ))
        [[ $remaining -lt 16 ]] && remaining=16
        duration=$remaining
    else
        [[ $remaining -gt 0 ]] && wait_for_pc5_sync $remaining
        duration=$(( duration - $(date +%s) + wait_start ))
    fi

    # Additional wait time for sidelink synchronization to complete
    if [[ "$use_extended_delays" == "1" ]]; then
        echo "Waiting additional ${sleep_timing[sync_stab_45s_v1]} seconds for sidelink sync to stabilize..."
        sleep $((0 + ${sleep_timing[sync_stab_45s_v1]}))
    fi

    evaluate_ping_test $nearby_host_name $src_if $dest_ip $sl_mode $test_name

    # Cleanup all processes (nearby_host_name was cleaned up in the evaluate_ping_test)
    kill_all $syncref_host_name nr-uesoftmodem
    kill_all $gnb_host_name nr-softmodem
    save_softmodem_logs "${test_name}"

    local end_time=$(date +%s)
    local elapsed=$((end_time - start_time))
    print_runtime $start_time $end_time

    # Print test summary
    print_test_summary "${test_name}" "$iteration" "$num_hosts" "$mcs" "$elapsed" "$LAST_TX_PACKETS" "$LAST_RX_PACKETS" "$LAST_TEST_RESULT"
}
#############################################################
rfsim_slmode1_srap_ping_test_on_three_hosts() {
#############################################################
    # Argumemt(s): duration, mcs, iteration
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && iteration=$3
    local test_type="rfsim"
    local gnb_host_name="local"
    local syncref_host_name=$RELAY_UE_HOST
    local nearby_host_name=$REMOTE_UE_HOST
    local num_hosts=3
    slmode1_srap_ping_test $duration $test_type $mcs $iteration $gnb_host_name $syncref_host_name $nearby_host_name $num_hosts "${FUNCNAME[0]}"
}
#############################################################
usrp_B210_slmode1_srap_ping_test_on_three_hosts() {
#############################################################
    # Argumemt(s): duration, mcs, iteration
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && iteration=$3
    local test_type="usrp"
    local gnb_host_name="local"
    local syncref_host_name=$RELAY_UE_HOST
    local nearby_host_name=$REMOTE_UE_HOST
    local num_hosts=3
    slmode1_srap_ping_test $duration $test_type $mcs $iteration $gnb_host_name $syncref_host_name $nearby_host_name $num_hosts "${FUNCNAME[0]}"
}
#############################################################
rfsim_slmode1_srap_ping_test_on_local_host() {
#############################################################
    # Argumemt(s): duration, mcs, iteration
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && iteration=$3
    local test_type="rfsim"
    local gnb_host_name="local"
    local syncref_host_name="local"
    local nearby_host_name="local"
    local num_hosts=1
    slmode1_srap_ping_test $duration $test_type $mcs $iteration $gnb_host_name $syncref_host_name $nearby_host_name $num_hosts "${FUNCNAME[0]}"
}

#############################################################
update_mcs_runtime() {
#############################################################
    # Change MCS while processes are running
    # For BLER tests with fixed Uu MCS: Must restart gNB + UEs
    # OPTIMIZED: Reduced overhead from 70s to ~30s

    local syncref_host=$1
    local nearby_host=$2
    local new_mcs=$3
    local noise_power=$4

    echo "  → Updating MCS to ${new_mcs}..."

    # Kill all processes (gNB + UEs) for MCS change
    kill_all $gnb_host_name nr-softmodem
    kill_all $syncref_host_name nr-uesoftmodem
    kill_all $nearby_host_name nr-uesoftmodem
    sleep 2  # Configurable: default 2s (was 5s)

    # Restart all with new MCS
    echo "  → Restarting gNB with MCS=${new_mcs}..."
    run_gNB_cmd_with_noise $test_type $sl_mode $gnb_host_name $noise_power $ploss_db $new_mcs
    sleep 3

    echo "  → Restarting Remote UE with MCS=${new_mcs}..."
    run_nearby_cmd_with_noise $test_type $new_mcs $sl_mode $nearby_host $noise_power $ploss_db &
    sleep 5

    echo "  → Restarting Relay UE with MCS=${new_mcs}..."
    run_syncref_cmd_with_noise $test_type $new_mcs $sl_mode $syncref_host $noise_power $ploss_db &

    # Smart sync wait: Poll for actual sidelink sync instead of blind 30s wait
    echo "  → Waiting for sidelink sync..."
    local sync_detected=0
    for i in {1..20}; do
        sleep 1
        # Check if nearby UE is receiving PSSCH (indicates sync achieved)
        if grep -q "PSSCH.*RX ok [1-9]" ~/result_nearby.log 2>/dev/null; then
            echo "  ✓ Sidelink synced after ${i} seconds"
            sync_detected=1
            break
        fi
    done

    if [ $sync_detected -eq 0 ]; then
        echo "  ⚠ Sync timeout after 20s, proceeding anyway..."
    fi

    wait_for_tun_interface $src_if $nearby_host 15  # Optimized: was 30
    echo "  ✓ MCS updated to ${new_mcs}, all processes restarted, ready for test"
}

#############################################################
save_logs_for_mcs() {
#############################################################
    # Save current logs with MCS/noise-specific naming
    # Allows parsing individual test points later

    local test_name=$1
    local mcs=$2
    local noise=$3

    local timestamp=$(date +"%H%M%S")
    local log_prefix="${test_name}_mcs${mcs}_noise${noise}_${timestamp}"

    # Copy current logs to archive (don't move - processes still writing)
    for log_file in "${softmodem_log_files[@]}"; do
        if [[ -f "$HOME/$log_file" ]]; then
            cp "$HOME/$log_file" "$log_dir/${log_prefix}_${log_file}"
        fi
    done

    # Also save ping results
    if [[ -f "/tmp/ping_result_mcs${mcs}.txt" ]]; then
        cp "/tmp/ping_result_mcs${mcs}.txt" "$log_dir/${log_prefix}_ping.txt"
    fi
}

#############################################################
run_gNB_cmd_with_noise() {
#############################################################
    # Launch gNB with channel model and noise injection

    [[ $# -ge 1 ]] && test_type=$1
    [[ $# -ge 2 ]] && sl_mode=$2
    [[ $# -ge 3 ]] && host_name=$3
    [[ $# -ge 4 ]] && noise_power=$4
    [[ $# -ge 5 ]] && ploss=$5
    [[ $# -ge 6 ]] && mcs_value=$6

    # Set defaults if not provided
    : ${noise_power:=0}
    : ${ploss:=5}
    : ${mcs_value:=28}

    [[ $sl_mode -eq 1 ]] && sl_relay_tag="--relay-type 1 --remote-ue-id 1 --ip-demo 1 --sl-mode 1" || sl_relay_tag=""
    [[ $sl_mode -eq 1 ]] && conf_tag="_relay_ue" || conf_tag=""

    gNB_cmd="cd $HOME/openairinterface5g/cmake_targets/ran_build/build; \
             sudo -E LD_LIBRARY_PATH=\$PWD ./nr-softmodem \
             -O $HOME/openairinterface5g/targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band78.fr1.106PRB.usrpb210${conf_tag}.conf \
             --gNBs.[0].min_rxtxtime 6 \
             --rfsimulator.serveraddr server --rfsimulator.serverport 4048 --rfsim --sa \
             --log_config.global_log_level info --log_config.global_log_options time \
             --rfsimulator.options chanmod --channelmod.modellist modellist_rfsimu_1 \
             --channelmod.modellist_rfsimu_1.[0].noise_power_dB ${noise_power} \
             --channelmod.modellist_rfsimu_1.[0].ploss_dB ${ploss} \
             --MACRLCs.[0].dl_max_mcs ${mcs_value} \
             --MACRLCs.[0].ul_max_mcs ${mcs_value} \
             $sl_relay_tag"

    log_file="$HOME/result_gNB.log"

    echo "=== gNB Command (noise=${noise_power}dB, ploss=${ploss}dB) ===" >> "$log_dir/commands.txt"
    echo "$gNB_cmd" >> "$log_dir/commands.txt"
    echo "" >> "$log_dir/commands.txt"

    run_cmd $host_name "$gNB_cmd" $log_file
}

#############################################################
run_syncref_cmd_with_noise() {
#############################################################
    # Launch Relay UE (sync ref) with channel model and noise injection

    [[ $# -ge 1 ]] && test_type=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && sl_mode=$3
    [[ $# -ge 4 ]] && host_name=$4
    [[ $# -ge 5 ]] && noise_power=$5
    [[ $# -ge 6 ]] && ploss=$6

    # Set defaults if not provided
    : ${noise_power:=0}
    : ${ploss:=5}

    syncref_cmd="cd $HOME/openairinterface5g/cmake_targets/ran_build/build; \
                 sudo -E LD_LIBRARY_PATH=\$PWD ./nr-uesoftmodem \
                 -O $HOME/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf \
                 -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 \
                 --rfsim --sa --sync-ref --sl-mode 1 \
                 --rfsimulator.serveraddr 127.0.0.1 --rfsimulator.serverport 4048 \
                 --rfsimulator.serveraddrsl 127.0.0.1 --rfsimulator.serverportsl 4148 \
                 --log_config.global_log_level info --log_config.global_log_options time \
                 --rfsimulator.options chanmod --channelmod.modellist modellist_rfsimu_1 \
                 --channelmod.modellist_rfsimu_1.[1].noise_power_dB ${noise_power} \
                 --channelmod.modellist_rfsimu_1.[1].ploss_dB ${ploss} \
                 --relay-type 1 --is-relay-ue 1 --ip-demo 1 --mcs ${mcs} --node-number 2"

    log_file="$HOME/result_nrUE_syncref.log"

    echo "=== Relay UE Command (noise=${noise_power}dB, mcs=${mcs}) ===" >> "$log_dir/commands.txt"
    echo "$syncref_cmd" >> "$log_dir/commands.txt"
    echo "" >> "$log_dir/commands.txt"

    run_cmd $host_name "$syncref_cmd" $log_file
}

#############################################################
run_nearby_cmd_with_noise() {
#############################################################
    # Launch Remote UE (nearby) with channel model and noise injection

    [[ $# -ge 1 ]] && test_type=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && sl_mode=$3
    [[ $# -ge 4 ]] && host_name=$4
    [[ $# -ge 5 ]] && noise_power=$5
    [[ $# -ge 6 ]] && ploss=$6

    # Set defaults if not provided
    : ${noise_power:=0}
    : ${ploss:=5}

    nearby_cmd="cd $HOME/openairinterface5g/cmake_targets/ran_build/build; \
                sudo -E LD_LIBRARY_PATH=\$PWD ./nr-uesoftmodem \
                -O $HOME/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf \
                -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000002 \
                --rfsim --sa --sl-mode 2 \
                --rfsimulator.serveraddrsl server --rfsimulator.serverportsl 4148 \
                --log_config.global_log_level info --log_config.global_log_options time \
                --rfsimulator.options chanmod --channelmod.modellist modellist_rfsimu_1 \
                --channelmod.modellist_rfsimu_1.[0].noise_power_dB ${noise_power} \
                --channelmod.modellist_rfsimu_1.[0].ploss_dB ${ploss} \
                --channelmod.modellist_rfsimu_1.[1].noise_power_dB ${noise_power} \
                --channelmod.modellist_rfsimu_1.[1].ploss_dB ${ploss} \
                --mcs ${mcs} --ip-demo 1 --node-number 3 --relay-type 1"

    log_file="$HOME/result_nearby.log"

    echo "=== Remote UE Command (noise=${noise_power}dB, mcs=${mcs}) ===" >> "$log_dir/commands.txt"
    echo "$nearby_cmd" >> "$log_dir/commands.txt"
    echo "" >> "$log_dir/commands.txt"

    run_cmd $host_name "$nearby_cmd" $log_file
}

#############################################################
bler_test() {
#############################################################
    # Core BLER test function - single test execution
    # Arguments: duration, test_type, mcs, iteration, noise_power, host parameters
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && test_type=$2
    [[ $# -ge 3 ]] && mcs=$3
    [[ $# -ge 4 ]] && iteration=$4
    [[ $# -ge 5 ]] && noise_power=$5
    [[ $# -ge 6 ]] && gnb_host_name=$6
    [[ $# -ge 7 ]] && syncref_host_name=$7
    [[ $# -ge 8 ]] && nearby_host_name=$8
    [[ $# -ge 9 ]] && num_hosts=$9
    [[ $# -ge 10 ]] && test_name=${10} || test_name="${FUNCNAME[0]}"

    echo "====================  Testing ${test_name}  ===================="

    # Validate test type for local host execution
    if [[ $num_hosts -eq 1 ]]; then
        validate_test_type_for_local_host $test_type $test_name || return 1
    fi
    cleanup_old_logs

    local start_time=$(date +%s)
    local sl_mode=1
    local src_if="oaitun_ue2"
    local dest_ip="8.8.8.8"

    # Sync CSI and PSFCH config (disable CSI, PSFCH=2)
    sync_default_config_params $csi_acquisition $psfch_period

    # Restart core network (full cycle: down then up)
    restart_core_network || {
        echo "ERROR: Core network restart failed. Aborting iteration i=$i, noise=$noise_power, mcs=$mcs"
        continue
    }

    # Launch processes with noise injection
    echo "[1/4] Starting gNB with noise=${noise_power}dB, MCS=${mcs}..."
    run_gNB_cmd_with_noise $test_type $sl_mode $gnb_host_name $noise_power $ploss_db $mcs
    sleep 2

    echo "[2/4] Starting Remote UE with MCS=${mcs}..."
    run_nearby_cmd_with_noise $test_type $mcs $sl_mode $nearby_host_name $noise_power $ploss_db
    sleep 2

    echo "[3/4] Starting Relay UE with MCS=${mcs}..."
    run_syncref_cmd_with_noise $test_type $mcs $sl_mode $syncref_host_name $noise_power $ploss_db
    sleep 3

    echo "[4/4] Waiting for tunnel interface..."
    wait_for_tun_interface $src_if $nearby_host_name 30

    # Run ping test
    local ping_count=$((duration * 15))
    echo "Starting ping: $ping_count packets (interval 0.0667s)..."
    ping -I $src_if $dest_ip -i 0.0667 -c $ping_count > /tmp/ping_result_mcs${mcs}.txt 2>&1

    # Parse ping results
    local tx_packets=$(grep "transmitted" /tmp/ping_result_mcs${mcs}.txt | awk '{print $1}')
    local rx_packets=$(grep "transmitted" /tmp/ping_result_mcs${mcs}.txt | awk '{print $4}')
    local test_result=$([[ ${rx_packets:-0} -gt 0 ]] && echo "PASS" || echo "FAIL")

    # Save logs for this specific test point
    save_logs_for_mcs "${test_name}" $mcs $noise_power

    # Cleanup processes
    kill_all $syncref_host_name nr-uesoftmodem
    kill_all $nearby_host_name nr-uesoftmodem
    kill_all $gnb_host_name nr-softmodem
    sleep 5

    local end_time=$(date +%s)
    local elapsed=$((end_time - start_time))
    print_runtime $start_time $end_time

    # Print test summary
    print_test_summary "${test_name}" "iter${iteration}_noise${noise_power}_mcs${mcs}" "$num_hosts" "$mcs" "$elapsed" "$tx_packets" "$rx_packets" "$test_result"
}

#############################################################
rfsim_slmode1_bler_test_on_local_host() {
#############################################################
    # BLER test wrapper - delegates to core bler_test function
    # Arguments: duration, mcs, iteration, noise_power
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && iteration=$3
    [[ $# -ge 4 ]] && noise_power=$4

    local test_type="rfsim"
    local gnb_host_name="local"
    local syncref_host_name="local"
    local nearby_host_name="local"
    local num_hosts=1

    bler_test $duration $test_type $mcs $iteration $noise_power $gnb_host_name $syncref_host_name $nearby_host_name $num_hosts "${FUNCNAME[0]}"
}

uu_ping_test() {
    # Argumemt(s): duration, test_type, mcs, iteration, host_name, test_name
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && test_type=$2
    [[ $# -ge 3 ]] && mcs=$3
    [[ $# -ge 4 ]] && iteration=$4
    [[ $# -ge 5 ]] && gnb_host_name=$5
    [[ $# -ge 6 ]] && nrue_host_name=$6
    [[ $# -ge 7 ]] && num_hosts=$7
    [[ $# -ge 8 ]] && test_name=$8 || test_name="${FUNCNAME[0]}"

    echo "====================  Testing ${test_name}  ===================="

    # Validate test type for local host execution
    if [[ $num_hosts -eq 1 ]]; then
        validate_test_type_for_local_host $test_type $test_name || return 1
    fi
    cleanup_old_logs

    local start_time=$(date +%s)
    local sl_mode=0
    local src_if="oaitun_ue1"
    local dest_ip="8.8.8.8"

    run_gNB_cmd $test_type $sl_mode $gnb_host_name
    run_nrUE_cmd $test_type $mcs $sl_mode $nrue_host_name
    wait_for_tun_interface $src_if $nrue_host_name $duration

    # Additional wait time for synchronization to complete
    if [[ "$use_extended_delays" == "1" ]]; then
        echo "Waiting additional 30 seconds for sidelink sync to stabilize..."
        sleep $((0 + ${sleep_timing[sync_stab_30s]}))  # Configurable: default 30s
    fi

    evaluate_ping_test $nrue_host_name $src_if $dest_ip $sl_mode "${test_name}"

    # Cleanup all processes (nrue_host_name was cleaned up in the evaluate_ping_test)
    kill_all $gnb_host_name nr-softmodem
    save_softmodem_logs "${test_name}"

    local end_time=$(date +%s)
    local elapsed=$((end_time - start_time))
    print_runtime $start_time $end_time

    # Print test summary
    print_test_summary "${test_name}" "$iteration" "$num_hosts" "$mcs" "$elapsed" "$LAST_TX_PACKETS" "$LAST_RX_PACKETS" "$LAST_TEST_RESULT"
}
#############################################################
rfsim_uu_ping_test_on_two_hosts() {
#############################################################
    # Argumemt(s): duration, mcs, iteration
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && iteration=$3
    local test_type="rfsim"
    local gnb_host_name="local"
    local nrue_host_name=$NR_UE_HOST
    local num_hosts=2
    uu_ping_test $duration $test_type $mcs $iteration $gnb_host_name $nrue_host_name $num_hosts "${FUNCNAME[0]}"
}
#############################################################
usrp_B210_uu_ping_test_on_two_hosts() {
#############################################################
    # Argumemt(s): duration, mcs, iteration
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && iteration=$3
    local test_type="usrp"
    local gnb_host_name="local"
    local nrue_host_name=$NR_UE_HOST
    local num_hosts=2
    uu_ping_test $duration $test_type $mcs $iteration $gnb_host_name $nrue_host_name $num_hosts "${FUNCNAME[0]}"
}
#############################################################
rfsim_uu_ping_test_on_local_host() {
#############################################################
    # Argumemt(s): duration, mcs, iteration
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && iteration=$3
    local test_type="rfsim"
    local gnb_host_name="local"
    local nrue_host_name="local"
    local num_hosts=1
    uu_ping_test $duration $test_type $mcs $iteration $gnb_host_name $nrue_host_name $num_hosts "${FUNCNAME[0]}"
}

pc5_ping_test() {
    # Argumemt(s): duration, test_type, mcs, iteration, host_name, test_name
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && test_type=$2
    [[ $# -ge 3 ]] && mcs=$3
    [[ $# -ge 4 ]] && iteration=$4
    [[ $# -ge 5 ]] && syncref_host_name=$5
    [[ $# -ge 6 ]] && nearby_host_name=$6
    [[ $# -ge 7 ]] && num_hosts=$7
    [[ $# -ge 8 ]] && test_name=$8 || test_name="${FUNCNAME[0]}"

    echo "====================  Testing ${test_name}  ===================="

    # Validate test type for local host execution
    if [[ $num_hosts -eq 1 ]]; then
        validate_test_type_for_local_host $test_type $test_name || return 1
    fi
    cleanup_old_logs

    # Sync default configuration parameters across all files
    sync_default_config_params 0 2

    local start_time=$(date +%s)
    local sl_mode=2
    local src_if="oaitun_ue1"
    local dest_ip="10.0.0.100"

    echo "test_type:" $test_type " mcs: " $mcs " sl_mode: " $sl_mode " syncref_host_name: " $syncref_host_name " nearby_host_name: " $nearby_host_name

    # Sync configuration files if using remote host
    if [[ $nearby_host_name != "local" ]]; then
        sync_config_files $nearby_host_name
    fi

    run_syncref_cmd $test_type $mcs $sl_mode $syncref_host_name
    run_nearby_cmd  $test_type $mcs $sl_mode $nearby_host_name

    local wait_start=$(date +%s)
    wait_for_tun_interface $src_if $syncref_host_name $duration
    local remaining=$(( duration - $(date +%s) + wait_start ))
    if [[ "$ensure_ping_test_time" == "1" ]]; then
        [[ $remaining -lt 5 ]] && remaining=5
        wait_for_pc5_sync $remaining
        remaining=$(( duration - $(date +%s) + wait_start ))
        [[ $remaining -lt 16 ]] && remaining=16
        duration=$remaining
    else
        [[ $remaining -gt 0 ]] && wait_for_pc5_sync $remaining
        duration=$(( duration - $(date +%s) + wait_start ))
    fi

    # Additional wait time for sidelink synchronization to complete
    if [[ "$use_extended_delays" == "1" ]]; then
        echo "Waiting additional ${sleep_timing[sync_stab_45s_v1]} seconds for sidelink sync to stabilize..."
        sleep $((0 + ${sleep_timing[sync_stab_45s_v1]}))
    fi

    evaluate_ping_test $syncref_host_name $src_if $dest_ip $sl_mode "${test_name}"

    # Cleanup all processes (nrue_host_name was cleaned up in the evaluate_ping_test)
    kill_all $nearby_host_name nr-uesoftmodem
    save_softmodem_logs "${test_name}"

    local end_time=$(date +%s)
    local elapsed=$((end_time - start_time))
    print_runtime $start_time $end_time

    # Print test summary
    print_test_summary "${test_name}" "$iteration" "$num_hosts" "$mcs" "$elapsed" "$LAST_TX_PACKETS" "$LAST_RX_PACKETS" "$LAST_TEST_RESULT"
}
#############################################################
rfsim_pc5_ping_test_on_two_hosts() {
#############################################################
    # Argumemt(s): duration, mcs, iteration
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && iteration=$3
    local test_type="rfsim"
    local num_hosts=2
    local syncref_host_name="local"
    local nearby_host_name=$REMOTE_UE_HOST
    pc5_ping_test $duration $test_type $mcs $iteration $syncref_host_name $nearby_host_name $num_hosts "${FUNCNAME[0]}"
}
#############################################################
usrp_B210_pc5_ping_test_on_two_hosts() {
#############################################################
    # Argumemt(s): duration, mcs, iteration
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && iteration=$3
    local test_type="usrp"
    local num_hosts=2
    local syncref_host_name="local"
    local nearby_host_name=$REMOTE_UE_HOST
    pc5_ping_test $duration $test_type $mcs $iteration $syncref_host_name $nearby_host_name $num_hosts "${FUNCNAME[0]}"
}
#############################################################
rfsim_pc5_ping_test_on_local_host() {
#############################################################
    # Argumemt(s): duration, mcs, iteration
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && iteration=$3
    local test_type="rfsim"
    local num_hosts=1
    local syncref_host_name="local"
    local nearby_host_name="local"
    pc5_ping_test $duration $test_type $mcs $iteration $syncref_host_name $nearby_host_name $num_hosts "${FUNCNAME[0]}"
}

pc5_csi_acquisition_psfch_period_test() {
    # Argumemt(s): csi_acq, psfch_period, duration, test_type, mcs, iteration
    [[ $# -ge 1 ]] && csi_acq=$1; echo "csi_acq = " $1
    [[ $# -ge 2 ]] && period=$2;  echo "psfch_period  = " $2
    [[ $# -ge 3 ]] && duration=$3
    [[ $# -ge 4 ]] && test_type=$4
    [[ $# -ge 5 ]] && mcs=$5
    [[ $# -ge 6 ]] && iteration=$6
    [[ $# -ge 7 ]] && syncref_host_name=$7
    [[ $# -ge 8 ]] && nearby_host_name=$8
    [[ $# -ge 9 ]] && num_hosts=$9
    [[ $# -ge 10 ]] && test_name=${10} || test_name="${FUNCNAME[0]}"

    echo "====================  Testing ${test_name}  ===================="

    # Validate test type for local host execution
    if [[ $num_hosts -eq 1 ]]; then
        validate_test_type_for_local_host "$test_type" "${FUNCNAME[0]}" || return 1
    fi
    if [[ $nearby_host_name != "local" ]]; then
        local remote_user=$(find_user_name "$nearby_host_name")
    fi
    cleanup_old_logs

    local start_time=$(date +%s)
    local sl_mode=2

    # Ensure both local and remote systems start with synchronized baseline
    # This MUST happen for every test run (8 iterations) to ensure consistency
    echo "==> Syncing baseline: CSI=$DEFAULT_CSI_ACQ, PSFCH=$DEFAULT_PSFCH_PERIOD across all systems..."
    sync_default_config_params $DEFAULT_CSI_ACQ $DEFAULT_PSFCH_PERIOD

    echo "==> Applying test-specific config: CSI=$csi_acq, PSFCH=$period"

    # Update local syncref config (always runs locally) - updates ALL occurrences
    sed -i "s/\(sl_CSI_Acquisition[[:space:]]*=[[:space:]]*\)[0-9]\+/\1$csi_acq/g" "$CONF_PATH/sl_sync_ref.conf"
    sed -i "s/\(sl_PSFCH_Period[[:space:]]*=[[:space:]]*\)[0-9]\+/\1$period/g" "$CONF_PATH/sl_sync_ref.conf"

    # Update nearby config (local or remote depending on nearby_host_name) - updates ALL occurrences
    if [[ $nearby_host_name == "local" ]]; then
        # Local nearby: update local sl_ue1.conf
        sed -i "s/\(sl_CSI_Acquisition[[:space:]]*=[[:space:]]*\)[0-9]\+/\1$csi_acq/g" "$CONF_PATH/sl_ue1.conf"
        sed -i "s/\(sl_PSFCH_Period[[:space:]]*=[[:space:]]*\)[0-9]\+/\1$period/g" "$CONF_PATH/sl_ue1.conf"
    else
        # Remote nearby: update local sl_ue1.conf first, then sync to remote
        # This ensures both syncref and nearby configs have matching values before copying
        sed -i "s/\(sl_CSI_Acquisition[[:space:]]*=[[:space:]]*\)[0-9]\+/\1$csi_acq/g" "$CONF_PATH/sl_ue1.conf"
        sed -i "s/\(sl_PSFCH_Period[[:space:]]*=[[:space:]]*\)[0-9]\+/\1$period/g" "$CONF_PATH/sl_ue1.conf"

        # Now sync both config files to remote (this will copy the updated values)
        sync_config_files $nearby_host_name
    fi

    # For SL mode 2 two-host tests: syncref runs locally, nearby runs remotely
    if [[ $nearby_host_name == "local" ]]; then
        run_syncref_cmd $test_type $mcs $sl_mode "local"
        run_nearby_cmd  $test_type $mcs $sl_mode "local"
    else
        run_syncref_cmd $test_type $mcs $sl_mode "local"
        run_nearby_cmd  $test_type $mcs $sl_mode $nearby_host_name
    fi

    local wait_start=$(date +%s)
    wait_for_tun_interface "oaitun_ue1" "$syncref_host_name" "$duration"
    local remaining=$(( duration - $(date +%s) + wait_start ))
    if [[ "$ensure_ping_test_time" == "1" ]]; then
        [[ $remaining -lt 5 ]] && remaining=5
        wait_for_pc5_sync $remaining
        remaining=$(( duration - $(date +%s) + wait_start ))
        [[ $remaining -lt 16 ]] && remaining=16
        duration=$remaining
    else
        [[ $remaining -gt 0 ]] && wait_for_pc5_sync $remaining
        duration=$(( duration - $(date +%s) + wait_start ))
    fi

    # Additional wait time for sidelink synchronization to complete
    if [[ "$use_extended_delays" == "1" ]]; then
        echo "Waiting additional ${sleep_timing[sync_stab_45s_v2]} seconds for sidelink sync to stabilize..."
        sleep $((0 + ${sleep_timing[sync_stab_45s_v2]}))
    fi

    evaluate_ping_test $syncref_host_name "oaitun_ue1" "10.0.0.100" $sl_mode "${test_name}_csi${csi_acq}_psfch${period}"

    # Cleanup all processes (nrue_host_name was cleaned up in the evaluate_ping_test)
    kill_all $nearby_host_name nr-uesoftmodem

    # Restore configs back to defaults for next test iteration
    echo "==> Restoring baseline: CSI=$DEFAULT_CSI_ACQ, PSFCH=$DEFAULT_PSFCH_PERIOD"
    sync_default_config_params $DEFAULT_CSI_ACQ $DEFAULT_PSFCH_PERIOD
    local end_time=$(date +%s)
    local elapsed=$((end_time - start_time))
    print_runtime $start_time $end_time

    # Print test summary
    print_test_summary "${test_name}_csi${csi_acq}_psfch${period}" "$iteration" "$num_hosts" "$mcs" "$elapsed" "$LAST_TX_PACKETS" "$LAST_RX_PACKETS" "$LAST_TEST_RESULT"
}
#############################################################
rfsim_pc5_csi_acquisition_psfch_period_test_on_two_hosts() {
#############################################################
    # Argumemt(s): csi_acq, psfch_period, duration, mcs, iteration
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && csi_acq=$1; echo "csi_acq = " $1
    [[ $# -ge 2 ]] && period=$2;  echo "psfch_period  = " $2
    [[ $# -ge 3 ]] && duration=$3
    [[ $# -ge 4 ]] && mcs=$4
    [[ $# -ge 5 ]] && iteration=$5
    local test_type="rfsim"
    local syncref_host_name="local"
    local nearby_host_name=$REMOTE_UE_HOST
    local num_hosts=2
    pc5_csi_acquisition_psfch_period_test $csi_acq $period $duration $test_type $mcs $iteration $syncref_host_name $nearby_host_name $num_hosts "${FUNCNAME[0]}"
}
#############################################################
usrp_B210_pc5_csi_acquisition_psfch_period_test_on_two_hosts() {
#############################################################
    # Argumemt(s): csi_acq, psfch_period, duration, mcs, iteration
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && csi_acq=$1; echo "csi_acq = " $1
    [[ $# -ge 2 ]] && period=$2;  echo "psfch_period  = " $2
    [[ $# -ge 3 ]] && duration=$3
    [[ $# -ge 4 ]] && mcs=$4
    [[ $# -ge 5 ]] && iteration=$5
    local test_type="usrp"
    local syncref_host_name="local"
    local nearby_host_name=$REMOTE_UE_HOST
    local num_hosts=2
    pc5_csi_acquisition_psfch_period_test $csi_acq $period $duration $test_type $mcs $iteration $syncref_host_name $nearby_host_name $num_hosts "${FUNCNAME[0]}"
}
#############################################################
rfsim_pc5_csi_acquisition_psfch_period_test_on_local_host() {
#############################################################
    # Argumemt(s): csi_acq, psfch_period, duration, mcs, iteration
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && csi_acq=$1; echo "csi_acq = " $1
    [[ $# -ge 2 ]] && period=$2;  echo "psfch_period  = " $2
    [[ $# -ge 3 ]] && duration=$3
    [[ $# -ge 4 ]] && mcs=$4
    [[ $# -ge 5 ]] && iteration=$5
    local test_type="rfsim"
    local syncref_host_name="local"
    local nearby_host_name="local"
    local num_hosts=1
    pc5_csi_acquisition_psfch_period_test $csi_acq $period $duration $test_type $mcs $iteration $syncref_host_name $nearby_host_name $num_hosts "${FUNCNAME[0]}"
}

#############################################################
### Driver function
#############################################################

# Helper function to check if a test is enabled
is_test_enabled() {
    local test_name=$1
    [[ " ${enabled_tests[@]} " =~ " ${test_name} " ]]
}

cleanup_zombie_processes() {
    echo "=========================================="
    echo "Checking for zombie softmodem processes..."
    echo "=========================================="

    # Cleanup local processes
    local found=0
    for proc in nr-softmodem nr-uesoftmodem nr-cuup; do
        if pgrep -x "$proc" > /dev/null 2>&1; then
            echo "Found zombie process on local: $proc"
            kill_all "local" "$proc"
            found=1
        fi
    done

    # Cleanup remote host processes (skip if remote_host is localhost)
    for remote_host in "$NR_UE_HOST" "$REMOTE_UE_HOST"; do
        if [[ -n "$remote_host" && "$remote_host" != "local" ]]; then
            # Skip localhost checks for BLER tests or if host is "local"
            if [[ "$remote_host" != "local" && "$remote_host" != "localhost" && "$remote_host" != "" ]]; then
                # Get IP to check if it resolves to localhost
                local remote_ip=$(ssh -G "$remote_host" 2>/dev/null | awk '/^hostname / {print $2}')
                if [[ "$remote_ip" != "localhost" && "$remote_ip" != "127.0.0.1" ]]; then
                    for proc in nr-softmodem nr-uesoftmodem nr-cuup; do
                        if safe_ssh "$remote_host" "pgrep -x $proc > /dev/null 2>&1" 2>/dev/null; then
                            echo "Found zombie process on $remote_host: $proc"
                            kill_all "$remote_host" "$proc"
                            found=1
                        fi
                    done
                fi
            fi
        fi
    done

    if [ $found -eq 1 ]; then
        echo "Zombie processes cleaned up."
    else
        echo "No zombie processes found."
    fi
    echo ""
}

main() {
    #########################################################
    ### Configuration already loaded at top of script ###
    #########################################################
    # Config was loaded at script start via BLER_CONFIG_FILE → SL_TEST_CONFIG_FILE
    # Just apply any overrides from config values
    [[ -n "$tx_gain" ]] && TX_GAIN=$tx_gain
    [[ -n "$rx_gain" ]] && RX_GAIN=$rx_gain
    [[ -n "$use_gnome" ]] && USE_GNOME=$use_gnome

    cleanup_zombie_processes

    echo "DEBUG: enabled_tests before resolve: ${enabled_tests[@]}"
    echo "DEBUG: Number of enabled_tests: ${#enabled_tests[@]}"
    echo "DEBUG: test_profile: $test_profile"

    #########################################################
    ### Resolve group names in enabled_tests ###
    #########################################################
    resolve_test_entries() {
        local result=()
        for entry in "$@"; do
            echo "DEBUG: Processing entry: '$entry'" >&2
            if [[ "$entry" =~ ^([a-zA-Z_][a-zA-Z0-9_]*)\[(.+)\]$ ]]; then
                local array_name="${BASH_REMATCH[1]}"
                local spec="${BASH_REMATCH[2]}"
                echo "DEBUG: Matched array '$array_name' with spec '$spec'" >&2
                local -n _g="$array_name"
                echo "DEBUG: Array contents: ${_g[@]}" >&2
                if [[ "$spec" == *:* ]]; then
                    local start=${spec%%:*} end=${spec##*:}
                    echo "DEBUG: Range slice [$start:$end]" >&2
                    result+=("${_g[@]:$start:$((end - start + 1))}")
                else
                    echo "DEBUG: Index slice: $spec" >&2
                    for i in ${spec//,/ }; do result+=("${_g[$i]}"); done
                fi
                echo "DEBUG: Result after this entry: ${result[@]}" >&2
            elif declare -p "$entry" 2>/dev/null | grep -q 'declare -a'; then
                local -n _g="$entry"
                result+=("${_g[@]}")
            else
                result+=("$entry")
            fi
        done
        echo "${result[@]}"
    }

    # Resolve repeatedly until no group names remain
    while true; do
        resolved_tests=($(resolve_test_entries "${enabled_tests[@]}"))
        [[ "${resolved_tests[*]}" == "${enabled_tests[*]}" ]] && break
        enabled_tests=("${resolved_tests[@]}")
    done
    enabled_tests=("${resolved_tests[@]}")

    echo "DEBUG: Resolved tests: ${enabled_tests[@]}"
    echo "DEBUG: Number of tests: ${#enabled_tests[@]}"

    #########################################################
    ### Execute tests in order specified by enabled_tests ###
    #########################################################
    for test_entry in "${enabled_tests[@]}"; do
        # Parse test name and optional CSI/PSFCH parameters
        # Format: test_name or test_name:csi_acq:psfch_period
        IFS=':' read -r test_name csi_param psfch_param <<< "$test_entry"

        # Determine test type and parameter array based on test name
        # Check for BLER first (before rfsim_* check, since BLER names contain "rfsim")
        if [[ $test_name == *"bler"* ]]; then
            # BLER tests: use noise_power_array and iteration range
            param_array=("${noise_power_array[@]}")
            is_bler=true
            iteration_start_val=${iteration_start:-1}
            iteration_end_val=${iteration_end:-$num_repeat}
        elif [[ $test_name == rfsim_* ]]; then
            # RFsim tests: use snr_array
            param_array=("${snr_array[@]}")
            is_bler=false
            iteration_start_val=1
            iteration_end_val=$num_repeat
        elif [[ $test_name == usrp_* ]]; then
            # USRP tests: use atten_array
            param_array=("${atten_array[@]}")
            is_bler=false
            iteration_start_val=1
            iteration_end_val=$num_repeat
        else
            echo "WARNING: Unknown test type for '$test_name'"
            continue
        fi

        # Run test with parameter sweeps
        # For BLER: iteration -> noise -> MCS (outer to inner)
        # For others: param -> iteration -> MCS
        if [[ $is_bler == true ]]; then
            # BLER loop order: iteration -> noise_power -> MCS
            for k in $(seq $iteration_start_val 1 $iteration_end_val); do
                for param in "${param_array[@]}"; do
                    for mcs in ${mcs_array[@]}; do
                        # Call BLER test with noise_power as 4th parameter
                        $test_name $duration $mcs $k $param
                        sleep 3
                    done
                done
            done
        else
            # Standard tests loop order: param -> iteration -> MCS
            for param in "${param_array[@]}"; do
                for k in $(seq $iteration_start_val 1 $iteration_end_val); do
                    for mcs in ${mcs_array[@]}; do
                        # CSI/PSFCH tests need special handling
                        if [[ $test_name == *"csi_acquisition_psfch"* ]]; then
                            if [[ -n "$csi_param" && -n "$psfch_param" ]]; then
                                # Run specific CSI/PSFCH combination
                                $test_name $csi_param $psfch_param $duration $mcs $k
                            elif [[ -n "$csi_param" && -z "$psfch_param" ]]; then
                                # Run specific CSI with all PSFCH values (e.g., test:1:)
                                for psfch in 0 1 2 3; do
                                    $test_name $csi_param $psfch $duration $mcs $k
                                done
                            elif [[ -z "$csi_param" && -n "$psfch_param" ]]; then
                                # Run specific PSFCH with all CSI values (e.g., test::1)
                                for csi in 0 1; do
                                    $test_name $csi $psfch_param $duration $mcs $k
                                done
                            else
                                # Run all 8 combinations
                                $test_name 0 0 $duration $mcs $k
                                $test_name 0 1 $duration $mcs $k
                                $test_name 0 2 $duration $mcs $k
                                $test_name 0 3 $duration $mcs $k
                                $test_name 1 0 $duration $mcs $k
                                $test_name 1 1 $duration $mcs $k
                                $test_name 1 2 $duration $mcs $k
                                $test_name 1 3 $duration $mcs $k
                            fi
                        else
                            # All other tests: just call with standard parameters
                            $test_name $duration $mcs $k
                        fi
                        sleep 3 # delay in second between tests.
                    done
                done
            done
        fi
    done

    #########################################################
    ### Display final summary ###
    #########################################################
    echo ""
    echo "=========================================="
    echo "Test Execution Complete"
    echo "=========================================="

    if [ -f "$test_summary_file" ]; then
        echo "Summary saved to: $test_summary_file"
        echo ""
        echo "All Test Results:"
        cat "$test_summary_file"
        echo ""
    else
        echo "No tests were executed (check test flags and num_hosts configuration)"
        echo ""
    fi
}
main
