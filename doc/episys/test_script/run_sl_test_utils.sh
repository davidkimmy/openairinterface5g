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
    # Expects: bler_hosts, num_repeat from config
    local num_hosts=${#bler_hosts[@]}
    local total_iterations=${num_repeat:-10}
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
    local config_file="$SCRIPT_DIR/run_sl_test_config.sh"

    # Update iteration parameters in config
    sed -i "s/^iteration_start=.*/iteration_start=$start/" "$config_file"
    sed -i "s/^iteration_end=.*/iteration_end=$end/" "$config_file"

    # Calculate num_repeat from range
    local num_repeat=$((end - start + 1))
    # Find and update num_repeat in bler profile section only
    sed -i "/test_profile == \"bler\"/,/^elif/s/^\([[:space:]]*num_repeat=\)[0-9]\+/\1$num_repeat/" "$config_file"
}

# Update remote host config with iteration range
# Usage: update_host_config_remote <hostname> <start> <end>
update_host_config_remote() {
    local hostname=$1
    local start=$2
    local end=$3
    local num_repeat=$((end - start + 1))

    ssh "$hostname" "bash -c '
        config_file=\"\$HOME/ci_script/run_sl_test_config.sh\"

        # Create backup
        cp \"\$config_file\" \"\${config_file}.bak\"

        # Update iteration parameters
        sed -i \"s/^iteration_start=.*/iteration_start=$start/\" \"\$config_file\"
        sed -i \"s/^iteration_end=.*/iteration_end=$end/\" \"\$config_file\"

        # Update num_repeat in bler profile section only
        sed -i \"/test_profile == \\\"bler\\\"/,/^elif/s/^\([[:space:]]*num_repeat=\)[0-9]\+/\1$num_repeat/\" \"\$config_file\"

        echo \"  ✓ Updated config on $hostname\"
    '" 2>/dev/null || echo "  ⚠ Failed to update config on ${hostname}"
}
