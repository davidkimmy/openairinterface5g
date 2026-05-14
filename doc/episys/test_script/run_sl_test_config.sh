#!/bin/bash
#############################################################
# Configuration file for run_sl_test.sh
# Define test profiles and their parameters
#############################################################
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
RELAY_UE_USRP_SN_FOR_SL=3271246
#RELAY_UE_USRP_SN_FOR_SL=340E9F3
#RELAY_UE_USRP_SN_FOR_SL=340EA3B

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
    slmode2_basic_tests[1:2]
)
# Base directory for logs. The default will be this script folder.
base_log_dir="~/openairinterface5g"
use_external_clock=0
use_gnome=0
use_sa=1
# Select active test profile among: pilot, regress, stress
test_profile='pilot'
#############################################################
# Test Profile Configuration
#############################################################
if [[ $test_profile == "pilot" ]]; then
    enabled_tests=("${pilot_tests[@]}")
    num_repeat=1
    mcs_array=(13)
    duration=20
    max_ldpc_iterations=30
    # RFSIM parameters (SNR values)
    snr_array=($(seq 0 1 0))  # [start, step, end]
    # USRP parameters (attenuation values in dB)
    atten_array=(20)
    # USRP TX/RX gain
    tx_gain=20
    rx_gain=110
elif [[ $test_profile == "regress" ]]; then
    enabled_tests=("${regress_tests[@]}")
    num_repeat=1
    mcs_array=(9 10 11 12 13)
    duration=30
    max_ldpc_iterations=30
    # RFSIM parameters (SNR values)
    snr_array=($(seq 0 1 0))  # [start, step, end]
    # USRP parameters (attenuation values in dB)
    atten_array=(20)
    # USRP TX/RX gain
    tx_gain=20
    rx_gain=110
elif [[ $test_profile == "stress" ]]; then
    enabled_tests=("${stress_tests[@]}")
    num_repeat=3
    mcs_array=(9 13)
    duration=300
    max_ldpc_iterations=30
    # RFSIM parameters (SNR values)
    snr_array=($(seq 0 1 0))  # [start, step, end]
    # USRP parameters (attenuation values in dB)
    atten_array=(20 30 40 50 55 60)
    # USRP TX/RX gain
    tx_gain=20
    rx_gain=110
else
    echo "ERROR: Unknown test profile '$test_profile'"
    echo "Available profiles: pilot, regress, stress"
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
echo "=========================================="
echo ""
