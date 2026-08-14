#!/usr/bin/env bash
# make_sl_code_sync.sh - copy only the locally changed files to one or more test hosts, so
# every host under test runs the same source. Newly created files bring their sub-directories along.
#
# Why this exists: the SL tests span several hosts (syncref/local, nearby, relay_ue, remote_ue, ...) and
# each host has its own checkout and its own build. Building on one host and not the others produces a
# split-brain measurement that looks like an RF or one-direction problem but is only a stale binary.
# That has cost real debugging time on this branch more than once.
#
# The remote workspace is always $HOME/openairinterface5g, resolved on the REMOTE side, so the remote
# login name does not have to match the local one. The login name comes from the ssh host alias
# (~/.ssh/config), i.e. this script never guesses a user.
#
# Usage:
#   make_sl_code_sync.sh [options] <host> [host ...]
#
# Options:
#   -r, --ref <ref>     Compare against this git ref (default: HEAD, i.e. uncommitted changes).
#                       Use a commit sha to ship everything since that commit.
#   -u, --untracked     Also copy new untracked files. Test output (test_*/, latest, *.log) is always
#                       excluded. Off by default so a stray results directory is never shipped.
#   -t, --from-tests    Derive the host list from the tests selected in run_sl_test_config.sh instead of
#                       taking it on the command line. This is the default when no host is given, so the
#                       hosts you sync are always the hosts the next run will use. Reads pilot_tests, then
#                       each selected test function's *_host_name= assignments, and drops "local".
#   -n, --dry-run       List what would be copied, transfer nothing.
#   -b, --build         After a successful sync, run the build on every host that received files.
#                       Refuses unless all hosts agree on BOTH the base commit AND the build script
#                       ($HOME/make_nr_oai_USRP.sh, compared by sha256) - hosts that differ in either
#                       do not produce comparable binaries. Builds run in parallel, one per host.
#                       Off by default: a build is long and takes the host out of service.
#       --remote-dir D  Override the remote workspace (default: <remote $HOME>/openairinterface5g).
#   -h, --help          This text.
#
# Examples:
#   make_sl_code_sync.sh -n remote_ue            # see what would go
#   make_sl_code_sync.sh remote_ue               # uncommitted changes -> remote_ue
#   make_sl_code_sync.sh -u remote_ue a32        # include new files, two hosts
#   make_sl_code_sync.sh -r a46a11dba8 remote_ue # everything since that commit
#   make_sl_code_sync.sh                         # hosts taken from the selected tests
#   make_sl_code_sync.sh -n -t                   # show which hosts those are, copy nothing
#   make_sl_code_sync.sh -b                      # sync the selected tests' hosts, then build on each

set -euo pipefail

REF="HEAD"
INCLUDE_UNTRACKED=0
FROM_TESTS=0
DRY_RUN=0
BUILD=0
REMOTE_DIR_OVERRIDE=""
HOSTS=()

WORKSPACE_NAME="openairinterface5g"
# The build script every host is expected to share, relative to that host's $HOME. This is what the
# interactive "makeoai" alias runs; the alias itself cannot be used over ssh (see the build stage).
MAKE_SCRIPT_REL="${MAKE_SCRIPT_REL:-make_nr_oai_USRP.sh}"

# Per-host state gathered during the sync, consumed by the build stage.
SYNCED=()
declare -A HEAD_OF=()
declare -A MAKEHASH_OF=()
BUILD_LOG_DIR=""

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
info() { printf '%s\n' "$*"; }

usage() { sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//; $d'; }

while (($#)); do
  case "$1" in
    -r|--ref)        [[ $# -ge 2 ]] || die "--ref needs a value"; REF="$2"; shift 2;;
    -u|--untracked)  INCLUDE_UNTRACKED=1; shift;;
    -t|--from-tests) FROM_TESTS=1; shift;;
    -n|--dry-run)    DRY_RUN=1; shift;;
    -b|--build)      BUILD=1; shift;;
    --remote-dir)    [[ $# -ge 2 ]] || die "--remote-dir needs a value"; REMOTE_DIR_OVERRIDE="$2"; shift 2;;
    -h|--help)       usage; exit 0;;
    -*)              die "unknown option: $1 (try --help)";;
    *)               HOSTS+=("$1"); shift;;
  esac
done

# ---- adaptive host discovery ----------------------------------------------------------------------
# Retyping the host list is how a 3-host run gets synced with 2-host arguments, which produces exactly the
# split-brain this script exists to prevent. The test definitions already state their hosts, so derive them:
# run_sl_test_config.sh names the selected tests (pilot_tests), each test function in run_sl_test.sh assigns
# *_host_name="local" or =$SOME_HOST, and the SOME_HOST values are defined at the top of run_sl_test.sh.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
CFG="$SCRIPT_DIR/run_sl_test_config.sh"
RUNNER="$SCRIPT_DIR/run_sl_test.sh"

# Items of a bash array literal, one per line, comments stripped.
array_items() { # file, array name
  awk -v name="$2" '
    index($0, name "=(") == 1 { f = 1; sub(/^[^(]*\(/, "") }
    f { line = $0; sub(/#.*/, "", line)
        if (line ~ /\)/) { sub(/\).*/, "", line); print line; exit }
        print line }' "$1" | tr ' \t' '\n\n' | grep -v '^$'
}

# The tests pilot_tests selects, as names. Entries look like slmode2_basic_tests[2,3] or uu_basic_tests[3].
selected_tests() {
  local entry name idxs i
  while IFS= read -r entry; do
    name="${entry%%[*}"; idxs="${entry#*[}"; idxs="${idxs%]}"
    [[ -n $name && $entry == *'['* ]] || continue
    local items=(); mapfile -t items < <(array_items "$CFG" "$name")
    # Two index forms, both accepted by run_sl_test.sh's resolve_test_entries(): "[a:b]" is an
    # INCLUSIVE range, "[a]" / "[a,b]" is a list. Handling only the list form made a range entry
    # expand to nothing and the host list come back empty, which reads as "no remote host to sync".
    if [[ $idxs == *:* ]]; then
      local start="${idxs%%:*}" end="${idxs##*:}"
      for ((i = start; i <= end; i++)); do
        [[ -n ${items[$i]:-} ]] && printf '%s\n' "${items[$i]}"
      done
    else
      for i in ${idxs//,/ }; do
        [[ -n ${items[$i]:-} ]] && printf '%s\n' "${items[$i]}"
      done
    fi
  done < <(array_items "$CFG" "pilot_tests")
}

# Hosts one test function talks to. "local" is us and is skipped; $N positionals come from the caller and
# cannot be resolved statically, so they are reported rather than silently dropped.
hosts_for_test() { # test name
  local raw var val
  while IFS= read -r raw; do
    raw="${raw//\"/}"
    case "$raw" in
      local|localhost) continue;;
      \$[0-9]*) printf '%s\n' "UNRESOLVED:$raw"; continue;;
      \$*) var="${raw#\$}"
           val="$(grep -m1 -E "^${var}=" "$RUNNER" | cut -d= -f2- | tr -d '"'"'"' ')"
           printf '%s\n' "${val:-UNRESOLVED:$raw}";;
      *) printf '%s\n' "$raw";;
    esac
  done < <(awk -v fn="$1" '
      index($0, fn "()") == 1 { f = 1 }
      f && /_host_name=/ { print }
      f && /^\}/ { exit }' "$RUNNER" | grep -oE '_host_name=[^ ]+' | sed 's/.*=//')
}

if ((${#HOSTS[@]} == 0)) || ((FROM_TESTS)); then
  [[ -r $CFG && -r $RUNNER ]] || die "cannot read run_sl_test_config.sh / run_sl_test.sh next to this script"
  info "Deriving hosts from the tests selected in run_sl_test_config.sh:"
  discovered=()
  while IFS= read -r t; do
    [[ -n $t ]] || continue
    mapfile -t th < <(hosts_for_test "$t")
    info "  $t -> ${th[*]:-<local only>}"
    for h in "${th[@]:-}"; do
      case "$h" in
        "") ;;
        UNRESOLVED:*) info "      note: $(printf '%s' "$h" | cut -d: -f2) is a caller argument, pass that host by hand";;
        *) discovered+=("$h");;
      esac
    done
  done < <(selected_tests)
  if ((${#discovered[@]})); then
    mapfile -t HOSTS < <(printf '%s\n' "${discovered[@]}" | sort -u)
    info "  => hosts: ${HOSTS[*]}"
  fi
  info ""
fi

((${#HOSTS[@]})) || die "no remote host to sync: the selected tests run entirely on this host (or none is selected)"

command -v git >/dev/null || die "git not found"
REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || die "not inside a git repository"
cd "$REPO_ROOT"

git rev-parse --verify --quiet "$REF" >/dev/null || die "not a valid git ref: $REF"

# ---- build the file list -------------------------------------------------------------------------
# Tracked changes: Added/Copied/Modified/Renamed. Deletions are handled separately - they cannot be
# copied, and silently leaving a deleted file behind on the remote can break its build.
LIST="$(mktemp)"; DELETED="$(mktemp)"
trap 'rm -f "$LIST" "$DELETED"; [[ -n "$BUILD_LOG_DIR" ]] && rm -rf "$BUILD_LOG_DIR"' EXIT

git diff --name-only --diff-filter=ACMR "$REF" -- > "$LIST"
git diff --name-only --diff-filter=D    "$REF" -- > "$DELETED"

# Test output and build artefacts must never be shipped, even with --untracked.
is_excluded() {
  case "$1" in
    test_[0-9]*|test_[0-9]*/*) return 0;;
    latest|latest/*)           return 0;;
    *.log|*.pcap)              return 0;;
    cmake_targets/*)           return 0;;
    *) return 1;;
  esac
}

if ((INCLUDE_UNTRACKED)); then
  while IFS= read -r f; do
    is_excluded "$f" && continue
    printf '%s\n' "$f" >> "$LIST"
  done < <(git ls-files --others --exclude-standard)
fi

# Drop anything that is not a regular file locally (defensive: symlinks, vanished paths).
TMP="$(mktemp)"
while IFS= read -r f; do
  [[ -n "$f" ]] || continue
  is_excluded "$f" && continue
  [[ -f "$f" ]] || continue
  printf '%s\n' "$f" >> "$TMP"
done < "$LIST"
sort -u "$TMP" > "$LIST"; rm -f "$TMP"

COUNT="$(wc -l < "$LIST" | tr -d ' ')"
((COUNT > 0)) || die "nothing to copy (no changes vs $REF$( ((INCLUDE_UNTRACKED)) || printf '%s' '; add --untracked for new files'))"

info "Workspace : $REPO_ROOT"
info "Comparing : $REF ($(git rev-parse --short "$REF"))"
info "Files     : $COUNT"
info ""
sed 's/^/  /' "$LIST"
info ""

if [[ -s "$DELETED" ]]; then
  info "WARNING: deleted locally but NOT removed on the remote - delete by hand if they break the build:"
  sed 's/^/  - /' "$DELETED"
  info ""
fi

if ((DRY_RUN)); then
  info "dry run: nothing transferred. Hosts that would receive it: ${HOSTS[*]}"
  exit 0
fi

# ---- transfer -----------------------------------------------------------------------------------
rc_all=0
for host in "${HOSTS[@]}"; do
  info "=== $host ==="

  # Resolve the remote workspace on the REMOTE side, so a different login name still works.
  if [[ -n "$REMOTE_DIR_OVERRIDE" ]]; then
    remote_dir="$REMOTE_DIR_OVERRIDE"
  else
    remote_home="$(ssh -o BatchMode=yes "$host" 'printf %s "$HOME"' 2>/dev/null)" || {
      info "  FAILED: cannot ssh to '$host' (check ~/.ssh/config and your agent)"; rc_all=1; continue; }
    [[ -n "$remote_home" ]] || { info "  FAILED: remote \$HOME came back empty"; rc_all=1; continue; }
    remote_dir="$remote_home/$WORKSPACE_NAME"
  fi

  remote_user="$(ssh -o BatchMode=yes "$host" 'printf %s "$(id -un)"' 2>/dev/null || echo '?')"
  info "  user      : $remote_user"
  info "  target    : $remote_dir"

  if ! ssh -o BatchMode=yes "$host" "[ -d '$remote_dir' ]"; then
    info "  FAILED: '$remote_dir' does not exist on $host - clone the workspace there first"
    rc_all=1; continue
  fi

  # rsync when both ends have it (only sends deltas, creates missing sub-directories via --files-from,
  # which implies --relative). Otherwise fall back to tar over ssh, which needs nothing but tar.
  if command -v rsync >/dev/null && ssh -o BatchMode=yes "$host" 'command -v rsync >/dev/null'; then
    if rsync -a --files-from="$LIST" ./ "$host:$remote_dir/"; then
      info "  copied    : $COUNT file(s) via rsync"
    else
      info "  FAILED: rsync returned $?"; rc_all=1; continue
    fi
  else
    info "  (rsync unavailable on one side - using tar over ssh)"
    if tar czf - -T "$LIST" | ssh -o BatchMode=yes "$host" "tar xzf - -C '$remote_dir'"; then
      info "  copied    : $COUNT file(s) via tar"
    else
      info "  FAILED: tar transfer returned $?"; rc_all=1; continue
    fi
  fi

  # Show the remote git state so a mismatched base commit is obvious before you build. Full shas are
  # kept for the build gate; abbreviations of different lengths would compare unequal by accident.
  remote_head="$(ssh -o BatchMode=yes "$host" "cd '$remote_dir' && git rev-parse HEAD 2>/dev/null" || echo '?')"
  local_head="$(git rev-parse HEAD)"
  info "  remote HEAD: ${remote_head:0:10}   (local: ${local_head:0:10})"
  [[ "$remote_head" == "$local_head" ]] || \
    info "  WARNING: base commits differ - the copied files sit on a different tree than yours"

  SYNCED+=("$host")
  HEAD_OF["$host"]="$remote_head"
  if ((BUILD)); then
    MAKEHASH_OF["$host"]="$(ssh -o BatchMode=yes "$host" \
      "sha256sum \"\$HOME/$MAKE_SCRIPT_REL\" 2>/dev/null | cut -d' ' -f1" || echo '')"
  fi
done

# ---- optional build ------------------------------------------------------------------------------
# Only worth doing when the hosts are genuinely interchangeable. Two things have to match: the base
# commit (already compared above) and the build script itself, because make_nr_oai_USRP.sh carries the
# target list and the build_oai options (-w USRP, -w SIMU, sanitizers, ...). A host building a different
# target set, or with the address sanitizer on, yields a binary whose behaviour is not comparable with
# the others - the same split-brain this script prevents on the source side, moved to the build side.
if ((BUILD)); then
  if ((rc_all)); then
    info ""
    info "NOT building: at least one host failed to sync (see FAILED above). Fix that first."
    exit "$rc_all"
  fi
  ((${#SYNCED[@]})) || die "nothing was synced, so there is nothing to build"

  info ""
  info "=== build preflight ==="
  local_make="$HOME/$MAKE_SCRIPT_REL"
  [[ -r "$local_make" ]] || die "local build script not found: $local_make"
  local_make_hash="$(sha256sum "$local_make" | cut -d' ' -f1)"
  local_head="$(git rev-parse HEAD)"

  info "  (build script compared: \$HOME/$MAKE_SCRIPT_REL)"
  printf '  %-14s %-12s %-14s %s\n' "host" "HEAD" "build-script" "status"
  printf '  %-14s %-12s %-14s %s\n' "local" "${local_head:0:10}" "${local_make_hash:0:12}" "reference"
  mismatch=0
  for host in "${SYNCED[@]}"; do
    h="${HEAD_OF[$host]:-}"; m="${MAKEHASH_OF[$host]:-}"
    status="ok"
    if [[ -z "$m" ]]; then
      status="MISSING $MAKE_SCRIPT_REL"; mismatch=1
    else
      [[ "$h" == "$local_head" ]]      || { status="HEAD differs";         mismatch=1; }
      [[ "$m" == "$local_make_hash" ]] || { status="build script differs"; mismatch=1; }
    fi
    printf '  %-14s %-12s %-14s %s\n' "$host" "${h:0:10}" "${m:0:12}" "$status"
  done

  if ((mismatch)); then
    info ""
    info "REFUSING to build: the hosts above are not interchangeable."
    info "Put every host on the same commit and give it the same $MAKE_SCRIPT_REL, then re-run with -b."
    exit 1
  fi

  info ""
  info "All hosts agree on commit and build script - building on ${#SYNCED[@]} host(s) in parallel."
  BUILD_LOG_DIR="$(mktemp -d)"
  pids=()
  for host in "${SYNCED[@]}"; do
    # "makeoai" is an interactive shell ALIAS (~/.bash_aliases), and ssh runs a non-interactive shell
    # that does not expand aliases - "ssh host makeoai" fails with "command not found". Run the script
    # the alias wraps instead; it is the same build, with no dependency on shell start-up files.
    ssh -o BatchMode=yes "$host" "bash \"\$HOME/$MAKE_SCRIPT_REL\"" \
      >"$BUILD_LOG_DIR/$host.log" 2>&1 &
    pids+=("$!:$host")
    info "  started : $host"
  done

  build_rc=0
  for entry in "${pids[@]}"; do
    if wait "${entry%%:*}"; then
      info "  OK      : ${entry#*:}"
    else
      info "  FAILED  : ${entry#*:}"
      build_rc=1
    fi
  done

  if ((build_rc)); then
    info ""
    info "Build failures - last 20 lines from each failing host:"
    for host in "${SYNCED[@]}"; do
      lg="$BUILD_LOG_DIR/$host.log"
      grep -qiE '(^| )(error|Error [0-9])' "$lg" 2>/dev/null || continue
      info "  --- $host ---"
      tail -20 "$lg" | sed 's/^/    /'
    done
    info ""
    info "Full logs are removed on exit; re-run with -b after fixing, or build that host by hand."
    exit 1
  fi

  info ""
  info "All remote builds succeeded."
  info "This script does not build THIS host - do that yourself before testing, or the local"
  info "binary is the stale one:"
  info "  makeoai"
  exit 0
fi

info ""
info "Now rebuild on EVERY host that received files, before testing:"
info "  makeoai          # or, explicitly:"
info "  cmake --build . --target nr-softmodem nr-uesoftmodem nr-cuup params_libconfig rfsimulator vrtsim -j 20"
info ""
info "(-b does the remote half of that for you, once every host agrees on commit + build script.)"
exit "$rc_all"
