# OpenAirInterface 5G NR Sidelink Test Script

Automated test harness for validating 5G NR PC5 (sidelink) and U2N relay functionality in OpenAirInterface.

## Overview

This bash script provides automated testing for OAI 5G NR sidelink features including:
- PC5 direct device-to-device communication (Mode 1 & Mode 2)
- CSI acquisition and PSFCH feedback
- U2N relay (SRAP protocol)
- RF simulator and USRP hardware modes

## Prerequisites

### Required Software
- OpenAirInterface 5G (`~/openairinterface5g/`)
- Built executables: `nr-uesoftmodem`, `nr-softmodem`
- `gnome-terminal` for launching parallel processes
- For SRAP relay tests: OAI 5G Core Network (Docker-based)

### Required Hardware (USRP tests only)
- Network-controlled RF attenuator at `http://169.254.10.10/`
- Two machines with USRP B210 radios
- SSH access configured in `~/.ssh/config` with hostname `nr_ue`

### Directory Structure
```
~/openairinterface5g/
  ├── cmake_targets/ran_build/build/    # Executables
  └── targets/PROJECTS/NR-SIDELINK/CONF/ # Config files
~/results_sl_test/                       # Test logs (auto-created)
```

## Usage

### Basic Execution
```bash
./run_sl_test.sh
```

### Configuration

Edit the `main()` function to select test type and profile:

**Test Type:**
```bash
test_type='rfsim'  # RF simulator mode (no hardware)
test_type='usrp'   # USRP hardware mode
```

**Test Profile:**
```bash
test_profile='pilot'    # num_repeat=1, mcs_array=(1), duration=15s
test_profile='regress'  # num_repeat=1, mcs_array=(1 9), duration=30s
test_profile='stress'   # num_repeat=3, mcs_array=(9 16 28), duration=300s
```

**Test Parameters:**
```bash
# RF simulator mode (default)
parm_array=$(seq 0 1 0)  # SNR sweep (currently unused in pilot profile)

# USRP hardware mode
parm_array=(20)          # Attenuation values in dB
# Example: parm_array=(20 30 40 50 55 60)
```

## Available Test Cases

### 1. `rfsim_pc5_ping_test_on_local_host`
**Purpose:** Basic PC5 connectivity test using RF simulator

**What it does:**
- Restores default config values for sl_CSI_Acquisition and sl_PSFCH_Period if modified
- Launches sync reference UE (UE1) on port 4148
- Launches nearby UE (UE2) connecting to sync ref
- Performs 5 pings from UE2 → UE1 (10.0.0.100) with real-time display
- Captures ping output to `~/results_sl_test/ping_result_<test_name>.txt`
- Pass criteria: ≥60% ping success rate

**Duration:** ~5 seconds (default)
**Arguments:**
- `$1`: Duration (default: 5)
- `$2`: MCS value (optional)

### 2. `rfsim_pc5_ping_test_on_two_hosts`
**Purpose:** PC5 connectivity test between two separate physical hosts

**What it does:**
- Launches sync reference UE on local host
- Launches nearby UE on remote host (via SSH to `nr_ue`)
- Performs ping from local host → remote UE (10.0.0.100)
- Pass criteria: ping returns status 0

**Duration:** ~15 seconds (default)

### 3. `rfsim_pc5_csi_acquisition_enable_test`
**Purpose:** Validates CSI acquisition can be toggled and system still functions

**What it does:**
- Restores default config values if modified
- Toggles `sl_CSI_Acquisition` to opposite of default value in both UE configs
- Launches UEs with toggled CSI setting
- Performs 5 pings with real-time display
- Restores original CSI configuration after test
- Pass criteria: ≥60% ping success rate

**Duration:** ~5 seconds (default)
**Arguments:**
- `$1`: Duration (default: 5)
- `$2`: MCS value (optional)

### 4. `rfsim_pc5_csi_acquisition_psfch_period_test`
**Purpose:** Parametric test for CSI acquisition and PSFCH period combinations

**Arguments:**
- `$1`: CSI acquisition enable (0=disabled, 1=enabled)
- `$2`: PSFCH period index (0, 1, 2, 3)
- `$3`: Duration (default: 15)
- `$4`: MCS value (optional)

**What it does:**
- Restores default config values if modified (sl_CSI_Acquisition and sl_PSFCH_Period)
- Modifies config files with specified CSI/PSFCH settings
- Launches UEs with specified configuration
- Performs 15 pings with real-time display
- Captures ping output to `~/results_sl_test/ping_result_<test_name>_csi<val>_psfch<val>.txt`
- Restores default config values after test
- Pass criteria: ≥60% ping success rate

**Duration:** ~15 seconds (default)

**Usage:**
```bash
rfsim_pc5_csi_acquisition_psfch_period_test 1 2 15 10  # CSI enabled, period index 2, 15s duration, MCS 10
```

### 5. `rfsim_srap_ping_test`
**Purpose:** U2N relay test (Remote UE → Relay UE → gNB)

**What it does:**
- Checks if OAI 5G Core Network (Docker) is running, starts if needed
- Launches gNB with relay support (relay-type 1, noS1 mode)
- Launches Relay UE (sync ref, Mode 1, connected to gNB via Uu + PC5)
- Launches Remote UE (Mode 2, connected to Relay UE via PC5 only)
- Pings internet (8.8.8.8) from Remote UE through relay
- Pass criteria: ping to internet succeeds

**Topology:**
```
Remote UE <--PC5--> Relay UE <--Uu--> gNB <--> 5G Core <--> Internet
(oaitun_ue2)       (is-relay-ue 1)         (noS1)
```

**Duration:** ~15 seconds (default)

### 6. `usrp_pc5_rsrp_test_on_two_B210s`
**Purpose:** Hardware-based RSRP measurement test

**What it does:**
- Launches sync ref UE on local machine with USRP B210
- Launches nearby UE on remote machine (via SSH to `nr_ue`) with USRP B210
- Runs for specified duration (default 15s), kills processes
- Validates "TotalTx 30" in sync ref log

**Requirements:**
- SSH access to remote host `nr_ue` (configured in `~/.ssh/config`)
- USRP B210 radios on both machines

### 7. `usrp_pc5_test_on_two_B210s`
**Purpose:** Hardware-based PC5 connectivity test with ping validation

**What it does:**
- Launches sync ref UE on local machine with USRP B210
- Launches nearby UE on remote machine with USRP B210
- Performs ping test from local → remote (10.0.0.100)
- Pass criteria: ping returns status 0

**Duration:** ~15 seconds (default)

## Utility Functions

### Process Management
- `check_process <name>` - Check if process is running
- `kill_process <PID...>` - Kill specific PIDs
- `kill_all <name...>` - Kill all processes by name

### RF Control
- `set_atten <value>` - Set RF attenuator (USRP mode)

### Execution
- `run_cmd <cmd...>` - Launch commands in separate gnome-terminal windows

### Validation
- `check_same_str <str1> <str2>` - String comparison with Pass/Fail output
- `check_same_val <num1> <num2>` - Numeric comparison with Pass/Fail output
- `is_same <val1> <val2>` - Auto-detect string vs numeric comparison
- `get_ping_stats_tuple <file>` - Extract transmitted/received packet counts from ping output file
- `check_ping_result <tx> <rx> [threshold]` - Validate ping success rate (default 60% threshold)

### Configuration Management
- `check_and_restore_default <config> <param> <default>` - Restore local config parameter if modified
- `check_and_restore_default_remote <host> <config> <param> <default>` - Restore remote config parameter if modified

### Runtime Tracking
- `print_runtime <start_time> <end_time>` - Display elapsed time in seconds and minutes

## Output & Logs

### Log Files
All logs saved to `~/results_sl_test/`:
- `result_syncref.log` - Sync reference UE output
- `result_nearby.log` - Nearby/Remote UE output
- `result_nrUE_syncref.log` - Relay UE output (SRAP tests)
- `result_gNB.log` - gNB output (relay tests)
- `result_summary.txt` - Test result summary
- `ping_result_<test_name>.txt` - Ping output for individual tests
- `ping_result_<test_name>_csi<val>_psfch<val>.txt` - Ping output for parametric tests

### Console Output
Each test prints:
```
====================  Testing <test_name>  ====================
Ping Statistics:
  Transmitted: 5
  Received: 5
  Packet Loss: 0%
Result: Pass (5/5 = 100% >= 60%)
Elapsed Time: 8 seconds (0m 8s)
```

**Ping Success Criteria:**
- Tests use ping statistics (transmitted vs received packets) for validation
- Default threshold: 60% success rate (configurable via `check_ping_result` parameter)
- Pass criteria: `(received / transmitted) * 100 >= 60%`
- Ping output is displayed in real-time in gnome-terminal and captured to log files

## Key Configuration Parameters

### RF Simulator
- **Server port (Uu):** 4048
- **Server port (SL/PC5):** 4148
- **Server address:** `127.0.0.1` (sync ref), `server` (nearby UE)

### Sidelink Mode
- **Mode 1:** Network-scheduled (requires gNB connection)
- **Mode 2:** Autonomous resource selection (no network)

### UE Transmit/Receive Gains
- Default Tx gain: 10 dB
- Default Rx gain: 100 dB

### Default Config Values (Read at Script Startup)
Script automatically reads default values from config files:
- `DEFAULT_CSI_ACQ` - Default sl_CSI_Acquisition value (read from sl_sync_ref.conf)
- `DEFAULT_PSFCH_PERIOD` - Default sl_PSFCH_Period value (read from sl_sync_ref.conf)

These values are used to restore configurations after parametric tests complete.

## Troubleshooting

### "Program not running" after test start
- Check if executables exist in `$EXEC_PATH`
- Verify config files exist in `$CONF_PATH`
- Ensure `LD_LIBRARY_PATH` includes required libraries

### Ping test fails (shows <60% success rate)
- Verify tunnel interfaces created: `oaitun_ue1`, `oaitun_ue2`
- Check logs for "RRC connection established"
- Ensure no firewall blocking tunnel traffic
- Review ping output file in `~/results_sl_test/` for detailed ping statistics
- Check UE logs for sidelink connection issues
- Verify both UEs are synchronized (check "sync ref" status)

### SRAP relay test fails
- Verify 5G Core is running: `docker ps | grep oai-amf`
- Check gNB registers with AMF (log: "Received NG_SETUP_RESPONSE")
- Verify Relay UE has both Uu and PC5 connections

### Config modification doesn't persist
- Check sed commands in pre/post sections of test functions
- Ensure config files are not read-only
- Verify `$CONF_PATH` points to correct directory
- Note: Script automatically restores default values before each test to ensure clean state

## Extending the Test Suite

### Adding a New Test Case

1. Define test function:
```bash
my_new_test() {
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    cmd1='<command for UE1>'
    cmd2='<command for UE2>'
    run_cmd "$cmd1" "$cmd2"
    sleep <duration>
    kill_all nr-uesoftmodem
    sleep 3
    # Add validation logic
}
```

2. Add to `main()` function:
```bash
if [[ $test_type == "rfsim" ]]; then
    my_new_test
fi
```

### Parametric Sweeps

Modify `parm_array` to sweep SNR/attenuation:
```bash
parm_array=$(seq 20 5 60)  # 20, 25, 30, 35...60
```

Access current value in test: `$val`

## Notes

- All tests require sudo privileges (RF access, tunnel creation)
- Tests automatically clean up processes on exit
- Config file modifications are reverted after parametric tests
- Use `Ctrl+C` to abort; manually run `kill_all nr-uesoftmodem nr-softmodem` if needed

## References

- OAI Sidelink docs: `~/openairinterface5g/doc/episys/README_SL.md`
- Config examples: `~/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/`
