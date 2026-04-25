#!/bin/bash
#############################################################
# Standalone shell script
# Usage:
#   Shell> ./run_sl_test.sh
#############################################################

dir_name="results_sl_test"
if [ -d $HOME/$dir_name ]; then
    #rm -rf $HOME/$dir_name/*.*
    echo "Need update: " "log file will be saved at $HOME/$dir_name"
else
    mkdir $HOME/$dir_name
fi

EXEC_PATH=$HOME/openairinterface5g/cmake_targets/ran_build/build
CONF_PATH=$HOME/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF
LD_LIB_PATH=$HOME/openairinterface5g/cmake_targets/ran_build/build
LOG_LEVEL="--log_config.global_log_level info"
SERVER_PORT="--rfsimulator.serverport 4048"
SERVER_PORT_SL="--rfsimulator.serverportsl 4148"
REMOTE_UE_HOST="nr_ue" # host name in the ~/.ssh/config
LOCAL_IP=$(ip route get 1.2.3.4 | awk '{print $7}')
REMOTE_USER=$(whoami) # Specify user name of the remote host
echo "Remote User = " $REMOTE_USER
echo "Local IP address = " $LOCAL_IP

# Read default values from config files (before any tests modify them)
DEFAULT_CSI_ACQ=$(grep "sl_CSI_Acquisition" $CONF_PATH/sl_sync_ref.conf | grep -oP '\d+' | head -1)
DEFAULT_PSFCH_PERIOD=$(grep "sl_PSFCH_Period" $CONF_PATH/sl_sync_ref.conf | grep -oP '\d+' | head -1)
echo "Default CSI Acquisition = " $DEFAULT_CSI_ACQ
echo "Default PSFCH Period = " $DEFAULT_PSFCH_PERIOD

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
        gnome-terminal -- bash -c "$m"
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
kill_all() {
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
block_comment() {
: <<'END_COMMENT'
END_COMMENT
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

    # Display statistics to stderr (so it shows but doesn't get captured in variable)
    echo "Ping Statistics:" >&2
    echo "  Transmitted: ${transmitted:-0}" >&2
    echo "  Received: ${received:-0}" >&2
    echo "  Packet Loss: ${loss:-100}%" >&2

    # Return both transmitted and received as "transmitted received" tuple to stdout
    echo "${transmitted:-0} ${received:-0}"
}
check_ping_result() {
    local transmitted=$1
    local received=$2
    local threshold_percent=${3:-60}  # Default 60% threshold

    if [ "$transmitted" -eq 0 ]; then
        echo "Result: Fail (no packets transmitted)"
        return 1
    fi

    # Calculate success percentage
    local success_percent=$((received * 100 / transmitted))

    if [ "$success_percent" -ge "$threshold_percent" ]; then
        echo "Result: Pass ($received/$transmitted = $success_percent% >= $threshold_percent%)"
        return 0
    else
        echo "Result: Fail ($received/$transmitted = $success_percent% < $threshold_percent%)"
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
#############################################################
### Test cases ###
#############################################################
usrp_pc5_rsrp_test_on_two_B210s() {
    # Argumemt: duration, mcs or empty
    # Attention: You need "TotalTx" log in result_syncref.log file
    # Ignore this test if not required.
    local start_time=$(date +%s)
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1 || duration=15
    [[ $# -ge 2 ]] && MCS=" --mcs $2" || MCS=""
    cmd1="cd ~/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=./build ./nr-uesoftmodem \
            -O ~/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf -E --sa --sl-mode 2 --sync-ref \
            --ue-txgain 10 --ue-rxgain 100 --thread-pool -1,-1 --device.name oai_usrpdevif $MCS"
    cmd2="LD_LIBRARY_PATH=/home/$REMOTE_USER/openairinterface5g/cmake_targets/ran_build/:$LD_LIBRARY_PATH sudo -E  /home/$REMOTE_USER/openairinterface5g/cmake_targets/ran_build/build/nr-uesoftmodem \
            -O /home/$REMOTE_USER/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf -E --sa --sl-mode 2 $MCS \
            --ue-txgain 10 --ue-rxgain 100 --thread-pool -1,-1 --device.name oai_usrpdevif $MCS"
    run_cmd "$cmd1";
    gnome-terminal -- bash -c "ssh $REMOTE_UE_HOST $cmd2"; sleep 3;
    sleep $duration;
    TARGET=nr-uesoftmodem
    kill_all $TARGET
    ssh $REMOTE_UE_HOST "$(declare -f kill_all); kill_all $TARGET"
    sleep 3
    src="~/result_syncref.log";str='TotalTx 30';dst="~/result_summary.txt"
    test_result=$(tail -n 100 $src_file | grep -m 1 $str_find  >> $dst_file)
    local end_time=$(date +%s)
    print_runtime $start_time $end_time
}
#############################################################
usrp_pc5_test_on_two_B210s() {
#############################################################
    # Argumemt: duration, mcs or empty
    local start_time=$(date +%s)
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1 || duration=15
    [[ $# -ge 2 ]] && MCS=" --mcs $2" || MCS=""
    cmd1="cd ~/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=./build ./nr-uesoftmodem \
            -O ~/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf -E --sa --sl-mode 2 --sync-ref \
            --ue-txgain 10 --ue-rxgain 100 --thread-pool -1,-1 --device.name oai_usrpdevif $MCS"
    cmd2="LD_LIBRARY_PATH=/home/$REMOTE_USER/openairinterface5g/cmake_targets/ran_build/:$LD_LIBRARY_PATH sudo -E  /home/$REMOTE_USER/openairinterface5g/cmake_targets/ran_build/build/nr-uesoftmodem \
            -O /home/$REMOTE_USER/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf -E --sa --sl-mode 2 $MCS \
            --ue-txgain 10 --ue-rxgain 100 --thread-pool -1,-1 --device.name oai_usrpdevif $MCS"
    cmd3="ping -c 5 -I oaitun_ue1 10.0.0.100"
    echo $cmd1; echo; echo $cmd2; echo; echo $cmd3; echo
    run_cmd "$cmd1";
    gnome-terminal -- bash -c "ssh $REMOTE_UE_HOST $cmd2"; sleep 3;
    run_cmd "$cmd3"; status=$?;
    sleep $duration;
    TARGET=nr-uesoftmodem
    kill_all $TARGET
    ssh $REMOTE_UE_HOST "$(declare -f kill_all); kill_all $TARGET"
    sleep 3
    check_same_str $status 0
    local end_time=$(date +%s)
    print_runtime $start_time $end_time
}
#############################################################
rfsim_pc5_ping_test_on_two_hosts() {
#############################################################
    # Argumemt: duration, mcs or empty
    local start_time=$(date +%s)
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1 || duration=15
    [[ $# -ge 2 ]] && MCS=" --mcs $2" || MCS=""
    cmd1="cd ~/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH ./nr-uesoftmodem \
            -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf --sync-ref --sl-mode 2 --rfsim --sa $MCS \
            --rfsimulator.serveraddrsl server --rfsimulator.serverportsl 4148 --log_config.global_log_level info 2>&1 | tee ~/result_syncref.log"
    cmd2="LD_LIBRARY_PATH=/home/$REMOTE_USER/openairinterface5g/cmake_targets/ran_build/:$LD_LIBRARY_PATH sudo -E  /home/$REMOTE_USER/openairinterface5g/cmake_targets/ran_build/build/nr-uesoftmodem \
            -O /home/$REMOTE_USER/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf --rfsim --sa --sl-mode 2 $MCS \
            --rfsimulator.serveraddrsl $LOCAL_IP --rfsimulator.serverportsl 4148 --log_config.global_log_level info 2>&1 | tee ~/result_nearby.log"
    cmd3="ping -c 5 -I oaitun_ue1 10.0.0.100"
    run_cmd "$cmd1";
    gnome-terminal -- bash -c "ssh $REMOTE_UE_HOST $cmd2"; sleep 3;
    run_cmd "$cmd3"; status=$?;
    sleep $duration;
    TARGET=nr-uesoftmodem
    kill_all $TARGET
    ssh $REMOTE_UE_HOST "$(declare -f kill_all); kill_all $TARGET"
    sleep 3
    check_same_str $status 0
    local end_time=$(date +%s)
    print_runtime $start_time $end_time
}
#############################################################
rfsim_pc5_ping_test_on_local_host() {
#############################################################
    # Argumemt: duration, mcs or empty
    local start_time=$(date +%s)
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1 || duration=5
    [[ $# -ge 2 ]] && MCS=" --mcs $2" || MCS=""
    cmd1="cd ~/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH ./nr-uesoftmodem \
            -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf --sync-ref --sl-mode 2 --rfsim --sa $MCS \
            --rfsimulator.serveraddrsl 127.0.0.1 --rfsimulator.serverportsl 4148 --log_config.global_log_level info  2>&1 | tee ~/result_syncref.log"
    cmd2="cd ~/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH ./nr-uesoftmodem \
            -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf --rfsim --sa --sl-mode 2 $MCS \
            --rfsimulator.serveraddrsl server --rfsimulator.serverportsl 4148 --log_config.global_log_level info  2>&1 | tee ~/result_nearby.log"
    ping_output="$HOME/$dir_name/ping_result_${FUNCNAME[0]}.txt"
    cmd3="ping -c 5 -I oaitun_ue1 10.0.0.100 2>&1 | tee $ping_output"

    echo $cmd1; echo; echo $cmd2; echo; echo $cmd3; echo
    run_cmd "$cmd1" "$cmd2"
    sleep 3  # Wait for UEs to be ready and tunnel to be created
    run_cmd "$cmd3"

    sleep $duration;
    kill_all nr-uesoftmodem; sleep 3

    # Extract and display ping statistics (file should exist now)
    if [ -f "$ping_output" ]; then
        ping_stats=$(get_ping_stats_tuple "$ping_output")
    else
        echo "Warning: Ping output file not found at $ping_output"
        ping_stats="0 0"
    fi
    check_ping_result $ping_stats 60  # Expect at least 60% success rate
    local end_time=$(date +%s)
    print_runtime $start_time $end_time
}
#############################################################
rfsim_pc5_csi_acquisition_enable_test() {
#############################################################
    # Argumemt: duration, mcs or empty
    local start_time=$(date +%s)
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1 || duration=5
    [[ $# -ge 2 ]] && MCS=" --mcs $2" || MCS=""

    # Check and restore default values if needed
    echo "Checking config file default values..."
    check_and_restore_default "$CONF_PATH/sl_sync_ref.conf" "sl_CSI_Acquisition" "$DEFAULT_CSI_ACQ"
    check_and_restore_default "$CONF_PATH/sl_ue1.conf" "sl_CSI_Acquisition" "$DEFAULT_CSI_ACQ"

    # Toggle CSI: if default is 0, set to 1; if default is 1, set to 0
    local test_csi_value=$((1 - DEFAULT_CSI_ACQ))
    pre1="sed -i 's/sl_CSI_Acquisition         = $DEFAULT_CSI_ACQ/sl_CSI_Acquisition         = $test_csi_value/g' $CONF_PATH/sl_sync_ref.conf"
    pre2="sed -i 's/sl_CSI_Acquisition         = $DEFAULT_CSI_ACQ/sl_CSI_Acquisition         = $test_csi_value/g' $CONF_PATH/sl_ue1.conf"
    pst1="sed -i 's/sl_CSI_Acquisition         = $test_csi_value/sl_CSI_Acquisition         = $DEFAULT_CSI_ACQ/g' $CONF_PATH/sl_sync_ref.conf"
    pst2="sed -i 's/sl_CSI_Acquisition         = $test_csi_value/sl_CSI_Acquisition         = $DEFAULT_CSI_ACQ/g' $CONF_PATH/sl_ue1.conf"
    eval "$pre1"; eval "$pre2";
    cmd1="cd ~/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH ./nr-uesoftmodem \
            -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf --sync-ref --sl-mode 2 --rfsim --sa $MCS \
            --rfsimulator.serveraddrsl 127.0.0.1 --rfsimulator.serverportsl 4148 --log_config.global_log_level info  2>&1 | tee ~/result_syncref.log"
    cmd2="cd ~/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH ./nr-uesoftmodem \
            -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf --rfsim --sa --sl-mode 2 $MCS \
            --rfsimulator.serveraddrsl server --rfsimulator.serverportsl 4148 --log_config.global_log_level info  2>&1 | tee ~/result_nearby.log"
    ping_output="$HOME/$dir_name/ping_result_${FUNCNAME[0]}.txt"
    cmd3="ping -c 5 -I oaitun_ue1 10.0.0.100 2>&1 | tee $ping_output"

    echo $cmd1; echo; echo $cmd2; echo; echo $cmd3; echo
    run_cmd "$cmd1" "$cmd2"
    sleep 3  # Wait for UEs to be ready and tunnel to be created
    run_cmd "$cmd3"

    sleep $duration;
    kill_all nr-uesoftmodem; sleep 3

    # Extract and display ping statistics (file should exist now)
    if [ -f "$ping_output" ]; then
        ping_stats=$(get_ping_stats_tuple "$ping_output")
    else
        echo "Warning: Ping output file not found at $ping_output"
        ping_stats="0 0"
    fi
    check_ping_result $ping_stats 60  # Expect at least 60% success rate

    eval "$pst1"; eval "$pst2";
    local end_time=$(date +%s)
    print_runtime $start_time $end_time
}
#############################################################
rfsim_pc5_csi_acquisition_psfch_period_test() {
#############################################################
    # Argumemt: csi_acq, psfch_period, duration, mcs (or empty)
    local start_time=$(date +%s)
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    csi_acq=$1; echo "csi_acq = " $1
    period=$2;  echo "psfch_period  = " $2
    [[ $# -ge 3 ]] && duration=$3 || duration=15
    [[ $# -ge 4 ]] && MCS=" --mcs $4" || MCS=""

    # Check and restore default values if needed
    echo "Checking config file default values..."
    check_and_restore_default "$CONF_PATH/sl_sync_ref.conf" "sl_CSI_Acquisition" "$DEFAULT_CSI_ACQ"
    check_and_restore_default "$CONF_PATH/sl_ue1.conf" "sl_CSI_Acquisition" "$DEFAULT_CSI_ACQ"
    check_and_restore_default "$CONF_PATH/sl_sync_ref.conf" "sl_PSFCH_Period" "$DEFAULT_PSFCH_PERIOD"
    check_and_restore_default "$CONF_PATH/sl_ue1.conf" "sl_PSFCH_Period" "$DEFAULT_PSFCH_PERIOD"

    pre1="sed -i 's/sl_CSI_Acquisition         = $DEFAULT_CSI_ACQ/sl_CSI_Acquisition         = $csi_acq/g' $CONF_PATH/sl_sync_ref.conf"
    pre2="sed -i 's/sl_CSI_Acquisition         = $DEFAULT_CSI_ACQ/sl_CSI_Acquisition         = $csi_acq/g' $CONF_PATH/sl_ue1.conf"
    pre3="sed -i 's/sl_PSFCH_Period                 = $DEFAULT_PSFCH_PERIOD/sl_PSFCH_Period                 = $period/g' $CONF_PATH/sl_sync_ref.conf"
    pre4="sed -i 's/sl_PSFCH_Period                 = $DEFAULT_PSFCH_PERIOD/sl_PSFCH_Period                 = $period/g' $CONF_PATH/sl_ue1.conf"
    pst1="sed -i 's/sl_CSI_Acquisition         = $csi_acq/sl_CSI_Acquisition         = $DEFAULT_CSI_ACQ/g' $CONF_PATH/sl_sync_ref.conf"
    pst2="sed -i 's/sl_CSI_Acquisition         = $csi_acq/sl_CSI_Acquisition         = $DEFAULT_CSI_ACQ/g' $CONF_PATH/sl_ue1.conf"
    pst3="sed -i 's/sl_PSFCH_Period                 = $period/sl_PSFCH_Period                 = $DEFAULT_PSFCH_PERIOD/g' $CONF_PATH/sl_sync_ref.conf"
    pst4="sed -i 's/sl_PSFCH_Period                 = $period/sl_PSFCH_Period                 = $DEFAULT_PSFCH_PERIOD/g' $CONF_PATH/sl_ue1.conf"
    eval "$pre1"; eval "$pre2"; eval "$pre3"; eval "$pre4";
    cmd1="cd ~/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH ./nr-uesoftmodem \
            -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf --sync-ref --sl-mode 2 --rfsim --sa $MCS \
            --rfsimulator.serveraddr 127.0.0.1 --rfsimulator.serverportsl 4148 --log_config.global_log_level info | tee ~/result_syncref.log 2>&1"
    cmd2="cd ~/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH ./nr-uesoftmodem \
            -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf --rfsim --sa --sl-mode 2 $MCS \
            --rfsimulator.serveraddr server --rfsimulator.serverportsl 4148 --log_config.global_log_level info | tee ~/result_nearby.log 2>&1"
    ping_output="$HOME/$dir_name/ping_result_${FUNCNAME[0]}_csi${csi_acq}_psfch${period}.txt"
    cmd3="ping -c 15 -I oaitun_ue1 10.0.0.100 2>&1 | tee $ping_output"

    echo $cmd1; echo; echo $cmd2; echo; echo $cmd3; echo
    run_cmd "$cmd1" "$cmd2"
    sleep 3  # Wait for UEs to be ready and tunnel to be created
    run_cmd "$cmd3"

    sleep $duration;
    kill_all nr-uesoftmodem; sleep 3

    # Extract and display ping statistics (file should exist now)
    if [ -f "$ping_output" ]; then
        ping_stats=$(get_ping_stats_tuple "$ping_output")
    else
        echo "Warning: Ping output file not found at $ping_output"
        ping_stats="0 0"
    fi
    check_ping_result $ping_stats 60  # Expect at least 60% success rate
    eval "$pst1"; eval "$pst2"; eval "$pst3"; eval "$pst4"
    local end_time=$(date +%s)
    print_runtime $start_time $end_time
}
#############################################################
rfsim_srap_ping_test() {
#############################################################
    # Argumemt: duration, mcs or empty
    local start_time=$(date +%s)
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1 || duration=15
    [[ $# -ge 2 ]] && MCS=" --mcs $2" || MCS=""
    cmd1="cd ~/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH ./nr-softmodem \
        -O ../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band78.fr1.106PRB.usrpb210.conf --gNBs.[0].min_rxtxtime 6 \
        --rfsimulator.serveraddr server --rfsimulator.serverport 4048 --rfsim --sa --log_config.global_log_level info \
        --relay-type 1 --remote-ue-id 1 --noS1 | tee ~/result_gNB.log 2>&1"
    cmd2="cd ~/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH ./nr-uesoftmodem \
        -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf -r 106 --numerology 1 --band 78 -C 3619200000 --sync-ref --sl-mode 1 $MCS \
        --uicc0.imsi 001010000000001 --rfsim --sa --rfsimulator.serveraddr 127.0.0.1 --rfsimulator.serverport 4048 --rfsimulator.serverportsl 4148 --log_config.global_log_level info --relay-type 1 --is-relay-ue 1 | tee ~/result_nrUE_syncref.log 2>&1"
    cmd3="cd ~/openairinterface5g/cmake_targets/ran_build/build; sudo -E LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH ./nr-uesoftmodem \
        -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf --rfsim --sa --sl-mode 2 $MCS \
        --rfsimulator.serveraddr server --rfsimulator.serverportsl 4148 --log_config.global_log_level info --relay-type 1 | tee ~/result_nearby.log 2>&1"
    cmd4='ping -c 5 -I oaitun_ue2 8.8.8.8'
    pre1='docker ps | grep oai-amf | wc -l' # expecting: 1
    act1='echo "core network is required !!!"; cd ~/oai-cn5g; systemctl start docker.service; docker compose up -d; sleep 2'
    [[ $(eval "$pre1") -eq 1 ]] && echo "Requirements are satisfied !!!" || eval "$act1"
    run_cmd "$cmd1" "$cmd2""$cmd3"; sleep 5
    run_cmd "$cmd4"; status=$?;
    sleep $duration;
    kill_all nr-uesoftmodem nr-softmodem; sleep 3
    check_same_str $status 0
    local end_time=$(date +%s)
    print_runtime $start_time $end_time
}
#############################################################
### Driver function
#############################################################
main() {
    #########################################################
    ### Selecte test profile ###
    #########################################################
    test_profile='pilot' # regress stress
    if [[ $test_profile == "pilot" ]]; then
        num_repeat=1
        mcs_array=(10)
        duration=30
        test_type='usrp'
        #test_type='rfsim'
        [[ $test_type == "rfsim" ]] && parm_array=$(seq 0 1 0) || parm_array=(20) # [start, step, end]
    elif [[ $test_profile == "regress" ]]; then
        num_repeat=1
        mcs_array=(1 9)
        duration=30
        # test_type='usrp'
        test_type='rfsim'
        [[ $test_type == "rfsim" ]] && parm_array=$(seq 0 1 0) || parm_array=(20) # [start, step, end]
    elif [[ $test_profile == "stress" ]]; then
        num_repeat=3
        mcs_array=(9 16 28)
        duration=300
        # test_type='usrp'
        test_type='rfsim'
        [[ $test_type == "rfsim" ]] && parm_array=$(seq 0 1 0) || parm_array=(20 30 40 50 55 60) # [start, step, end]
    fi
    [ $test_type = 'rfsim' ] && parm_name='snr' || parm_name='atten'
    #########################################################
    ### Selecte test cases ###
    #########################################################
    for val in $parm_array; do # [start, step, end] snr or atten
        # echo "Ignored this param loop. Update if you want to test various snrs or atten values: " $parm_name ":===> " $val
        # [ $test_type = 'usrp' ] && set_atten $val
        for k in $(seq 1 1 $num_repeat); do
            for mcs in $mcs_array; do
                if [[ $test_type == "rfsim" ]]; then
                    rfsim_pc5_ping_test_on_local_host $duration $mcs
                    #rfsim_pc5_ping_test_on_two_hosts $duration $mcs
                    #rfsim_pc5_csi_acquisition_enable_test $duration $mcs
                    #rfsim_pc5_csi_acquisition_psfch_period_test 0 1 $duration $mcs # (csi_acq  psfch_period_index) pair with (1 2) default value for given mcs value
: <<'END_COMMENT'
                    rfsim_pc5_csi_acquisition_psfch_period_test 0 0 $duration $mcs # (csi_acq  psfch_period_index) pair with (1 2) default value for given mcs value
                    rfsim_pc5_csi_acquisition_psfch_period_test 0 1 $duration $mcs # (csi_acq  psfch_period_index) pair with (1 2) default value for given mcs value
                    rfsim_pc5_csi_acquisition_psfch_period_test 0 2 $duration $mcs # (csi_acq  psfch_period_index) pair with (1 2) default value for given mcs value
                    rfsim_pc5_csi_acquisition_psfch_period_test 0 3 $duration $mcs # (csi_acq  psfch_period_index) pair with (1 2) default value for given mcs value
                    rfsim_pc5_csi_acquisition_psfch_period_test 1 0 $duration $mcs # (csi_acq  psfch_period_index) pair with (1 2) default value for given mcs value
                    rfsim_pc5_csi_acquisition_psfch_period_test 1 1 $duration $mcs # (csi_acq  psfch_period_index) pair with (1 2) default value for given mcs value
                    rfsim_pc5_csi_acquisition_psfch_period_test 1 2 $duration $mcs # (csi_acq  psfch_period_index) pair with (1 2) default value for given mcs value
                    rfsim_pc5_csi_acquisition_psfch_period_test 1 3 $duration $mcs # (csi_acq  psfch_period_index) pair with (1 2) default value for given mcs value
END_COMMENT
                    # rfsim_srap_ping_test
                fi
                if [[ $test_type == "usrp" ]]; then
                    usrp_pc5_test_on_two_B210s $duration $mcs
                fi
            done
        done
    done
}
main