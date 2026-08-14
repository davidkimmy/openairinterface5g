#!/bin/bash
#############################################################
# merge_runs.sh [-d <output_parent_dir>] <test_dir1> <test_dir2> ...
#
# Re-process several existing BLER run dirs TOGETHER (averaged) WITHOUT copying, renaming, or
# symlinking any log file. The folder names are passed straight to the extractors, which glob
# across all of them and emit one row per log -> plot_results.py averages by (mcs, snr).
#
# The original run dirs are read in place and left untouched. Output folder:
#   <output_parent_dir>/bler_results_merged_<timestamp>/
#   -d <output_parent_dir>  parent dir for the output (default: OAI_BASE_DIR, i.e. where the
#                           test_<ts> dirs live).
#
# This is a LOCAL, single-host merge (post-processing forces localhost-only).
#############################################################

BLER_SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PARENT_DIR="$(dirname "$BLER_SCRIPT_DIR")"

# Get OAI_BASE_DIR from the config, else default.
SL_TEST_CONFIG_FILE="$PARENT_DIR/run_sl_test_config.sh"
[[ -f "$SL_TEST_CONFIG_FILE" ]] && source "$SL_TEST_CONFIG_FILE" > /dev/null 2>&1
OAI_BASE_DIR="${OAI_BASE_DIR:-$HOME/openairinterface5g}"

usage() {
    echo "Usage: $(basename "$0") [-d <output_parent_dir>] <test_dir1> [<test_dir2> ...]"
    echo "  Re-process several BLER run dirs together (averaged). Nothing is copied/renamed."
    echo "  Dirs may be absolute or relative to $OAI_BASE_DIR (e.g. test_20260724_171422)."
    echo "  -d <output_parent_dir>  parent dir for bler_results_merged_<ts> (default: $OAI_BASE_DIR)."
}

OUT_PARENT="$OAI_BASE_DIR"          # default output parent
while getopts "d:h" opt; do
    case $opt in
        d) OUT_PARENT="$OPTARG" ;;
        h) usage; exit 0 ;;
        *) usage; exit 1 ;;
    esac
done
shift $((OPTIND - 1))

if [[ $# -lt 1 ]]; then
    usage
    exit 1
fi

# Resolve each arg to an absolute dir and join with ':' for the extractors.
dirs=()
for RUN in "$@"; do
    # allow bare dir names relative to OAI_BASE_DIR
    [[ -d "$RUN" ]] || RUN="$OAI_BASE_DIR/$RUN"
    if [[ ! -d "$RUN" ]]; then
        echo "ERROR: not a directory: $RUN"
        exit 1
    fi
    dirs+=("$(cd "$RUN" && pwd)")
done
MERGE_DIRS=$(IFS=:; echo "${dirs[*]}")

echo "Merging ${#dirs[@]} run dir(s):"
printf '  %s\n' "${dirs[@]}"
echo "Output parent: $OUT_PARENT"
echo ""

# Hand the folder list to process_and_fetch: it reads the dirs directly (localhost only) and
# the extractors average across all of them. No 'latest' change, no copied/renamed files.
BLER_MERGE_DIRS="$MERGE_DIRS" BLER_MERGE_OUT_PARENT="$OUT_PARENT" \
    bash "$BLER_SCRIPT_DIR/process_and_fetch_results.sh"
