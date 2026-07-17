#!/bin/bash
# Detailed status check for full BLER tests on all machines
# Reads bler_hosts from config file and shows detailed progress

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
SL_TEST_CONFIG_FILE="${SCRIPT_DIR}/../run_sl_test_config.sh"

# Source config to get bler_hosts array
if [[ -f "$SL_TEST_CONFIG_FILE" ]]; then
    source "$SL_TEST_CONFIG_FILE" 2>/dev/null
else
    echo "ERROR: Config file not found: $SL_TEST_CONFIG_FILE"
    exit 1
fi

# Check if bler_hosts is defined
if [[ -z "${bler_hosts[@]}" ]]; then
    echo "ERROR: bler_hosts array not defined in config"
    echo "Define it in $SL_TEST_CONFIG_FILE:"
    echo "  bler_hosts=(host1 host2 host3 localhost)"
    exit 1
fi

echo "=========================================="
echo "Full BLER Test Status - $(date '+%Y-%m-%d %H:%M:%S')"
echo "=========================================="
echo ""

# Helper function to retrieve log files and CSV data (local or remote SSH)
# Sets the following variables for caller (without 'local' keyword):
#   - log_header: First 200 lines of log file
#   - log_content: Last 5000 lines of log file
#   - csv_lines: Number of lines in CSV file
#   - csv_first: First data row from CSV file
#   - csv_last: Last row from CSV file
#   - test_start_str: Test name from first CSV row (contains timestamp)
#   - test_end_str: Test name from last CSV row (contains timestamp)
#   - log_mtime: Log file modification time (epoch seconds)
fetch_log_content() {
    local log_file=$1
    local is_remote=$2
    local host=$3

    # Get raw log content (head for config, tail for current status)
    if [[ "$is_remote" == "true" ]]; then
        # Get config from beginning of log
        log_header=$(ssh -o ForwardX11=no -o ConnectTimeout=5 "$host" "head -200 $log_file 2>/dev/null" 2>/dev/null)
        # Get recent activity
        log_content=$(ssh -o ForwardX11=no -o ConnectTimeout=5 "$host" "tail -5000 $log_file 2>/dev/null" 2>/dev/null)
        # Get completed test count from CSV file
        csv_file="\${HOME}/openairinterface5g/latest/test_summary_*.csv"
        csv_lines=$(ssh -o ForwardX11=no -o ConnectTimeout=5 "$host" "wc -l $csv_file 2>/dev/null | tail -1 | awk '{print \$1}'" 2>/dev/null)
        # Get test start and end time from CSV (more reliable than log timestamps)
        # CSV has format: TestName,Itrn,NumHosts,MCS,Runtime,... First data row after header
        csv_first=$(ssh -o ForwardX11=no -o ConnectTimeout=5 "$host" "head -2 $csv_file 2>/dev/null | tail -1" 2>/dev/null)
        csv_last=$(ssh -o ForwardX11=no -o ConnectTimeout=5 "$host" "tail -1 $csv_file 2>/dev/null" 2>/dev/null)
        # Extract test name which contains timestamp: rfsim_slmode1_bler_test_on_local_host,iter1_noise-12_mcs0
        test_start_str=$(echo "$csv_first" | cut -d',' -f1)
        test_end_str=$(echo "$csv_last" | cut -d',' -f1)
        # Get file modification time as current time
        log_mtime=$(ssh -o ForwardX11=no -o ConnectTimeout=5 "$host" "stat -c %Y $log_file 2>/dev/null" 2>/dev/null)
    else
        log_header=$(head -200 "$log_file" 2>/dev/null)
        log_content=$(tail -5000 "$log_file" 2>/dev/null)
        # Get completed test count from CSV file
        csv_lines=$(wc -l ${HOME}/openairinterface5g/latest/test_summary_*.csv 2>/dev/null | tail -1 | awk '{print $1}')
        # Get test start and end time from CSV
        csv_first=$(head -2 ${HOME}/openairinterface5g/latest/test_summary_*.csv 2>/dev/null | tail -1)
        csv_last=$(tail -1 ${HOME}/openairinterface5g/latest/test_summary_*.csv 2>/dev/null)
        test_start_str=$(echo "$csv_first" | cut -d',' -f1)
        test_end_str=$(echo "$csv_last" | cut -d',' -f1)
        log_mtime=$(stat -c %Y "$log_file" 2>/dev/null)
    fi
}

# Helper function to extract test configuration from log header
# Parses the configuration parameters from the test log header and calculates
# iteration count and total test count.
# Sets the following variables for caller (without 'local' keyword):
#   - iter_count: Number of iterations assigned to this host
#   - total_tests: Total number of tests (num_mcs × num_noise × iter_count)
#   - num_mcs: Number of MCS values being tested
#   - num_noise: Number of noise/SNR values being tested
extract_test_config() {
    local log_header_content=$1
    local start_iter=$2
    local end_iter=$3

    # Extract actual config used from log header
    actual_iterations=$(echo "$log_header_content" | grep "Iterations:" | head -1 | sed -n 's/.*Iterations: \([0-9]\+\) to \([0-9]\+\) (\([0-9]\+\) iterations).*/\3/p')
    actual_mcs_count=$(echo "$log_header_content" | grep "MCS:" | head -1 | sed -n 's/.*MCS: \([0-9]\+\) values.*/\1/p')
    actual_noise_count=$(echo "$log_header_content" | grep "SNR:" | head -1 | sed -n 's/.*SNR: \([0-9]\+\) values.*/\1/p')
    actual_total_tests=$(echo "$log_header_content" | grep "Total tests:" | head -1 | sed -n 's/.*Total tests: \([0-9]\+\).*/\1/p')

    # Override with actual values if found in log
    if [[ -n "$actual_iterations" && "$actual_iterations" != "0" ]]; then
        iter_count=$actual_iterations
        total_tests=${actual_total_tests:-$total_tests}
    fi

    if [[ -n "$actual_mcs_count" ]]; then
        num_mcs=$actual_mcs_count
    fi

    if [[ -n "$actual_noise_count" ]]; then
        num_noise=$actual_noise_count
    fi

    # Calculate test progress (use log config if available, otherwise estimate)
    if [[ -z "$num_mcs" || "$num_mcs" == "0" ]]; then
        num_mcs=${#mcs_array[@]}
        [[ -z "$num_mcs" || "$num_mcs" == "0" ]] && num_mcs=29
    fi

    if [[ -z "$num_noise" || "$num_noise" == "0" ]]; then
        num_noise=${#noise_power_array[@]}
        [[ -z "$num_noise" || "$num_noise" == "0" ]] && num_noise=17
    fi

    # Fix iteration calculation (start might be > end if host has no iterations)
    # But if we have actual values from log, use those
    if [[ -n "$iter_count" && "$iter_count" != "0" && -n "$total_tests" ]]; then
        # Use values extracted from log header
        :
    elif [[ $start_iter -gt $end_iter ]]; then
        iter_count=0
        total_tests=0
    else
        iter_count=$((end_iter - start_iter + 1))
        total_tests=$((num_mcs * num_noise * iter_count))
    fi
}

# Helper function to extract current test status from log content
# Parses the log to find current MCS, noise level, and test progress.
# Sets the following variables for caller (without 'local' keyword):
#   - current_mcs: MCS value of currently running test
#   - current_noise: Noise/SNR value of current test
#   - completed_tests: Number of tests completed (csv_lines - 1)
#   - current_test_num: Test number currently running (completed + 1)
extract_current_test_status() {
    local log_content_data=$1
    local csv_line_count=$2

    # Extract current test info from recent lines
    # Try to get from "Starting gNB" line which has both MCS and noise
    gnb_start_line=$(echo "$log_content_data" | grep -E "Starting gNB with noise=" | tail -1)
    current_mcs=$(echo "$gnb_start_line" | grep -oP 'MCS=\K[0-9]+')

    # Handle format variations: "noise=-11dB" or "noise=-11" (try dB format first)
    current_noise=$(echo "$gnb_start_line" | sed -n 's/.*noise=\(-\?[0-9]\+\)dB.*/\1/p')

    # If no dB suffix, try without
    if [[ -z "$current_noise" ]]; then
        current_noise=$(echo "$gnb_start_line" | grep -oP 'noise=\K-?[0-9]+')
    fi

    # If not found in gNB line, try other sources
    if [[ -z "$current_mcs" ]]; then
        current_mcs=$(echo "$log_content_data" | grep -E "Starting (Remote UE|Relay UE) with.*MCS=" | tail -1 | grep -oP 'MCS=\K[0-9]+')
    fi

    # Get latest HARQ stats
    uu_harq=$(echo "$log_content_data" | grep "UU_HARQ_SUCCESS" | tail -1)
    pc5_harq=$(echo "$log_content_data" | grep "PC5_HARQ_SUCCESS" | tail -1)

    # Get MCS from HARQ stats if not found in startup
    if [[ -z "$current_mcs" && -n "$uu_harq" ]]; then
        current_mcs=$(echo "$uu_harq" | grep -oP 'mcs=\K[0-9]+')
    fi

    # Get noise from channel model if not found
    if [[ -z "$current_noise" ]]; then
        current_noise=$(echo "$log_content_data" | grep "noise_power_dB" | tail -1 | grep -oP 'noise_power_dB.*\K-?[0-9]+' | tail -1)
    fi

    # Get completed test count from CSV (subtract 1 for header line)
    if [[ -n "$csv_line_count" && "$csv_line_count" -gt 1 ]]; then
        completed_tests=$((csv_line_count - 1))
    else
        completed_tests=0
    fi

    # Calculate current test number
    if [[ $completed_tests -gt 0 ]]; then
        # Currently running = completed + 1
        current_test_num=$((completed_tests + 1))
    else
        # No completed tests yet, must be test 1
        current_test_num=1
    fi
}

# Helper: Calculate progress metrics (elapsed time, remaining time estimates)
# Uses CSV runtime data to calculate elapsed time and estimates remaining time
# based on average test completion time.
#
# Globals used:
#   - total_tests: total number of tests to run
#   - completed_tests: number of tests completed
#   - duration: estimated test duration in seconds
#
# Globals modified:
#   - current_test_num: current test number (completed + 1)
#   - elapsed_sec, elapsed_min, elapsed_hr: elapsed time in various units
#   - remaining_sec, remaining_min, remaining_hr: remaining time estimates
#   - avg_test_time: average time per test in seconds
#
# Parameters:
#   $1 - csv_lines: number of lines in CSV file
#   $2 - csv_file: path to CSV file
#   $3 - is_remote: "true" if remote host, "false" if local
#   $4 - host: hostname (used if is_remote=true)
calculate_progress_metrics() {
    local csv_lines=$1
    local csv_file=$2
    local is_remote=$3
    local host=$4

    # Calculate current test number
    if [[ $completed_tests -gt 0 ]]; then
        # Currently running = completed + 1
        current_test_num=$((completed_tests + 1))
    else
        # No completed tests yet, must be test 1
        current_test_num=1
    fi

    # Calculate elapsed time from CSV runtime sum
    if [[ -n "$csv_lines" && "$csv_lines" -gt 1 ]]; then
        # Sum all runtimes from CSV (column 5, format: "119s")
        if [[ "$is_remote" == "true" ]]; then
            elapsed_sec=$(ssh -o ForwardX11=no -o ConnectTimeout=5 $host "tail -n +2 $csv_file 2>/dev/null | cut -d',' -f5 | sed 's/s$//' | awk '{sum+=\$1} END {print sum}'" 2>/dev/null)
        else
            elapsed_sec=$(tail -n +2 ${HOME}/openairinterface5g/latest/test_summary_*.csv 2>/dev/null | cut -d',' -f5 | sed 's/s$//' | awk '{sum+=$1} END {print sum}')
        fi

        # Add current test time (assume it's been running for duration/2 on average)
        if [[ -n "$elapsed_sec" && "$elapsed_sec" -gt 0 ]]; then
            # Add partial time for current test
            elapsed_sec=$((elapsed_sec + ${duration:-70} / 2))
        else
            # Fallback: estimate from completed tests
            elapsed_sec=$((completed_tests * ${duration:-70}))
        fi

        elapsed_min=$(awk "BEGIN {printf \"%.1f\", $elapsed_sec / 60}" 2>/dev/null || echo "0")
        elapsed_hr=$(awk "BEGIN {printf \"%.1f\", $elapsed_min / 60}" 2>/dev/null || echo "0")
    else
        elapsed_sec=0
        elapsed_min=0
        elapsed_hr=0
    fi

    # Calculate remaining time estimate
    if [[ $completed_tests -gt 0 && $elapsed_sec -gt 0 ]]; then
        avg_test_time=$(awk "BEGIN {printf \"%.2f\", $elapsed_sec / $completed_tests}" 2>/dev/null || echo "70")
        remaining_tests=$((total_tests - completed_tests))
        remaining_sec=$(awk "BEGIN {printf \"%.0f\", $remaining_tests * $avg_test_time}" 2>/dev/null || echo "0")
        remaining_min=$(awk "BEGIN {printf \"%.1f\", $remaining_sec / 60}" 2>/dev/null || echo "0")
        remaining_hr=$(awk "BEGIN {printf \"%.1f\", $remaining_min / 60}" 2>/dev/null || echo "0")
    else
        avg_test_time=${duration:-70}
        remaining_sec=$((total_tests * avg_test_time))
        remaining_min=$(awk "BEGIN {printf \"%.1f\", $remaining_sec / 60}" 2>/dev/null || echo "0")
        remaining_hr=$(awk "BEGIN {printf \"%.1f\", $remaining_min / 60}" 2>/dev/null || echo "0")
    fi
}

# Helper function to parse HARQ statistics from log content
# Extracts HARQ success/failure counts and calculates BLER metrics.
# Sets the following variables for caller (without 'local' keyword):
#   - uu_harq: Raw Uu HARQ log line
#   - uu_round: Uu HARQ round number
#   - uu_mcs: Uu MCS value
#   - pc5_harq: Raw PC5 HARQ log line
#   - pc5_r0: PC5 round 0 count (successes)
#   - pc5_r1: PC5 round 1 count (retransmissions)
#   - pc5_bler: PC5 BLER percentage
parse_harq_statistics() {
    local log_content_data=$1

    # Get latest HARQ stats
    uu_harq=$(echo "$log_content_data" | grep "UU_HARQ_SUCCESS" | tail -1)
    pc5_harq=$(echo "$log_content_data" | grep "PC5_HARQ_SUCCESS" | tail -1)

    # Extract BLER/success rates from HARQ
    if [[ -n "$uu_harq" ]]; then
        uu_round=$(echo "$uu_harq" | grep -oP 'round=\K[0-9]+')
        uu_mcs=$(echo "$uu_harq" | grep -oP 'mcs=\K[0-9]+')
    fi

    if [[ -n "$pc5_harq" ]]; then
        pc5_r0=$(echo "$pc5_harq" | grep -oP 'cumul_r0=\K[0-9]+')
        pc5_r1=$(echo "$pc5_harq" | grep -oP 'r1=\K[0-9]+')
        if [[ -n "$pc5_r0" && -n "$pc5_r1" ]]; then
            local pc5_total=$((pc5_r0 + pc5_r1))
            if [[ $pc5_total -gt 0 ]]; then
                pc5_bler=$(awk "BEGIN {printf \"%.2f\", 100 * $pc5_r1 / $pc5_total}" 2>/dev/null || echo "0.00")
            else
                pc5_bler="0.00"
            fi
        fi
    fi
}

# Display formatted status output box
# Parameters:
#   $1 - host_id: Machine identifier (e.g., "host1", "host2")
#   $2 - host: Hostname or IP address
#   $3 - start_iter: Starting iteration number for this host
#   $4 - end_iter: Ending iteration number for this host
# Uses variables from calling scope:
#   - total_tests, current_test_num, completed_tests, current_mcs, current_noise
#   - elapsed_hr, elapsed_min, remaining_hr, remaining_min, avg_test_time
#   - uu_harq, uu_round, uu_mcs, pc5_harq, pc5_r0, pc5_r1, pc5_bler
display_status_output() {
    local host_id=$1
    local host=$2
    local start_iter=$3
    local end_iter=$4

    # Display status
    echo "  ╔═══════════════════════════════════════════════════════════╗"
    echo "  ║ Machine: $host_id ($host)"
    echo "  ║ Iterations: $start_iter-$end_iter | Total Tests: $total_tests"
    echo "  ╠═══════════════════════════════════════════════════════════╣"

    if [[ $total_tests -eq 0 ]]; then
        echo "  ║ Status: No tests assigned to this host"
        echo "  ║ Progress: N/A"
    elif [[ -n "$current_mcs" ]]; then
        echo "  ║ Current Test: ${current_test_num}/$total_tests"
        if [[ -n "$current_noise" ]]; then
            echo "  ║ MCS: $current_mcs | Noise: ${current_noise}dB"
        else
            echo "  ║ MCS: $current_mcs | Noise: (detecting...)"
        fi
        if [[ $total_tests -gt 0 && $completed_tests -gt 0 ]]; then
            printf "  ║ Progress: %d%% (%d/%d tests completed)\n" $(( (completed_tests * 100) / total_tests )) $completed_tests $total_tests
        elif [[ $total_tests -gt 0 ]]; then
            printf "  ║ Progress: %d%% (test in progress)\n" $(( ((current_test_num - 1) * 100) / total_tests ))
        fi
    else
        echo "  ║ Status: Initializing..."
        echo "  ║ Progress: 0% (0/$total_tests tests)"
    fi

    echo "  ╠═══════════════════════════════════════════════════════════╣"

    if [[ $(awk "BEGIN {print ($elapsed_hr >= 1) ? 1 : 0}") -eq 1 ]]; then
        printf "  ║ Elapsed: %.1f hours\n" $elapsed_hr
    else
        printf "  ║ Elapsed: %.1f minutes\n" $elapsed_min
    fi

    if [[ $completed_tests -gt 0 ]]; then
        if [[ $(awk "BEGIN {print ($remaining_hr >= 1) ? 1 : 0}") -eq 1 ]]; then
            printf "  ║ Remaining: ~%.1f hours\n" $remaining_hr
        else
            printf "  ║ Remaining: ~%.1f minutes\n" $remaining_min
        fi
        printf "  ║ Avg Test Time: %.1f seconds\n" $avg_test_time
    else
        printf "  ║ Estimated Time: ~%.1f hours\n" $remaining_hr
    fi

    echo "  ╠═══════════════════════════════════════════════════════════╣"

    if [[ -n "$uu_harq" ]]; then
        echo "  ║ Uu HARQ: Round=$uu_round MCS=$uu_mcs (SUCCESS)"
    fi

    if [[ -n "$pc5_harq" && -n "$pc5_r0" ]]; then
        echo "  ║ PC5 HARQ: R0=$pc5_r0 R1=$pc5_r1 BLER=${pc5_bler}%"
    fi

    echo "  ╚═══════════════════════════════════════════════════════════╝"
}

# Orchestrator function that coordinates status retrieval and display
get_detailed_status() {
    local log_file=$1
    local is_remote=$2
    local host=$3
    local host_id=$4
    local start_iter=$5
    local end_iter=$6

    # Step 1: Fetch logs (sets: log_header, log_content, csv_lines, csv_first, csv_last, etc.)
    fetch_log_content "$log_file" "$is_remote" "$host"

    # Early exit if no content
    if [[ -z "$log_content" ]]; then
        echo "  ⏸  Not started yet or log file missing"
        return
    fi

    # Step 2: Extract config (sets: iter_count, total_tests, num_mcs, num_noise)
    extract_test_config "$log_header" "$start_iter" "$end_iter"

    # Step 3: Extract current test status (sets: current_mcs, current_noise, completed_tests, current_test_num)
    extract_current_test_status "$log_content" "$csv_lines"

    # Step 4: Calculate progress metrics (sets: elapsed_sec/min/hr, remaining_sec/min/hr, avg_test_time)
    csv_file="\${HOME}/openairinterface5g/latest/test_summary_*.csv"
    calculate_progress_metrics "$csv_lines" "$csv_file" "$is_remote" "$host"

    # Step 5: Parse HARQ statistics (sets: uu_harq, uu_round, uu_mcs, pc5_harq, pc5_r0, pc5_r1, pc5_bler)
    parse_harq_statistics "$log_content"

    # Step 6: Display output
    display_status_output "$host_id" "$host" "$start_iter" "$end_iter"
}

# Calculate iterations per host
num_hosts=${#bler_hosts[@]}

# Check status for each host
host_idx=0
for hostname in "${bler_hosts[@]}"; do
    host_idx=$((host_idx + 1))
    host_id="host${host_idx}"

    # Try to load worker config if it exists
    if [[ "$hostname" == "localhost" || "$hostname" == "local" ]]; then
        host_config_file="${SCRIPT_DIR}/../run_sl_test_config_worker_local.sh"
    else
        host_config_file="${SCRIPT_DIR}/../run_sl_test_config_worker_${hostname}.sh"
    fi

    if [[ -f "$host_config_file" ]]; then
        # Source worker config to get iteration_start and iteration_end
        source "$host_config_file" 2>/dev/null
        start=${iteration_start:-1}
        end=${iteration_end:-0}
    else
        # Fallback to distributed calculation
        iterations_per_host=$((${num_repeat:-12} / num_hosts))
        start=$(( (host_idx - 1) * iterations_per_host + 1 ))
        end=$(( host_idx * iterations_per_host ))
        if [[ $host_idx -eq $num_hosts ]]; then
            remainder=$((${num_repeat:-12} % num_hosts))
            end=$((end + remainder))
        fi
    fi

    if [[ "$hostname" == "localhost" || "$hostname" == "local" ]]; then
        # Check localhost
        if ps aux | grep "bash run_sl_test.sh" | grep -v grep > /dev/null 2>&1; then
            get_detailed_status "${OAI_BASE_DIR:-$HOME/openairinterface5g}/bler_${host_id}.log" "false" "" "$host_id" "$start" "$end"
        else
            echo "  ╔═══════════════════════════════════════════════════════════╗"
            echo "  ║ Machine: $host_id ($hostname)"
            echo "  ║ Status: NOT RUNNING"
            echo "  ╚═══════════════════════════════════════════════════════════╝"
        fi
    else
        # Check remote host
        if timeout 5 ssh -o ForwardX11=no -o ConnectTimeout=3 $hostname "ps aux | grep 'bash run_sl_test.sh' | grep -v grep" > /dev/null 2>&1; then
            # Use OAI_BASE_DIR if defined in config, otherwise default
            remote_oai_dir="\${OAI_BASE_DIR:-\$HOME/openairinterface5g}"
            get_detailed_status "$remote_oai_dir/bler_${host_id}.log" "true" "$hostname" "$host_id" "$start" "$end"
        else
            echo "  ╔═══════════════════════════════════════════════════════════╗"
            echo "  ║ Machine: $host_id ($hostname)"
            echo "  ║ Status: NOT RUNNING or UNREACHABLE"
            echo "  ╚═══════════════════════════════════════════════════════════╝"
        fi
    fi
    echo ""
done

echo "=========================================="
echo "Summary"
echo "=========================================="

num_mcs_vals=${#mcs_array[@]}
num_noise_vals=${#noise_power_array[@]}
[[ -z "$num_mcs_vals" || "$num_mcs_vals" == "0" ]] && num_mcs_vals=29
[[ -z "$num_noise_vals" || "$num_noise_vals" == "0" ]] && num_noise_vals=17

# Calculate iterations per host (for even distribution)
iterations_per_host=$((${num_repeat:-12} / num_hosts))

tests_per_host=$((num_mcs_vals * num_noise_vals * iterations_per_host))
total_all_hosts=$((num_mcs_vals * num_noise_vals * ${num_repeat:-12}))

echo "Test Distribution:"
host_idx=0
for hostname in "${bler_hosts[@]}"; do
    host_idx=$((host_idx + 1))
    start=$(( (host_idx - 1) * iterations_per_host + 1 ))
    end=$(( host_idx * iterations_per_host ))
    if [[ $host_idx -eq $num_hosts ]]; then
        remainder=$((${num_repeat:-12} % num_hosts))
        end=$((end + remainder))
        iter_count=$((end - start + 1))
        host_tests=$((num_mcs_vals * num_noise_vals * iter_count))
    else
        host_tests=$tests_per_host
    fi
    printf "  host%d (%-15s): Iterations %2d-%-2d | %d tests\n" $host_idx "$hostname" $start $end $host_tests
done

echo ""
echo "Test Parameters:"
echo "  • Total tests (all hosts): $total_all_hosts"
echo "  • MCS values: ${num_mcs_vals}"
echo "  • Noise values: ${num_noise_vals}"
echo "  • Iterations: ${num_repeat:-12}"
echo "  • Duration per test: ${duration:-70}s"
echo ""
echo "Monitor: watch -n 30 '$SCRIPT_DIR/check_test_status.sh'"
echo "=========================================="
