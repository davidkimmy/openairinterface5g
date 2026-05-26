#!/bin/bash
#############################################################
# Configuration file for run_sl_test.sh
# Define test profiles and their parameters
#############################################################

#############################################################
# System-Specific Configuration
#############################################################
# Extended delays for slower systems or specific test environments
# Set to 1 if your system needs additional time for:
#   - TUN interface initialization
#   - Sidelink synchronization stabilization
# Most users should keep this at 0 (default: disabled)
use_extended_delays=0

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

# USRP serial numbers for the SL mode 1 relay test.
RELAY_UE_USRP_SN_FOR_UU=340EA03
RELAY_UE_USRP_SN_FOR_SL=340EA3B

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
slmode1_basic_tests=(
    rfsim_slmode1_srap_ping_test_on_local_host
    rfsim_slmode1_srap_ping_test_on_three_hosts
    usrp_B210_slmode1_srap_ping_test_on_three_hosts
)
regress_tests=(
    uu_basic_tests
    slmode1_basic_tests[0]
    slmode2_basic_tests
    slmode2_csi_psfch_tests
)
stress_tests=(
    slmode2_basic_tests[1:2]
)
pilot_tests=(
    #slmode1_basic_tests[2]
    slmode2_basic_tests[2]
    #slmode2_csi_psfch_tests[2]
)
# Base directory for logs. The default will be this script folder.
base_log_dir="~/openairinterface5g"
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
    mcs_array=(15)
    duration=30
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
    mcs_array=($(seq 9 6 15))  # [start, step, end] # (9 15)
    duration=30
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
    duration=85               # 85 seconds per test
    max_ldpc_iterations=30    # LDPC iterations for BLER testing

    # Noise power sweep: -12 to 4 dB with step of 1 (17 levels)
    # Assuming: TX_power ~20 dBm, ploss 8 dB
    # SINR = TX_power - ploss - noise_power = 20 - 8 - noise_power
    # -12 dB noise → 24 dB SINR (high end, 64QAM)
    # +4 dB noise → 8 dB SINR (low end, all modulations reach 100% BLER)
    noise_power_array=($(seq -12 4))

    ploss_db=8                # Fixed path loss (8 dB)
    csi_acquisition=0         # Disable CSI
    psfch_period=2            # PSFCH Period = 2

    # Ping configuration (15 pkt/s for 85 seconds = 1275 packets)
    ping_count=1275
    ping_interval=0.0667      # 1/15 seconds ≈ 66.7ms

    # Optimization mode
    bler_optimization="parallel_mcs"  # Keep processes running across MCS

    # Distributed BLER test machine configuration
    # Enable parallel mode to distribute tests across multiple machines
    parallel_mode="true"

    # List machines for parallel testing. Iterations are split evenly across machines.
    # Format: hostname (use "localhost" or "local" for local execution)
    # Current setup: 4 machines, 12 total iterations → 3 iterations per machine
    bler_hosts=(l3 l4 l5 localhost)
    # For 2 machines (6 iterations each): bler_hosts=(l3 localhost)
    # For 1 machine (all 12 iterations): bler_hosts=(localhost)
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
    echo "Optimization         : ${bler_optimization}"
fi
echo "=========================================="
echo ""
