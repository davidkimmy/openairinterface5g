#!/bin/bash
#############################################################
# Configuration file for run_sl_test.sh
# Define test profiles and their parameters
#############################################################

#############################################################
# Paths and Directory Configuration
#############################################################
# OAI base directory
export OAI_BASE_DIR="$HOME/openairinterface5g"

# Relative path from $HOME to OAI base (for remote host usage)
export OAI_BASE_REL_PATH="openairinterface5g"

# Relative paths from OAI base directory
export BUILD_REL_PATH="cmake_targets/ran_build/build"
export CONF_REL_PATH="targets/PROJECTS/NR-SIDELINK/CONF"
export GNB_CONF_REL_PATH="targets/PROJECTS/GENERIC-NR-5GC/CONF"

# Absolute paths (derived from base + relative paths)
export OAI_BUILD_DIR="$OAI_BASE_DIR/$BUILD_REL_PATH"
export CONF_PATH="$OAI_BASE_DIR/$CONF_REL_PATH"
export GNB_CONF_DIR="$OAI_BASE_DIR/$GNB_CONF_REL_PATH"

# BLER test results directory pattern
BLER_RESULTS_DIR="$OAI_BASE_DIR/bler_results"

# Core network directory (for docker compose)
CN_DIR="$HOME/oai-cn5g"

# Configuration files for different scenarios
export GNB_CONF_USRP="$GNB_CONF_DIR/gnb.sa.band78.fr1.106PRB.usrpb210.conf"
export GNB_CONF_RELAY="$GNB_CONF_DIR/gnb.sa.band78.fr1.106PRB.usrpb210_relay_ue.conf"

#############################################################
# Hardware Configuration (USRP)
#############################################################
# Default TX/RX gain for USRP tests
TX_GAIN=0      # Default: 0 dB (can be overridden per profile)
RX_GAIN=110    # Default: 110 dB (can be overridden per profile)

# USRP serial numbers for SL mode 1 relay tests
RELAY_UE_USRP_SN_FOR_UU="340EA03"
RELAY_UE_USRP_SN_FOR_SL="340EA3B"

#############################################################
# Default Configuration Values
#############################################################
# These values are read from config files but can be overridden here
# Leave empty to auto-detect from configuration files
DEFAULT_CSI_ACQ=""        # Auto-detect from sl_sync_ref.conf if empty
DEFAULT_PSFCH_PERIOD=""   # Auto-detect from sl_sync_ref.conf if empty

#############################################################
# Test Case Selection
#############################################################
# List tests to run in the order you want them executed.
# Tests will run in the order they appear in this array.
# Use exact function names from the list below.
#
# For CSI/PSFCH tests, you can optionally specify parameters:
#   test_name:csi_acq:psfch_period
# Examples:
#   rfsim_pc5_csi_acquisition_psfch_period_test_on_two_hosts       (runs all 8 combinations)
#   rfsim_pc5_csi_acquisition_psfch_period_test_on_two_hosts:0:1   (runs only csi=0, psfch=1)
#   rfsim_pc5_csi_acquisition_psfch_period_test_on_two_hosts:1:    (runs csi=1 for all psfch: 0,1,2,3)
#   rfsim_pc5_csi_acquisition_psfch_period_test_on_two_hosts::2    (runs psfch=2 for all csi: 0,1)
#
# Each profile has its own test list: pilot_tests, regress_tests, stress_tests.
# The active profile's list is assigned to enabled_tests automatically.
# You can optionally specify test cases or test range as following example:
# pilot_tests=(
#     slmode2_basic_tests            # entire group
#     slmode2_basic_tests[0:2]       # range: indices 0,1,2 (inclusive)
#     slmode2_basic_tests[0,2]       # pick: indices 0 and 2
#     rfsim_pc5_ping_test_on_two_hosts   # individual test
#     rfsim_pc5_csi_acquisition_psfch_period_test_on_two_hosts:0:1
# )

uu_basic_tests=(
    rfsim_uu_ping_test_on_local_host
    rfsim_uu_ping_test_on_two_hosts
    usrp_B210_uu_ping_test_on_two_hosts
)
slmode2_basic_tests=(
    rfsim_pc5_ping_test_on_local_host
    rfsim_pc5_ping_test_on_two_hosts
    usrp_B210_pc5_ping_test_on_two_hosts
)
slmode2_csi_psfch_tests=( # Each item includes 8 sub-test cases.
    rfsim_pc5_csi_acquisition_psfch_period_test_on_local_host
    rfsim_pc5_csi_acquisition_psfch_period_test_on_two_hosts
    usrp_B210_pc5_csi_acquisition_psfch_period_test_on_two_hosts
)
slmode2_iperf3_tests=(
    rfsim_pc5_iperf3_test_on_local_host
    rfsim_pc5_iperf3_test_on_two_hosts
    usrp_B210_pc5_iperf3_test_on_two_hosts
)
slmode1_basic_tests=(
    rfsim_slmode1_srap_ping_test_on_local_host
    rfsim_slmode1_srap_ping_test_on_three_hosts
    usrp_B210_slmode1_srap_ping_test_on_three_hosts
)
slmode1_iperf3_tests=(
    rfsim_slmode1_srap_iperf3_test_on_local_host
    rfsim_slmode1_srap_iperf3_test_on_three_hosts
    usrp_B210_slmode1_srap_iperf3_test_on_three_hosts
)
regress_tests=(
    uu_basic_tests
    slmode1_basic_tests
    slmode2_basic_tests
    slmode2_csi_psfch_tests
    slmode1_iperf3_tests
    slmode2_iperf3_tests
)
stress_tests=(
    slmode2_basic_tests[1:2]
)
pilot_tests=(
    slmode1_basic_tests[2]
    #slmode2_basic_tests[2]
    #slmode2_csi_psfch_tests[2]
    #slmode1_iperf3_tests[2]
    #slmode2_iperf3_tests[2]
)

# Set to 1 if your system needs additional time for:
#   - TUN interface initialization
#   - Sidelink synchronization stabilization
use_extended_delays=0 # (0 default: disabled)

# iperf3 test parameters
iperf3_bw_array=(1M 2M 3M 4M 5M 6M 7M 8M 9M 10M)
iperf3_port=5001
iperf3_run_duration=10  # seconds per bandwidth step

# Base directory for logs. The default will be this script folder.
base_log_dir="$HOME/openairinterface5g"
use_external_clock=0
use_gnome=0
use_sa=1
ensure_ping_test_time=1 # Adding 16 seconds to ensure ping test if needed.
# Select active test profile among: pilot, regress, stress, bler
test_profile='pilot'
#############################################################
# Test Profile Configuration
#############################################################
if [[ $test_profile == "pilot" ]]; then
    enabled_tests=("${pilot_tests[@]}")
    num_repeat=1
    mcs_array=(9)
    duration=30
    [[ "$use_extended_delays" == "1" ]] && duration=90
    max_ldpc_iterations=30
    # RFSIM parameters (SNR values)
    # Ignore snr_array unless you have any specific snr values to test
    snr_array=($(seq 0 1 0))  # [start, step, end]
    # USRP parameters (attenuation values in dB)
    # Ignore atten_array unless you have any specific attenuation values to test
    atten_array=(20)  # Removed all attenuation for max link budget
    # USRP TX/RX gain
    tx_gain=20  # 20, 30
    rx_gain=110  # 110, 70
    # BLER test parameters (for noise sweep)
    noise_power_array=(0)  # Single noise level for quick test
    ploss_db=10
elif [[ $test_profile == "regress" ]]; then
    enabled_tests=("${regress_tests[@]}")
    num_repeat=1
    mcs_array=(10) # ($(seq 0 1 10)) # [start, step, end]
    duration=30
    [[ "$use_extended_delays" == "1" ]] && duration=90
    max_ldpc_iterations=30
    # RFSIM parameters (SNR values)
    # Ignore snr_array unless you have any specific snr values to test
    snr_array=($(seq 0 1 0))  # [start, step, end]
    # USRP parameters (attenuation values in dB)
    # Ignore atten_array unless you have any specific attenuation values to test
    atten_array=(20)
    # USRP TX/RX gain
    tx_gain=20  # 20, 30
    rx_gain=110  # 110, 70
elif [[ $test_profile == "stress" ]]; then
    enabled_tests=("${stress_tests[@]}")
    num_repeat=3
    mcs_array=(9 13)
    duration=300
    [[ "$use_extended_delays" == "1" ]] && duration=360
    max_ldpc_iterations=30
    # RFSIM parameters (SNR values)
    # Ignore snr_array unless you have any specific snr values to test
    snr_array=($(seq 0 1 0))  # [start, step, end]
    # USRP parameters (attenuation values in dB)
    # Ignore atten_array unless you have any specific attenuation values to test
    atten_array=(20) # (20 30 40 50 55 60)
    # USRP TX/RX gain
    tx_gain=20  # 20, 30
    rx_gain=110  # 110, 70
elif [[ $test_profile == "bler" ]]; then
    # BLER test configuration
    enabled_tests=(
        rfsim_slmode1_bler_test_on_local_host
    )

    num_repeat=12             # 12 iterations per configuration for statistical validity
    mcs_array=($(seq 0 28))   # MCS 0-28 (all modulation orders, complete waterfall)
    duration=30
    [[ "$use_extended_delays" == "1" ]] && duration=90
    max_ldpc_iterations=30    # LDPC iterations for BLER testing

    # Assuming: TX_power ~20 dBm, ploss 8 dB
    # -12 dB noise → 24 dB SINR (high end, 64QAM)
    # +4 dB noise → 8 dB SINR (low end, all modulations reach 100% BLER)
    noise_power_array=($(seq -12 4))

    ploss_db=8                # Fixed path loss (8 dB)
    csi_acquisition=0         # Disable CSI
    psfch_period=2            # PSFCH Period = 2
    ping_per_second=15        # Ping rate (packets per second)

    # Distributed BLER test machine configuration
    # Enable parallel mode to distribute tests across multiple machines
    parallel_mode="true"

    # List machines for parallel testing. Iterations are split evenly across machines.
    # Format: hostname (use "localhost" or "local" for local execution)
    # Current setup: 4 machines, 12 total iterations → 3 iterations per machine
    bler_hosts=(l3 l4 l5 localhost)
    # For 2 machines (6 iterations each): bler_hosts=(l3 localhost)
    # For 1 machine (all 12 iterations): bler_hosts=(localhost)

    # Default: run all iterations (updated per host during parallel setup)
    iteration_start=1
    iteration_end=12
else
    echo "ERROR: Unknown test profile '$test_profile'"
    echo "Available profiles: pilot, regress, stress, bler"
    exit 1
fi

# Softmodem log file names (used for cleanup and saving to log folder)
softmodem_log_files=(
    result_gNB.log
    result_nrUE.log
    result_syncref.log
    result_nearby.log
    result_nrUE_syncref.log
)

#############################################################
# Display loaded configuration
#############################################################
echo "=========================================="
echo "Loaded Test Profile: $test_profile"
echo "=========================================="
echo "Number of Repeats    : $num_repeat"
echo "MCS Array            : ${mcs_array[@]}"
echo "Duration per Test    : ${duration}s"
echo "SNR Array (RFSIM)    : ${snr_array[@]}"
echo "Atten Array (USRP)   : ${atten_array[@]}"
echo "TX Gain (USRP)       : $tx_gain"
echo "RX Gain (USRP)       : $rx_gain"
echo "Max LDPC Iterations  : $max_ldpc_iterations"
echo "Use --sa flag        : $use_sa"
echo "Use External Clock   : $use_external_clock"
echo "Use GNOME            : $use_gnome"
echo "Ensure Ping Test Time: $ensure_ping_test_time"
if printf '%s\n' "${enabled_tests[@]}" | grep -q "iperf3"; then
    echo "iperf3 BW Array      : ${iperf3_bw_array[@]}"
    echo "iperf3 Port          : $iperf3_port"
    echo "iperf3 Run Duration  : ${iperf3_run_duration}s"
fi
echo "Base Log Directory   : ${base_log_dir:-$SCRIPT_DIR}"
echo ""
echo "Enabled Test Cases:"
if [ ${#enabled_tests[@]} -eq 0 ]; then
    echo "  - None"
else
    for test in "${enabled_tests[@]}"; do
        echo "  - $test"
    done
fi
if [[ $test_profile == "bler" ]]; then
    echo "Noise Power Array    : ${noise_power_array[@]}"
    echo "Path Loss            : ${ploss_db} dB"
    echo "CSI Acquisition      : ${csi_acquisition}"
    echo "PSFCH Period         : ${psfch_period}"
fi
echo "=========================================="
echo ""

