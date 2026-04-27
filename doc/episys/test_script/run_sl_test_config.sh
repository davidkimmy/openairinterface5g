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
# You can optionally specify test cases or test range as following example:
# enabled_tests=(
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
slmode1_basic_tests=(
    rfsim_slmode1_srap_ping_test_on_local_host
    rfsim_slmode1_srap_ping_test_on_three_hosts
    usrp_B210_slmode1_srap_ping_test_on_three_hosts
)
enabled_tests=(
    # uu_basic_tests[0]
    slmode2_basic_tests[2]
    # slmode2_csi_psfch_tests[1]
    # slmode1_basic_tests[k]
    # rfsim_pc5_csi_acquisition_psfch_period_test_on_local_host:0:0
    # usrp_B210_pc5_csi_acquisition_psfch_period_test_on_two_hosts:0:0
    ######   Enable the following lines for the regression test. ######
    # uu_basic_tests
    # slmode1_basic_tests
    # slmode2_csi_psfch_tests
)

# Select active test profile among: pilot, regress, stress
test_profile='pilot'
#############################################################
# Test Profile Configuration
#############################################################
if [[ $test_profile == "pilot" ]]; then
    num_repeat=1
    mcs_array=(1)
    duration=30
    # RFSIM parameters (SNR values)
    snr_array=($(seq 0 1 0))  # [start, step, end]
    # USRP parameters (attenuation values in dB)
    atten_array=(20)
    # USRP TX/RX gain
    tx_gain=20
    rx_gain=110

elif [[ $test_profile == "regress" ]]; then
    num_repeat=1
    mcs_array=(1 9)
    duration=30
    # RFSIM parameters (SNR values)
    snr_array=($(seq 0 1 0))  # [start, step, end]
    # USRP parameters (attenuation values in dB)
    atten_array=(20)
    # USRP TX/RX gain
    tx_gain=20
    rx_gain=110

elif [[ $test_profile == "stress" ]]; then
    num_repeat=3
    mcs_array=(9 16 28)
    duration=300
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