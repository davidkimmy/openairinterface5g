# OpenAirInterface 5G NR Sidelink Test Framework

Automated test harness for validating 5G NR PC5 (sidelink) and U2N relay functionality in OpenAirInterface.

## Overview

This test framework provides automated validation for OAI 5G NR sidelink features:
- **PC5 direct device-to-device communication** (Mode 1 & Mode 2)
- **CSI acquisition and PSFCH feedback** with parametric testing
- **U2N relay (SRAP protocol)** for remote UE connectivity
- **RF simulator and USRP hardware modes** for flexibility

### Key Features
- **Array-based test selection** - Simple list of tests to run in order
- **Parametric CSI/PSFCH testing** - Run specific combinations or sweep all parameters
- **Multi-host support** - Test across multiple machines with SSH
- **Automated result tracking** - Summary table with ping and PSSCH statistics
- **Profile-based configuration** - Quick switch between pilot, regression, and stress testing

## Architecture

### Test Structure

```
run_sl_test.sh                   # Main test execution engine
run_sl_test_config.sh            # User configuration (tests, profiles, parameters)
~/.ssh/config                    # SSH host aliases for multi-host testing
~/openairinterface5g/            # OAI source and binaries
<script_dir>/test_<timestamp>/     # Test logs and results (auto-created)
```

### Execution Flow

```
1. Load configuration from run_sl_test_config.sh
2. Validate enabled tests and parameters
3. For each test in enabled_tests array:
   - Launch UEs (local/remote via SSH)
   - Execute test scenario (ping, SRAP, etc.)
   - Collect statistics (ping, PSSCH TX/RX)
   - Kill processes and cleanup
   - Generate result summary row
4. Display final summary table
```

### Multi-Host Testing

For two-host or three-host tests:
- **Local host**: Runs syncref UE (RF simulator server in Mode 2)
- **Remote host(s)**: Run nearby/relay UEs (RF simulator clients)
- **Communication**: SSH with passwordless keys, gnome-terminal for real-time logs

## Prerequisites

### Required Software
- OpenAirInterface 5G (`~/openairinterface5g/`)
- Built executables: `nr-uesoftmodem`, `nr-softmodem`, `nr-cuup`
- `gnome-terminal` for launching parallel processes
- For SRAP relay tests: OAI 5G Core Network (Docker-based)
- `ssh` with passwordless authentication for multi-host tests

### Required Hardware (USRP tests only)
- Network-controlled RF attenuator at `http://169.254.10.10/`
- Two or three machines with USRP B210 radios
- SSH access configured in `~/.ssh/config` (see setup below)

### Directory Structure
```
~/openairinterface5g/
  ├── cmake_targets/ran_build/build/      # Executables
  └── targets/PROJECTS/NR-SIDELINK/CONF/  # Config files
~/ci_test/                                # Test logs (auto-created)
```

## Quick Start

### 1. Configure SSH for Multi-Host Testing

Edit `~/.ssh/config` to define host aliases:

```bash
# Remote UE host and nrUE (for two-host PC5 tests)
Host remote_ue nr_ue
    HostName 192.168.1.100
    User your_username
    IdentityFile ~/.ssh/id_ed25519
    StrictHostKeyChecking no

# Relay UE host (for three-host SRAP tests)
Host relay_ue
    HostName 192.168.1.101
    User your_username
    IdentityFile ~/.ssh/id_ed25519
    StrictHostKeyChecking no

# gNB host (for three-host SRAP tests or Uu interface test)
# local host (the host running test scrip. In SL mode 2 test, it will work as SyncRef UE)
Host gNB local
    HostName 192.168.1.102
    User your_username
    IdentityFile ~/.ssh/id_ed25519
    StrictHostKeyChecking no
```

> **Important:** The test script requires these exact host alias names in `~/.ssh/config`:
> - `remote_ue` — Used for nearby UE in two-host and three-host tests
> - `relay_ue` — Used for relay UE (syncref) in three-host SRAP tests
> - `nr_ue` — Used for nrUE in Uu interface test
> - `gNB` — Used for gNB in three-host SRAP tests or Uu interface test
> - `local` — Used to identify the local host IP address and user name
>
> These names are hardcoded in `run_sl_test.sh`. If your SSH config uses different names, update the script variables (`REMOTE_UE_HOST`, `RELAY_UE_HOST`, `GNB_HOST`) accordingly.

**Setup passwordless SSH:**
```bash
# Generate SSH key if you don't have one
ssh-keygen -t ed25519

# Copy public key to remote hosts
ssh-copy-id remote_ue
ssh-copy-id relay_ue
ssh-copy-id gNB
ssh-copy-id local

# If remote_ue and nr_ue are different hosts, copy public key to nr_ue host.
ssh-copy-id gNB

# Test connection (should not prompt for password)
ssh remote_ue hostname
```

### 2. Configure Test Selection

Edit `run_sl_test_config.sh`:

```bash
# Select test profile
test_profile='pilot'    # Quick validation (1 repeat, MCS=1, 30s)

# List tests to run (in order)
# You can mix group names, group subsets, and individual test cases
enabled_tests=(
    slmode2_basic_tests[0]
    rfsim_pc5_csi_acquisition_psfch_period_test_on_local_host:0:1
)
```

### 3. Run Tests

```bash
./run_sl_test.sh
```

## Configuration Guide

### Test Profiles

Three built-in profiles in `run_sl_test_config.sh`:

| Profile   | Repeats | MCS Values | Duration | SNR/Atten   | TX/RX Gain | Use Case |
|-----------|---------|------------|----------|-------------|------------|----------|
| `pilot`   | 1       | 1          | 30s      | 0 / 20dB    | 20 / 110   | Quick smoke test |
| `regress` | 1       | 1, 9       | 30s      | 0 / 20dB    | 20 / 110   | Regression validation |
| `stress`  | 3       | 9, 16, 28  | 300s     | 0 / 20-60dB | 20 / 110   | Long-term stability |

### Test Selection Syntax

The `enabled_tests` array supports three types of entries: **group names**, **group subsets**, and **individual test cases**. These can be freely mixed in any order. Group names are resolved recursively, so a group can contain other group names.

#### Predefined Test Groups

The config file defines four test groups, each containing all tests of that category:

| Group Name | Index | Test Case |
|---|---|---|
| `uu_basic_tests`
| | 0 | `rfsim_uu_ping_test_on_local_host` |
| | 1 | `rfsim_uu_ping_test_on_two_hosts` |
| | 2 | `usrp_B210_uu_ping_test_on_two_hosts` |
| `slmode2_basic_tests`
| | 0 | `rfsim_pc5_ping_test_on_local_host` |
| | 1 | `rfsim_pc5_ping_test_on_two_hosts` |
| | 2 | `usrp_B210_pc5_ping_test_on_two_hosts` |
| `slmode2_csi_psfch_tests`
| | 0 | `rfsim_pc5_csi_acquisition_psfch_period_test_on_local_host` |
| | 1 | `rfsim_pc5_csi_acquisition_psfch_period_test_on_two_hosts` |
| | 2 | `usrp_B210_pc5_csi_acquisition_psfch_period_test_on_two_hosts` |
| `slmode1_basic_tests`
| | 0 | `rfsim_slmode1_srap_ping_test_on_local_host` |
| | 1 | `rfsim_slmode1_srap_ping_test_on_three_hosts` |
| | 2 | `usrp_B210_slmode1_srap_ping_test_on_three_hosts` |

You can also define your own groups that reference other groups or individual tests:

```bash
regress_tests=(
    slmode2_basic_tests
    slmode2_csi_psfch_tests
)
```

#### Selection Examples

```bash
enabled_tests=(
    # Entire group — runs all 4 tests in the group
    slmode2_basic_tests

    # Range — indices 0 through 2 (inclusive), runs 3 tests
    slmode2_basic_tests[0:2]

    # Pick — runs only indices 0 and 2
    slmode2_basic_tests[0,2]

    # Single index — runs only index 0
    slmode2_basic_tests[0]

    # Nested group — resolves recursively (expands regress_tests, then its sub-groups)
    regress_tests

    # Individual test case
    rfsim_pc5_ping_test_on_two_hosts

    # Individual CSI/PSFCH test with specific parameters
    rfsim_pc5_csi_acquisition_psfch_period_test_on_two_hosts:0:1
)
```

#### CSI/PSFCH Parameter Format

For CSI/PSFCH tests, append `:csi_acq:psfch_period` to specify which combinations to run:

```bash
    # CSI/PSFCH specific combination
    rfsim_pc5_csi_acquisition_psfch_period_test_on_local_host:0:1

    # CSI/PSFCH parameter sweep (all PSFCH values for CSI=1)
    rfsim_pc5_csi_acquisition_psfch_period_test_on_local_host:1:

    # CSI/PSFCH parameter sweep (all CSI values for PSFCH=2)
    rfsim_pc5_csi_acquisition_psfch_period_test_on_two_hosts::2

    # All combinations (no colon suffix = run all 8 combinations)
    rfsim_pc5_csi_acquisition_psfch_period_test_on_local_host
```

- `test_name` → Runs all 8 combinations (CSI: 0,1 × PSFCH: 0,1,2,3)
- `test_name:0:1` → Runs only CSI=0, PSFCH=1
- `test_name:1:` → Runs CSI=1 with all PSFCH values (0,1,2,3)
- `test_name::2` → Runs PSFCH=2 with all CSI values (0,1)

## Available Test Cases

### Uu Interface Tests

#### `rfsim_uu_ping_test_on_local_host`
Uu interface connectivity test on local machine (requires 5G Core).
- Launches gNB and nrUE locally with RF simulator
- Pings 8.8.8.8 from UE through gNB and 5G Core
- **Pass criteria:** ≥60% ping success rate

#### `rfsim_uu_ping_test_on_two_hosts`
Uu interface connectivity test across two machines (requires 5G Core).
- gNB runs locally, nrUE runs on `nr_ue` host
- Pings 8.8.8.8 from UE through gNB and 5G Core
- **Pass criteria:** ≥60% ping success rate

#### `usrp_B210_uu_ping_test_on_two_hosts`
Uu interface connectivity test with USRP B210 hardware (requires 5G Core).
- gNB runs locally, nrUE runs on `nr_ue` host
- Uses USRP B210 radios for over-the-air transmission
- **Pass criteria:** ≥60% ping success rate

### Single-Host Tests (RF Simulator)

#### `rfsim_pc5_ping_test_on_local_host`
Basic PC5 Mode 2 connectivity test on local machine.
- Launches syncref UE (10.0.0.99) and nearby UE (10.0.0.100)
- Pings between UEs via sidelink
- **Pass criteria:** ≥60% ping success rate

#### `rfsim_pc5_csi_acquisition_psfch_period_test_on_local_host`
Parametric test for CSI and PSFCH combinations on local machine.
- Modifies UE config files with specified CSI/PSFCH parameters
- Restores default config after test
- **Parameters:** CSI (0=disabled, 1=enabled), PSFCH period (0,1,2,3)
- **Pass criteria:** ≥60% ping success rate

#### `rfsim_slmode1_srap_ping_test_on_local_host`
U2N relay test on local machine (requires 5G Core).
- Topology: Remote UE → Relay UE (via PC5) → gNB → 5G Core → Internet
- Pings 8.8.8.8 from remote UE through relay
- **Pass criteria:** Internet ping succeeds

### Two-Host Tests (RF Simulator)

#### `rfsim_pc5_ping_test_on_two_hosts`
PC5 Mode 2 connectivity test across two machines.
- Syncref UE runs locally (RF sim server)
- Nearby UE runs on `remote_ue` host (RF sim client)
- Ping executes locally (where syncref creates oaitun_ue1)
- UE logs automatically copied from remote host via SCP before statistics extraction
- **Pass criteria:** ≥60% ping success rate

#### `rfsim_pc5_csi_acquisition_psfch_period_test_on_two_hosts`
Parametric CSI/PSFCH test across two machines.
- Updates config files on both local and remote hosts
- Same parameter syntax as single-host version
- **Pass criteria:** ≥60% ping success rate

### Three-Host Tests (RF Simulator)

#### `rfsim_slmode1_srap_ping_test_on_three_hosts`
U2N relay test across three machines (requires 5G Core).
- gNB runs on `gNB` host
- Relay UE runs on `relay_ue` host
- Remote UE runs on `remote_ue` host
- **Pass criteria:** Internet ping from remote UE succeeds

### USRP Hardware Tests

Replace `rfsim` prefix with `usrp_B210` for hardware tests:
- `usrp_B210_uu_ping_test_on_two_hosts`
- `usrp_B210_pc5_ping_test_on_two_hosts`
- `usrp_B210_pc5_csi_acquisition_psfch_period_test_on_two_hosts`
- `usrp_B210_slmode1_srap_ping_test_on_three_hosts`

**Requirements:**
- USRP B210 radios on all participating hosts
- RF attenuator at `http://169.254.10.10/` (controlled via `set_atten`)

## Test Results

### Summary Table

After all tests complete, a summary table is displayed and saved to `<script_dir>/test_<timestamp>/test_summary_<timestamp>.csv`:

```
Test Name                                                              | Itrn | Hosts | MCS | Runtime | Ping Rate | PSSCH Rate1        | PSSCH Rate2        | PSSCH Total | Result
========================================================================================================================================================================================
rfsim_pc5_csi_acquisition_psfch_period_test_on_two_hosts_csi0_psfch1  |    1 |     1 |   1 |    35s  |      100% | 1234/1200 (97%)    | 1150/1234 (93%)    |          95% |   PASS
```

**Columns:**
- **Test Name:** Full test function name with parameters
- **Itrn:** Iteration number (for repeated tests)
- **Hosts:** Number of hosts involved (1=local, 2=two-host, etc.)
- **MCS:** Modulation and coding scheme
- **Runtime:** Test duration in seconds
- **Ping Rate:** ICMP ping success rate (RX/TX packets)
- **PSSCH Rate1:** Syncref TX → Nearby RX (TX_syncref/RX_nearby with percentage)
- **PSSCH Rate2:** Nearby TX → Syncref RX (TX_nearby/RX_syncref with percentage)
- **PSSCH Total:** Aggregated PSSCH success rate across both directions
- **Result:** PASS/FAIL based on ping threshold (≥60%)

### Log Files

All logs saved to `<script_dir>/test_<timestamp>/` (where script_dir is the location of run_sl_test.sh):
- `test_summary_<timestamp>.csv` - Summary table
- `result_syncref.log` - Syncref UE output (local host, Mode 2)
- `result_syncref_remote.log` - Syncref UE output (remote host, copied via SCP)
- `result_nearby.log` - Nearby UE output (local host)
- `result_nearby_remote.log` - Nearby UE output (remote host, copied via SCP)
- `result_gNB.log` - gNB output (relay tests)
- `result_nrUE_syncref.log` - Relay UE output (local host, Mode 1 SRAP tests)
- `result_nrUE_syncref_remote.log` - Relay UE output (remote host, copied via SCP)
- `ping_result_<test_name>_<timestamp>.txt` - Ping output per test

**Remote Log Copying:**
For multi-host tests, UE logs from remote hosts are automatically copied to the local test directory via SCP before PSSCH statistics extraction. The script uses `copy_syncref_ue_logs()` and `copy_nearby_ue_logs()` functions to:
1. Copy logs from remote host based on the test mode (Mode 1 vs Mode 2)
2. Clean up remote log files after successful copy
3. Ensure statistics are computed from the correct log files

## Key Concepts

### Sidelink Modes

**Mode 1 (Network-Scheduled):**
- UEs connected to gNB via Uu interface
- Network controls resource allocation
- Used in U2N relay scenarios (Relay UE connects to gNB)

**Mode 2 (Autonomous):**
- Direct D2D communication without network
- UEs autonomously select resources
- Used in most PC5 connectivity tests

### RF Simulator Architecture

**Single-Host:**
```
Syncref UE (server, 127.0.0.1:4148) <--rfsim--> Nearby UE (client, 127.0.0.1:4148)
```

**Two-Host (Mode 2):**
```
Local:  Syncref UE (server, 0.0.0.0:4148)
Remote: Nearby UE (client, <local_ip>:4148)
```

**Key insight:** In Mode 2 two-host tests, syncref always runs locally (RF sim server), nearby runs remotely (RF sim client), and ping runs locally where syncref creates `oaitun_ue1`.

### PSSCH Statistics

PSSCH (Physical Sidelink Shared Channel) carries sidelink user data.

**TX/RX Extraction:**
Script parses UE logs for:
```
[SIDELINK] Frame XXXX, TotalTx YYYY
[SIDELINK] Frame XXXX, TotalRx YYYY
```

**Rate Calculation:**
- **PSSCH Rate1:** Syncref transmits → Nearby receives (RX_nearby / TX_syncref)
- **PSSCH Rate2:** Nearby transmits → Syncref receives (RX_syncref / TX_nearby)
- **PSSCH Total:** Aggregated success rate: (RX_sync + RX_nearby) / (TX_sync + TX_nearby)

**Multi-Host Statistics:**
For two-host and three-host tests, the script handles log collection differently based on where each UE runs:
- **Mode 2 two-host:** Syncref (local) + Nearby (remote) → Copy nearby logs from remote host
- **Mode 1 three-host:** Relay UE and Remote UE on separate hosts → Copy both logs from respective remote hosts

The `evaluate_ping_test()` function accepts `syncref_host` and `nearby_host` parameters to determine which logs need to be copied before statistics extraction. This ensures accurate PSSCH TX/RX rates even when UEs run on different machines.

### CSI Acquisition & PSFCH

**CSI (Channel State Information):**
- `sl_CSI_Acquisition = 0` → Enabled
- `sl_CSI_Acquisition = 1` → Disabled

**PSFCH (Physical Sidelink Feedback Channel):**
- `sl_PSFCH_Period = 0, 1, 2, 3` → Different feedback periodicities

Parametric tests sweep these combinations to validate feedback mechanisms.

## SSH Configuration for Newbies

### Why SSH Config?

Instead of typing:
```bash
ssh -i ~/.ssh/id_ed25519 username@192.168.1.100
```

You can use:
```bash
ssh remote_ue
```

### Step-by-Step Setup

**1. Generate SSH Key (if you don't have one):**
```bash
ssh-keygen -t ed25519 -f ~/.ssh/id_ed25519
# Press Enter for all prompts (no passphrase recommended for automation)
```

**2. Create `~/.ssh/config` file:**
```bash
touch ~/.ssh/config
chmod 600 ~/.ssh/config
nano ~/.ssh/config
```

**3. Add host definitions:**
```
# Example: Remote UE Host
Host remote_ue
    HostName 192.168.1.100          # Replace with actual IP
    User your_username              # Replace with remote username
    IdentityFile ~/.ssh/id_ed25519
    StrictHostKeyChecking no
    UserKnownHostsFile /dev/null

# Example: Relay UE Host
Host relay_ue
    HostName 192.168.1.101
    User your_username
    IdentityFile ~/.ssh/id_ed25519
    StrictHostKeyChecking no
    UserKnownHostsFile /dev/null

# Example: gNB Host
Host gNB
    HostName 192.168.1.102
    User your_username
    IdentityFile ~/.ssh/id_ed25519
    StrictHostKeyChecking no
    UserKnownHostsFile /dev/null
```

**4. Copy public key to remote hosts:**
```bash
ssh-copy-id -i ~/.ssh/id_ed25519.pub remote_ue
ssh-copy-id -i ~/.ssh/id_ed25519.pub relay_ue
ssh-copy-id -i ~/.ssh/id_ed25519.pub gNB
```

**5. Test passwordless login:**
```bash
ssh remote_ue hostname
# Should print remote hostname without password prompt
```

**6. Verify script can resolve hosts:**
```bash
ssh -G remote_ue | grep "^hostname"
# Should output: hostname 192.168.1.100
```

### Common SSH Issues

**Permission denied (publickey):**
```bash
# Check permissions
chmod 700 ~/.ssh
chmod 600 ~/.ssh/config
chmod 600 ~/.ssh/id_ed25519
chmod 644 ~/.ssh/id_ed25519.pub

# Verify key is copied
ssh remote_ue "cat ~/.ssh/authorized_keys"
```

**Host key verification failed:**
```bash
# Remove old host key
ssh-keygen -R 192.168.1.100

# Or add to config:
StrictHostKeyChecking no
UserKnownHostsFile /dev/null
```

**Connection refused:**
```bash
# Check SSH service on remote
ssh remote_ue "sudo systemctl status ssh"

# Check firewall
ssh remote_ue "sudo ufw status"
```

## Troubleshooting

### Test Execution Issues

**"No such device" for oaitun_ue1:**
- UE not fully initialized → Increase sleep after UE launch (line 789: `sleep 5`)
- Check UE logs for RRC connection errors

**Ping fails (<60% success rate):**
- Verify tunnel interfaces: `ip addr show oaitun_ue1`
- Check UE logs for sidelink synchronization
- Increase test duration in `run_sl_test_config.sh`

**gnome-terminal windows stay open:**
- Fixed in latest version (removed `exec bash`)
- Terminals should auto-close when processes are killed

**PSSCH statistics show 0/0 or N/A:**
- UEs not transmitting data → Check sidelink logs for resource allocation
- Increase test duration to allow more data exchange
- For two-host tests: Verify remote UE logs are being copied (check for `result_nearby_remote.log` in test directory)
- Check SSH connectivity to remote hosts: `ssh remote_ue hostname`

### Multi-Host Issues

**SSH command fails:**
- Test SSH config: `ssh remote_ue hostname`
- Verify passwordless auth: `ssh-copy-id remote_ue`

**Remote UE config not updated:**
- Check `REMOTE_USER` variable in script header
- Verify file paths on remote host match local paths

**SRAP relay test fails:**
- Verify 5G Core running: `docker ps | grep oai-amf`
- Check gNB logs for NG setup response
- Verify Relay UE has both Uu and PC5 connections

### Configuration Issues

**Config modifications don't persist:**
- Script restores defaults before each test (intentional)
- To change defaults, edit config files in `~/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/`

**USRP attenuator error:**
- Check attenuator reachable: `curl http://169.254.10.10/`
- Disable USRP tests if no hardware: comment out in `enabled_tests` array

## Extending the Framework

### Adding a New Test

**1. Define test function in `run_sl_test.sh`:**
```bash
my_custom_test() {
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && test_type=$2
    [[ $# -ge 3 ]] && mcs=$3
    [[ $# -ge 4 ]] && iteration=$4

    local start_time=$(date +%s)
    local sl_mode=2

    # Launch UEs
    run_syncref_cmd $test_type $mcs $sl_mode "local"
    run_nearby_cmd  $test_type $mcs $sl_mode "local"

    # Wait for initialization
    sleep 5

    # Run test (e.g., ping)
    evaluate_ping_test "local" "oaitun_ue1" "10.0.0.100" $sl_mode "${FUNCNAME[0]}"

    # Cleanup
    local end_time=$(date +%s)
    local elapsed=$((end_time - start_time))
    print_runtime $start_time $end_time

    # Print summary
    print_test_summary "${FUNCNAME[0]}" "$iteration" "1" "$mcs" "$elapsed" \
        "$LAST_TX_PACKETS" "$LAST_RX_PACKETS" "$LAST_TEST_RESULT"
}
```

**2. Add wrapper functions for rfsim/usrp variants:**
```bash
rfsim_my_custom_test_on_local_host() {
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && iteration=$3
    local test_type="rfsim"
    my_custom_test $duration $test_type $mcs $iteration
}

usrp_B210_my_custom_test_on_local_host() {
    echo "====================  Testing ${FUNCNAME[0]}  ===================="
    [[ $# -ge 1 ]] && duration=$1
    [[ $# -ge 2 ]] && mcs=$2
    [[ $# -ge 3 ]] && iteration=$3
    local test_type="usrp"
    my_custom_test $duration $test_type $mcs $iteration
}
```

**3. Add to available tests list in `run_sl_test_config.sh`:**
```bash
: <<'AVAILABLE_TESTS'
Available test functions:

rfsim_my_custom_test_on_local_host
usrp_B210_my_custom_test_on_local_host
...
AVAILABLE_TESTS
```

**4. Enable in `enabled_tests` array:**
```bash
enabled_tests=(
    rfsim_my_custom_test_on_local_host
)
```

### Modifying Test Profiles

Edit `run_sl_test_config.sh` to add custom profiles:
```bash
elif [[ $test_profile == "custom" ]]; then
    num_repeat=2
    mcs_array=(1 5 9)
    duration=60
    snr_array=($(seq 0 2 10))  # 0, 2, 4, 6, 8, 10
    atten_array=(20 30 40)
```

## References

- **OAI Sidelink documentation:** `~/openairinterface5g/doc/episys/README_SL.md`
- **Config file examples:** `~/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/`
- **3GPP specs:** TS 38.331 (RRC), TS 38.321 (MAC), TS 38.211 (Physical layer)
- **RF simulator:** `~/openairinterface5g/radio/rfsimulator/`

## License

This test framework follows the OpenAirInterface license (OAI Public License V1.1).
