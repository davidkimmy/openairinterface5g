# BLER Testing Framework

Comprehensive guide for automated Block Error Rate (BLER) performance characterization of 5G NR Sidelink.

## Overview

The BLER (Block Error Rate) testing framework provides automated performance characterization across the full MCS range (0-28) and SNR sweep (-12 to 4 dB) using **RFSIM (RF Simulator)**. It supports both local execution and distributed parallel testing across multiple machines for faster completion.

> **Note**: This framework is designed for **RFSIM testing** with configurable noise injection. For USRP hardware testing, see separate hardware test documentation.

**Key Features:**
- **RFSIM-based testing**: Uses RF simulator with controlled noise power injection
- Full MCS coverage (0-28) with all modulation schemes (QPSK, 16-QAM, 64-QAM)
- SNR sweep across 17 noise power levels (-12 to 4 dB)
- Multiple test iterations for statistical confidence
- Distributed parallel execution (2-hour completion with 4 machines vs 7.5 hours sequential)
- Comprehensive logging: MAC BLER, LDPC iterations, HARQ rounds
- Automated data collection, processing, and plotting
- Multi-perspective analysis: UE RX and Syncref RX

## Prerequisites: Build with BLER Instrumentation

> **⚠️ IMPORTANT:** Before running BLER tests, you **must** build with BLER instrumentation enabled on all test machines.

### What is BLER Instrumentation?

BLER instrumentation is compiled into the code using the `--bler-instrumentation` build flag, which enables the CMake option `ENABLE_BLER_INSTRUMENTATION`. This adds conditional logging wrapped in `#ifdef ENABLE_BLER_INSTRUMENTATION` blocks in the source code.

**What it provides:**
- **PC5 RX BLER tracking** (`nr_ue_procedures_sl.c`):
  - Block-level error counting (total blocks, errors)
  - Periodic BLER summaries every 100 blocks (up to 1000)
  - Log tag: `[BLER_STATS]`
  
- **HARQ round statistics** (`nr_slsch_scheduler.c`):
  - Success counts per HARQ round (R0, R1, R2, R3)
  - Log tag: `[HARQ_STATS]`
  
- **LDPC decoder iterations** (`nr_pscch_pssch_rx.c`):
  - Iteration count per decoded block with MCS
  - Convergence success/failure indication
  - Log tag: `[LDPC_STATS]`

**Zero runtime overhead when disabled**: When built without `--bler-instrumentation`, all logging code is completely removed at compile time (not just disabled).

### Build Commands

**Single machine:**

```bash
cd ~/openairinterface5g

# First time: Install dependencies
./build_oai -I

# Build with BLER instrumentation enabled
./build_oai --nrUE -w SIMU --ninja -c --bler-instrumentation

# Verify it's enabled (should show: BLER instrumentation: ENABLED)
grep "BLER instrumentation" cmake_targets/log/nr-uesoftmodem.Rel15.txt
```

**Distributed testing** - build on **all machines** in your `bler_hosts` array:

```bash
# From the control machine
for host in l3 l4 l5; do
  echo "=== Building with BLER instrumentation on $host ==="
  ssh $host "cd ~/openairinterface5g && ./build_oai --nrUE -w SIMU --ninja -c --bler-instrumentation"
done
```

### Verification

**Check CMake option was set:**
```bash
cd ~/openairinterface5g/cmake_targets/ran_build/build
cmake -L | grep ENABLE_BLER_INSTRUMENTATION
# Should show: ENABLE_BLER_INSTRUMENTATION:BOOL=ON
```

**Verify logging works in test:**
```bash
# Run a quick test, then check for instrumentation logs
grep "BLER_STATS\|HARQ_STATS\|LDPC_STATS" ~/openairinterface5g/test_*/result_nearby_*.log

# Should see output like:
# [BLER_STATS] PC5_RX_SUMMARY total=100 errors=5 BLER=0.0500
# [HARQ_STATS] PC5_HARQ_SUCCESS round=0 cumul_r0=95 r1=5 r2=0 r3=0
# [LDPC_STATS] PC5_LDPC_ITERATIONS mcs=16 iterations=3 max=50 success=1
```

### Production Builds (Without BLER Instrumentation)

For production deployments or non-BLER testing, simply omit the flag:

```bash
./build_oai --nrUE -w SIMU --ninja -c

# Verify it's disabled (should show: BLER instrumentation: DISABLED)
grep "BLER instrumentation" cmake_targets/log/nr-uesoftmodem.Rel15.txt
```

**Benefits of disabling:**
- Zero performance overhead (code removed at compile time)
- Smaller binary size
- No log clutter from BLER statistics

## System Performance Configuration

BLER tests adapt to different system speeds (fast servers vs. slow VMs/containers).

**Configuration:**

In `run_sl_test_config.sh`, set the extended delays flag:

```bash
# Set to 1 if your system needs additional time for:
#   - TUN interface initialization
#   - Sidelink synchronization stabilization
use_extended_delays=0  # Default: 0 (fast system), set to 1 for slow systems
```

Most users should keep `use_extended_delays=0` (default). Only enable if you experience timing issues with TUN interface setup or sync stabilization
- Impact analysis on test duration

## BLER Test Types

### `rfsim_slmode1_bler_test_on_local_host`
Comprehensive BLER characterization on a single machine using **RFSIM (RF Simulator)** with noise injection.

- **Test method**: RFSIM with configurable noise power injection
- Runs on local machine only (no physical hardware required)
- Full MCS range: 0-28 (29 values)
- Noise power sweep: -12 to 4 dB (17 values)
- Configurable iterations (default: 12)
- **Total tests:** 29 MCS × 17 noise × 12 iterations = 5,916 tests
- **Duration:** ~7.5 hours sequential, ~2 hours with 4 machines parallel
- **Logs collected:**
  - PC5_RX_SUMMARY: BLER, HARQ rounds, SNR
  - PC5_LDPC_ITER: LDPC decoder iterations
  - PSSCH TX/RX statistics
- **Pass criteria:** Completes full sweep, generates CSV outputs

## Running BLER Tests Locally

**1. Configure test in `run_sl_test_config.sh`:**

```bash
# Enable BLER test
enabled_tests=(
    rfsim_slmode1_bler_test_on_local_host
)

# Use test_profile for BLER configuration
test_profile="bler"

# BLER profile automatically sets:
# - num_repeat=12                    # Number of iterations (split across machines)
# - duration=85                      # Duration per test (seconds)
# - mcs_array=($(seq 0 28))         # Full MCS range: 0 to 28
# - noise_power_array=($(seq -12 4)) # Noise power: -12 to 4 dB (17 values)
```

**2. Run the test:**

```bash
cd ~/ci_script
./run_sl_test.sh
```

**3. Monitor progress:**

Test logs are saved to `~/openairinterface5g/test_<timestamp>/` (where `<timestamp>` is `YYYYMMDD_HHMMSS` format):
- Real-time output shows current MCS, noise power, and iteration
- Progress: "Test X / Y" displayed for each combination
- Logs: `result_syncref_*.log`, `result_nearby_*.log`
- Example directory: `~/openairinterface5g/test_20260518_120045/`

**4. Expected completion time:**

- **Sequential (1 machine):** ~7.5 hours
  - 29 MCS × 17 noise × 10 iterations × 30s = 4,930 tests × 30s = ~41 hours
  - (Actual: ~7.5 hours due to parallel processing within local tests)

## Running BLER Tests in Parallel (Distributed)

For faster completion, distribute the test load across multiple machines. The distribution strategy is **iteration-based**: each machine runs a subset of iterations with the full MCS range, ensuring complete coverage across all machines.

**Distribution Strategy:**
- Each machine tests the **full MCS range (0-28)** and **full noise range (-12 to 4 dB)**
- Iterations are split evenly across machines
- Results are combined after collection for statistical analysis

**Example Configuration (12 iterations, 4 machines):**
```
host1:      Iterations 1-3   | MCS 0-28 | Noise -12 to 4 dB
host2:      Iterations 4-6   | MCS 0-28 | Noise -12 to 4 dB
host3:      Iterations 7-9   | MCS 0-28 | Noise -12 to 4 dB
host4:      Iterations 10-12 | MCS 0-28 | Noise -12 to 4 dB
```

**1. Configure machines and parallel mode in `run_sl_test_config.sh`:**

```bash
# In the BLER profile section, configure machines for distributed testing
elif [[ $test_profile == "bler" ]]; then
    # Enable parallel mode
    parallel_mode=true
    
    # BLER test configuration
    enabled_tests=(
        rfsim_slmode1_bler_test_on_local_host
    )
    
    # Machine configuration for distributed testing
    # List machines for parallel testing. Iterations are split evenly.
    bler_hosts=(
        host1
        host2
        host3
        host4
    )
    # For 2 machines: bler_hosts=(host1 host2)
    # For 1 machine:  bler_hosts=(localhost)
    
    num_repeat=12             # Total iterations
    mcs_array=($(seq 0 28))   # Full MCS range
    noise_power_array=($(seq -12 4))  # Noise sweep
```

**2. Launch distributed tests:**

The script automatically:
- Reads `bler_hosts` array from config
- Generates machine-specific config files (`run_sl_test_config_M1.sh`, etc.)
- Distributes iterations evenly (12 iterations ÷ 4 machines = 3 per machine)
- Copies configs to remote machines via SCP
- Launches tests in parallel

```bash
cd ~/ci_script
./run_sl_test.sh
```

**Expected output:**
```
==========================================
Parallel Mode Detected
==========================================
Found 4 machines in bler_hosts array

Machine Configuration:
  host1: Iterations 1-3 | MCS 0-28
  host2: Iterations 4-6 | MCS 0-28
  host3: Iterations 7-9 | MCS 0-28
  host4: Iterations 10-12 | MCS 0-28

Generating configs and launching tests...
Total iterations: 12
Per machine: 3

→ host1: iterations 1-3
→ host2: iterations 4-6
→ host3: iterations 7-9
→ host4: iterations 10-12
```

**4. Monitor progress across machines:**

Each machine logs independently to its own `~/openairinterface5g/test_<timestamp>/` directory.

**Use the monitoring script (recommended):**
```bash
# Check status of all machines with progress details
cd ~/ci_script
./check_test_status.sh

# Or watch continuously (updates every 30 seconds)
watch -n 30 '~/ci_script/check_test_status.sh'
```

The monitoring script shows:
- Which machines are running tests
- Current progress (test X / Y)
- Current MCS and noise power values
- Elapsed time and estimated remaining time
- Latest HARQ statistics

**Or SSH into remote machines to check logs directly:**
```bash
ssh l3 "tail -f ~/openairinterface5g/test_*/commands.txt"
ssh l4 "tail -f ~/openairinterface5g/test_*/commands.txt"
ssh l5 "tail -f ~/openairinterface5g/test_*/commands.txt"
```

**5. Expected completion time (parallel):**

- **4 machines:** ~2 hours
  - Each machine: 29 MCS × 17 noise × 3 iterations × 30s = 1,478 tests × 30s ≈ 12 hours
  - (Actual: ~2 hours due to internal parallelization)

## Collecting and Plotting BLER Results

After tests complete (locally or distributed), collect results from all machines and generate plots.

**Important:** The `process_and_fetch_results.sh` script automatically finds the **most recent** test directory on each machine using:
```bash
ls -dt ~/openairinterface5g/test_2026* | head -1
```
You don't need to specify the exact timestamp. The script processes whichever test ran most recently.

**Run the collection and plotting script:**

```bash
cd ~/ci_script
./process_and_fetch_results.sh
```

The script uses SSH timeouts and connection retries to handle network issues gracefully. If a remote machine is unavailable, it will skip that machine and continue processing others.

**Expected console output:**
```
==========================================
BLER Results - Process & Plot
==========================================

Detected 4 machines from config

Machine Configuration:
  host1: Iterations 1-3 | MCS 0-28
  host2: Iterations 4-6 | MCS 0-28
  host3: Iterations 7-9 | MCS 0-28
  host4: Iterations 10-12 | MCS 0-28

Results will be saved to: ~/openairinterface5g/bler_results_<timestamp>

==========================================
Processing host1 - MCS 0,4
==========================================
  → SSHing to host1 to process logs...
  Test directory: ~/openairinterface5g/test_<timestamp>
  Processing logs...
  ✓ Generated: bler_host1.csv (234 lines)
  ✓ Generated: bler_host1_pc5_rx.csv (234 lines)
  Fetching CSVs...
  ✓ Fetched: bler_host1.csv
  ✓ Fetched: bler_host1_pc5_rx.csv

[... host2, host3, host4 processing ...]

==========================================
Combining Results from All Machines
==========================================
Combining MAC BLER results...
✓ Combined MAC BLER: 936 data points
Combining PC5_RX BLER results...
✓ Combined PC5_RX BLER: 936 data points

==========================================
Extracting Nearby (RX) BLER Data
==========================================
→ Processing host1...
  Test directory: ~/openairinterface5g/test_<timestamp>
[... extraction continues ...]

Combining nearby BLER...
✓ Combined nearby BLER: 468 data points
Combining nearby LDPC...
✓ Combined nearby LDPC: 468 data points

==========================================
Skipping Syncref (TX) BLER Data
==========================================

==========================================
Extracting Syncref RX BLER Data
==========================================
[... extraction continues ...]

Combining syncref RX BLER...
✓ Combined syncref RX BLER: 468 data points
Combining syncref RX LDPC...
✓ Combined syncref RX LDPC: 468 data points

==========================================
Generating Focused Plots
==========================================
→ Nearby (UE Rx) 4-panel plot...
  ✓ nearby_bler_4panel.png
→ Syncref RX 4-panel plot...
  ✓ syncref_rx_bler_4panel.png

==========================================
✓ Complete!
==========================================
Location: ~/openairinterface5g/bler_results_<timestamp>

Generated plots:
-rw-rw-r-- 1 user user 234K <timestamp> nearby_bler_4panel.png
-rw-rw-r-- 1 user user 231K <timestamp> syncref_rx_bler_4panel.png

CSV files:
 32 CSV files generated

Opening plots...
✓ Done!
```

**Note:** `<timestamp>` is dynamically generated in `YYYYMMDD_HHMMSS` format when the script runs:
- **Test directories** (`test_<timestamp>`): Created by `run_sl_test.sh` when tests start
- **Results directories** (`bler_results_<timestamp>`): Created by `process_and_fetch_results.sh` when processing begins
- Each script run creates a new timestamped directory

### Script Workflow

1. **Display machine configuration**
   - Sources `run_sl_test_config.sh` to read `bler_hosts` array
   - Reads machine-specific config files (`run_sl_test_config_M*.sh`) for iteration ranges
   - Displays iteration assignments and MCS coverage

2. **Process MAC BLER logs** on each machine using `process_bler_local.py`:
   - Connects via SSH with 10s timeout and up to 3 retry attempts
   - Finds most recent `test_2026*` directory
   - Generates per-machine CSV files:
     - `bler_host1.csv` through `bler_host4.csv` (MAC BLER data)
     - `bler_host1_pc5_rx.csv` through `bler_host4_pc5_rx.csv` (PC5_RX BLER data)
   - Fetches only CSV files via SCP (not full logs)

3. **Combine MAC BLER data** from all machines:
   - Concatenates headers once, then appends data rows
   - Outputs:
     - `bler_combined.csv` - All MAC BLER data points
     - `bler_combined_pc5_rx.csv` - All PC5_RX BLER data points

4. **Extract Nearby (UE RX) BLER data** using `extract_bler.py`:
   - Processes each machine's test logs (on remote or local)
   - Extracts UE RX perspective: BLER and LDPC iterations
   - Combines into:
     - `nearby_bler_combined.csv` - Nearby UE BLER data
     - `nearby_ldpc_combined.csv` - Nearby UE LDPC iterations

5. **Skip Syncref TX data** (not needed for analysis)

6. **Extract Syncref RX BLER data** using `extract_bler.py`:
   - Processes each machine's test logs (on remote or local)
   - Extracts Syncref RX perspective: BLER and LDPC iterations
   - Combines into:
     - `syncref_rx_bler_combined.csv` - Syncref RX BLER data
     - `syncref_rx_ldpc_combined.csv` - Syncref RX LDPC iterations

7. **Generate focused 4-panel plots** using `plot_results.py`:
   - `nearby_bler_4panel.png` - Nearby UE RX performance
   - `syncref_rx_bler_4panel.png` - Syncref RX performance

8. **Open plots automatically** if `eog` (Eye of GNOME) is available

### View Results

```bash
cd ~/openairinterface5g/bler_results_<timestamp>

# View plots
eog nearby_bler_4panel.png syncref_rx_bler_4panel.png

# Inspect CSV data
head nearby_bler_combined.csv
```

### CSV File Formats

**MAC BLER CSVs** (`bler_combined.csv`, `bler_M*.csv`):
- Generated by `process_bler_local.py`
- Columns: `mcs`, `noise`, `snr`, `rounds_0`, `rounds_1`, `rounds_2`, `rounds_3`, `bler`

**PC5_RX BLER CSVs** (`bler_combined_pc5_rx.csv`, `bler_M*_pc5_rx.csv`):
- Generated by `process_bler_local.py`
- Same format as MAC BLER CSVs

**Nearby BLER CSVs** (`nearby_bler_combined.csv`, `nearby_bler_M*.csv`):
- Generated by `extract_bler.py`
- UE RX perspective with MAC BLER data

**Nearby LDPC CSVs** (`nearby_ldpc_combined.csv`, `nearby_ldpc_M*.csv`):
- Generated by `extract_bler.py`
- Columns include `avg_ldpc_iter` for decoder iteration analysis

**Syncref RX BLER CSVs** (`syncref_rx_bler_combined.csv`, `syncref_rx_bler_M*.csv`):
- Generated by `extract_bler.py`
- Syncref RX perspective with MAC BLER data

**Syncref RX LDPC CSVs** (`syncref_rx_ldpc_combined.csv`, `syncref_rx_ldpc_M*.csv`):
- Generated by `extract_bler.py`
- Columns include `avg_ldpc_iter` for decoder iteration analysis

### Results Directory Structure

```
~/openairinterface5g/bler_results_<timestamp>/
├── nearby_bler_4panel.png          # UE RX 4-panel plot
├── syncref_rx_bler_4panel.png      # Syncref RX 4-panel plot
├── bler_combined.csv               # Combined MAC BLER (all machines)
├── bler_combined_pc5_rx.csv        # Combined PC5_RX BLER (all machines)
├── nearby_bler_combined.csv        # UE RX BLER data (all machines)
├── nearby_ldpc_combined.csv        # UE RX LDPC data (all machines)
├── syncref_rx_bler_combined.csv    # Syncref RX BLER data (all machines)
├── syncref_rx_ldpc_combined.csv    # Syncref RX LDPC data (all machines)
├── bler_host1.csv                  # host1 MAC BLER data
├── bler_host1_pc5_rx.csv           # host1 PC5_RX BLER data
├── bler_host2.csv                  # host2 MAC BLER data
├── bler_host2_pc5_rx.csv           # host2 PC5_RX BLER data
├── bler_host3.csv                  # host3 MAC BLER data
├── bler_host3_pc5_rx.csv           # host3 PC5_RX BLER data
├── bler_host4.csv                  # host4 MAC BLER data
├── bler_host4_pc5_rx.csv           # host4 PC5_RX BLER data
├── nearby_bler_host1.csv           # host1 Nearby BLER
├── nearby_ldpc_host1.csv           # host1 Nearby LDPC
├── nearby_bler_host2.csv           # host2 Nearby BLER
├── nearby_ldpc_host2.csv           # host2 Nearby LDPC
├── nearby_bler_host3.csv           # host3 Nearby BLER
├── nearby_ldpc_host3.csv           # host3 Nearby LDPC
├── nearby_bler_host4.csv           # host4 Nearby BLER
├── nearby_ldpc_host4.csv           # host4 Nearby LDPC
├── syncref_rx_bler_host1.csv       # host1 Syncref RX BLER
├── syncref_rx_ldpc_host1.csv       # host1 Syncref RX LDPC
├── syncref_rx_bler_host2.csv       # host2 Syncref RX BLER
├── syncref_rx_ldpc_host2.csv       # host2 Syncref RX LDPC
├── syncref_rx_bler_host3.csv       # host3 Syncref RX BLER
├── syncref_rx_ldpc_host3.csv       # host3 Syncref RX LDPC
├── syncref_rx_bler_host4.csv       # host4 Syncref RX BLER
└── syncref_rx_ldpc_host4.csv       # host4 Syncref RX LDPC
```

## Understanding BLER Plots

Each 4-panel plot provides comprehensive performance analysis:

### Panel 1: PC5 MAC BLER vs SNR (Top Left)
- BLER curves for MCS > 7 (filtered for clarity)
- Modulation order ellipses: QPSK, 16-QAM, 64-QAM
- Shows how each MCS performs across SNR range
- Lower BLER = better performance
- SNR range: 5 to 25 dB

### Panel 2: HARQ Rounds Distribution (Top Right)
- Stacked bar chart showing HARQ retransmission behavior
- Filtered by MCS-specific SNR thresholds:
  - MCS 0-9: SNR = 9 dB
  - MCS 10-16: SNR = 15 dB
  - MCS 17-28: SNR = 24 dB
- Colors: Green (Round 0), Yellow (Round 1), Orange (Round 2), Red (Round 3)
- Higher Round 0 percentage = fewer retransmissions = better performance

### Panel 3: SNR Required for 10% BLER Target (Bottom Left)
- Shows minimum SNR needed for each MCS to achieve ≤10% BLER
- Useful for link budget calculations
- Higher MCS requires higher SNR (expected for higher order modulation)

### Panel 4: LDPC Decoder Iterations Heatmap (Bottom Right)
- Heatmap of average LDPC iterations vs (MCS, Noise Power)
- Color intensity: Yellow (low) → Red (high)
- More iterations = more challenging decoding (lower SNR)
- Noise power range: -12 dB and above

### Interpreting Results

- **Good performance:** Low BLER at target SNR, high Round 0 percentage, low LDPC iterations
- **Poor performance:** High BLER, many retransmissions (Rounds 1-3), high LDPC iterations
- **MCS selection:** Choose MCS where BLER ≤ 10% at your operating SNR

## Data Processing Flow

The `process_and_fetch_results.sh` script orchestrates three Python scripts in sequence:

1. **`process_bler_local.py`** - Initial MAC BLER extraction
   - Input: Raw test logs (`result_*.log` files)
   - Output: Machine-specific MAC BLER CSVs (`bler_M*.csv`, `bler_M*_pc5_rx.csv`)
   - Runs on each machine (local or remote via SSH)

2. **`extract_bler.py`** - Unified BLER and LDPC extraction
   - Input: Test log directories + log type (`nearby` or `syncref`)
   - Output: BLER and LDPC CSVs for specified UE type
   - Called twice:
     - `extract_bler.py ... nearby` → Extracts from `result_nearby_*.log`
     - `extract_bler.py ... syncref` → Extracts from `result_nrUE_syncref_*.log`

3. **`plot_results.py`** - Plot generation
   - Input: Combined CSV files
   - Output: 4-panel PNG plots
   - Called twice: once for "nearby", once for "syncref_rx"

## Manual Data Processing (Optional)

If you need to process logs manually without `process_and_fetch_results.sh`:

**1. Extract UE RX BLER data (Nearby UE perspective):**

```bash
cd ~/ci_script
python3 extract_bler.py \
    ~/openairinterface5g/test_<timestamp> \
    nearby_bler.csv \
    nearby_ldpc.csv \
    nearby
```

**2. Extract Syncref RX BLER data (Syncref UE perspective):**

```bash
python3 extract_bler.py \
    ~/openairinterface5g/test_<timestamp> \
    syncref_rx_bler.csv \
    syncref_rx_ldpc.csv \
    syncref
```

**3. Generate plots:**

```bash
python3 plot_results.py ~/openairinterface5g/bler_results_<timestamp> nearby
python3 plot_results.py ~/openairinterface5g/bler_results_<timestamp> syncref_rx
```

## BLER Test Configuration Options

Additional settings in `run_sl_test_config.sh` for BLER tests:

```bash
# External clock synchronization (USRP only)
use_external_clock=1              # 0=internal, 1=external clock source

# Standalone mode
use_sa=1                          # 0=disabled, 1=enabled

# Parallel mode
parallel_mode=true                # Enable distributed testing

# Log directory
base_log_dir="~/openairinterface5g"

# MCS and noise arrays
mcs_array=($(seq 0 28))           # Full MCS range
noise_power_array=($(seq -12 4))  # 17 noise levels

# Test duration and iterations
duration=30                       # Seconds per test
num_repeat=10                     # Iterations per (MCS, noise) combo

# Path loss (RFSIM)
ploss_db=30                       # dB
```

## Troubleshooting BLER Tests

### Test hangs or freezes
- Check for zombie processes: `pkill -9 nr-uesoftmodem`
- Verify disk space: `df -h ~/openairinterface5g`
- Check memory usage: `free -h`

### Missing BLER data in logs
- **CRITICAL:** Verify BLER instrumentation was enabled during build (see "Prerequisites" section above)
- Check if compiled with instrumentation:
  ```bash
  # Look for BLER instrumentation message in build output
  grep "BLER instrumentation: ENABLED" cmake_targets/log/nr-uesoftmodem.Rel15.txt
  
  # Or check if logging appears in test
  grep "PC5_RX_SUMMARY" ~/openairinterface5g/test_*/result_nearby_*.log
  ```
- Ensure test runs for full duration (85s default) to accumulate 100+ blocks
- Verify MCS and noise power are valid ranges
- If no logging appears, rebuild with instrumentation:
  ```bash
  ./build_oai --nrUE -w SIMU --ninja -c --bler-instrumentation
  ```

### CSV parsing errors
- Check log format matches expected pattern
- Verify Python dependencies: `pip3 install pandas matplotlib numpy scipy`
- Run extraction script manually with `-v` flag for verbose output

### Distributed test failures
- Verify SSH connectivity: `ssh l3 hostname`
- Check remote machine has OAI built: `ssh l3 "ls ~/openairinterface5g/cmake_targets/ran_build/build/nr-uesoftmodem"`
- Ensure `bler_hosts` array in config has correct hostnames: `bler_hosts=(l3 l4 l5 localhost)`
- Check remote disk space: `ssh l3 "df -h"`
- If SSH times out after 3 attempts, the script skips that machine and continues

### Connection timeouts
- SSH operations use 5-10 second timeouts with 3 retry attempts
- If a machine is unavailable, processing continues with remaining machines
- Check network connectivity: `ping l3`

### Plotting fails
- Verify CSV files exist: `ls ~/openairinterface5g/bler_results_*/nearby_bler_combined.csv`
- Check for sufficient data points: `wc -l *.csv`

### System-specific performance issues

If tests experience timing issues (TUN interface not ready, sync failures):

1. **Enable extended delays** in `run_sl_test_config.sh`:
   ```bash
   use_extended_delays=1  # Set to 1 for slower systems (VMs, older hardware)
   ```

2. Most modern systems should keep the default `use_extended_delays=0`
   
   # Run with custom config
   BLER_CONFIG_FILE=my_config.sh ./run_sl_test.sh
   ```

### Missing Python libraries
- Install missing Python libraries: `pip3 install matplotlib scipy`

### Remote machine not starting tests
- **CRITICAL:** Verify BLER instrumentation is compiled on remote machine:
  ```bash
  ssh l3 "grep 'BLER instrumentation: ENABLED' ~/openairinterface5g/cmake_targets/log/nr-uesoftmodem.Rel15.txt"
  ```
- If not enabled, rebuild on remote machine:
  ```bash
  ssh l3 "cd ~/openairinterface5g && ./build_oai --nrUE -w SIMU --ninja -c --bler-instrumentation"
  ```
- Check if config was copied: `ssh l3 "ls ~/ci_script/run_sl_test_config_host*.sh"`
- Verify config is executable: `ssh l3 "chmod +x ~/ci_script/run_sl_test_config_host1.sh"`
- Manually trigger on remote: `ssh l3 "cd ~/ci_script && BLER_CONFIG_FILE=run_sl_test_config_host1.sh ./run_sl_test.sh"`

### Process script shows wrong MCS display
- Note: The script displays hardcoded MCS strings (e.g., "MCS 0,4") for visual separation only
- Actual processing: All machines test the full MCS range (0-28) as configured
- The displayed MCS values don't affect data collection or processing

## Back to Main Documentation

Return to [README_sl_test.md](README_sl_test.md) for other test types and general framework documentation.
