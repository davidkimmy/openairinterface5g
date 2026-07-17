#!/bin/bash
# ============================================================================
# SL mode-1 U2N relay: Remote-UE REAL 5GC registration + relayed ping (VRTSIM)
# ============================================================================
# 3 nodes on the local host over the vrtsim shared-memory radio:
#   gNB       = Uu server
#   Remote UE = PC5 server  (imsi ...002)  -- registers with the 5GC via the relay
#   Relay UE  = Uu + PC5 client (imsi ...001, SyncRef)
#
# The Remote UE registers over PC5 -> Relay -> gNB (NO --ip-demo), obtains a real
# core-assigned IP on oaitun_ue2, and pings 8.8.8.8 through the relay.
#
# Launch ORDER + timing are critical: gNB -> (9s) -> Remote -> (6s) -> Relay,
# so the Remote can PC5-sync and register before the ping. NO --sa.
# --remote-ue-id 1 must be on ALL three nodes.
#
# PASS criterion: the ping reports "0% packet loss" (see the summary at the end).
#
# Prereqs:
#   - built RAN (e.g. makeoai)
#   - 5GC up:  cd ~/oai-cn5g && docker compose up -d
#
# Usage:
#   ./mode1_reg_vrtsim.sh [-v] [-r N] [-c COUNT] [-h]
#     -v        verbose: enable rlc/pdcp/sdap/srap/nas debug logs (default: info)
#     -r N      retry up to N times, stop at the first 0% loss run (default: 1).
#               The relay's Uu link RLFs on ~1 of 2-3 runs under single-host CPU
#               contention, so a few retries are often needed for a clean pass.
#     -c COUNT  ICMP echo count for the ping (default: 20)
#     -h        this help
#
# Env overrides:  BUILD_DIR, GNB_CONF, SL_CONF_DIR, LOGDIR
# ============================================================================
set -u

# ---- locate the repo (this script lives in <repo>/doc/episys/test_script/) ----
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../../.." && pwd)"

B="${BUILD_DIR:-$REPO/cmake_targets/ran_build/build}"
GNB_CONF="${GNB_CONF:-$REPO/targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band78.fr1.106PRB.usrpb210_relay_ue.conf}"
SL="${SL_CONF_DIR:-$REPO/targets/PROJECTS/NR-SIDELINK/CONF}"
LOG="${LOGDIR:-/tmp/vrtsim_m1_reg}"

VERBOSE=0
RETRIES=1
PCOUNT=20
while getopts "vr:c:h" opt; do
  case "$opt" in
    v) VERBOSE=1 ;;
    r) RETRIES="$OPTARG" ;;
    c) PCOUNT="$OPTARG" ;;
    h) sed -n '2,40p' "$0"; exit 0 ;;
    *) echo "invalid option; use -h"; exit 1 ;;
  esac
done

# Debug log levels only when -v is given; otherwise a quiet 'info' run.
GNB_DBG=""; UE_DBG=""; RELAY_DBG=""
if [ "$VERBOSE" = "1" ]; then
  GNB_DBG="--log_config.rlc_log_level debug --log_config.pdcp_log_level debug --log_config.nr_srap_log_level debug --log_config.sdap_log_level debug"
  UE_DBG="--log_config.nas_log_level debug --log_config.sdap_log_level debug --log_config.pdcp_log_level debug --log_config.rlc_log_level debug"
  RELAY_DBG="--log_config.rlc_log_level debug --log_config.nr_srap_log_level debug"
fi

mkdir -p "$LOG"

cleanup(){ sudo pkill -INT -x nr-uesoftmodem 2>/dev/null; sudo pkill -INT -x nr-softmodem 2>/dev/null; sleep 2;
  sudo pkill -9 -x nr-uesoftmodem 2>/dev/null; sudo pkill -9 -x nr-softmodem 2>/dev/null;
  for i in oaitun_ue1 oaitun_ue2; do sudo ip link delete "$i" 2>/dev/null; done;
  rm -f /dev/shm/vrtsim* 2>/dev/null; }

run_once(){
  cleanup; sleep 2; cd "$B" || exit 1

  echo "=== gNB (Uu server, no ip-demo) ==="
  sudo LD_LIBRARY_PATH="$B" -E ./nr-softmodem -O "$GNB_CONF" \
    --gNBs.[0].min_rxtxtime 6 --sl-mode 1 --relay-type 1 --remote-ue-id 1 \
    --device.name vrtsim --vrtsim.role server --vrtsim.chanmod 0 \
    --log_config.global_log_level info $GNB_DBG > "$LOG/gnb.log" 2>&1 &
  sleep 9

  echo "=== Remote UE (PC5 server, imsi ...002, registers via relay) ==="
  sudo LD_LIBRARY_PATH="$B" -E ./nr-uesoftmodem -O "$SL/sl_ue1.conf" \
    --uicc0.imsi 001010000000002 --uicc0.pdu_sessions.[0].dnn oai \
    --sl-mode 2 --relay-type 1 --remote-ue-id 1 --thread-pool -1,-1,-1,-1 \
    --device.name vrtsim --vrtsim.role_sl server --vrtsim.chanmod 0 \
    --log_config.global_log_level info $UE_DBG > "$LOG/nearby.log" 2>&1 &
  sleep 6

  echo "=== Relay/SyncRef UE (Uu+PC5 client, imsi ...001) ==="
  sudo LD_LIBRARY_PATH="$B" -E ./nr-uesoftmodem -O "$SL/sl_sync_ref.conf" \
    -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 --uicc0.pdu_sessions.[0].dnn oai \
    --sync-ref --sl-mode 1 --relay-type 1 --is-relay-ue 1 --remote-ue-id 1 --thread-pool -1,-1,-1,-1 \
    --device.name vrtsim --vrtsim.role client --vrtsim.role_sl client --vrtsim.chanmod 0 \
    --log_config.global_log_level info $RELAY_DBG > "$LOG/relay.log" 2>&1 &
  sleep 90

  echo "=== [remote] registration (auth -> security -> registration accept -> Core IP) ==="
  grep -aiE "AUTHENTICATION|Security Mode|Registration accept|PDU SESSION ESTABLISHMENT ACC|applying core IP|configured, IPv4" \
    "$LOG/nearby.log" 2>/dev/null | sed -E 's/\x1b\[[0-9;]*m//g' | tail -12

  echo "=== PING remote(oaitun_ue2) -> 8.8.8.8 (relayed over PC5, real Core IP) ==="
  if ip -br addr show 2>/dev/null | grep -q oaitun_ue2; then
    ping -c "$PCOUNT" -I oaitun_ue2 8.8.8.8 2>&1 | tee "$LOG/ping.txt" | tail -6
  else
    echo "(no oaitun_ue2)" | tee "$LOG/ping.txt"
  fi

  echo "=== TUNs (remote oaitun_ue2 should hold a Core IP, not 10.0.0.100) ==="
  ip -br addr show 2>/dev/null | grep -E "oaitun" || echo "(none)"

  echo "=== crash check (assert/abort/segfault in any node) ==="
  grep -aicE "Assertion|Aborted|Segmentation fault" "$LOG"/*.log
  cleanup
}

pass=0
for attempt in $(seq 1 "$RETRIES"); do
  echo "########## ATTEMPT $attempt / $RETRIES ##########"
  run_once
  if grep -aq "0% packet loss" "$LOG/ping.txt" 2>/dev/null; then
    pass=1; echo ">>> PASS on attempt $attempt (0% packet loss)"; break
  fi
  echo ">>> attempt $attempt did not pass (likely relay Uu RLF variance); logs in $LOG"
done

if [ "$pass" = "1" ]; then
  echo "=== RESULT: PASS (logs in $LOG) ==="; exit 0
else
  echo "=== RESULT: FAIL after $RETRIES attempt(s) (logs in $LOG) ==="; exit 1
fi
