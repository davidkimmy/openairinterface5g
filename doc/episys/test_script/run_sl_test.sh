#!/bin/bash
#############################################################
# Standalone shell script
# Usage:
#   Shell> ./run_sl_test.sh [-d <base_dir>] [-g <0|1>]
#############################################################

timestamp=$(date +"%Y%m%d_%H%M%S")
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
# Path of this script relative to the user's home. Used for remote (ssh) hosts, which may
# run under a DIFFERENT user id but keep the same layout under their home directory.
# If the script is not under $HOME the strip is a no-op and this keeps the absolute path.
SCRIPT_DIR_REL="${SCRIPT_DIR#$HOME/}"

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
        # Execute remotely. BatchMode + ConnectTimeout so an unreachable or
        # auth-prompting host fails fast (3s) instead of blocking the script.
        ssh -o BatchMode=yes -o ConnectTimeout=3 "$host" "$cmd"
    fi
}

#############################################################
# SSH Host Configuration
#############################################################
# Host names must match entries in ~/.ssh/config
# For BLER tests (local execution), these are overridden to "local"
REMOTE_UE_HOST="remote_ue"
LOCAL_HOST="local"
RELAY_UE_HOST="relay_ue"
NR_UE_HOST="nr_ue"
GNB_HOST="gNB"

#############################################################
# Host Initialization Function
#############################################################
# Usage: init_host_variables <mode>
#   resolve_ssh - use remote_ue, relay_ue, etc. as defaults
#   skip_ssh    - use localhost as default
init_host_variables() {
    local mode=${1:-resolve_ssh}

    if [[ "$mode" == "resolve_ssh" ]]; then
        REMOTE_UE_HOST="${REMOTE_UE_HOST:-remote_ue}"
        REMOTE_HOST_IP=$(ssh -G $REMOTE_UE_HOST 2>/dev/null | awk '/^hostname / {print $2}')
        echo "Remote UE Host IP address = $REMOTE_HOST_IP"

        LOCAL_HOST="${LOCAL_HOST:-local}"
        LOCAL_HOST_IP=$(ssh -G $LOCAL_HOST 2>/dev/null | awk '/^hostname / {print $2}')
        echo "Local Host IP address = $LOCAL_HOST_IP"

        RELAY_UE_HOST="${RELAY_UE_HOST:-relay_ue}"
        RELAY_UE_HOST_IP=$(ssh -G $RELAY_UE_HOST 2>/dev/null | awk '/^hostname / {print $2}')
        echo "Relay UE Host IP address = $RELAY_UE_HOST_IP"

        NR_UE_HOST="${NR_UE_HOST:-nr_ue}"
        NR_UE_HOST_IP=$(ssh -G $NR_UE_HOST 2>/dev/null | awk '/^hostname / {print $2}')
        echo "nrUE Host IP address = $NR_UE_HOST_IP"

        GNB_HOST="${GNB_HOST:-gNB}"
        GNB_HOST_IP=$(ssh -G $GNB_HOST 2>/dev/null | awk '/^hostname / {print $2}')
        echo "gNB Host IP address = $GNB_HOST_IP"
    else
        # Force localhost for local execution (override any previous values)
        REMOTE_UE_HOST="localhost"
        REMOTE_HOST_IP=$(ssh -G $REMOTE_UE_HOST 2>/dev/null | awk '/^hostname / {print $2}')
        echo "Remote UE Host IP address = $REMOTE_HOST_IP"

        LOCAL_HOST="localhost"
        LOCAL_HOST_IP=$(ssh -G $LOCAL_HOST 2>/dev/null | awk '/^hostname / {print $2}')
        echo "Local Host IP address = $LOCAL_HOST_IP"

        RELAY_UE_HOST="localhost"
        RELAY_UE_HOST_IP=$(ssh -G $RELAY_UE_HOST 2>/dev/null | awk '/^hostname / {print $2}')
        echo "Relay UE Host IP address = $RELAY_UE_HOST_IP"

        NR_UE_HOST="localhost"
        NR_UE_HOST_IP=$(ssh -G $NR_UE_HOST 2>/dev/null | awk '/^hostname / {print $2}')
        echo "nrUE Host IP address = $NR_UE_HOST_IP"

        GNB_HOST="localhost"
        GNB_HOST_IP=$(ssh -G $GNB_HOST 2>/dev/null | awk '/^hostname / {print $2}')
        echo "gNB Host IP address = $GNB_HOST_IP"
    fi

    # Export variables so they're available to the main script
    export REMOTE_UE_HOST REMOTE_HOST_IP LOCAL_HOST LOCAL_HOST_IP
    export RELAY_UE_HOST RELAY_UE_HOST_IP NR_UE_HOST NR_UE_HOST_IP
    export GNB_HOST GNB_HOST_IP
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

# Initialize default sleep timings and test_extra_duration (before sourcing config)
declare -A sleep_timing

# Apply extended delays for slower systems or specific environments
# Only called if use_extended_delays=1 in config file
apply_default_delays() {
    sleep_timing["tun_wait_1st"]=0
    sleep_timing["tun_wait_2nd"]=0
    sleep_timing["sync_stab_30s"]=0
    sleep_timing["sync_stab_45s_v1"]=0
    sleep_timing["sync_stab_45s_v2"]=0
    sleep_timing["cn_shutdown"]=3
    sleep_timing["cn_init"]=3
}
apply_extended_delays() {
    sleep_timing["tun_wait_1st"]=3
    sleep_timing["tun_wait_2nd"]=3
    sleep_timing["sync_stab_30s"]=30
    sleep_timing["sync_stab_45s_v1"]=45
    sleep_timing["sync_stab_45s_v2"]=45
    sleep_timing["cn_shutdown"]=5
    sleep_timing["cn_init"]=5
}

echo "DEBUG: Sourcing config from: $SL_TEST_CONFIG_FILE"
if [[ ! -f "$SL_TEST_CONFIG_FILE" ]]; then
    echo "ERROR: Config file not found: $SL_TEST_CONFIG_FILE"
    exit 1
fi
source "$SL_TEST_CONFIG_FILE"

# Source utility functions
SL_TEST_UTILS_FILE="$SCRIPT_DIR/run_sl_test_utils.sh"
if [[ -f "$SL_TEST_UTILS_FILE" ]]; then
    source "$SL_TEST_UTILS_FILE"
else
    echo "WARNING: Utilities file not found: $SL_TEST_UTILS_FILE"
fi

echo "Tests: ${enabled_tests[@]}"
echo "Test_profile: $test_profile"
[[ -n "$base_log_dir" ]] && base_dir="${base_log_dir/#\~/$HOME}"
[[ "$use_external_clock" == "1" ]] && ext_clock_flag=" --clock-source 1 --time-source 1"
[[ -n "$use_gnome" ]] && USE_GNOME="$use_gnome"
[[ "$use_sa" == "1" ]] && sa_flag="--sa"

# Apply extended delays if enabled in config
if [[ "$use_extended_delays" == "1" ]]; then
    apply_extended_delays
else
    apply_default_delays
fi

# Parallel mode: auto-generate host-specific configs and launch
if [[ "$test_profile" == "bler" && "$parallel_mode" == "true" ]]; then
    echo "=========================================="
    echo "Parallel Mode Detected"
    echo "=========================================="

    # Check if bler_hosts array is defined in config
    if [[ ${#bler_hosts[@]} -eq 0 ]]; then
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

    # Setup parallel hosts by updating configs (function defined in run_sl_test_utils.sh)
    setup_parallel_bler_hosts

    echo ""
    echo "=========================================="
    echo "Launching Tests on All Machines"
    echo "=========================================="

    # Launch on each host (using worker-specific config files)
    host_idx=0
    for hostname in "${bler_hosts[@]}"; do
        host_idx=$((host_idx + 1))
        host_id="host${host_idx}"
        log_file="~/openairinterface5g/bler_${host_id}.log"
        pid_file="~/openairinterface5g/bler_${host_id}.pid"

        echo ""
        echo "→ ${host_id} (${hostname})"

        if [[ "$hostname" == "localhost" || "$hostname" == "local" ]]; then
            # Launch locally - uses worker-specific config
            cd "$SCRIPT_DIR"
            # Override config file via environment variable
            BLER_CONFIG_FILE="run_sl_test_config_worker_local.sh" nohup bash run_sl_test.sh > "${log_file/#\~/$HOME}" 2>&1 &
            echo $! > "${pid_file/#\~/$HOME}"
            echo "  ✓ Started locally (PID: $!)"
        else
            # Launch remotely - uses worker-specific config
            # Use a home-relative path with an escaped \$HOME so it expands on the REMOTE
            # host: works even if the remote user id differs, as long as the script lives
            # at the same path under that user's home.
            ssh -n -f "$hostname" "cd \"\$HOME/$SCRIPT_DIR_REL\" && BLER_CONFIG_FILE=\"run_sl_test_config_worker_${hostname}.sh\" nohup bash run_sl_test.sh > $log_file 2>&1 & echo \$! > $pid_file" 2>/dev/null
            if [[ $? -eq 0 ]]; then
                echo "  ✓ Started on ${hostname}"
            else
                echo "  ✗ Failed to start on ${hostname}"
            fi
            sleep 2  # Wait for SSH remote process to start
        fi
    done

    echo ""
    echo "=========================================="
    echo "✓ All Tests Launched!"
    echo "=========================================="
    echo ""
    echo "Monitor progress:"
    echo "  bash $SCRIPT_DIR/check_test_status.sh"
    echo ""
    echo "Logs:"
    host_idx=0
    for hostname in "${bler_hosts[@]}"; do
        host_idx=$((host_idx + 1))
        host_id="host${host_idx}"
        echo "  ${host_id} (${hostname}): ~/openairinterface5g/bler_${host_id}.log"
    done
    echo ""

    # Calculate expected completion time
    total_tests=$((${#mcs_array[@]} * ${#noise_power_array[@]} * num_repeat))
    tests_per_host=$((total_tests / num_hosts))
    test_time=$((duration + 10))  # duration + 10s overhead (restart, cleanup)
    time_per_host_sec=$((tests_per_host * test_time))
    time_per_host_hours=$((time_per_host_sec / 3600))
    time_per_host_min=$(((time_per_host_sec % 3600) / 60))

    echo "Expected completion time (parallel execution on ${num_hosts} machines):"
    echo "  Total tests: ${total_tests} (${#mcs_array[@]} MCS × ${#noise_power_array[@]} SNR × ${num_repeat} iterations)"
    echo "  Per machine: ${tests_per_host} tests × ${test_time}s = ${time_per_host_hours}h ${time_per_host_min}m"
    echo ""
    echo "Note: Worker config files created (will persist for debugging):"
    echo "  Local: $SCRIPT_DIR/run_sl_test_config_worker_local.sh"
    for hostname in "${bler_hosts[@]}"; do
        if [[ "$hostname" != "localhost" && "$hostname" != "local" ]]; then
            echo "  ${hostname}: ~/$SCRIPT_DIR_REL/run_sl_test_config_worker_${hostname}.sh"
        fi
    done
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

# Initialize host variables based on test configuration
# Skip SSH resolution if tests are running on local host
if [[ "${enabled_tests[*]}" =~ "on_local_host" ]]; then
    init_host_variables "skip_ssh"
else
    init_host_variables "resolve_ssh"
fi

# Read default values from config files (before any tests modify them)
# Use config file values if provided, otherwise auto-detect
if [[ -z "$DEFAULT_CSI_ACQ" ]]; then
    DEFAULT_CSI_ACQ=$(grep "sl_CSI_Acquisition" $CONF_PATH/sl_sync_ref.conf | grep -oP '\d+' | head -1)
fi
if [[ -z "$DEFAULT_PSFCH_PERIOD" ]]; then
    DEFAULT_PSFCH_PERIOD=$(grep "sl_PSFCH_Period" $CONF_PATH/sl_sync_ref.conf | grep -oP '\d+' | head -1)
fi
echo "Default CSI Acquisition = " $DEFAULT_CSI_ACQ
echo "Default PSFCH Period = " $DEFAULT_PSFCH_PERIOD

# Use TX/RX gain from config file, or set defaults if not defined
TX_GAIN="${TX_GAIN:-0}"
RX_GAIN="${RX_GAIN:-110}"

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
    elif [[ $host_name == 'upf_docker' ]]; then
        docker exec oai-upf bash -c "pkill $target" 2>/dev/null
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
    sleep ${sleep_timing[cn_shutdown]}  # Configurable: wait for clean shutdown

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

    sleep ${sleep_timing[cn_init]}  # Configurable: wait for initialization

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

#############################################################
# Four-Tier Configuration Helper Functions
#############################################################

build_test_to_group_map() {
    # Build mapping from individual tests to their group names
    # This function is called after config is loaded to populate test_to_group_map
    #
    # Auto-discovery mode (default):
    #   - Finds all arrays ending with: _basic_tests, _iperf3_tests, _csi_psfch_tests
    #   - Excludes profile arrays: pilot_tests, regress_tests, stress_tests
    #
    # Manual mode (optional):
    #   - Uses test_group_names array from config if defined

    local group_names=()

    if [[ -n "${test_group_names[*]}" ]]; then
        # Manual mode: use explicitly defined test_group_names
        group_names=("${test_group_names[@]}")
    else
        # Auto-discovery mode: find test group arrays by naming convention
        # Pattern: *_basic_tests, *_iperf3_tests, *_csi_psfch_tests
        # Exclude: pilot_tests, regress_tests, stress_tests (profile arrays)

        local all_arrays=$(declare -p | grep -o 'declare -a [a-zA-Z0-9_]*' | awk '{print $3}')

        for array_name in $all_arrays; do
            # Check if it matches test group pattern
            if [[ "$array_name" =~ _(basic_tests|iperf3_tests|csi_psfch_tests)$ ]]; then
                # Exclude profile arrays
                if [[ "$array_name" != "pilot_tests" &&
                      "$array_name" != "regress_tests" &&
                      "$array_name" != "stress_tests" ]]; then
                    group_names+=("$array_name")
                fi
            fi
        done
    fi

    # Build the mapping
    for group_name in "${group_names[@]}"; do
        # Check if array exists
        if declare -p "$group_name" &>/dev/null 2>&1; then
            local -n group_array="$group_name"
            for test in "${group_array[@]}"; do
                test_to_group_map["$test"]="$group_name"
            done
        fi
    done
}

get_test_group() {
    # Get the group name for a given test
    # Args: test_name
    # Returns: group_name via stdout (empty if not in any group)

    local test_name=$1
    echo "${test_to_group_map[$test_name]}"
}

slice_matches_index() {
    # Check if an index matches a slice specification
    # Args: slice (e.g., "0:2" or "1,3,5"), index
    # Returns: 0 if matches, 1 if not

    local slice=$1
    local idx=$2

    if [[ "$slice" == *:* ]]; then
        # Range slice: "0:2" means indices 0,1,2
        local start=${slice%%:*}
        local end=${slice##*:}
        [[ $idx -ge $start && $idx -le $end ]] && return 0
    else
        # Comma-separated indices: "0,2,4"
        for i in ${slice//,/ }; do
            [[ $i -eq $idx ]] && return 0
        done
    fi

    return 1
}

get_test_slice_key() {
    # Check if test belongs to a slice-specific config
    # Args: test_name
    # Returns: slice_key if found (e.g., "slmode2_basic_tests[0:1]"), empty otherwise

    local test_name=$1
    local group_name=$(get_test_group "$test_name")

    if [[ -z "$group_name" ]]; then
        return
    fi

    # Get the index of this test within its group
    local -n group_array="$group_name"
    local test_idx=-1
    for i in "${!group_array[@]}"; do
        if [[ "${group_array[$i]}" == "$test_name" ]]; then
            test_idx=$i
            break
        fi
    done

    if [[ $test_idx -eq -1 ]]; then
        return
    fi

    # Check all slice-specific configs for this group (check both MCS and duration maps)
    local all_keys=()
    for key in "${!group_specific_duration[@]}"; do
        all_keys+=("$key")
    done
    for key in "${!group_specific_mcs[@]}"; do
        # Only add if not already in list
        local found=0
        for existing in "${all_keys[@]}"; do
            [[ "$existing" == "$key" ]] && found=1 && break
        done
        [[ $found -eq 0 ]] && all_keys+=("$key")
    done

    # Check if any key matches this test's group and index
    for key in "${all_keys[@]}"; do
        # Pattern: group_name[slice]
        if [[ "$key" =~ ^${group_name}\[(.+)\]$ ]]; then
            local slice="${BASH_REMATCH[1]}"

            # Check if test_idx matches this slice
            if slice_matches_index "$slice" "$test_idx"; then
                echo "$key"
                return
            fi
        fi
    done
}

get_test_mcs_array() {
    # Get MCS array for specific test (four-tier resolution)
    # Priority: Test-specific > Slice-specific > Group-specific > Profile-default
    # Args: test_name
    # Returns: space-separated MCS values via stdout
    #
    # Supports two input formats:
    #   - Comma-separated: "16,20,28"
    #   - Space-separated: "16 20 28" or "$(seq 0 1 10)"

    local test_name=$1
    local group_name=$(get_test_group "$test_name")
    local slice_key=$(get_test_slice_key "$test_name")
    local mcs_string=""

    # Priority 1: Test-specific override
    if [[ -n "${test_specific_mcs[$test_name]}" ]]; then
        mcs_string="${test_specific_mcs[$test_name]}"
    # Priority 2: Slice-specific override
    elif [[ -n "$slice_key" && -n "${group_specific_mcs[$slice_key]}" ]]; then
        mcs_string="${group_specific_mcs[$slice_key]}"
    # Priority 3: Group-specific override
    elif [[ -n "$group_name" && -n "${group_specific_mcs[$group_name]}" ]]; then
        mcs_string="${group_specific_mcs[$group_name]}"
    else
        # Priority 4: Profile default
        echo "${mcs_array[@]}"
        return
    fi

    # Convert to space-separated format (handle both comma and space-separated input)
    if [[ "$mcs_string" == *","* ]]; then
        # Comma-separated: convert commas to spaces
        echo "${mcs_string//,/ }"
    else
        # Already space-separated: use as-is
        echo "$mcs_string"
    fi
}

get_test_duration() {
    # Get duration for specific test (four-tier resolution)
    # Priority: Test-specific > Slice-specific > Group-specific > Profile-default
    # Args: test_name
    # Returns: duration value via stdout

    local test_name=$1
    local group_name=$(get_test_group "$test_name")
    local slice_key=$(get_test_slice_key "$test_name")

    # Priority 1: Test-specific override
    if [[ -n "${test_specific_duration[$test_name]}" ]]; then
        echo "${test_specific_duration[$test_name]}"
        return
    fi

    # Priority 2: Slice-specific override
    if [[ -n "$slice_key" && -n "${group_specific_duration[$slice_key]}" ]]; then
        echo "${group_specific_duration[$slice_key]}"
        return
    fi

    # Priority 3: Group-specific override
    if [[ -n "$group_name" && -n "${group_specific_duration[$group_name]}" ]]; then
        echo "${group_specific_duration[$group_name]}"
        return
    fi

    # Priority 4: Profile default
    echo "$duration"
}

print_test_config_info() {
    # Debug function to show resolved configuration
    # Args: test_name

    local test_name=$1
    local group_name=$(get_test_group "$test_name")
    local slice_key=$(get_test_slice_key "$test_name")
    local test_mcs=($(get_test_mcs_array "$test_name"))
    local test_duration=$(get_test_duration "$test_name")

    echo "=========================================="
    echo "Test Configuration: $test_name"
    echo "=========================================="
    echo "  Group: ${group_name:-<none>}"
    echo "  Slice: ${slice_key:-<none>}"
    echo "  MCS: [${test_mcs[@]}]"
    echo "  Duration: ${test_duration}s"

    # Show where MCS values came from
    if [[ -n "${test_specific_mcs[$test_name]}" ]]; then
        echo "  MCS source: Test-specific"
    elif [[ -n "$slice_key" && -n "${group_specific_mcs[$slice_key]}" ]]; then
        echo "  MCS source: Slice-specific ($slice_key)"
    elif [[ -n "$group_name" && -n "${group_specific_mcs[$group_name]}" ]]; then
        echo "  MCS source: Group-specific ($group_name)"
    else
        echo "  MCS source: Profile-default"
    fi

    # Show where duration value came from
    if [[ -n "${test_specific_duration[$test_name]}" ]]; then
        echo "  Duration source: Test-specific"
    elif [[ -n "$slice_key" && -n "${group_specific_duration[$slice_key]}" ]]; then
        echo "  Duration source: Slice-specific ($slice_key)"
    elif [[ -n "$group_name" && -n "${group_specific_duration[$group_name]}" ]]; then
        echo "  Duration source: Group-specific ($group_name)"
    else
        echo "  Duration source: Profile-default"
    fi
    echo "=========================================="
}

build_test_to_group_map

#############################################################
# Test Library Functions
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
    local log_file="/tmp/result_nearby.log"
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

wait_for_remote_ue_core_ip() {
    # The nr-uesoftmodem logs the assigned address only after
    # nas_config() has already reconfigured oaitun_ue2
    # (PduSessionEstablishmentAccept.c).
    local log_file="/tmp/result_nearby.log"
    local timeout=${1:-40}
    # Marker must match what the Remote UE actually logs when its Core-assigned IP lands on the PC5 SL TUN.
    # (The develop-based build prints "[SDAP] [Remote UE] applying core IP <ip> to PC5 SL TUN ...".)
    local marker="applying core IP"

    REMOTE_UE_CORE_IP=""
    echo "Waiting for remote UE Core IP (timeout: ${timeout}s)..."
    local elapsed=0
    while [ $elapsed -lt $timeout ]; do
        if [ -f "$log_file" ] && grep -q "$marker" "$log_file"; then
            REMOTE_UE_CORE_IP=$(grep "$marker" "$log_file" | tail -1 | grep -oE '([0-9]{1,3}\.){3}[0-9]{1,3}' | tail -1)
            echo "Remote UE obtained Core IP: ${REMOTE_UE_CORE_IP} (after ${elapsed}s) - starting ping"
            return 0
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    echo "ERROR: remote UE never obtained a Core IP after ${timeout}s - registration failed (e.g. max RETX on SL-SRB0)"
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

    # Calculate PSSCH pass rates (RX/TX format for success rate).
    # RX and TX counts come from each node's last "PSSCH Stats" log line, which
    # are sampled at slightly different times across nodes. This can make a
    # receiver's count exceed the paired transmitter's (RX > TX), producing
    # impossible >100% rates. Clamp each RX to its paired TX to avoid that.
    local rx_nearby_c=${LAST_PSSCH_RX_NEARBY:-0}
    [ "$rx_nearby_c" -gt "${LAST_PSSCH_TX_SYNCREF:-0}" ] && rx_nearby_c=${LAST_PSSCH_TX_SYNCREF:-0}
    local rx_syncref_c=${LAST_PSSCH_RX_SYNCREF:-0}
    [ "$rx_syncref_c" -gt "${LAST_PSSCH_TX_NEARBY:-0}" ] && rx_syncref_c=${LAST_PSSCH_TX_NEARBY:-0}

    # Rate1: syncref TX -> nearby RX
    if [ -n "$LAST_PSSCH_TX_SYNCREF" ] && [ "$LAST_PSSCH_TX_SYNCREF" -gt 0 ]; then
        local pssch_rate1=$((rx_nearby_c * 100 / LAST_PSSCH_TX_SYNCREF))
        local pssch_rate1_str="${rx_nearby_c}/${LAST_PSSCH_TX_SYNCREF} (${pssch_rate1}%)"
    else
        local pssch_rate1_str="N/A"
    fi

    # Rate2: nearby TX -> syncref RX
    if [ -n "$LAST_PSSCH_TX_NEARBY" ] && [ "$LAST_PSSCH_TX_NEARBY" -gt 0 ]; then
        local pssch_rate2=$((rx_syncref_c * 100 / LAST_PSSCH_TX_NEARBY))
        local pssch_rate2_str="${rx_syncref_c}/${LAST_PSSCH_TX_NEARBY} (${pssch_rate2}%)"
    else
        local pssch_rate2_str="N/A"
    fi

    # Calculate total PSSCH rate (aggregated both directions).
    # Both syncref->nearby and nearby->syncref links exist on single- and
    # multi-host tests, so aggregate RX/TX over both directions in all cases.
    # Use the clamped RX values so the total also stays <= 100%.
    local total_tx=$((${LAST_PSSCH_TX_SYNCREF:-0} + ${LAST_PSSCH_TX_NEARBY:-0}))
    local total_rx=$((rx_syncref_c + rx_nearby_c))
    if [ "$total_tx" -gt 0 ]; then
        local pssch_total=$((total_rx * 100 / total_tx))
        local pssch_total_str="${pssch_total}%"
    else
        local pssch_total_str="N/A"
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

get_gnb_config_path() {
    # Get the appropriate gNB config file path
    # Args: sl_mode, host_name, [bler_mode]
    # Returns: full path to config file
    local sl_mode=$1
    local host_name=$2
    local bler_mode=${3:-0}  # 0=normal, 1=bler

    # Determine base config file
    if [[ $sl_mode -eq 1 ]]; then
        local base_config="$GNB_CONF_RELAY"
    else
        local base_config="$GNB_CONF_USRP"
    fi

    # Add _bler suffix if needed
    if [[ $bler_mode -eq 1 ]]; then
        base_config="${base_config%.conf}_bler.conf"
    fi

    # Adjust path for remote host
    if [[ $host_name != "local" ]]; then
        local user_name=$(find_user_name "$host_name")
        base_config=$(echo "$base_config" | sed "s|\$HOME|/home/$user_name|g")
    fi

    echo "$base_config"
}

sync_default_config_params() {
    # Sync sl_PSFCH_Period and sl_CSI_Acquisition across all config files
    # Args: csi_acq psfch_period [sl_mode]
    local csi_acq=${1:-0}
    local psfch_period=${2:-2}
    local sl_mode=${3:-}   # optional; relay sync only applies to SL mode 1

    echo "Syncing default config params: CSI=$csi_acq, PSFCH=$psfch_period"

    # Local sidelink configs
    sed -i "s/\(sl_CSI_Acquisition[[:space:]]*=[[:space:]]*\)[0-9]\+/\1${csi_acq}/g" "$CONF_PATH/sl_sync_ref.conf"
    sed -i "s/\(sl_PSFCH_Period[[:space:]]*=[[:space:]]*\)[0-9]\+/\1${psfch_period}/g" "$CONF_PATH/sl_sync_ref.conf"
    sed -i "s/\(sl_CSI_Acquisition[[:space:]]*=[[:space:]]*\)[0-9]\+/\1${csi_acq}/g" "$CONF_PATH/sl_ue1.conf"
    sed -i "s/\(sl_PSFCH_Period[[:space:]]*=[[:space:]]*\)[0-9]\+/\1${psfch_period}/g" "$CONF_PATH/sl_ue1.conf"

    # Local gNB relay config — only relevant for SL mode 1 (relay scenario)
    local gnb_conf="$GNB_CONF_RELAY"
    if [[ "$sl_mode" == "1" ]] && [[ -f "$gnb_conf" ]]; then
        sed -i "s/\(sl_CSI_Acquisition[[:space:]]*=[[:space:]]*\)[0-9]\+/\1${csi_acq}/g" "$gnb_conf"
        sed -i "s/\(sl_PSFCH_Period[[:space:]]*=[[:space:]]*\)[0-9]\+/\1${psfch_period}/g" "$gnb_conf"
    fi

    # Remote config if REMOTE_UE_HOST is set
    if [[ -n "$REMOTE_UE_HOST" ]] && [[ "$REMOTE_UE_HOST" != "local" ]]; then
        local remote_user=$(find_user_name "$REMOTE_UE_HOST")
        local remote_conf="/home/$remote_user/$OAI_BASE_REL_PATH/$CONF_REL_PATH/sl_ue1.conf"
        safe_ssh "$REMOTE_UE_HOST" "sed -i 's/\(sl_CSI_Acquisition[[:space:]]*=[[:space:]]*\)[0-9]\+/\1${csi_acq}/g' $remote_conf" 2>/dev/null
        safe_ssh "$REMOTE_UE_HOST" "sed -i 's/\(sl_PSFCH_Period[[:space:]]*=[[:space:]]*\)[0-9]\+/\1${psfch_period}/g' $remote_conf" 2>/dev/null
    fi

    # Relay config — only relevant for SL mode 1 (relay scenario)
    if [[ "$sl_mode" == "1" ]] && [[ -n "$RELAY_UE_HOST" ]] && [[ "$RELAY_UE_HOST" != "local" ]]; then
        local relay_user=$(find_user_name "$RELAY_UE_HOST")
        local relay_conf="/home/$relay_user/$OAI_BASE_REL_PATH/$CONF_REL_PATH/sl_sync_ref.conf"
        safe_ssh "$RELAY_UE_HOST" "sed -i 's/\(sl_CSI_Acquisition[[:space:]]*=[[:space:]]*\)[0-9]\+/\1${csi_acq}/g' $relay_conf" 2>/dev/null
        safe_ssh "$RELAY_UE_HOST" "sed -i 's/\(sl_PSFCH_Period[[:space:]]*=[[:space:]]*\)[0-9]\+/\1${psfch_period}/g' $relay_conf" 2>/dev/null
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
    local remote_conf_path="/home/$remote_user/$OAI_BASE_REL_PATH/$CONF_REL_PATH"

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
        rm -f "/tmp/$f"
    done
    GNOME_WIN_IDX=0
}

save_softmodem_logs() {
    local test_name=$1
    local ts=$(date +"%Y%m%d_%H%M%S")
    for f in "${softmodem_log_files[@]}"; do
        if [[ -f "/tmp/$f" ]]; then
            local basename=$(basename "$f" .log)
            mv "/tmp/$f" "$log_dir/${basename}_${test_name}_${ts}.log"
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
            gnome-terminal --geometry=$geom -- bash -c "source ~/.bashrc 2>/dev/null; eval \"$cmd\" 2>&1 | tee $log_file" &
        else
            eval "$cmd" 2>&1 | tee $log_file &
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

    # Set default ping parameters if not already set (for non-BLER tests)
    : ${ping_count:=10}
    : ${ping_interval:=1}

    # ping always exits and prints its summary before the cleanup below SIGKILLs it.
    local ping_deadline
    ping_deadline=$(awk "BEGIN { d = $ping_count * $ping_interval + 5; printf \"%d\", (d==int(d)?d:int(d)+1) }")

    local user_name
    user_name=$(find_user_name "$host_name")
    # Generate unique filename with timestamp
    local timestamp=$(date +%Y%m%d_%H%M%S)

    if [[ $host_name == "local" ]]; then
        # Run ping locally
        ping_output="$log_dir/ping_result_${test_name}_${timestamp}.txt"
        cmd="ping -c $ping_count -i $ping_interval -w $ping_deadline -I $src_if $dest_ip"
        echo "Ping command: $cmd (count=$ping_count, interval=${ping_interval}s)"

        # Save command to commands.txt
        echo "=== Ping Command (host: $host_name) ===" >> "$log_dir/commands.txt"
        echo "$cmd" >> "$log_dir/commands.txt"
        echo "" >> "$log_dir/commands.txt"

        run_cmd $host_name "$cmd" $ping_output
    else
        # Run ping on remote host
        remote_log_dir="/home/$user_name/test_${timestamp}"
        ping_output="$remote_log_dir/ping_result_${test_name}_${timestamp}.txt"
        echo "Ping command (remote): ping -c $ping_count -i $ping_interval -I $src_if $dest_ip on $host_name (count=$ping_count, interval=${ping_interval}s)"
        local safe_filename="$log_dir/ping_result_${test_name}_${timestamp}.txt"

        # Build remote command with proper variable expansion
        local cmd="source /home/$user_name/.bashrc 2>/dev/null; mkdir -p $remote_log_dir && cd $remote_log_dir && ping -c $ping_count -i $ping_interval -w $ping_deadline -I $src_if $dest_ip"

        # Save command to commands.txt
        echo "=== Ping Command (host: $host_name) ===" >> "$log_dir/commands.txt"
        echo "ping -c $ping_count -i $ping_interval -w $ping_deadline -I $src_if $dest_ip" >> "$log_dir/commands.txt"
        echo "" >> "$log_dir/commands.txt"

        run_cmd $host_name "$cmd" "$safe_filename"
    fi

    # Wait for the ping to finish before tearing things down. Wait at least as
    # long as the ping's own deadline so it can always print its summary, even
    # when the remaining test duration is short.
    local kill_wait=$duration
    [[ $kill_wait -lt $ping_deadline ]] && kill_wait=$ping_deadline
    sleep $kill_wait

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
        if [ -f "/tmp/result_nrUE_syncref.log" ]; then
            local syncref_log="/tmp/result_nrUE_syncref.log"
        elif [ -f "$log_dir/result_nrUE_syncref.log" ]; then
            local syncref_log="$log_dir/result_nrUE_syncref.log"
        else
            local syncref_log=""
        fi
    elif [[ $sl_mode -eq 2 ]]; then
        # sl_mode 2: check local first, then remote
        if [ -f "/tmp/result_syncref.log" ]; then
            local syncref_log="/tmp/result_syncref.log"
        elif [ -f "$log_dir/result_syncref.log" ]; then
            local syncref_log="$log_dir/result_syncref.log"
        else
            local syncref_log=""
        fi
    fi

    if [[ $sl_mode -eq 0 ]]; then
        if [[ $host_name == "local" ]]; then
            local nrue_log="/tmp/result_nrUE.log"
        else
            local nrue_log="$log_dir/result_nrUE.log"
        fi
        local nrue_log=""
    else
        # For sidelink: check local first, then remote
        if [ -f "/tmp/result_nearby.log" ]; then
            local nearby_log="/tmp/result_nearby.log"
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

#############################################################
### iperf3 test functions
#############################################################
get_iperf3_stats() {
    local output_file=$1
    if [ ! -f "$output_file" ]; then
        echo "0 0 0"
        return
    fi
    local summary=$(grep -A1 '\[SUM\].*receiver\|.*receiver' "$output_file" | grep 'receiver' | tail -1)
    if [ -z "$summary" ]; then
        summary=$(grep 'receiver' "$output_file" | tail -1)
    fi
    local bw=$(echo "$summary" | grep -oP '[\d.]+(?=\s+[KMG]bits/sec)' | tail -1)
    local bw_unit=$(echo "$summary" | grep -oP '[\d.]+\s+\K[KMG](?=bits/sec)' | tail -1)
    local loss_pct=$(echo "$summary" | grep -oP '[\d.]+(?=%)' | tail -1)
    local jitter=$(echo "$summary" | grep -oP '[\d.]+(?=\s+ms)' | tail -1)
    local transfer=$(echo "$summary" | grep -oP '[\d.]+(?=\s+[KMG]Bytes)' | tail -1)
    local transfer_unit=$(echo "$summary" | grep -oP '[\d.]+\s+\K[KMG](?=Bytes)' | tail -1)
    # Normalize to Mbps
    case "$bw_unit" in
        K) bw=$(printf "%.3f" "$(echo "$bw / 1000" | bc -l 2>/dev/null || echo "0")") ;;
        G) bw=$(printf "%.3f" "$(echo "$bw * 1000" | bc -l 2>/dev/null || echo "0")") ;;
    esac
    # Normalize transfer to MB
    case "$transfer_unit" in
        K) transfer=$(printf "%.3f" "$(echo "${transfer:-0} / 1024" | bc -l 2>/dev/null || echo "0")") ;;
        G) transfer=$(printf "%.3f" "$(echo "${transfer:-0} * 1024" | bc -l 2>/dev/null || echo "0")") ;;
    esac
    echo "${bw:-0} ${loss_pct:-0} ${jitter:-0} ${transfer:-0}"
}

print_iperf3_summary_header() {
    local summary_file=$1
    if [ ! -f "$summary_file" ]; then
        echo "Test Name,Itrn,Num Hosts,MCS,BW Target,BW Actual (Mbps),Transfer (MB),Jitter (ms),Loss%,Result" \
            | tee -a "$summary_file"
    fi
}

print_iperf3_summary() {
    local summary_file=$1
    local test_name=$2
    local iteration=$3
    local num_hosts=$4
    local mcs=$5
    local bw_target=$6
    local bw_actual=$7
    local transfer=$8
    local jitter=$9
    local loss_pct=${10}
    local result=${11}

    print_iperf3_summary_header "$summary_file"
    echo "$test_name,$iteration,$num_hosts,${mcs:-N/A},$bw_target,${bw_actual}Mbps,${transfer}MB,${jitter}ms,${loss_pct}%,$result" \
        | tee -a "$summary_file"
}

run_iperf3_server() {
    local host_name=$1
    local bind_ip=$2
    local port=${3:-5001}
    local log_file=$4

    local cmd="iperf3 -s -B $bind_ip -p $port -i 1"

    if [[ "$host_name" == "upf_docker" ]]; then
        cmd="docker exec oai-upf bash -c 'iperf3 -s -B $bind_ip -p $port -i 1'"
    elif [[ "$host_name" == "$REMOTE_UE_HOST" || "$host_name" == "remote_ue" ]]; then
        # For remote_ue with policy routing, bind to device instead of IP
        cmd="iperf3 -s --bind-dev oaitun_ue2 -p $port -i 1"
    fi

    echo "=== iperf3 Server Command (host: $host_name) ===" >> "$log_dir/commands.txt"
    echo "$cmd" >> "$log_dir/commands.txt"
    echo "" >> "$log_dir/commands.txt"

    if [[ "$host_name" == "upf_docker" ]]; then
        bash -c "$cmd" 2>&1 | tee -a "$log_file" &
    elif [[ "$host_name" == "local" ]]; then
        bash -c "iperf3 -s -B $bind_ip -p $port -i 1" 2>&1 | tee -a "$log_file" &
    else
        local user_name=$(find_user_name "$host_name")
        bash -c "ssh $host_name '$cmd'" 2>&1 | tee -a "$log_file" &
    fi
}

run_iperf3_client() {
    local host_name=$1
    local server_ip=$2
    local bind_ip=$3
    local port=${4:-5001}
    local bandwidth=$5
    local iperf3_duration=${6:-10}
    local log_file=$7

    # A bind value that is not an IPv4 address is treated as an interface name (e.g. oaitun_ue2),
    # which iperf3 binds with --bind-dev instead of -B. Used by the SL Mode 1 relay case where the
    # Remote UE IP changes after core re-registration.
    local bind_opt="-B $bind_ip"
    if ! [[ "$bind_ip" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        bind_opt="--bind-dev $bind_ip"
    fi

    local cmd="iperf3 -u -c $server_ip $bind_opt -p $port -i 1 -b $bandwidth -t $iperf3_duration"

    if [[ "$host_name" == "upf_docker" ]]; then
        cmd="docker exec oai-upf bash -c 'iperf3 -u -c $server_ip $bind_opt -p $port -i 1 -b $bandwidth -t $iperf3_duration'"
    elif [[ "$host_name" == "$REMOTE_UE_HOST" || "$host_name" == "remote_ue" ]]; then
        # For remote_ue with policy routing, bind to device instead of IP
        cmd="iperf3 -u -c $server_ip --bind-dev oaitun_ue2 -p $port -i 1 -b $bandwidth -t $iperf3_duration"
    fi

    echo "=== iperf3 Client Command (host: $host_name) ===" >> "$log_dir/commands.txt"
    echo "$cmd" >> "$log_dir/commands.txt"
    echo "" >> "$log_dir/commands.txt"

    if [[ "$host_name" == "upf_docker" ]]; then
        bash -c "$cmd" 2>&1 | tee "$log_file" &
    elif [[ "$host_name" == "local" ]]; then
        bash -c "$cmd" 2>&1 | tee "$log_file" &
    else
        bash -c "ssh $host_name '$cmd'" 2>&1 | tee "$log_file" &
    fi
}

evaluate_iperf3_sweep() {
    local server_host=$1
    local server_bind_ip=$2
    local client_host=$3
    local client_bind_ip=$4
    local sl_mode=$5
    local test_name=$6
    local iteration=$7
    local num_hosts=$8
    local mcs=$9

    local iperf3_port=${iperf3_port:-5001}
    local iperf3_run_duration=${iperf3_run_duration:-10}
    local iperf3_summary_file="$log_dir/iperf3_summary_${timestamp}.csv"
    local prev_bw=0
    local saturation_threshold=10

    echo ""
    echo "========== iperf3 Bandwidth Sweep =========="
    echo "Server: $server_host ($server_bind_ip)"
    echo "Client: $client_host ($client_bind_ip)"
    echo "BW Array: ${iperf3_bw_array[@]}"
    echo "============================================="

    local server_log="$log_dir/iperf3_server_${timestamp}.txt"
    local server_pid=0

    for bw_target in "${iperf3_bw_array[@]}"; do
        echo ""
        echo "--- iperf3: target bandwidth = $bw_target ---"

        # Start fresh server for each bandwidth step
        if [ "$server_pid" -gt 0 ]; then
            kill $server_pid 2>/dev/null
        fi
        kill_all "$server_host" "iperf3"
        sleep 3
        run_iperf3_server "$server_host" "$server_bind_ip" "$iperf3_port" "$server_log"
        server_pid=$!
        sleep 3

        # Verify link is alive before each bandwidth step
        echo "=== Pre-iperf3 Link Verification (bandwidth: $bw_target) ===" >> "$log_dir/commands.txt"
        local ping_ok=0
        local ping_cmd=""
        if [[ "$client_host" == "local" ]]; then
            ping_cmd="ping -c 3 -W 2 -I $client_bind_ip $server_bind_ip"
            ping -c 3 -W 2 -I "$client_bind_ip" "$server_bind_ip" &>/dev/null && ping_ok=1
        else
            # For remote_ue with policy routing, use device binding instead of IP binding
            if [[ "$client_host" == "$REMOTE_UE_HOST" || "$client_host" == "remote_ue" ]]; then
                ping_cmd="ping -c 3 -W 2 -I oaitun_ue2 $server_bind_ip"
                safe_ssh "$client_host" "ping -c 3 -W 2 -I oaitun_ue2 $server_bind_ip" &>/dev/null && ping_ok=1
            else
                ping_cmd="ping -c 3 -W 2 -I $client_bind_ip $server_bind_ip"
                safe_ssh "$client_host" "ping -c 3 -W 2 -I $client_bind_ip $server_bind_ip" &>/dev/null && ping_ok=1
            fi
        fi
        echo "Command: $ping_cmd" >> "$log_dir/commands.txt"
        if [ "$ping_ok" -eq 0 ]; then
            echo "Result: FAILED" >> "$log_dir/commands.txt"
            echo "" >> "$log_dir/commands.txt"
            echo "WARNING: ping to $server_bind_ip failed before $bw_target — link is down"
            print_iperf3_summary "$iperf3_summary_file" "$test_name" "$iteration" "$num_hosts" "$mcs" \
                "$bw_target" "0" "0" "0" "0" "FAIL"
            break
        fi
        echo "Result: PASSED" >> "$log_dir/commands.txt"
        echo "" >> "$log_dir/commands.txt"

        local ts=$(date +%Y%m%d_%H%M%S)
        local client_log="$log_dir/iperf3_client_${bw_target}_${ts}.txt"

        # Start client (server is already running)
        run_iperf3_client "$client_host" "$server_bind_ip" "$client_bind_ip" "$iperf3_port" "$bw_target" "$iperf3_run_duration" "$client_log"
        local client_pid=$!

        # Wait for client to finish (timeout = run_duration + 15s for connection + summary overhead)
        local wait_timeout=$(( iperf3_run_duration + 15 ))
        local wait_count=0
        while kill -0 $client_pid 2>/dev/null; do
            sleep 1
            wait_count=$(( wait_count + 1 ))
            if [ $wait_count -ge $wait_timeout ]; then
                echo "WARNING: iperf3 client timed out after ${wait_timeout}s, killing..."
                kill $client_pid 2>/dev/null
                kill_all "$client_host" "iperf3"
                break
            fi
        done
        sleep 3

        # Parse results
        local stats=$(get_iperf3_stats "$client_log")
        local bw_actual=$(echo "$stats" | awk '{print $1}')
        local loss_pct=$(echo "$stats" | awk '{print $2}')
        local jitter=$(echo "$stats" | awk '{print $3}')
        local transfer=$(echo "$stats" | awk '{print $4}')

        # Determine result
        local result="PASS"
        local loss_int=$(echo "$loss_pct" | awk '{printf "%d", $1}')
        if [ "$loss_int" -gt 20 ]; then
            result="FAIL"
        fi

        # Detect connection failure (0 Mbps actual = server likely dead)
        local bw_zero=$(echo "$bw_actual" | awk '{print ($1 == 0) ? 1 : 0}')
        if [ "$bw_zero" -eq 1 ] && [ "$loss_int" -eq 0 ]; then
            result="FAIL"
            echo "WARNING: iperf3 client got 0 Mbps with 0% loss — server connection failed"
        # Check saturation: if actual BW didn't increase by at least 10% over previous
        elif [ "$prev_bw" != "0" ]; then
            local increase=$(echo "$bw_actual $prev_bw" | awk '{if ($2 > 0) printf "%d", (($1 - $2) / $2) * 100; else print 100}')
            if [ "$increase" -lt "$saturation_threshold" ]; then
                result="SATURATED"
            fi
        fi

        print_iperf3_summary "$iperf3_summary_file" "$test_name" "$iteration" "$num_hosts" "$mcs" \
            "$bw_target" "$bw_actual" "$transfer" "$jitter" "$loss_pct" "$result"

        prev_bw=$bw_actual

        if [[ "$result" == "SATURATED" || "$result" == "FAIL" ]]; then
            echo "iperf3 sweep stopped: $result at target=$bw_target (actual=${bw_actual}Mbps, loss=${loss_pct}%)"
            break
        fi
    done

    # Kill server after sweep completes
    kill $server_pid 2>/dev/null
    kill_all "$server_host" "iperf3"

    echo ""
    echo "iperf3 sweep complete. Results saved to: $iperf3_summary_file"
    if [ -f "$iperf3_summary_file" ]; then
        cat "$iperf3_summary_file"
        python3 "$SCRIPT_DIR/plot_sl_test_iperf3.py" "$iperf3_summary_file"
    fi
}

verify_ping() {
    local host_name=$1
    local src_if=$2
    local dest_ip=$3
    local count=${4:-5}

    local cmd="ping -c $count -I $src_if $dest_ip"

    echo "=== Ping Verification Command (host: $host_name) ===" >> "$log_dir/commands.txt"
    echo "$cmd" >> "$log_dir/commands.txt"
    echo "" >> "$log_dir/commands.txt"

    echo "Verifying connectivity with ping -c $count -I $src_if $dest_ip on $host_name..."
    local ping_result
    if [[ "$host_name" == "local" ]]; then
        ping_result=$(ping -c $count -I $src_if $dest_ip 2>&1)
    else
        ping_result=$(safe_ssh "$host_name" "$cmd" 2>&1)
    fi
    local received=$(echo "$ping_result" | grep -oP '\d+(?= received)')
    if [ "${received:-0}" -gt 0 ]; then
        echo "Ping verification PASSED ($received/$count received)"
        echo "=== Ping Result: PASSED ($received/$count received) ===" >> "$log_dir/commands.txt"
        echo "" >> "$log_dir/commands.txt"
        return 0
    else
        echo "Ping verification FAILED (0/$count received)"
        echo "=== Ping Result: FAILED (0/$count received) ===" >> "$log_dir/commands.txt"
        echo "" >> "$log_dir/commands.txt"
        return 1
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

    src_file="/tmp/result_syncref.log"; str_to_find='TotalTx 30'; dst_file="/tmp/result_summary.txt"
    test_result=$(tail -n 100 $src_file | grep -m 1 $str_to_find >> $dst_file)
}

run_gNB_cmd() {
    [[ $# -ge 1 ]] && test_type=$1
    [[ $# -ge 2 ]] && sl_mode=$2
    [[ $# -ge 3 ]] && host_name=$3

    # Get config path using helper function
    local config_path=$(get_gnb_config_path $sl_mode $host_name 0)

    [[ $sl_mode -eq 1 ]] && sl_relay_tag="--relay-type 1 --remote-ue-id 1 --sl-mode 1" || sl_relay_tag=""

    if [[ $test_type == "rfsim" ]]; then
        if [[ $host_name == 'local' ]]; then
            gNB_cmd="cd $OAI_BUILD_DIR; sudo -E LD_LIBRARY_PATH=$OAI_BUILD_DIR ./nr-softmodem \
                    -O $config_path --gNBs.[0].min_rxtxtime 6 \
                    --rfsimulator.serveraddr server --rfsimulator.serverport 4048 --rfsim $sa_flag --log_config.global_log_level info $sl_relay_tag"
        else
            gNB_cmd="LD_LIBRARY_PATH=$config_path:$LD_LIBRARY_PATH \
                    sudo -E $(dirname $config_path)/../../cmake_targets/ran_build/build/nr-softmodem \
                    -O $config_path --gNBs.[0].min_rxtxtime 6 \
                    --rfsimulator.serveraddr server --rfsimulator.serverport 4048 --rfsim $sa_flag --log_config.global_log_level info $sl_relay_tag"
        fi
    elif [[ $test_type == "usrp" ]]; then
        # For USRP, use relative path from build directory
        local rel_config="../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/$(basename $config_path)"
        gNB_cmd="cd $OAI_BUILD_DIR; sudo -E LD_LIBRARY_PATH=$OAI_BUILD_DIR ./nr-softmodem \
                -O $rel_config --gNBs.[0].min_rxtxtime 6 \
                -E $sa_flag --max-ldpc-iterations ${max_ldpc_iterations} --ue-txgain ${TX_GAIN} --ue-rxgain ${RX_GAIN} --device.name oai_usrpdevif $sl_relay_tag"
    elif [[ $test_type == "vrtsim" ]]; then
        # vrtsim (shared-memory radio) is local-host only; gNB is the Uu server.
        # NOTE: sl_relay_tag (sl_mode 1) intentionally omits --ip-demo (dropped after
        # base commit 3e4e0c196c; the remote UE now registers with the Core).
        gNB_cmd="cd $OAI_BUILD_DIR; sudo -E LD_LIBRARY_PATH=$OAI_BUILD_DIR ./nr-softmodem \
                -O $config_path --gNBs.[0].min_rxtxtime 6 \
                $sa_flag --device.name vrtsim --vrtsim.role server --vrtsim.chanmod 0 \
                --log_config.global_log_level info $sl_relay_tag"
    fi
    log_file="/tmp/result_gNB.log"
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
            nrUE_cmd="cd $OAI_BUILD_DIR; sudo -E LD_LIBRARY_PATH=$OAI_BUILD_DIR \
                    ./nr-uesoftmodem \
                    -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 \
                    --rfsimulator.serveraddr 127.0.0.1 --rfsimulator.serverport 4048 --rfsim $sa_flag \
                    --log_config.global_log_level info"
        else
            nrUE_cmd="LD_LIBRARY_PATH=/home/$user_name/$OAI_BASE_REL_PATH/$BUILD_REL_PATH:$LD_LIBRARY_PATH \
                    sudo -E /home/$user_name/$OAI_BASE_REL_PATH/$BUILD_REL_PATH/nr-uesoftmodem \
                    -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 \
                    --rfsimulator.serveraddr $LOCAL_HOST_IP --rfsimulator.serverport 4048 --rfsim $sa_flag \
                    --log_config.global_log_level info"
        fi
    elif [[ $test_type == "usrp" ]]; then
        nrUE_cmd="LD_LIBRARY_PATH=/home/$user_name/$OAI_BASE_REL_PATH/$BUILD_REL_PATH \
                    sudo -E /home/$user_name/$OAI_BASE_REL_PATH/$BUILD_REL_PATH/nr-uesoftmodem \
                    -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 \
                    -E $sa_flag --ue-txgain ${TX_GAIN} --ue-rxgain ${RX_GAIN} --thread-pool -1,-1 --device.name oai_usrpdevif \
                    --max-ldpc-iterations ${max_ldpc_iterations} --log_config.global_log_level info"
    elif [[ $test_type == "vrtsim" ]]; then
        # vrtsim (shared-memory radio) is local-host only; UE is the Uu client.
        nrUE_cmd="cd $OAI_BUILD_DIR; sudo -E LD_LIBRARY_PATH=$OAI_BUILD_DIR \
                    ./nr-uesoftmodem \
                    -r 106 --numerology 1 --band 78 -C 3619200000 --ssb 516 --uicc0.imsi 001010000000001 \
                    $sa_flag --device.name vrtsim --vrtsim.role client --vrtsim.chanmod 0 \
                    --log_config.global_log_level info"
    fi
    log_file="/tmp/result_nrUE.log"
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
                syncref_cmd="cd $OAI_BUILD_DIR; sudo -E LD_LIBRARY_PATH=$OAI_BUILD_DIR ./nr-uesoftmodem \
                            -O $CONF_PATH/sl_sync_ref.conf \
                            -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 \
                            --rfsim $sa_flag --sync-ref --node-number 2 --sl-mode 1 --remote-ue-id 1 \
                            --rfsimulator.serveraddr 127.0.0.1 --rfsimulator.serverport 4048 \
                            --rfsimulator.serveraddrsl 127.0.0.1 --rfsimulator.serverportsl 4148 \
                            --log_config.global_log_level info --relay-type 1 --is-relay-ue 1  $mcs"
            else
                syncref_cmd="LD_LIBRARY_PATH=/home/$user_name/$OAI_BASE_REL_PATH/$BUILD_REL_PATH:$LD_LIBRARY_PATH \
                            sudo -E /home/$user_name/$OAI_BASE_REL_PATH/$BUILD_REL_PATH/nr-uesoftmodem \
                            -O /home/$user_name/$OAI_BASE_REL_PATH/$CONF_REL_PATH/sl_sync_ref.conf \
                            -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 \
                            --rfsim $sa_flag --sync-ref --node-number 2 --sl-mode 1 --remote-ue-id 1 --relay-type 1 --is-relay-ue 1 \
                            --rfsimulator.serveraddr $GNB_HOST_IP  --rfsimulator.serverport 4048 \
                            --rfsimulator.serveraddrsl $REMOTE_HOST_IP  --rfsimulator.serverportsl 4148 \
                            --log_config.global_log_level info $mcs"
            fi
        elif [[ $test_type == "usrp" ]]; then
            if [[ $host_name == 'local' ]]; then
                syncref_cmd="cd $OAI_BUILD_DIR; sudo -E LD_LIBRARY_PATH=$OAI_BUILD_DIR ./nr-uesoftmodem \
                            -O $CONF_PATH/sl_sync_ref.conf \
                            -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 \
                            -E $sa_flag --sl-mode 1 --sync-ref --node-number 2 --relay-type 1 --is-relay-ue 1 --remote-ue-id 1 \
                            --usrp-args 'serial=$RELAY_UE_USRP_SN_FOR_UU,type=b200,num_recv_frames=64,num_send_frames=64' --usrp-args-sl 'serial=$RELAY_UE_USRP_SN_FOR_SL,type=b200,num_recv_frames=64,num_send_frames=64' \
                            $ext_clock_flag \
                            --max-ldpc-iterations ${max_ldpc_iterations} --ue-txgain ${TX_GAIN} --ue-rxgain ${RX_GAIN} --thread-pool -1,-1 --device.name oai_usrpdevif $mcs"
            else
                syncref_cmd="LD_LIBRARY_PATH=/home/$user_name/$OAI_BASE_REL_PATH/$BUILD_REL_PATH \
                            sudo -E /home/$user_name/$OAI_BASE_REL_PATH/$BUILD_REL_PATH/nr-uesoftmodem \
                            -O /home/$user_name/$OAI_BASE_REL_PATH/$CONF_REL_PATH/sl_sync_ref.conf \
                            -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 \
                            -E $sa_flag --sl-mode 1 --sync-ref --node-number 2 --relay-type 1 --is-relay-ue 1 --remote-ue-id 1 \
                            --usrp-args 'serial=$RELAY_UE_USRP_SN_FOR_UU,type=b200,num_recv_frames=64,num_send_frames=64' --usrp-args-sl 'serial=$RELAY_UE_USRP_SN_FOR_SL,type=b200,num_recv_frames=64,num_send_frames=64' \
                            $ext_clock_flag \
                            --max-ldpc-iterations ${max_ldpc_iterations} --ue-txgain ${TX_GAIN} --ue-rxgain ${RX_GAIN} --thread-pool -1,-1 --device.name oai_usrpdevif $mcs"
            fi
        elif [[ $test_type == "vrtsim" ]]; then
            # vrtsim relay/SyncRef (sl_mode 1): Uu client + PC5 client, local host only.
            # --remote-ue-id 1 (SRAP header match) + --thread-pool -1,-1,-1,-1 (CPU) + PDU-session DNN, as above.
            syncref_cmd="cd $OAI_BUILD_DIR; sudo -E LD_LIBRARY_PATH=$OAI_BUILD_DIR ./nr-uesoftmodem \
                        -O $CONF_PATH/sl_sync_ref.conf \
                        -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 --uicc0.pdu_sessions.[0].dnn oai \
                        $sa_flag --sync-ref --node-number 2 --sl-mode 1 --remote-ue-id 1 --thread-pool -1,-1,-1,-1 \
                        --device.name vrtsim --vrtsim.role client --vrtsim.role_sl client --vrtsim.chanmod 0 \
                        --log_config.global_log_level info --relay-type 1 --is-relay-ue 1 $mcs"
        fi
        log_file="/tmp/result_nrUE_syncref.log"
    elif [[ $sl_mode -eq 2 ]]; then
        if [[ $test_type == "rfsim" ]]; then
            syncref_cmd="cd $OAI_BUILD_DIR; sudo -E LD_LIBRARY_PATH=$OAI_BUILD_DIR \
                         $OAI_BUILD_DIR/nr-uesoftmodem \
                        -O $CONF_PATH/sl_sync_ref.conf --sync-ref --sl-mode 2 --rfsim $sa_flag \
                        --rfsimulator.serveraddrsl server --rfsimulator.serverportsl 4148 --log_config.global_log_level info  $mcs"
        elif [[ $test_type == "usrp" ]]; then
            syncref_cmd="cd $OAI_BUILD_DIR; sudo -E LD_LIBRARY_PATH=$OAI_BUILD_DIR \
                        $OAI_BUILD_DIR/nr-uesoftmodem \
                        -O $CONF_PATH/sl_sync_ref.conf -E $sa_flag --sl-mode 2 --sync-ref \
                        $ext_clock_flag \
                        --max-ldpc-iterations ${max_ldpc_iterations} --ue-txgain ${TX_GAIN} --ue-rxgain ${RX_GAIN} --thread-pool -1,-1 --device.name oai_usrpdevif $mcs"
        elif [[ $test_type == "vrtsim" ]]; then
            # vrtsim SyncRef (sl_mode 2): PC5 server, local host only.
            syncref_cmd="cd $OAI_BUILD_DIR; sudo -E LD_LIBRARY_PATH=$OAI_BUILD_DIR \
                        $OAI_BUILD_DIR/nr-uesoftmodem \
                        -O $CONF_PATH/sl_sync_ref.conf --sync-ref --sl-mode 2 $sa_flag \
                        --device.name vrtsim --vrtsim.role_sl server --vrtsim.chanmod 0 --log_config.global_log_level info $mcs"
        fi
        log_file="/tmp/result_syncref.log"
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
                nearby_cmd="cd $OAI_BUILD_DIR; sudo -E LD_LIBRARY_PATH=$OAI_BUILD_DIR ./nr-uesoftmodem \
                            -O $CONF_PATH/sl_ue1.conf --uicc0.imsi 001010000000002 \
                            --rfsim $sa_flag --sl-mode 2 $mcs --node-number 3 --relay-type 1 --remote-ue-id 1 \
                            --rfsimulator.serveraddrsl server --rfsimulator.serverportsl 4148 \
                            --log_config.global_log_level info"
            else
                nearby_cmd="LD_LIBRARY_PATH=/home/$user_name/$OAI_BASE_REL_PATH/$BUILD_REL_PATH:$LD_LIBRARY_PATH \
                            sudo -E /home/$user_name/$OAI_BASE_REL_PATH/$BUILD_REL_PATH/nr-uesoftmodem \
                            -O /home/$user_name/$OAI_BASE_REL_PATH/$CONF_REL_PATH/sl_ue1.conf --uicc0.imsi 001010000000002 \
                            --rfsim $sa_flag --sl-mode 2 $mcs --node-number 3 --relay-type 1 --remote-ue-id 1 \
                            --rfsimulator.serveraddrsl server --rfsimulator.serverportsl 4148 \
                            --log_config.global_log_level info"
            fi
        elif [[ $test_type == "usrp" ]]; then
            nearby_cmd="LD_LIBRARY_PATH=/home/$user_name/$OAI_BASE_REL_PATH/$BUILD_REL_PATH \
                        sudo -E /home/$user_name/$OAI_BASE_REL_PATH/$BUILD_REL_PATH/nr-uesoftmodem \
                        -O /home/$user_name/$OAI_BASE_REL_PATH/$CONF_REL_PATH/sl_ue1.conf --uicc0.imsi 001010000000002 \
                        -E $sa_flag --sl-mode 2 --node-number 3 --relay-type 1 --remote-ue-id 1 $ext_clock_flag $mcs \
                        --usrp-args 'type=b200,num_recv_frames=64,num_send_frames=64' \
                        --max-ldpc-iterations ${max_ldpc_iterations} --ue-txgain ${TX_GAIN} --ue-rxgain ${RX_GAIN} --thread-pool -1,-1 --device.name oai_usrpdevif"
        elif [[ $test_type == "vrtsim" ]]; then
            # vrtsim remote UE (sl_mode 1): PC5 server; own IMSI ...002 (registers with Core).
            # --remote-ue-id 1 (encoded in the SRAP header; must match gNB+relay), --thread-pool -1,-1,-1,-1
            # (avoid RT-thread CPU oversubscription that starves the relay Uu link), and the PDU-session DNN.
            nearby_cmd="cd $OAI_BUILD_DIR; sudo -E LD_LIBRARY_PATH=$OAI_BUILD_DIR ./nr-uesoftmodem \
                        -O $CONF_PATH/sl_ue1.conf --uicc0.imsi 001010000000002 --uicc0.pdu_sessions.[0].dnn oai \
                        $sa_flag --sl-mode 2 $mcs --node-number 3 --relay-type 1 --remote-ue-id 1 --thread-pool -1,-1,-1,-1 \
                        --device.name vrtsim --vrtsim.role_sl server --vrtsim.chanmod 0 \
                        --log_config.global_log_level info"
        fi
    elif [[ $sl_mode -eq 2 ]]; then
        if [[ $test_type == "rfsim" ]]; then
            if [[ $host_name == 'local' ]]; then
                nearby_cmd="cd $OAI_BUILD_DIR; sudo -E LD_LIBRARY_PATH=$OAI_BUILD_DIR ./nr-uesoftmodem \
                        -O $CONF_PATH/sl_ue1.conf --rfsim $sa_flag --sl-mode 2 $mcs \
                        --rfsimulator.serveraddrsl 127.0.0.1 --rfsimulator.serverportsl 4148 --log_config.global_log_level info"
            else
                nearby_cmd="LD_LIBRARY_PATH=/home/$user_name/$OAI_BASE_REL_PATH/$BUILD_REL_PATH:$LD_LIBRARY_PATH \
                        sudo -E /home/$user_name/$OAI_BASE_REL_PATH/$BUILD_REL_PATH/nr-uesoftmodem \
                        -O /home/$user_name/$OAI_BASE_REL_PATH/$CONF_REL_PATH/sl_ue1.conf --rfsim $sa_flag --sl-mode 2 $mcs \
                        --rfsimulator.serveraddrsl $LOCAL_HOST_IP --rfsimulator.serverportsl 4148 --log_config.global_log_level info"
            fi
        elif [[ $test_type == "usrp" ]]; then
            nearby_cmd="LD_LIBRARY_PATH=/home/$user_name/$OAI_BASE_REL_PATH/$BUILD_REL_PATH \
                        sudo -E /home/$user_name/$OAI_BASE_REL_PATH/$BUILD_REL_PATH/nr-uesoftmodem \
                        -O /home/$user_name/$OAI_BASE_REL_PATH/$CONF_REL_PATH/sl_ue1.conf -E $sa_flag --sl-mode 2 \
                        $ext_clock_flag \
                        --max-ldpc-iterations ${max_ldpc_iterations} --ue-txgain ${TX_GAIN} --ue-rxgain ${RX_GAIN} --thread-pool -1,-1 --device.name oai_usrpdevif $mcs"
        elif [[ $test_type == "vrtsim" ]]; then
            # vrtsim Nearby (sl_mode 2): PC5 client, local host only.
            nearby_cmd="cd $OAI_BUILD_DIR; sudo -E LD_LIBRARY_PATH=$OAI_BUILD_DIR ./nr-uesoftmodem \
                        -O $CONF_PATH/sl_ue1.conf $sa_flag --sl-mode 2 $mcs \
                        --device.name vrtsim --vrtsim.role_sl client --vrtsim.chanmod 0 --log_config.global_log_level info"
        fi
    fi
    log_file="/tmp/result_nearby.log"
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

    # Validate test type for local host execution
    if [[ $num_hosts -eq 1 ]]; then
        validate_test_type_for_local_host $test_type $test_name || return 1
    fi
    cleanup_old_logs

    local start_time=$(date +%s)
    local sl_mode=1
    local src_if="oaitun_ue2"
    local dest_ip="8.8.8.8"

    pre1='docker ps | grep oai-upf | wc -l' # expecting: 1
    act1='echo "core network is required !!!"; cd ~/oai-cn5g; systemctl start docker.service; docker compose up -d; sleep 2'
    [[ $(eval "$pre1") -eq 1 ]] && echo "Requirements are satisfied !!!" || eval "$act1"

    # Sync configuration files to remote hosts if needed
    if [[ $syncref_host_name != "local" ]]; then
        sync_config_files $syncref_host_name
    fi
    if [[ $nearby_host_name != "local" ]]; then
        sync_config_files $nearby_host_name
    fi

    echo "========== Test: $test_name ==========" >> "$log_dir/commands.txt"

    # vrtsim (shared-memory radio) launch SEQUENCE + timing are critical: bring up the
    # servers first and give each stage time to settle. gNB (Uu server) -> Remote UE
    # (PC5 server) -> Relay UE (Uu+PC5 client). The relay must complete Uu registration
    # and PC5 bring-up so the Remote UE finishes its Core registration (transitioning
    # from the initial demo IP to its Core-assigned IP) BEFORE the ping runs.
    # vrtsim uses POSIX shared-memory radio channels (/dev/shm/vrtsim_channel[_sl]); stale segments left by a
    # previous (killed) run corrupt the PC5/Uu sample exchange -> the remote's RRCSetupRequest never reaches
    # the gNB and registration fails. Clear them before launching so every run starts with fresh channels.
    [[ $test_type == "vrtsim" ]] && sudo rm -f /dev/shm/vrtsim* 2>/dev/null
    if [[ $test_type == "vrtsim" ]]; then
        run_gNB_cmd $test_type $sl_mode $gnb_host_name
        sleep 3
        run_nearby_cmd  $test_type $mcs $sl_mode $nearby_host_name
        sleep 3
        run_syncref_cmd $test_type $mcs $sl_mode $syncref_host_name
    else
        run_gNB_cmd $test_type $sl_mode $gnb_host_name
        sleep 1
        run_nearby_cmd  $test_type $mcs $sl_mode $nearby_host_name
        sleep 1
        run_syncref_cmd $test_type $mcs $sl_mode $syncref_host_name
    fi

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
        [[ $duration -lt 0 ]] && duration=0
    fi

    # Additional wait time for sidelink synchronization to complete
    if [[ "$use_extended_delays" == "1" ]]; then
        echo "Waiting additional ${sleep_timing[sync_stab_45s_v1]} seconds for sidelink sync to stabilize..."
        sleep ${sleep_timing[sync_stab_45s_v1]}
    fi

    # Gate the ping on remote UE Core registration (SL mode-1 relay). PC5 sync
    # alone is not enough: the remote UE's oaitun_ue2 keeps its pre-registration
    # default IP until the PDU Session Establishment Accept arrives via the relay.
    if wait_for_remote_ue_core_ip 40; then
        evaluate_ping_test $nearby_host_name $src_if $dest_ip $sl_mode $test_name
    else
        LAST_TEST_RESULT="FAIL"
        LAST_TX_PACKETS=0
        LAST_RX_PACKETS=0
        echo "Skipping ping: remote UE registration did not complete (no Core IP)."
    fi

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
vrtsim_slmode1_srap_ping_test_on_local_host() {
#############################################################
    # Argumemt(s): duration, mcs, iteration
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && iteration=$3
    local test_type="vrtsim"
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
        if grep -q "PSSCH.*RX ok [1-9]" /tmp/result_nearby.log 2>/dev/null; then
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

    # Copy current logs to archive (don't move - processes still writing).
    # Softmodem logs are written to /tmp by run_cmd (tee /tmp/result_*.log), same as the
    # other tests; copy from there (not $HOME, which never holds these files).
    for log_file in "${softmodem_log_files[@]}"; do
        if [[ -f "/tmp/$log_file" ]]; then
            cp "/tmp/$log_file" "$log_dir/${log_prefix}_${log_file}"
        fi
    done

    # Also save ping results
    if [[ -f "/tmp/ping_result_mcs${mcs}.txt" ]]; then
        cp "/tmp/ping_result_mcs${mcs}.txt" "$log_dir/${log_prefix}_ping.txt"
    fi
}

#############################################################
create_bler_config() {
#############################################################
    # Create BLER-specific config with fixed MCS settings
    # Only called during BLER tests
    # Arguments: sl_mode, host_name

    local sl_mode=$1
    local host_name=$2

    # Get base config path and derive BLER config path
    local base_conf=$(get_gnb_config_path $sl_mode $host_name 0)
    local bler_conf="${base_conf%.conf}_bler.conf"

    # Check if BLER config already exists
    if [[ $host_name == 'local' ]]; then
        if [[ -f "$bler_conf" ]]; then
            return 0
        fi

        # Copy base config and add BLER-specific parameters
        cp "$base_conf" "$bler_conf"

        # Add fixed MCS configuration after ul_max_mcs line
        sed -i '/ul_max_mcs[[:space:]]*=[[:space:]]*28;/a\
\
  # Fixed MCS configuration for BLER testing\
  # Setting harq_round_max=1 disables adaptive MCS (no CQI/HARQ-based adaptation)\
  # The scheduler will use dl_max_mcs/ul_max_mcs as fixed values\
  dl_max_mcs                  = 28;          # Downlink MCS (0-28, adjust for test)\
  dl_harq_round_max           = 1;           # Set to 1 for fixed MCS, >1 for adaptive\
  ul_harq_round_max           = 1;           # Set to 1 for fixed MCS, >1 for adaptive\
  min_grant_mcs               = 28;          # MUST match ul_max_mcs for high MCS testing (fixes UL MCS >9)' "$bler_conf"
    else
        # Remote host - check and create via SSH
        if safe_ssh "$host_name" "test -f $bler_conf" 2>/dev/null; then
            return 0
        fi

        # Copy and modify on remote host
        safe_ssh "$host_name" "cp $base_conf $bler_conf && sed -i '/ul_max_mcs[[:space:]]*=[[:space:]]*28;/a\\
\\
  # Fixed MCS configuration for BLER testing\\
  # Setting harq_round_max=1 disables adaptive MCS (no CQI/HARQ-based adaptation)\\
  # The scheduler will use dl_max_mcs/ul_max_mcs as fixed values\\
  dl_max_mcs                  = 28;          # Downlink MCS (0-28, adjust for test)\\
  dl_harq_round_max           = 1;           # Set to 1 for fixed MCS, >1 for adaptive\\
  ul_harq_round_max           = 1;           # Set to 1 for fixed MCS, >1 for adaptive\\
  min_grant_mcs               = 28;          # MUST match ul_max_mcs for high MCS testing (fixes UL MCS >9)' $bler_conf" 2>/dev/null
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

    [[ $sl_mode -eq 1 ]] && sl_relay_tag="--relay-type 1 --remote-ue-id 1 --sl-mode 1" || sl_relay_tag=""

    # Create BLER config with fixed MCS settings
    create_bler_config $sl_mode $host_name

    # Get BLER config path using helper function
    local config_path=$(get_gnb_config_path $sl_mode $host_name 1)

    if [[ $test_type == "vrtsim" ]]; then
        # vrtsim gNB: Uu server. chanmod is DELIBERATELY OFF on the gNB so the Uu DL stays
        # clean. vrtsim's cold cell-search breaks under any gNB-Uu noise, which would stop
        # the relay from ever syncing/registering. BLER noise is injected on the PC5
        # sidelink only (relay + remote UE); the fixed-MCS overrides still apply here.
        # (ploss=${ploss} is accepted for signature parity but not applied on the clean Uu.)
        gNB_cmd="cd $OAI_BUILD_DIR; \
                 sudo -E LD_LIBRARY_PATH=\$PWD ./nr-softmodem \
                 -O $config_path \
                 --gNBs.[0].min_rxtxtime 6 \
                 --sa --device.name vrtsim --vrtsim.role server --vrtsim.chanmod 0 \
                 --log_config.global_log_level info --log_config.global_log_options time \
                 --MACRLCs.[0].dl_max_mcs ${mcs_value} \
                 --MACRLCs.[0].ul_max_mcs ${mcs_value} \
                 --MACRLCs.[0].dl_harq_round_max 1 \
                 --MACRLCs.[0].ul_harq_round_max 1 \
                 $sl_relay_tag"
    else
        gNB_cmd="cd $OAI_BUILD_DIR; \
                 sudo -E LD_LIBRARY_PATH=\$PWD ./nr-softmodem \
                 -O $config_path \
                 --gNBs.[0].min_rxtxtime 6 \
                 --rfsimulator.serveraddr server --rfsimulator.serverport 4048 --rfsim --sa \
                 --log_config.global_log_level info --log_config.global_log_options time \
                 --rfsimulator.options chanmod --channelmod.modellist modellist_rfsimu_1 \
                 --channelmod.modellist_rfsimu_1.[0].noise_power_dB ${noise_power} \
                 --channelmod.modellist_rfsimu_1.[0].ploss_dB ${ploss} \
                 --MACRLCs.[0].dl_max_mcs ${mcs_value} \
                 --MACRLCs.[0].ul_max_mcs ${mcs_value} \
                 --MACRLCs.[0].dl_harq_round_max 1 \
                 --MACRLCs.[0].ul_harq_round_max 1 \
                 $sl_relay_tag"
    fi

    log_file="/tmp/result_gNB.log"

    echo "=== gNB Command (noise=${noise_power}dB, ploss=${ploss}dB, config=${bler_conf_tag}) ===" >> "$log_dir/commands.txt"
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

    if [[ $test_type == "vrtsim" ]]; then
        # vrtsim relay/SyncRef: Uu client + PC5 client. chanmod ON injects the swept
        # noise/ploss on the relay's PC5 TX (relay->remote). Note: --vrtsim.chanmod is
        # process-global so the same noise also touches the relay's Uu UL; that Uu-side
        # effect is intentionally ignored for this BLER test. (Flags may need tuning.)
        syncref_cmd="cd $OAI_BUILD_DIR; \
                     sudo -E LD_LIBRARY_PATH=\$PWD ./nr-uesoftmodem \
                     -O $CONF_PATH/sl_sync_ref.conf \
                     -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 \
                     --sa --sync-ref --sl-mode 1 --node-number 2 \
                     --device.name vrtsim --vrtsim.role client --vrtsim.role_sl client --vrtsim.chanmod 1 \
                     --channelmod.modellist_vrtsim.[0].noise_power_dB ${noise_power} \
                     --channelmod.modellist_vrtsim.[0].ploss_dB ${ploss} \
                     --channelmod.modellist_vrtsim.[1].noise_power_dB ${noise_power} \
                     --channelmod.modellist_vrtsim.[1].ploss_dB ${ploss} \
                     --log_config.global_log_level info --log_config.global_log_options time \
                     --relay-type 1 --is-relay-ue 1 --mcs ${mcs}"
    else
        syncref_cmd="cd $OAI_BUILD_DIR; \
                     sudo -E LD_LIBRARY_PATH=\$PWD ./nr-uesoftmodem \
                     -O $CONF_PATH/sl_sync_ref.conf \
                     -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 \
                     --rfsim --sa --sync-ref --sl-mode 1 \
                     --rfsimulator.serveraddr 127.0.0.1 --rfsimulator.serverport 4048 \
                     --rfsimulator.serveraddrsl 127.0.0.1 --rfsimulator.serverportsl 4148 \
                     --log_config.global_log_level info --log_config.global_log_options time \
                     --rfsimulator.options chanmod --channelmod.modellist modellist_rfsimu_1 \
                     --channelmod.modellist_rfsimu_1.[1].noise_power_dB ${noise_power} \
                     --channelmod.modellist_rfsimu_1.[1].ploss_dB ${ploss} \
                     --relay-type 1 --is-relay-ue 1 --mcs ${mcs} --node-number 2"
    fi

    log_file="/tmp/result_nrUE_syncref.log"

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

    if [[ $test_type == "vrtsim" ]]; then
        # vrtsim remote UE: PC5 server (sl-mode 2), sidelink-only node -> all of its chanmod
        # noise is on PC5 (remote->relay). This is the clean, fully-isolated PC5 BLER knob;
        # its own sidelink sync (RX of the relay's PC5 TX) is unaffected by this TX noise.
        # (Flags may need tuning; command line differs from rfsim.)
        nearby_cmd="cd $OAI_BUILD_DIR; \
                    sudo -E LD_LIBRARY_PATH=\$PWD ./nr-uesoftmodem \
                    -O $CONF_PATH/sl_ue1.conf \
                    -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000002 \
                    --sa --sl-mode 2 --node-number 3 --relay-type 1 \
                    --device.name vrtsim --vrtsim.role_sl server --vrtsim.chanmod 1 \
                    --channelmod.modellist_vrtsim.[0].noise_power_dB ${noise_power} \
                    --channelmod.modellist_vrtsim.[0].ploss_dB ${ploss} \
                    --channelmod.modellist_vrtsim.[1].noise_power_dB ${noise_power} \
                    --channelmod.modellist_vrtsim.[1].ploss_dB ${ploss} \
                    --log_config.global_log_level info --log_config.global_log_options time \
                    --mcs ${mcs}"
    else
        nearby_cmd="cd $OAI_BUILD_DIR; \
                    sudo -E LD_LIBRARY_PATH=\$PWD ./nr-uesoftmodem \
                    -O $CONF_PATH/sl_ue1.conf \
                    -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000002 \
                    --rfsim --sa --sl-mode 2 \
                    --rfsimulator.serveraddrsl server --rfsimulator.serverportsl 4148 \
                    --log_config.global_log_level info --log_config.global_log_options time \
                    --rfsimulator.options chanmod --channelmod.modellist modellist_rfsimu_1 \
                    --channelmod.modellist_rfsimu_1.[0].noise_power_dB ${noise_power} \
                    --channelmod.modellist_rfsimu_1.[0].ploss_dB ${ploss} \
                    --channelmod.modellist_rfsimu_1.[1].noise_power_dB ${noise_power} \
                    --channelmod.modellist_rfsimu_1.[1].ploss_dB ${ploss} \
                    --mcs ${mcs} --node-number 3 --relay-type 1"
    fi

    log_file="/tmp/result_nearby.log"

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
    sync_default_config_params $csi_acquisition $psfch_period $sl_mode

    # Restart core network (full cycle: down then up)
    restart_core_network || {
        echo "ERROR: Core network restart failed. Aborting iteration i=$i, noise=$noise_power, mcs=$mcs"
        continue
    }

    # Set Uu interface MCS in gNB configuration (config file approach for reliability)
    echo "[0/4] Configuring Uu interface MCS=${mcs}..."
    if [[ "$gnb_host_name" == "local" || "$gnb_host_name" == "localhost" ]]; then
        set_uu_mcs $mcs
    else
        set_uu_mcs_remote $gnb_host_name $mcs
    fi

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

    # Run ping test (use configured rate)
    local bler_ping_count=$((duration * ping_per_second))
    local bler_ping_interval=$(awk "BEGIN {print 1.0/$ping_per_second}")
    echo "Starting ping: $bler_ping_count packets @ ${ping_per_second} pkt/s (interval ${bler_ping_interval}s)..."

    # Save the ping command to commands.txt (mirrors the node-launch commands logged above)
    echo "=== Ping Command (mcs=${mcs}, noise=${noise_power}) ===" >> "$log_dir/commands.txt"
    echo "ping -I $src_if $dest_ip -i $bler_ping_interval -c $bler_ping_count > /tmp/ping_result_mcs${mcs}.txt 2>&1" >> "$log_dir/commands.txt"
    echo "" >> "$log_dir/commands.txt"

    ping -I $src_if $dest_ip -i $bler_ping_interval -c $bler_ping_count > /tmp/ping_result_mcs${mcs}.txt 2>&1

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

    # Populate the PSSCH (SLSCH) figures for the summary. bler_test does not call
    # evaluate_ping_test, so LAST_PSSCH_* would otherwise stay unset (-> 0 -> N/A).
    # bler_test is always sl_mode 1: relay/syncref -> result_nrUE_syncref.log,
    # remote/nearby -> result_nearby.log. Each UE's TX and RX are read from ITS OWN log;
    # print_test_summary pairs TX of one UE with RX of the other (cross-UE).
    local bler_syncref_log=""
    if [ -f "/tmp/result_nrUE_syncref.log" ]; then
        bler_syncref_log="/tmp/result_nrUE_syncref.log"
    elif [ -f "$log_dir/result_nrUE_syncref.log" ]; then
        bler_syncref_log="$log_dir/result_nrUE_syncref.log"
    fi
    local bler_nearby_log=""
    if [ -f "/tmp/result_nearby.log" ]; then
        bler_nearby_log="/tmp/result_nearby.log"
    elif [ -f "$log_dir/result_nearby.log" ]; then
        bler_nearby_log="$log_dir/result_nearby.log"
    fi
    if [ -f "$bler_syncref_log" ]; then
        local pssch_syncref=$(get_pssch_stats "$bler_syncref_log" "syncref")
        LAST_PSSCH_TX_SYNCREF=$(echo $pssch_syncref | awk '{print $1}')
        LAST_PSSCH_RX_SYNCREF=$(echo $pssch_syncref | awk '{print $2}')
    else
        LAST_PSSCH_TX_SYNCREF=0
        LAST_PSSCH_RX_SYNCREF=0
    fi
    if [ -f "$bler_nearby_log" ]; then
        local pssch_nearby=$(get_pssch_stats "$bler_nearby_log" "nearby")
        LAST_PSSCH_TX_NEARBY=$(echo $pssch_nearby | awk '{print $1}')
        LAST_PSSCH_RX_NEARBY=$(echo $pssch_nearby | awk '{print $2}')
    else
        LAST_PSSCH_TX_NEARBY=0
        LAST_PSSCH_RX_NEARBY=0
    fi

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

#############################################################
vrtsim_slmode1_bler_test_on_local_host() {
#############################################################
    # vrtsim BLER test wrapper - delegates to the core bler_test function.
    # Local-host only (vrtsim is a shared-memory radio). Noise/ploss is applied on the PC5
    # sidelink only; the gNB Uu is kept clean (see run_gNB_cmd_with_noise) so the relay can
    # sync/register. Sweep values come from vrtsim_noise_power_array (vrtsim noise scale:
    # 100 = off, more-negative = less noise, ~-30 dB breaks sync), which the dispatcher
    # selects automatically for vrtsim_*bler* tests.
    # Arguments: duration, mcs, iteration, noise_power
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && iteration=$3
    [[ $# -ge 4 ]] && noise_power=$4

    local test_type="vrtsim"
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

    # Validate test type for local host execution
    if [[ $num_hosts -eq 1 ]]; then
        validate_test_type_for_local_host $test_type $test_name || return 1
    fi
    cleanup_old_logs

    local start_time=$(date +%s)
    local sl_mode=0
    local src_if="oaitun_ue1"
    local dest_ip="8.8.8.8"

    echo "========== Test: $test_name ==========" >> "$log_dir/commands.txt"
    run_gNB_cmd $test_type $sl_mode $gnb_host_name
    sleep 1
    run_nrUE_cmd $test_type $mcs $sl_mode $nrue_host_name
    wait_for_tun_interface $src_if $nrue_host_name $duration

    # Additional wait time for synchronization to complete
    if [[ "$use_extended_delays" == "1" ]]; then
        echo "Waiting additional 30 seconds for sidelink sync to stabilize..."
        sleep ${sleep_timing[sync_stab_30s]}  # Configurable: default 30s
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
#############################################################
vrtsim_uu_ping_test_on_local_host() {
#############################################################
    # Argumemt(s): duration, mcs, iteration
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && iteration=$3
    local test_type="vrtsim"
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

    # Validate test type for local host execution
    if [[ $num_hosts -eq 1 ]]; then
        validate_test_type_for_local_host $test_type $test_name || return 1
    fi
    cleanup_old_logs

    local start_time=$(date +%s)
    local sl_mode=2
    local src_if="oaitun_ue1"
    local dest_ip="10.0.0.100"

    echo "test_type:" $test_type " mcs: " $mcs " sl_mode: " $sl_mode " syncref_host_name: " $syncref_host_name " nearby_host_name: " $nearby_host_name

    # Sync configuration files if using remote host
    if [[ $nearby_host_name != "local" ]]; then
        sync_config_files $nearby_host_name
    fi

    echo "========== Test: $test_name ==========" >> "$log_dir/commands.txt"
    run_syncref_cmd $test_type $mcs $sl_mode $syncref_host_name
    sleep 1
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
        [[ $duration -lt 0 ]] && duration=0
    fi

    # Additional wait time for sidelink synchronization to complete
    if [[ "$use_extended_delays" == "1" ]]; then
        echo "Waiting additional ${sleep_timing[sync_stab_45s_v1]} seconds for sidelink sync to stabilize..."
        sleep ${sleep_timing[sync_stab_45s_v1]}
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
#############################################################
vrtsim_pc5_ping_test_on_local_host() {
#############################################################
    # Argumemt(s): duration, mcs, iteration
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && iteration=$3
    local test_type="vrtsim"
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
    sync_default_config_params $DEFAULT_CSI_ACQ $DEFAULT_PSFCH_PERIOD $sl_mode

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

    echo "========== Test: ${test_name}_csi${csi_acq}_psfch${period} ==========" >> "$log_dir/commands.txt"
    # For SL mode 2 two-host tests: syncref runs locally, nearby runs remotely
    if [[ $nearby_host_name == "local" ]]; then
        run_syncref_cmd $test_type $mcs $sl_mode "local"
        sleep 1
        run_nearby_cmd  $test_type $mcs $sl_mode "local"
    else
        run_syncref_cmd $test_type $mcs $sl_mode "local"
        sleep 1
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
        [[ $duration -lt 0 ]] && duration=0
    fi

    # Additional wait time for sidelink synchronization to complete
    if [[ "$use_extended_delays" == "1" ]]; then
        echo "Waiting additional ${sleep_timing[sync_stab_45s_v2]} seconds for sidelink sync to stabilize..."
        sleep ${sleep_timing[sync_stab_45s_v2]}
    fi

    evaluate_ping_test $syncref_host_name "oaitun_ue1" "10.0.0.100" $sl_mode "${test_name}_csi${csi_acq}_psfch${period}"

    # Cleanup all processes (nrue_host_name was cleaned up in the evaluate_ping_test)
    kill_all $nearby_host_name nr-uesoftmodem
    save_softmodem_logs "${test_name}_csi${csi_acq}_psfch${period}"

    # Restore configs back to defaults for next test iteration
    echo "==> Restoring baseline: CSI=$DEFAULT_CSI_ACQ, PSFCH=$DEFAULT_PSFCH_PERIOD"
    sync_default_config_params $DEFAULT_CSI_ACQ $DEFAULT_PSFCH_PERIOD $sl_mode

    # Push the restored baseline configs back to the remote hosts. The test
    # scp-copied both config files to each remote host, so we must overwrite
    # them with the baseline versions to fully revert (sync_default_config_params
    # only seds the primary file per host).
    [[ $nearby_host_name != "local" ]] && sync_config_files $nearby_host_name
    [[ $syncref_host_name != "local" ]] && sync_config_files $syncref_host_name

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
vrtsim_pc5_csi_acquisition_psfch_period_test_on_local_host() {
#############################################################
    # Argumemt(s): csi_acq, psfch_period, duration, mcs, iteration
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && csi_acq=$1; echo "csi_acq = " $1
    [[ $# -ge 2 ]] && period=$2;  echo "psfch_period  = " $2
    [[ $# -ge 3 ]] && duration=$3
    [[ $# -ge 4 ]] && mcs=$4
    [[ $# -ge 5 ]] && iteration=$5
    local test_type="vrtsim"
    local syncref_host_name="local"
    local nearby_host_name="local"
    local num_hosts=1
    pc5_csi_acquisition_psfch_period_test $csi_acq $period $duration $test_type $mcs $iteration $syncref_host_name $nearby_host_name $num_hosts "${FUNCNAME[0]}"
}

slmode1_srap_csi_acquisition_psfch_period_test() {
    # Argumemt(s): csi_acq, psfch_period, duration, test_type, mcs, iteration
    [[ $# -ge 1 ]] && csi_acq=$1; echo "csi_acq = " $1
    [[ $# -ge 2 ]] && period=$2;  echo "psfch_period  = " $2
    [[ $# -ge 3 ]] && duration=$3
    [[ $# -ge 4 ]] && test_type=$4
    [[ $# -ge 5 ]] && mcs=$5
    [[ $# -ge 6 ]] && iteration=$6
    [[ $# -ge 7 ]] && gnb_host_name=$7
    [[ $# -ge 8 ]] && syncref_host_name=$8
    [[ $# -ge 9 ]] && nearby_host_name=$9
    [[ $# -ge 10 ]] && num_hosts=${10}
    [[ $# -ge 11 ]] && test_name=${11} || test_name="${FUNCNAME[0]}"

    # Validate test type for local host execution
    if [[ $num_hosts -eq 1 ]]; then
        validate_test_type_for_local_host "$test_type" "${FUNCNAME[0]}" || return 1
    fi
    if [[ $nearby_host_name != "local" ]]; then
        local remote_user=$(find_user_name "$nearby_host_name")
    fi
    cleanup_old_logs

    local start_time=$(date +%s)
    local sl_mode=1

    pre1='docker ps | grep oai-upf | wc -l'
    act1='echo "core network is required !!!"; cd ~/oai-cn5g; systemctl start docker.service; docker compose up -d; sleep 2'
    [[ $(eval "$pre1") -eq 1 ]] && echo "Requirements are satisfied !!!" || eval "$act1"

    # Ensure both local and remote systems start with synchronized baseline
    # This MUST happen for every test run (8 iterations) to ensure consistency
    echo "==> Syncing baseline: CSI=$DEFAULT_CSI_ACQ, PSFCH=$DEFAULT_PSFCH_PERIOD across all systems..."
    sync_default_config_params $DEFAULT_CSI_ACQ $DEFAULT_PSFCH_PERIOD $sl_mode

    echo "==> Applying test-specific config: CSI=$csi_acq, PSFCH=$period"

    # Update local syncref config (always runs locally) - updates ALL occurrences
    sed -i "s/\(sl_CSI_Acquisition[[:space:]]*=[[:space:]]*\)[0-9]\+/\1$csi_acq/g" "$CONF_PATH/sl_sync_ref.conf"
    sed -i "s/\(sl_PSFCH_Period[[:space:]]*=[[:space:]]*\)[0-9]\+/\1$period/g" "$CONF_PATH/sl_sync_ref.conf"

    # Keep the local gNB relay config (SL mode 1) in sync with the test-specific
    # values so the gNB's sidelink resource pool matches the UEs'. The gNB runs
    # locally, so no scp is needed.
    if [[ "$sl_mode" == "1" ]] && [[ -f "$GNB_CONF_RELAY" ]]; then
        sed -i "s/\(sl_CSI_Acquisition[[:space:]]*=[[:space:]]*\)[0-9]\+/\1$csi_acq/g" "$GNB_CONF_RELAY"
        sed -i "s/\(sl_PSFCH_Period[[:space:]]*=[[:space:]]*\)[0-9]\+/\1$period/g" "$GNB_CONF_RELAY"
    fi

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

    # Push updated configs to remote syncref/relay host if applicable
    if [[ $syncref_host_name != "local" ]]; then
        sync_config_files $syncref_host_name
    fi

    echo "========== Test: ${test_name}_csi${csi_acq}_psfch${period} ==========" >> "$log_dir/commands.txt"
    # For SL mode 1 three-host tests: gNB runs locally, syncref and nearby run remotely
    run_gNB_cmd $test_type $sl_mode $gnb_host_name
    sleep 1
    run_nearby_cmd  $test_type $mcs $sl_mode $nearby_host_name
    sleep 1
    run_syncref_cmd $test_type $mcs $sl_mode $syncref_host_name

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
        [[ $duration -lt 0 ]] && duration=0
    fi

    # Additional wait time for sidelink synchronization to complete
    if [[ "$use_extended_delays" == "1" ]]; then
        echo "Waiting additional ${sleep_timing[sync_stab_45s_v2]} seconds for sidelink sync to stabilize..."
        sleep ${sleep_timing[sync_stab_45s_v2]}
    fi
    sleep 3
    evaluate_ping_test $nearby_host_name "oaitun_ue2" "8.8.8.8" $sl_mode "${test_name}_csi${csi_acq}_psfch${period}"

    # Cleanup all processes (nrue_host_name was cleaned up in the evaluate_ping_test)
    kill_all $nearby_host_name nr-uesoftmodem
    kill_all $syncref_host_name nr-uesoftmodem
    kill_all $gnb_host_name nr-softmodem
    save_softmodem_logs "${test_name}_csi${csi_acq}_psfch${period}"


    # Restore configs back to defaults for next test iteration
    echo "==> Restoring baseline: CSI=$DEFAULT_CSI_ACQ, PSFCH=$DEFAULT_PSFCH_PERIOD"
    sync_default_config_params $DEFAULT_CSI_ACQ $DEFAULT_PSFCH_PERIOD $sl_mode

    # Push the restored baseline configs back to the remote hosts. The test
    # scp-copied both config files to each remote host, so we must overwrite
    # them with the baseline versions to fully revert (sync_default_config_params
    # only seds the primary file per host).
    [[ $nearby_host_name != "local" ]] && sync_config_files $nearby_host_name
    [[ $syncref_host_name != "local" ]] && sync_config_files $syncref_host_name

    local end_time=$(date +%s)
    local elapsed=$((end_time - start_time))
    print_runtime $start_time $end_time

    # Print test summary
    print_test_summary "${test_name}_csi${csi_acq}_psfch${period}" "$iteration" "$num_hosts" "$mcs" "$elapsed" "$LAST_TX_PACKETS" "$LAST_RX_PACKETS" "$LAST_TEST_RESULT"
}
#############################################################
rfsim_slmode1_srap_csi_acquisition_psfch_period_test_on_local_host() {
#############################################################
    # Argumemt(s): csi_acq, psfch_period, duration, mcs, iteration
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && csi_acq=$1; echo "csi_acq = " $1
    [[ $# -ge 2 ]] && period=$2;  echo "psfch_period  = " $2
    [[ $# -ge 3 ]] && duration=$3
    [[ $# -ge 4 ]] && mcs=$4
    [[ $# -ge 5 ]] && iteration=$5
    local test_type="rfsim"
    local gnb_host_name="local"
    local syncref_host_name="local"
    local nearby_host_name="local"
    local num_hosts=1
    slmode1_srap_csi_acquisition_psfch_period_test $csi_acq $period $duration $test_type $mcs $iteration $gnb_host_name $syncref_host_name $nearby_host_name $num_hosts "${FUNCNAME[0]}"
}
#############################################################
vrtsim_slmode1_srap_csi_acquisition_psfch_period_test_on_local_host() {
#############################################################
    # Argumemt(s): csi_acq, psfch_period, duration, mcs, iteration
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && csi_acq=$1; echo "csi_acq = " $1
    [[ $# -ge 2 ]] && period=$2;  echo "psfch_period  = " $2
    [[ $# -ge 3 ]] && duration=$3
    [[ $# -ge 4 ]] && mcs=$4
    [[ $# -ge 5 ]] && iteration=$5
    local test_type="vrtsim"
    local gnb_host_name="local"
    local syncref_host_name="local"
    local nearby_host_name="local"
    local num_hosts=1
    slmode1_srap_csi_acquisition_psfch_period_test $csi_acq $period $duration $test_type $mcs $iteration $gnb_host_name $syncref_host_name $nearby_host_name $num_hosts "${FUNCNAME[0]}"
}
#############################################################
rfsim_slmode1_srap_csi_acquisition_psfch_period_test_on_three_hosts() {
#############################################################
    # Argumemt(s): csi_acq, psfch_period, duration, mcs, iteration
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && csi_acq=$1; echo "csi_acq = " $1
    [[ $# -ge 2 ]] && period=$2;  echo "psfch_period  = " $2
    [[ $# -ge 3 ]] && duration=$3
    [[ $# -ge 4 ]] && mcs=$4
    [[ $# -ge 5 ]] && iteration=$5
    local test_type="rfsim"
    local gnb_host_name="local"
    local syncref_host_name=$RELAY_UE_HOST
    local nearby_host_name=$REMOTE_UE_HOST
    local num_hosts=3
    slmode1_srap_csi_acquisition_psfch_period_test $csi_acq $period $duration $test_type $mcs $iteration $gnb_host_name $syncref_host_name $nearby_host_name $num_hosts "${FUNCNAME[0]}"
}
#############################################################
usrp_B210_slmode1_srap_csi_acquisition_psfch_period_test_on_three_hosts() {
#############################################################
    # Argumemt(s): csi_acq, psfch_period, duration, mcs, iteration
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && csi_acq=$1; echo "csi_acq = " $1
    [[ $# -ge 2 ]] && period=$2;  echo "psfch_period  = " $2
    [[ $# -ge 3 ]] && duration=$3
    [[ $# -ge 4 ]] && mcs=$4
    [[ $# -ge 5 ]] && iteration=$5
    local test_type="usrp"
    local gnb_host_name="local"
    local syncref_host_name=$RELAY_UE_HOST
    local nearby_host_name=$REMOTE_UE_HOST
    local num_hosts=3
    slmode1_srap_csi_acquisition_psfch_period_test $csi_acq $period $duration $test_type $mcs $iteration $gnb_host_name $syncref_host_name $nearby_host_name $num_hosts "${FUNCNAME[0]}"
}

#############################################################
### iperf3 test cases
#############################################################
pc5_iperf3_test() {
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && test_type=$2
    [[ $# -ge 3 ]] && mcs=$3
    [[ $# -ge 4 ]] && iteration=$4
    [[ $# -ge 5 ]] && syncref_host_name=$5
    [[ $# -ge 6 ]] && nearby_host_name=$6
    [[ $# -ge 7 ]] && num_hosts=$7
    [[ $# -ge 8 ]] && test_name=$8 || test_name="${FUNCNAME[0]}"

    # Validate test type for local host execution
    if [[ $num_hosts -eq 1 ]]; then
        validate_test_type_for_local_host $test_type $test_name || return 1
    fi
    cleanup_old_logs

    local start_time=$(date +%s)
    local sl_mode=2
    local src_if="oaitun_ue1"
    local dest_ip="10.0.0.100"
    local server_ip="10.0.0.1"
    local client_ip="10.0.0.100"

    if [[ $nearby_host_name != "local" ]]; then
        sync_config_files $nearby_host_name
    fi

    echo "========== Test: $test_name ==========" >> "$log_dir/commands.txt"
    run_syncref_cmd $test_type $mcs $sl_mode $syncref_host_name
    sleep 1
    run_nearby_cmd  $test_type $mcs $sl_mode $nearby_host_name

    local wait_start=$(date +%s)
    wait_for_tun_interface $src_if $syncref_host_name $duration
    local remaining=$(( duration - $(date +%s) + wait_start ))
    if [[ $remaining -gt 0 ]]; then
        wait_for_pc5_sync $remaining
    fi

    # Verify connectivity with ping before iperf3
    if ! verify_ping "$syncref_host_name" "$src_if" "$dest_ip" 5; then
        echo "SKIP: iperf3 test skipped due to ping failure"
        LAST_TEST_RESULT="FAIL"
        kill_all $syncref_host_name nr-uesoftmodem
        kill_all $nearby_host_name nr-uesoftmodem
        save_softmodem_logs "${test_name}"
        local end_time=$(date +%s)
        print_runtime $start_time $end_time
        return 1
    fi

    # Run iperf3 sweep: server on syncref, client on nearby
    evaluate_iperf3_sweep "$syncref_host_name" "$server_ip" "$nearby_host_name" "$client_ip" \
        $sl_mode "$test_name" "$iteration" "$num_hosts" "$mcs"

    # Cleanup
    kill_all $syncref_host_name nr-uesoftmodem
    kill_all $nearby_host_name nr-uesoftmodem
    kill_all $syncref_host_name iperf3
    kill_all $nearby_host_name iperf3
    save_softmodem_logs "${test_name}"

    local end_time=$(date +%s)
    print_runtime $start_time $end_time
}

#############################################################
rfsim_pc5_iperf3_test_on_local_host() {
#############################################################
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && iteration=$3
    local test_type="rfsim"
    local syncref_host_name="local"
    local nearby_host_name="local"
    local num_hosts=1
    pc5_iperf3_test $duration $test_type $mcs $iteration $syncref_host_name $nearby_host_name $num_hosts "${FUNCNAME[0]}"
}
#############################################################
vrtsim_pc5_iperf3_test_on_local_host() {
#############################################################
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && iteration=$3
    local test_type="vrtsim"
    local syncref_host_name="local"
    local nearby_host_name="local"
    local num_hosts=1
    pc5_iperf3_test $duration $test_type $mcs $iteration $syncref_host_name $nearby_host_name $num_hosts "${FUNCNAME[0]}"
}
#############################################################
rfsim_pc5_iperf3_test_on_two_hosts() {
#############################################################
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && iteration=$3
    local test_type="rfsim"
    local syncref_host_name="local"
    local nearby_host_name=$REMOTE_UE_HOST
    local num_hosts=2
    pc5_iperf3_test $duration $test_type $mcs $iteration $syncref_host_name $nearby_host_name $num_hosts "${FUNCNAME[0]}"
}
#############################################################
usrp_B210_pc5_iperf3_test_on_two_hosts() {
#############################################################
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && iteration=$3
    local test_type="usrp"
    local syncref_host_name="local"
    local nearby_host_name=$REMOTE_UE_HOST
    local num_hosts=2
    pc5_iperf3_test $duration $test_type $mcs $iteration $syncref_host_name $nearby_host_name $num_hosts "${FUNCNAME[0]}"
}

slmode1_srap_iperf3_test() {
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && test_type=$2
    [[ $# -ge 3 ]] && mcs=$3
    [[ $# -ge 4 ]] && iteration=$4
    [[ $# -ge 5 ]] && gnb_host_name=$5
    [[ $# -ge 6 ]] && syncref_host_name=$6
    [[ $# -ge 7 ]] && nearby_host_name=$7
    [[ $# -ge 8 ]] && num_hosts=$8
    [[ $# -ge 9 ]] && test_name=$9 || test_name="${FUNCNAME[0]}"

    # Validate test type for local host execution
    if [[ $num_hosts -eq 1 ]]; then
        validate_test_type_for_local_host $test_type $test_name || return 1
    fi
    cleanup_old_logs

    local start_time=$(date +%s)
    local sl_mode=1
    local src_if="oaitun_ue2"
    local dest_ip="8.8.8.8"
    local server_ip="192.168.70.134"
    # SL Mode 1 relay: the Remote UE re-registers with the core and its IP changes (no longer the
    # fixed 10.0.0.100). Bind the iperf3 client and the pre-iperf3 ping to the interface name
    # instead of a stale IP; the runners detect a non-IP bind and use -I / --bind-dev.
    local client_ip="oaitun_ue2"

    pre1='docker ps | grep oai-upf | wc -l'
    act1='echo "core network is required !!!"; cd ~/oai-cn5g; systemctl start docker.service; docker compose up -d; sleep 2'
    [[ $(eval "$pre1") -eq 1 ]] && echo "Requirements are satisfied !!!" || eval "$act1"

    if [[ $syncref_host_name != "local" ]]; then
        sync_config_files $syncref_host_name
    fi
    if [[ $nearby_host_name != "local" ]]; then
        sync_config_files $nearby_host_name
    fi

    echo "========== Test: $test_name ==========" >> "$log_dir/commands.txt"

    # vrtsim (shared-memory radio) launch SEQUENCE + timing are critical: bring up the
    # servers first and give each stage time to settle. gNB (Uu server) -> Remote UE
    # (PC5 server) -> Relay UE (Uu+PC5 client). The relay must complete Uu registration
    # and PC5 bring-up so the Remote UE finishes its Core registration (transitioning
    # from the initial demo IP to its Core-assigned IP) BEFORE the ping runs.
    if [[ $test_type == "vrtsim" ]]; then
        run_gNB_cmd $test_type $sl_mode $gnb_host_name
        sleep 5
        run_nearby_cmd  $test_type $mcs $sl_mode $nearby_host_name
        sleep 5
        run_syncref_cmd $test_type $mcs $sl_mode $syncref_host_name
        sleep 5
    else
        run_gNB_cmd $test_type $sl_mode $gnb_host_name
        sleep 1
        run_nearby_cmd  $test_type $mcs $sl_mode $nearby_host_name
        sleep 1
        run_syncref_cmd $test_type $mcs $sl_mode $syncref_host_name
    fi

    local wait_start=$(date +%s)
    wait_for_tun_interface $src_if $nearby_host_name $duration
    local remaining=$(( duration - $(date +%s) + wait_start ))
    if [[ $remaining -gt 0 ]]; then
        wait_for_pc5_sync $remaining
    fi

    # Wait for end-to-end connectivity through relay (retry ping with timeout)
    echo "Waiting for end-to-end connectivity through relay..."
    local ping_attempts=0
    local max_ping_attempts=30
    local ping_success=0
    while [ $ping_attempts -lt $max_ping_attempts ]; do
        if verify_ping "$nearby_host_name" "$src_if" "$dest_ip" 3; then
            ping_success=1
            break
        fi
        echo "Ping attempt $((ping_attempts + 1))/$max_ping_attempts failed, retrying in 2s..."
        sleep 2
        ping_attempts=$((ping_attempts + 1))
    done

    if [ $ping_success -eq 0 ]; then
        echo "SKIP: iperf3 test skipped due to ping failure after $max_ping_attempts attempts"
        LAST_TEST_RESULT="FAIL"
        kill_all $nearby_host_name nr-uesoftmodem
        kill_all $syncref_host_name nr-uesoftmodem
        kill_all $gnb_host_name nr-softmodem
        save_softmodem_logs "${test_name}"
        local end_time=$(date +%s)
        print_runtime $start_time $end_time
        return 1
    fi

    echo "End-to-end connectivity established successfully!"

    # Run iperf3 sweep: server on UPF docker, client on nearby UE
    evaluate_iperf3_sweep "upf_docker" "$server_ip" "$nearby_host_name" "$client_ip" \
        $sl_mode "$test_name" "$iteration" "$num_hosts" "$mcs"

    # Cleanup
    kill_all $nearby_host_name nr-uesoftmodem
    kill_all $syncref_host_name nr-uesoftmodem
    kill_all $gnb_host_name nr-softmodem
    kill_all $nearby_host_name iperf3
    kill_all "local" iperf3
    save_softmodem_logs "${test_name}"

    local end_time=$(date +%s)
    print_runtime $start_time $end_time
}
#############################################################
rfsim_slmode1_srap_iperf3_test_on_local_host() {
#############################################################
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && iteration=$3
    local test_type="rfsim"
    local gnb_host_name="local"
    local syncref_host_name="local"
    local nearby_host_name="local"
    local num_hosts=1
    slmode1_srap_iperf3_test $duration $test_type $mcs $iteration $gnb_host_name $syncref_host_name $nearby_host_name $num_hosts "${FUNCNAME[0]}"
}
#############################################################
vrtsim_slmode1_srap_iperf3_test_on_local_host() {
#############################################################
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && iteration=$3
    local test_type="vrtsim"
    local gnb_host_name="local"
    local syncref_host_name="local"
    local nearby_host_name="local"
    local num_hosts=1
    slmode1_srap_iperf3_test $duration $test_type $mcs $iteration $gnb_host_name $syncref_host_name $nearby_host_name $num_hosts "${FUNCNAME[0]}"
}
#############################################################
rfsim_slmode1_srap_iperf3_test_on_three_hosts() {
#############################################################
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && iteration=$3
    local test_type="rfsim"
    local gnb_host_name="local"
    local syncref_host_name=$RELAY_UE_HOST
    local nearby_host_name=$REMOTE_UE_HOST
    local num_hosts=3
    slmode1_srap_iperf3_test $duration $test_type $mcs $iteration $gnb_host_name $syncref_host_name $nearby_host_name $num_hosts "${FUNCNAME[0]}"
}
#############################################################
usrp_B210_slmode1_srap_iperf3_test_on_three_hosts() {
#############################################################
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && iteration=$3
    local test_type="usrp"
    local gnb_host_name="local"
    local syncref_host_name=$RELAY_UE_HOST
    local nearby_host_name=$REMOTE_UE_HOST
    local num_hosts=3
    slmode1_srap_iperf3_test $duration $test_type $mcs $iteration $gnb_host_name $syncref_host_name $nearby_host_name $num_hosts "${FUNCNAME[0]}"
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

        # Get test-specific configuration (four-tier resolution)
        local test_mcs_array=($(get_test_mcs_array "$test_name"))
        local test_duration=$(get_test_duration "$test_name")

        # Print configuration info if verbose mode is enabled
        if [[ "$verbose_config" == "1" ]]; then
            print_test_config_info "$test_name"
        else
            # Brief config summary
            echo "Running $test_name with MCS=[${test_mcs_array[@]}], duration=${test_duration}s"
        fi

        # Determine test type and parameter array based on test name
        # Check for BLER first (before rfsim_* check, since BLER names contain "rfsim")
        if [[ $test_name == *"bler"* ]]; then
            # BLER tests: use the noise sweep + iteration range.
            # vrtsim uses its own noise scale, so pick vrtsim_noise_power_array for
            # vrtsim_*bler* tests when it is defined; otherwise fall back to the
            # (rfsim) noise_power_array.
            if [[ $test_name == vrtsim_* && -n "${vrtsim_noise_power_array+x}" ]]; then
                param_array=("${vrtsim_noise_power_array[@]}")
            else
                param_array=("${noise_power_array[@]}")
            fi
            is_bler=true
            iteration_start_val=${iteration_start:-1}
            iteration_end_val=${iteration_end:-$num_repeat}
        elif [[ $test_name == rfsim_* ]]; then
            # RFsim tests: use snr_array
            param_array=("${snr_array[@]}")
            is_bler=false
            iteration_start_val=1
            iteration_end_val=$num_repeat
        elif [[ $test_name == vrtsim_* ]]; then
            # vrtsim tests (shared-memory radio, local host): no SNR sweep, run once per iteration
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
                    for mcs in ${test_mcs_array[@]}; do  # Use test-specific MCS
                        # Call BLER test with noise_power as 4th parameter
                        $test_name $test_duration $mcs $k $param  # Use test-specific duration
                        sleep 3
                    done
                done
            done
        else
            # Standard tests loop order: param -> iteration -> MCS
            for param in "${param_array[@]}"; do
                for k in $(seq $iteration_start_val 1 $iteration_end_val); do
                    for mcs in ${test_mcs_array[@]}; do  # Use test-specific MCS
                        # CSI/PSFCH tests need special handling
                        if [[ $test_name == *"csi_acquisition_psfch"* ]]; then
                            if [[ -n "$csi_param" && -n "$psfch_param" ]]; then
                                # Run specific CSI/PSFCH combination
                                $test_name $csi_param $psfch_param $test_duration $mcs $k
                            elif [[ -n "$csi_param" && -z "$psfch_param" ]]; then
                                # Run specific CSI with all PSFCH values (e.g., test:1:)
                                for psfch in 0 1 2 3; do
                                    $test_name $csi_param $psfch $test_duration $mcs $k
                                done
                            elif [[ -z "$csi_param" && -n "$psfch_param" ]]; then
                                # Run specific PSFCH with all CSI values (e.g., test::1)
                                for csi in 0 1; do
                                    $test_name $csi $psfch_param $test_duration $mcs $k
                                done
                            else
                                # Run all 8 combinations
                                $test_name 0 0 $test_duration $mcs $k
                                $test_name 0 1 $test_duration $mcs $k
                                $test_name 0 2 $test_duration $mcs $k
                                $test_name 0 3 $test_duration $mcs $k
                                $test_name 1 0 $test_duration $mcs $k
                                $test_name 1 1 $test_duration $mcs $k
                                $test_name 1 2 $test_duration $mcs $k
                                $test_name 1 3 $test_duration $mcs $k
                            fi
                        else
                            # All other tests: just call with standard parameters
                            $test_name $test_duration $mcs $k  # Use test-specific duration
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

    local iperf3_csv
    iperf3_csv=$(find "$log_dir" -maxdepth 1 -name 'iperf3_summary_*.csv' 2>/dev/null | head -1)

    if [ -n "$iperf3_csv" ]; then
        echo "iperf3 Summary: $iperf3_csv"
        echo ""
        echo "iperf3 Test Results:"
        cat "$iperf3_csv"
        echo ""
    elif [ -f "$test_summary_file" ]; then
        echo "Summary saved to: $test_summary_file"
        echo ""
        echo "All Test Results:"
        cat "$test_summary_file"
        echo ""
    else
        echo "No tests were executed (check test flags and num_hosts configuration)"
        echo ""
    fi

    # For a local BLER sweep, automatically post-process the results once the full
    # sweep is done (extract BLER/LDPC CSVs + plots). Distributed/parallel runs exit
    # earlier (the coordinator does not reach this point), so this only fires locally.
    if [[ "$test_profile" == "bler" ]]; then
        local bler_processor="$SCRIPT_DIR/bler_test/process_and_fetch_results.sh"
        if [[ -x "$bler_processor" ]]; then
            echo ""
            echo "=========================================="
            echo "Post-processing BLER results"
            echo "=========================================="
            bash "$bler_processor"
        else
            echo "WARNING: BLER post-processor not found or not executable: $bler_processor"
        fi
    fi
}
main
