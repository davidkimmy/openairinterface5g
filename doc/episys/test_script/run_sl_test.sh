#!/bin/bash
#############################################################
# Standalone shell script
# Usage:
#   Shell> ./run_sl_test.sh [-d <base_dir>]
#############################################################

timestamp=$(date +"%Y%m%d_%H%M%S")
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")

# Priority: CLI -d > config base_log_dir > SCRIPT_DIR
base_dir="$SCRIPT_DIR"
source "$SCRIPT_DIR/run_sl_test_config.sh" 2>/dev/null
[[ -n "$base_log_dir" ]] && base_dir="${base_log_dir/#\~/$HOME}"

while getopts "d:" opt; do
    case $opt in
        d) base_dir="$OPTARG" ;;
        *) echo "Usage: $0 [-d <base_dir>]"; exit 1 ;;
    esac
done
shift $((OPTIND - 1))

log_dir="$base_dir/test_${timestamp}"
mkdir -p "$log_dir"
ln -sfn "test_${timestamp}" "$base_dir/latest"
echo "Log files will be saved at $log_dir"

test_summary_file="$log_dir/test_summary_${timestamp}.csv"

CONF_PATH=$HOME/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF
REMOTE_UE_HOST="remote_ue" # host name in the ~/.ssh/config
REMOTE_HOST_IP=$(ssh -G $REMOTE_UE_HOST | awk '/^hostname / {print $2}')
echo "Remote Host IP address = " $REMOTE_HOST_IP

LOCAL_HOST="local" # host name in the ~/.ssh/config
LOCAL_HOST_IP=$(ssh -G $LOCAL_HOST | awk '/^hostname / {print $2}') #  #LOCAL_HOST_IP=$(ip route get 1.2.3.4 | awk '{print $7}')
echo "Local Host IP address = " $LOCAL_HOST_IP

RELAY_UE_HOST="relay_ue" # host name in the ~/.ssh/config
RELAY_UE_HOST_IP=$(ssh -G $RELAY_UE_HOST | awk '/^hostname / {print $2}')
echo "Relay UE Host IP address = " $RELAY_UE_HOST_IP

NR_UE_HOST="nr_ue" # host name in the ~/.ssh/config
NR_UE_HOST_IP=$(ssh -G $NR_UE_HOST | awk '/^hostname / {print $2}')
echo "nrUE Host IP address = " $NR_UE_HOST_IP

GNB_HOST="gNB" # host name in the ~/.ssh/config
GNB_HOST_IP=$(ssh -G $GNB_HOST | awk '/^hostname / {print $2}')
echo "gNB Host IP address = " $GNB_HOST_IP

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

run_cmd() {
    for m in "$@"; do
        gnome-terminal -- bash -c "source ~/.bashrc 2>/dev/null; $m"
    done
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
    printf "Attempting to terminate PIDs: %s\n" "$*"
    echo "$@" | xargs -n 1 sudo kill -9
}

kill_process() {
    printf "Removing %s\n" "$*"
    for process_name in "$@"; do
        sudo killall -KILL "$process_name" 2>/dev/null
        sleep 1
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
    sleep 3

    local elapsed=3
    while [ $elapsed -lt $timeout ]; do
        echo "Checking for $iface... (${elapsed}s elapsed)"
        local interface_exists=0
        if [[ "$host" == "local" ]]; then
            if ifconfig | grep -q "$iface"; then
                interface_exists=1
            fi
        else
            if ssh "$host" "ifconfig | grep -q $iface"; then
                interface_exists=1
            fi
        fi
        if [ $interface_exists -eq 1 ]; then
            echo "Tunnel interface $iface detected on $host, waiting for interface sync..."
            sleep 2
            return 0
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    echo "ERROR: $iface not available on $host after ${timeout}s"
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
    local current_value=$(ssh "$remote_host" "grep '$param_name' '$config_file' | grep -oP '\d+' | head -1")

    if [ "$current_value" != "$default_value" ]; then
        echo "Restoring remote $config_file: $param_name from $current_value to $default_value"
        ssh "$remote_host" "sed -i 's/$param_name *= $current_value/$param_name = $default_value/g' '$config_file'"
    fi
}

find_user_name() {
    local host=$1
    if [[ $host == "local" ]]; then
        whoami
    else
        ssh -G "$host" | awk '/^user / {print $2}'
    fi
}

copy_syncref_ue_logs() {
    # Copy syncref UE logs from remote host to local log directory and clean up
    local syncref_host=$1
    local sl_mode=$2

    local syncref_user=$(find_user_name "$syncref_host")
    if [[ $sl_mode -eq 1 ]]; then
        scp $syncref_host:/home/$syncref_user/result_nrUE_syncref.log $log_dir/result_nrUE_syncref_remote.log 2>/dev/null
        ssh $syncref_host "rm -f /home/$syncref_user/result_nrUE_syncref.log" 2>/dev/null
    elif [[ $sl_mode -eq 2 ]]; then
        scp $syncref_host:/home/$syncref_user/result_syncref.log $log_dir/result_syncref_remote.log 2>/dev/null
        ssh $syncref_host "rm -f /home/$syncref_user/result_syncref.log" 2>/dev/null
    fi
}

copy_nearby_ue_logs() {
    # Copy nearby UE logs from remote host to local log directory and clean up
    local nearby_host=$1

    local nearby_user=$(find_user_name "$nearby_host")
    scp $nearby_host:/home/$nearby_user/result_nearby.log $log_dir/result_nearby_remote.log 2>/dev/null
    ssh $nearby_host "rm -f /home/$nearby_user/result_nearby.log" 2>/dev/null
}

evaluate_ping_test() {
    [[ $# -ge 1 ]] && host_name=$1
    [[ $# -ge 2 ]] && src_if=$2
    [[ $# -ge 3 ]] && dest_ip=$3
    [[ $# -ge 4 ]] && local sl_mode=$4
    [[ $# -ge 5 ]] && local test_name=$5
    [[ $# -ge 6 ]] && local syncref_host=$6
    [[ $# -ge 7 ]] && local nearby_host=$7

    local user_name
    user_name=$(find_user_name "$host_name")
    # Generate unique filename with timestamp
    local timestamp=$(date +%Y%m%d_%H%M%S)

    if [[ $host_name == "local" ]]; then
        # Run ping locally
        ping_output="$log_dir/ping_result_${test_name}_${timestamp}.txt"
        ping_cmd="ping -c 15 -I $src_if $dest_ip 2>&1 | tee $ping_output"
        echo "Ping command: $ping_cmd"
        run_cmd "$ping_cmd"
    else
        # Run ping on remote host
        remote_log_dir="/home/$user_name/test_${timestamp}"
        ping_output="$remote_log_dir/ping_result_${test_name}_${timestamp}.txt"
        echo "Ping command (remote): ping -c 15 -I $src_if $dest_ip on $host_name"
        local safe_filename="ping_result_${test_name}_${timestamp}.txt"

        # Build remote command with proper variable expansion
        local cmd="source /home/$user_name/.bashrc 2>/dev/null; mkdir -p $remote_log_dir && cd $remote_log_dir && ping -c 15 -I $src_if $dest_ip 2>&1 | tee $safe_filename"
        gnome-terminal -- bash -c "ssh -t $host_name '$cmd'"
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
        # Copy ping result from remote host and clean up remote log directory
        scp $host_name:$remote_log_dir/* $log_dir/ 2>/dev/null
        ssh $host_name "rm -rf $remote_log_dir" 2>/dev/null

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

    # Copy UE logs from remote hosts before reading stats
    if [[ -n "$syncref_host" && "$syncref_host" != "local" ]]; then
        copy_syncref_ue_logs "$syncref_host" "$sl_mode"
    fi
    if [[ -n "$nearby_host" && "$nearby_host" != "local" ]]; then
        copy_nearby_ue_logs "$nearby_host"
    fi

    # Get PSSCH statistics from logs
    # sl_mode 1 uses result_nrUE_syncref.log, sl_mode 2 uses result_syncref.log
    # For PC5 two-host tests: syncref runs locally, nearby runs remotely
    if [[ $sl_mode -eq 0 ]]; then
        local syncref_log=""
    elif [[ $sl_mode -eq 1 ]]; then
        # sl_mode 1: check local first, then remote
        if [ -f "$HOME/result_nrUE_syncref.log" ]; then
            local syncref_log="$HOME/result_nrUE_syncref.log"
        elif [ -f "$log_dir/result_nrUE_syncref_remote.log" ]; then
            local syncref_log="$log_dir/result_nrUE_syncref_remote.log"
        else
            local syncref_log=""
        fi
    elif [[ $sl_mode -eq 2 ]]; then
        # sl_mode 2: check local first, then remote
        if [ -f "$HOME/result_syncref.log" ]; then
            local syncref_log="$HOME/result_syncref.log"
        elif [ -f "$log_dir/result_syncref_remote.log" ]; then
            local syncref_log="$log_dir/result_syncref_remote.log"
        else
            local syncref_log=""
        fi
    fi

    if [[ $sl_mode -eq 0 ]]; then
        if [[ $host_name == "local" ]]; then
            local nrue_log="$HOME/result_nrUE.log"
        else
            local nrue_log="$log_dir/result_nrUE_remote.log"
        fi
        local nearby_log=""
    else
        # For sidelink: check local first, then remote
        if [ -f "$HOME/result_nearby.log" ]; then
            local nearby_log="$HOME/result_nearby.log"
        elif [ -f "$log_dir/result_nearby_remote.log" ]; then
            local nearby_log="$log_dir/result_nearby_remote.log"
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
                    --rfsimulator.serveraddr server --rfsimulator.serverport 4048 --rfsim --sa --log_config.global_log_level info \
                    $sl_relay_tag 2>&1 | tee $HOME/result_gNB.log"
        else
            gNB_cmd="LD_LIBRARY_PATH=/home/$user_name/openairinterface5g/cmake_targets/ran_build/build:$LD_LIBRARY_PATH \
                    sudo -E /home/$user_name/openairinterface5g/cmake_targets/ran_build/build/nr-softmodem \
                    -O /home/$user_name/openairinterface5g/targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band78.fr1.106PRB.usrpb210$conf_tag.conf --gNBs.[0].min_rxtxtime 6 \
                    --rfsimulator.serveraddr server --rfsimulator.serverport 4048 --rfsim --sa --log_config.global_log_level info \
                    $sl_relay_tag 2>&1 | tee /home/$user_name/result_gNB.log"
        fi
    elif [[ $test_type == "usrp" ]]; then
        gNB_cmd="cd $HOME/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=$HOME/openairinterface5g/cmake_targets/ran_build/build ./nr-softmodem \
                -O ../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band78.fr1.106PRB.usrpb210$conf_tag.conf --gNBs.[0].min_rxtxtime 6 \
                -E --sa  --ue-txgain ${TX_GAIN} --ue-rxgain ${RX_GAIN}  --device.name oai_usrpdevif $sl_relay_tag 2>&1 | tee /home/$user_name/result_gNB.log"
    fi
    echo $gNB_cmd; echo
    if [[ $host_name == "local" ]]; then
        run_cmd "$gNB_cmd"; sleep 1
    else
        gnome-terminal -- bash -c "ssh $host_name '$gNB_cmd'"; sleep 1
    fi
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
                    --rfsimulator.serveraddr 127.0.0.1 --rfsimulator.serverport 4048 --rfsim --sa \
                    --log_config.global_log_level info 2>&1 | tee $HOME/result_nrUE.log"
        else
            nrUE_cmd="LD_LIBRARY_PATH=/home/$user_name/openairinterface5g/cmake_targets/ran_build/build:$LD_LIBRARY_PATH \
                    sudo -E /home/$user_name/openairinterface5g/cmake_targets/ran_build/build/nr-uesoftmodem \
                    -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 \
                    --rfsimulator.serveraddr $LOCAL_HOST_IP --rfsimulator.serverport 4048 --rfsim --sa \
                    --log_config.global_log_level info 2>&1 | tee /home/$user_name/result_nrUE.log"
        fi
    elif [[ $test_type == "usrp" ]]; then
        nrUE_cmd="LD_LIBRARY_PATH=/home/$user_name/openairinterface5g/cmake_targets/ran_build/build \
                    sudo -E /home/$user_name/openairinterface5g/cmake_targets/ran_build/build/nr-uesoftmodem \
                    -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 \
                    -E --sa --ue-txgain ${TX_GAIN} --ue-rxgain ${RX_GAIN} --thread-pool -1,-1 --device.name oai_usrpdevif \
                    --log_config.global_log_level info 2>&1 | tee /home/$user_name/result_nrUE.log"
    fi
    echo $nrUE_cmd; echo
    if [[ $host_name == "local" ]]; then
        run_cmd "$nrUE_cmd"; sleep 1
    else
        gnome-terminal -- bash -c "ssh $host_name '$nrUE_cmd'"; sleep 1
    fi
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
                            --rfsim --sa --sync-ref --sl-mode 1 \
                            --rfsimulator.serveraddr 127.0.0.1 --rfsimulator.serverport 4048 \
                            --rfsimulator.serveraddrsl 127.0.0.1 --rfsimulator.serverportsl 4148 \
                            --log_config.global_log_level info --relay-type 1 --is-relay-ue 1  $mcs  2>&1 | tee $HOME/result_nrUE_syncref.log"
            else
                syncref_cmd="LD_LIBRARY_PATH=/home/$user_name/openairinterface5g/cmake_targets/ran_build/build:$LD_LIBRARY_PATH \
                            sudo -E /home/$user_name/openairinterface5g/cmake_targets/ran_build/build/nr-uesoftmodem \
                            -O /home/$user_name/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf \
                            -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 \
                            --rfsim --sa  --sync-ref --sl-mode 1 --relay-type 1 --is-relay-ue 1 \
                            --rfsimulator.serveraddr $GNB_HOST_IP  --rfsimulator.serverport 4048 \
                            --rfsimulator.serveraddrsl $REMOTE_HOST_IP  --rfsimulator.serverportsl 4148 \
                            --log_config.global_log_level info $mcs  2>&1 | tee /home/$user_name/result_nrUE_syncref.log"
            fi
        elif [[ $test_type == "usrp" ]]; then
            syncref_cmd="cd $HOME/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=$HOME/openairinterface5g/cmake_targets/ran_build/build ./nr-uesoftmodem \
                        -O $HOME/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf \
                        -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 \
                        -E --sa --sl-mode 1 --sync-ref --node-number 2 --ip-demo 1 --relay-type 1 --is-relay-ue 1 \
                        --usrp-args 'serial=$RELAY_UE_USRP_SN_FOR_UU,type=b200' --usrp-args-sl 'serial=$RELAY_UE_USRP_SN_FOR_SL,type=b200' \
                        --ue-txgain ${TX_GAIN} --ue-rxgain ${RX_GAIN} --device.name oai_usrpdevif $mcs 2>&1 | tee $HOME/result_nrUE_syncref.log"
        fi
    elif [[ $sl_mode -eq 2 ]]; then
        if [[ $test_type == "rfsim" ]]; then
            syncref_cmd="cd $HOME/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=$HOME/openairinterface5g/cmake_targets/ran_build/build \
                         $HOME/openairinterface5g/cmake_targets/ran_build/build/nr-uesoftmodem \
                        -O $HOME/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf --sync-ref --sl-mode 2 --rfsim --sa \
                        --rfsimulator.serveraddrsl server --rfsimulator.serverportsl 4148 --log_config.global_log_level info  $mcs 2>&1 | tee $HOME/result_syncref.log"
        elif [[ $test_type == "usrp" ]]; then
            syncref_cmd="cd $HOME/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=$HOME/openairinterface5g/cmake_targets/ran_build/build \
                        $HOME/openairinterface5g/cmake_targets/ran_build/build/nr-uesoftmodem \
                        -O $HOME/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf -E --sa --sl-mode 2 --sync-ref \
                        --ue-txgain ${TX_GAIN} --ue-rxgain ${RX_GAIN} --thread-pool -1,-1 --device.name oai_usrpdevif $mcs 2>&1 | tee $HOME/result_syncref.log"
        fi
    fi
    echo $syncref_cmd; echo;
    if [[ $host_name == "local" ]] || [[ $host_name == "" ]] ; then
        run_cmd "$syncref_cmd"; sleep 1
    else
        gnome-terminal -- bash -c "ssh $host_name '$syncref_cmd'"; sleep 1
    fi
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
                            -O $HOME/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf --rfsim --sa --sl-mode 2 $mcs \
                            --rfsimulator.serveraddrsl server --rfsimulator.serverportsl 4148 \
                            --log_config.global_log_level info --relay-type 1  2>&1 | tee $HOME/result_nearby.log"
            else
                nearby_cmd="LD_LIBRARY_PATH=/home/$user_name/openairinterface5g/cmake_targets/ran_build/build:$LD_LIBRARY_PATH \
                            sudo -E /home/$user_name/openairinterface5g/cmake_targets/ran_build/build/nr-uesoftmodem \
                            -O /home/$user_name/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf --rfsim --sa --sl-mode 2 $mcs \
                            --rfsimulator.serveraddrsl server --rfsimulator.serverportsl 4148 \
                            --log_config.global_log_level info --relay-type 1  2>&1 | tee /home/$user_name/result_nearby.log"
            fi
        elif [[ $test_type == "usrp" ]]; then
            nearby_cmd="LD_LIBRARY_PATH=/home/$user_name/openairinterface5g/cmake_targets/ran_build/build \
                        sudo -E /home/$user_name/openairinterface5g/cmake_targets/ran_build/build/nr-uesoftmodem \
                        -O /home/$user_name/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf -E --sa --sl-mode 2 --relay-type 1 \
                        --ue-txgain ${TX_GAIN} --ue-rxgain ${RX_GAIN} --thread-pool -1,-1 --device.name oai_usrpdevif $mcs 2>&1 | tee /home/$user_name/result_nearby.log"
        fi
    elif [[ $sl_mode -eq 2 ]]; then
        if [[ $test_type == "rfsim" ]]; then
            if [[ $host_name == 'local' ]]; then
                nearby_cmd="cd $HOME/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=$HOME/openairinterface5g/cmake_targets/ran_build/build ./nr-uesoftmodem \
                        -O $HOME/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf --rfsim --sa --sl-mode 2 $mcs \
                        --rfsimulator.serveraddrsl 127.0.0.1 --rfsimulator.serverportsl 4148 --log_config.global_log_level info  2>&1 | tee $HOME/result_nearby.log"
            else
                nearby_cmd="LD_LIBRARY_PATH=/home/$user_name/openairinterface5g/cmake_targets/ran_build/build:$LD_LIBRARY_PATH \
                        sudo -E /home/$user_name/openairinterface5g/cmake_targets/ran_build/build/nr-uesoftmodem \
                        -O /home/$user_name/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf --rfsim --sa --sl-mode 2 $mcs \
                        --rfsimulator.serveraddrsl $LOCAL_HOST_IP --rfsimulator.serverportsl 4148 --log_config.global_log_level info 2>&1 | tee /home/$user_name/result_nearby.log"
            fi
        elif [[ $test_type == "usrp" ]]; then
            nearby_cmd="LD_LIBRARY_PATH=/home/$user_name/openairinterface5g/cmake_targets/ran_build/build \
                        sudo -E /home/$user_name/openairinterface5g/cmake_targets/ran_build/build/nr-uesoftmodem \
                        -O /home/$user_name/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf -E --sa --sl-mode 2 \
                        --ue-txgain ${TX_GAIN} --ue-rxgain ${RX_GAIN} --thread-pool -1,-1 --device.name oai_usrpdevif $mcs 2>&1 | tee /home/$user_name/result_nearby.log"
        fi
    fi
    echo $nearby_cmd; echo
    if [[ $host_name == "local" ]] || [[ $host_name == "" ]] ; then
        run_cmd "$nearby_cmd"; sleep 1
    else
        gnome-terminal -- bash -c "ssh $host_name '$nearby_cmd'"; sleep 1
    fi
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
    validate_test_type_for_local_host $test_type $test_name || return 1

    # Clean up old log files to prevent accumulation of statistics
    rm -f $HOME/result_syncref.log $HOME/result_nearby.log $HOME/result_nrUE_syncref.log 2>/dev/null
    if [[ $syncref_host_name != "local" ]]; then
        ssh $syncref_host_name "rm -f ~/result_syncref.log ~/result_nearby.log ~/result_nrUE_syncref.log" 2>/dev/null
    fi
    if [[ $nearby_host_name != "local" && $nearby_host_name != "$syncref_host_name" ]]; then
        ssh $nearby_host_name "rm -f ~/result_syncref.log ~/result_nearby.log ~/result_nrUE_syncref.log" 2>/dev/null
    fi

    local start_time=$(date +%s)
    local sl_mode=1
    local src_if="oaitun_ue2"
    local dest_ip="8.8.8.8"

    pre1='docker ps | grep oai-upf | wc -l' # expecting: 1
    act1='echo "core network is required !!!"; cd ~/oai-cn5g; systemctl start docker.service; docker compose up -d; sleep 2'
    [[ $(eval "$pre1") -eq 1 ]] && echo "Requirements are satisfied !!!" || eval "$act1"

    run_gNB_cmd $test_type $sl_mode $gnb_host_name
    run_syncref_cmd $test_type $mcs $sl_mode $syncref_host_name
    run_nearby_cmd  $test_type $mcs $sl_mode $nearby_host_name
    wait_for_tun_interface $src_if $nearby_host_name $duration
    evaluate_ping_test $nearby_host_name $src_if $dest_ip $sl_mode $test_name $syncref_host_name $nearby_host_name

    # Cleanup all processes (nearby_host_name was cleaned up in the evaluate_ping_test)
    kill_all $syncref_host_name nr-uesoftmodem
    kill_all $gnb_host_name nr-softmodem

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

    # Clean up old log files to prevent accumulation of statistics
    rm -f $HOME/result_gNB.log $HOME/result_nrUE.log 2>/dev/null
    if [[ $gnb_host_name != "local" ]]; then
        ssh $gnb_host_name "rm -f ~/result_gNB.log" 2>/dev/null
    fi
    if [[ $nrue_host_name != "local" ]]; then
        ssh $nrue_host_name "rm -f ~/result_nrUE.log" 2>/dev/null
    fi

    local start_time=$(date +%s)
    local sl_mode=0
    local src_if="oaitun_ue1"
    local dest_ip="8.8.8.8"

    pre1='docker ps | grep oai-upf | wc -l' # expecting: 1
    act1='echo "core network is required !!!"; cd ~/oai-cn5g; systemctl start docker.service; docker compose up -d; sleep 2'
    [[ $(eval "$pre1") -eq 1 ]] && echo "Requirements are satisfied !!!" || eval "$act1"

    run_gNB_cmd $test_type $sl_mode $gnb_host_name
    run_nrUE_cmd $test_type $mcs $sl_mode $nrue_host_name
    wait_for_tun_interface $src_if $nrue_host_name $duration
    evaluate_ping_test $nrue_host_name $src_if $dest_ip $sl_mode "${test_name}"

    # Cleanup all processes (nrue_host_name was cleaned up in the evaluate_ping_test)
    kill_all $gnb_host_name nr-softmodem

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
    # Clean up old log files to prevent accumulation of statistics
    rm -f $HOME/result_syncref.log $HOME/result_nearby.log 2>/dev/null
    if [[ $syncref_host_name != "local" ]]; then
        ssh $syncref_host_name "rm -f ~/result_syncref.log ~/result_nearby.log" 2>/dev/null
    fi
    if [[ $nearby_host_name != "local" ]]; then
        ssh $nearby_host_name "rm -f ~/result_syncref.log ~/result_nearby.log" 2>/dev/null
    fi

    local start_time=$(date +%s)
    local sl_mode=2
    local src_if="oaitun_ue1"
    local dest_ip="10.0.0.100"

    echo "test_type:" $test_type " mcs: " $mcs " sl_mode: " $sl_mode " syncref_host_name: " $syncref_host_name " nearby_host_name: " $nearby_host_name

    run_syncref_cmd $test_type $mcs $sl_mode $syncref_host_name
    run_nearby_cmd  $test_type $mcs $sl_mode $nearby_host_name
    wait_for_tun_interface $src_if $syncref_host_name $duration
    evaluate_ping_test $syncref_host_name $src_if $dest_ip $sl_mode "${test_name}" $syncref_host_name $nearby_host_name

    # Cleanup all processes (nrue_host_name was cleaned up in the evaluate_ping_test)
    kill_all $nearby_host_name nr-uesoftmodem

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

    local start_time=$(date +%s)
    local sl_mode=2

    # Clean up old log files to prevent stale statistics
    rm -f $HOME/result_syncref.log $HOME/result_nearby.log 2>/dev/null
    if [[ $syncref_host_name != "local" ]]; then
        ssh $syncref_host_name "rm -f ~/result_syncref.log ~/result_nearby.log" 2>/dev/null
    fi
    if [[ $nearby_host_name != "local" ]]; then
        ssh $nearby_host_name "rm -f ~/result_syncref.log ~/result_nearby.log" 2>/dev/null
    fi

    # Check and restore default values if needed
    echo "Checking config file default values..."
    check_and_restore_default "$CONF_PATH/sl_sync_ref.conf" "sl_CSI_Acquisition" "$DEFAULT_CSI_ACQ"
    check_and_restore_default "$CONF_PATH/sl_sync_ref.conf" "sl_PSFCH_Period" "$DEFAULT_PSFCH_PERIOD"
    if [[ $nearby_host_name == "local" ]]; then
        check_and_restore_default "$CONF_PATH/sl_ue1.conf" "sl_CSI_Acquisition" "$DEFAULT_CSI_ACQ"
        check_and_restore_default "$CONF_PATH/sl_ue1.conf" "sl_PSFCH_Period" "$DEFAULT_PSFCH_PERIOD"
    else
        local REMOTE_CONF_PATH="/home/$remote_user/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF"
        check_and_restore_default_remote "$nearby_host_name" "$REMOTE_CONF_PATH/sl_ue1.conf" "sl_CSI_Acquisition" "$DEFAULT_CSI_ACQ"
        check_and_restore_default_remote "$nearby_host_name" "$REMOTE_CONF_PATH/sl_ue1.conf" "sl_PSFCH_Period" "$DEFAULT_PSFCH_PERIOD"
    fi

    # Update local syncref config (always runs locally)
    pre1="sed -i 's/sl_CSI_Acquisition         = $DEFAULT_CSI_ACQ/sl_CSI_Acquisition         = $csi_acq/g' $CONF_PATH/sl_sync_ref.conf"
    pre3="sed -i 's/sl_PSFCH_Period                 = $DEFAULT_PSFCH_PERIOD/sl_PSFCH_Period                 = $period/g' $CONF_PATH/sl_sync_ref.conf"
    pst1="sed -i 's/sl_CSI_Acquisition         = $csi_acq/sl_CSI_Acquisition         = $DEFAULT_CSI_ACQ/g' $CONF_PATH/sl_sync_ref.conf"
    pst3="sed -i 's/sl_PSFCH_Period                 = $period/sl_PSFCH_Period                 = $DEFAULT_PSFCH_PERIOD/g' $CONF_PATH/sl_sync_ref.conf"
    eval "$pre1"; eval "$pre3";

    # Update nearby config (local or remote depending on nearby_host_name)
    if [[ $nearby_host_name == "local" ]]; then
        # Local nearby: update local sl_ue1.conf
        pre2="sed -i 's/sl_CSI_Acquisition         = $DEFAULT_CSI_ACQ/sl_CSI_Acquisition         = $csi_acq/g' $CONF_PATH/sl_ue1.conf"
        pre4="sed -i 's/sl_PSFCH_Period                 = $DEFAULT_PSFCH_PERIOD/sl_PSFCH_Period                 = $period/g' $CONF_PATH/sl_ue1.conf"
        pst2="sed -i 's/sl_CSI_Acquisition         = $csi_acq/sl_CSI_Acquisition         = $DEFAULT_CSI_ACQ/g' $CONF_PATH/sl_ue1.conf"
        pst4="sed -i 's/sl_PSFCH_Period                 = $period/sl_PSFCH_Period                 = $DEFAULT_PSFCH_PERIOD/g' $CONF_PATH/sl_ue1.conf"
        eval "$pre2"; eval "$pre4";
    else
        # Remote nearby: update remote sl_ue1.conf
        [[ -z "$REMOTE_CONF_PATH" ]] && local REMOTE_CONF_PATH="/home/$remote_user/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF"
        ssh $nearby_host_name "sed -i 's/sl_CSI_Acquisition         = $DEFAULT_CSI_ACQ/sl_CSI_Acquisition         = $csi_acq/g' $REMOTE_CONF_PATH/sl_ue1.conf"
        ssh $nearby_host_name "sed -i 's/sl_PSFCH_Period                 = $DEFAULT_PSFCH_PERIOD/sl_PSFCH_Period                 = $period/g' $REMOTE_CONF_PATH/sl_ue1.conf"
    fi

    # For SL mode 2 two-host tests: syncref runs locally, nearby runs remotely
    if [[ $nearby_host_name == "local" ]]; then
        run_syncref_cmd $test_type $mcs $sl_mode "local"
        run_nearby_cmd  $test_type $mcs $sl_mode "local"
    else
        run_syncref_cmd $test_type $mcs $sl_mode "local"
        run_nearby_cmd  $test_type $mcs $sl_mode $nearby_host_name
    fi

    wait_for_tun_interface "oaitun_ue1" "$syncref_host_name" "$duration"
    evaluate_ping_test $syncref_host_name "oaitun_ue1" "10.0.0.100" $sl_mode "${test_name}_csi${csi_acq}_psfch${period}" "local" $nearby_host_name

    # Cleanup all processes (nrue_host_name was cleaned up in the evaluate_ping_test)
    kill_all $nearby_host_name nr-uesoftmodem

    # Restore configs
    eval "$pst1"; eval "$pst3";
    if [[ $nearby_host_name == "local" ]]; then
        eval "$pst2"; eval "$pst4";
    else
        # Restore remote config
        [[ -z "$REMOTE_CONF_PATH" ]] && local REMOTE_CONF_PATH="/home/$remote_user/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF"
        ssh $nearby_host_name "sed -i 's/sl_CSI_Acquisition         = $csi_acq/sl_CSI_Acquisition         = $DEFAULT_CSI_ACQ/g' $REMOTE_CONF_PATH/sl_ue1.conf"
        ssh $nearby_host_name "sed -i 's/sl_PSFCH_Period                 = $period/sl_PSFCH_Period                 = $DEFAULT_PSFCH_PERIOD/g' $REMOTE_CONF_PATH/sl_ue1.conf"
    fi
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
    [[ $# -ge 4 ]] && iteration=$4
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
    [[ $# -ge 4 ]] && iteration=$4
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
    [[ $# -ge 4 ]] && iteration=$4
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
    local found=0
    for proc in nr-softmodem nr-uesoftmodem nr-cuup; do
        if pgrep -x "$proc" > /dev/null 2>&1; then
            echo "Found zombie process: $proc"
            kill_all "local" "$proc"
            found=1
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
    ### Load test configuration from external config file ###
    #########################################################
    local config_file="$(dirname "$0")/run_sl_test_config.sh"

    if [ -f "$config_file" ]; then
        echo "Loading configuration from: $config_file"
        source "$config_file"
        if [ $? -ne 0 ]; then
            echo "ERROR: Failed to load configuration file"
            return 1
        fi
        # Override defaults with config values if set
        [[ -n "$tx_gain" ]] && TX_GAIN=$tx_gain
        [[ -n "$rx_gain" ]] && RX_GAIN=$rx_gain
    else
        echo "ERROR: Configuration file not found: $config_file"
        echo "Please create run_sl_test_config.sh in the same directory"
        return 1
    fi

    cleanup_zombie_processes

    #########################################################
    ### Resolve group names in enabled_tests ###
    #########################################################
    resolve_test_entries() {
        local result=()
        for entry in "$@"; do
            if [[ "$entry" =~ ^([a-zA-Z_][a-zA-Z0-9_]*)\[(.+)\]$ ]]; then
                local -n _g="${BASH_REMATCH[1]}"
                local spec="${BASH_REMATCH[2]}"
                if [[ "$spec" == *:* ]]; then
                    local start=${spec%%:*} end=${spec##*:}
                    result+=("${_g[@]:$start:$((end - start + 1))}")
                else
                    for i in ${spec//,/ }; do result+=("${_g[$i]}"); done
                fi
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

    #########################################################
    ### Execute tests in order specified by enabled_tests ###
    #########################################################
    for test_entry in "${enabled_tests[@]}"; do
        # Parse test name and optional CSI/PSFCH parameters
        # Format: test_name or test_name:csi_acq:psfch_period
        IFS=':' read -r test_name csi_param psfch_param <<< "$test_entry"

        # Determine test parameters based on test name
        if [[ $test_name == rfsim_* ]]; then
            local param_array=("${snr_array[@]}")
        elif [[ $test_name == usrp_* ]]; then
            local param_array=("${atten_array[@]}")
        else
            echo "WARNING: Unknown test type for '$test_name'"
            continue
        fi

        # Run test with parameter sweeps
        for param in "${param_array[@]}"; do
            for k in $(seq 1 1 $num_repeat); do
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
