#!/bin/bash
#############################################################
# Utility Functions for run_sl_test.sh
# This file contains all helper functions used by the test framework
#############################################################

#############################################################
# Uu Interface MCS Configuration Functions
#############################################################
# Two approaches available:
# 1. Config file modification (current approach - reliable)
# 2. Command-line parameters --MACRLCs.[0].dl_max_mcs (backup if command-line override works)

# Set fixed MCS for Uu interface in gNB configuration files
# Usage: set_uu_mcs <mcs_value>
set_uu_mcs() {
    local mcs=$1

    # Validate MCS range
    if [ "$mcs" -lt 0 ] || [ "$mcs" -gt 28 ]; then
        echo "Error: MCS value must be between 0 and 28"
        return 1
    fi

    # Configuration files to update (use variables defined in config)
    local conf_files=(
        "$GNB_CONF_USRP"
        "$GNB_CONF_RELAY"
    )

    for conf_file in "${conf_files[@]}"; do
        if [ -f "$conf_file" ]; then
            # Update DL MCS
            sed -i "s/^\([[:space:]]*dl_max_mcs[[:space:]]*=\)[[:space:]]*[0-9]\+/\1 $mcs/" "$conf_file"
            # Update UL MCS
            sed -i "s/^\([[:space:]]*ul_max_mcs[[:space:]]*=\)[[:space:]]*[0-9]\+/\1 $mcs/" "$conf_file"
            # Update min_grant_mcs (critical for UL MCS > 9)
            sed -i "s/^\([[:space:]]*min_grant_mcs[[:space:]]*=\)[[:space:]]*[0-9]\+/\1 $mcs/" "$conf_file"
        fi
    done

    return 0
}

# Set MCS on remote hosts via SSH
# Usage: set_uu_mcs_remote <hostname> <mcs_value>
set_uu_mcs_remote() {
    local hostname=$1
    local mcs=$2

    if [[ "$hostname" == "localhost" || "$hostname" == "local" ]]; then
        set_uu_mcs "$mcs"
    else
        # Execute on remote host - paths are expanded on remote side
        ssh "$hostname" "bash -c '
            mcs=$mcs
            if [ \"\$mcs\" -lt 0 ] || [ \"\$mcs\" -gt 28 ]; then
                echo \"Error: MCS value must be between 0 and 28\"
                exit 1
            fi

            # Config files on remote host (using standard paths)
            conf_files=(
                \"\$HOME/openairinterface5g/targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band78.fr1.106PRB.usrpb210.conf\"
                \"\$HOME/openairinterface5g/targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band78.fr1.106PRB.usrpb210_relay_ue.conf\"
            )

            for conf_file in \"\${conf_files[@]}\"; do
                if [ -f \"\$conf_file\" ]; then
                    sed -i \"s/^\([[:space:]]*dl_max_mcs[[:space:]]*=\)[[:space:]]*[0-9]\+/\1 \$mcs/\" \"\$conf_file\"
                    sed -i \"s/^\([[:space:]]*ul_max_mcs[[:space:]]*=\)[[:space:]]*[0-9]\+/\1 \$mcs/\" \"\$conf_file\"
                    sed -i \"s/^\([[:space:]]*min_grant_mcs[[:space:]]*=\)[[:space:]]*[0-9]\+/\1 \$mcs/\" \"\$conf_file\"
                fi
            done
        '" 2>/dev/null
    fi

    return 0
}

#############################################################
# Parallel BLER Test Setup Functions
#############################################################
# Deploy configuration and utilities to remote hosts for parallel BLER testing
# Usage: setup_parallel_bler_hosts
setup_parallel_bler_hosts() {
    # Expects: bler_hosts, iteration_start, iteration_end from config
    local num_hosts=${#bler_hosts[@]}
    # Calculate total iterations from iteration_start and iteration_end (not from num_repeat)
    # Fallback to num_repeat if iteration_start/end not defined
    local iter_start=${iteration_start:-1}
    local iter_end=${iteration_end:-${num_repeat:-10}}
    local total_iterations=$((iter_end - iter_start + 1))
    if [[ $total_iterations -le 0 ]]; then
        echo "ERROR: Invalid iteration range: start=$iter_start, end=$iter_end"
        echo "iteration_end must be >= iteration_start"
        return 1
    fi

    local iterations_per_host=$((total_iterations / num_hosts))
    local remainder=$((total_iterations % num_hosts))

    echo "=========================================="
    echo "Setting up parallel BLER test hosts"
    echo "=========================================="
    echo "Total iterations: $total_iterations"
    echo "Per host: $iterations_per_host"
    echo ""

    local host_idx=0
    for hostname in "${bler_hosts[@]}"; do
        host_idx=$((host_idx + 1))

        # Calculate iteration range for this host
        local start=$(( (host_idx - 1) * iterations_per_host + 1 ))
        local end=$(( host_idx * iterations_per_host ))
        [[ $host_idx -eq $num_hosts ]] && end=$((end + remainder))

        echo "→ Host ${host_idx} (${hostname}): iterations ${start}-${end}"

        if [[ "$hostname" == "localhost" || "$hostname" == "local" ]]; then
            # Update local config
            update_host_config_local "$start" "$end"
        else
            # Copy utilities to remote host
            scp -q "$SCRIPT_DIR/run_sl_test_utils.sh" "${hostname}:~/ci_script/" 2>/dev/null || {
                echo "  ⚠ Failed to copy utilities to ${hostname}"
                continue
            }

            # Update remote config
            update_host_config_remote "$hostname" "$start" "$end"
        fi
    done
    echo ""
}

# Update local host config with iteration range
# Usage: update_host_config_local <start> <end>
update_host_config_local() {
    local start=$1
    local end=$2
    local source_config="$SCRIPT_DIR/run_sl_test_config.sh"
    local worker_config="$SCRIPT_DIR/run_sl_test_config_worker_local.sh"

    # Create worker-specific config from master config
    cp "$source_config" "$worker_config"

    # Update iteration parameters in worker config
    sed -i "s/^[[:space:]]*iteration_start=.*/    iteration_start=$start/" "$worker_config"
    sed -i "s/^[[:space:]]*iteration_end=.*/    iteration_end=$end/" "$worker_config"

    # Calculate num_repeat from range
    local num_repeat=$((end - start + 1))
    # Find and update num_repeat in bler profile section only
    sed -i "/test_profile == \"bler\"/,/^elif/s/^\([[:space:]]*num_repeat=\)[0-9]\+/\1$num_repeat/" "$worker_config"

    # CRITICAL: Disable parallel mode to prevent recursive launches
    sed -i 's/^[[:space:]]*parallel_mode=.*/    parallel_mode="false"/' "$worker_config"
}

# Update remote host config with iteration range
# Usage: update_host_config_remote <hostname> <start> <end>
update_host_config_remote() {
    local hostname=$1
    local start=$2
    local end=$3
    local num_repeat=$((end - start + 1))

    ssh "$hostname" "bash -c '
        source_config=\"\$HOME/ci_script/run_sl_test_config.sh\"
        worker_config=\"\$HOME/ci_script/run_sl_test_config_worker_${hostname}.sh\"

        # Create worker-specific config from master config
        cp \"\$source_config\" \"\$worker_config\"

        # Update iteration parameters in worker config
        sed -i \"s/^[[:space:]]*iteration_start=.*/    iteration_start=$start/\" \"\$worker_config\"
        sed -i \"s/^[[:space:]]*iteration_end=.*/    iteration_end=$end/\" \"\$worker_config\"

        # Update num_repeat in bler profile section only
        sed -i \"/test_profile == \\\"bler\\\"/,/^elif/s/^\([[:space:]]*num_repeat=\)[0-9]\+/\1$num_repeat/\" \"\$worker_config\"

        # CRITICAL: Disable parallel mode to prevent recursive launches
        sed -i \"s/^[[:space:]]*parallel_mode=.*/    parallel_mode=\\\"false\\\"/\" \"\$worker_config\"

        echo \"  ✓ Updated config on $hostname\"
    '" 2>/dev/null || echo "  ⚠ Failed to update config on ${hostname}"
}
