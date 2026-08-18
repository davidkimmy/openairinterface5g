/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/* \file  nr_slsch_scheduler.c
 * \brief episys SL PSFCH port (Stage 3c): TX-UE SLSCH HARQ feedback processing. On receiving PSFCH
 *        (HARQ ACK/NACK) for a previously transmitted PSSCH, advance/retire the HARQ round and, for a
 *        relay UE, record the ACK/NACK in the static SL HARQ report table. DORMANT until Stage 4 wires
 *        a caller (the PSFCH-decode results are threaded into the SL RX indication and this is invoked).
 */

#include <stdio.h>
#include <math.h>
#include <stdbool.h>

#include "common/utils/nr/nr_common.h"
#include "mac_defs.h"
#include "mac_defs_sl.h"
#include "mac_proto.h"
#include "nr_ue_sci.h"
#include "executables/softmodem-common.h"

// NR_list_t helpers: defined in NR_MAC_gNB/gNB_scheduler_primitives.c, but that lib is NOT linked into
// nr-uesoftmodem. Provide UE-side copies here so the SL HARQ code (and config_ue_sl.c) can use them.
void create_nr_list(NR_list_t *list, int len)
{
  list->head = -1;
  list->next = malloc(len * sizeof(*list->next));
  AssertFatal(list->next, "cannot malloc() memory for NR_list_t->next\n");
  for (int i = 0; i < len; ++i)
    list->next[i] = -1;
  list->tail = -1;
  list->len = len;
}
void remove_nr_list(NR_list_t *listP, int id)
{
  int *cur = &listP->head;
  int *prev = &listP->head;
  while (*cur != -1 && *cur != id) {
    prev = cur;
    cur = &listP->next[*cur];
  }
  AssertFatal(*cur != -1, "ID %d not found in UE_list\n", id);
  int *next = &listP->next[*cur];
  *cur = listP->next[*cur];
  *next = -1;
  listP->tail = *prev >= 0 && listP->next[*prev] >= 0 ? listP->tail : *prev;
}
void add_tail_nr_list(NR_list_t *listP, int id)
{
  int *last = listP->tail < 0 ? &listP->head : &listP->next[listP->tail];
  *last = id;
  listP->next[id] = -1;
  listP->tail = id;
}
void add_front_nr_list(NR_list_t *listP, int id)
{
  const int ohead = listP->head;
  listP->head = id;
  listP->next[id] = ohead;
  if (listP->tail < 0)
    listP->tail = id;
}
void remove_front_nr_list(NR_list_t *listP)
{
  AssertFatal(listP->head >= 0, "Nothing to remove\n");
  const int ohead = listP->head;
  listP->head = listP->next[ohead];
  listP->next[ohead] = -1;
  if (listP->head < 0)
    listP->tail = -1;
}

/* get_mcs_from_cqi() and the CQI -> MCS tables live in NR_MAC_COMMON/nr_mac_common.c (shared
   with the gNB DL path); declared in nr_mac_common.h. */

/* BLER-based SL MCS adaptation: the develop gNB replaced this API with
   update_bler_stats()/nr_adapt_mcs_from_bler(), so the UE SL path keeps its own copy here.
   Backs MCS down when the HARQ retransmission ratio rises above upper, steps it up toward the
   CQI-derived ceiling max_mcs when it falls below lower. rounds[0] = initial tx, rounds[1] = retx. */
#define BLER_UPDATE_FRAME 10
#define BLER_FILTER 0.9f
int get_mcs_from_bler(const NR_bler_options_t *bler_options,
                      const NR_mac_dir_stats_t *stats,
                      NR_bler_stats_t *bler_stats,
                      int max_mcs,
                      frame_t frame)
{
  int diff = frame - bler_stats->last_frame;
  if (diff < 0) // wrap around
    diff += 1024;

  max_mcs = min(max_mcs, bler_options->max_mcs);
  const uint8_t old_mcs = min(bler_stats->mcs, max_mcs);
  if (diff < BLER_UPDATE_FRAME)
    return old_mcs; // no update

  // last update is longer than x frames ago
  const int num_dl_sched = (int)(stats->rounds[0] - bler_stats->rounds[0]);
  const int num_dl_retx = (int)(stats->rounds[1] - bler_stats->rounds[1]);
  const float bler_window = num_dl_sched > 0 ? (float) num_dl_retx / num_dl_sched : bler_stats->bler;
  bler_stats->bler = BLER_FILTER * bler_stats->bler + (1 - BLER_FILTER) * bler_window;

  int new_mcs = old_mcs;
  if (bler_stats->bler < bler_options->lower && old_mcs < max_mcs && num_dl_sched > 3)
    new_mcs += 1;
  else if (bler_stats->bler > bler_options->upper || num_dl_sched <= 3) // above threshold or no activity
    new_mcs -= 1;
  // else we are within threshold boundaries

  new_mcs = max(new_mcs, bler_options->min_mcs);
  bler_stats->last_frame = frame;
  bler_stats->mcs = new_mcs;
  memcpy(bler_stats->rounds, stats->rounds, sizeof(stats->rounds));
  LOG_D(NR_MAC, "frame %4d SL MCS %d -> %d (num_sched %d, num_retx %d, BLER wnd %.3f avg %.6f)\n",
        frame, old_mcs, new_mcs, num_dl_sched, num_dl_retx, bler_window, bler_stats->bler);
  return new_mcs;
}

// Map (remote UE src_id, HARQ pid) -> a bit index in the static SL HARQ report table, allocating a
// remote-UE slot on first sight.
int nr_mac_get_static_sl_report_bit_index(NR_UE_MAC_INST_t *mac, uint16_t src_id, int8_t sl_harq_pid)
{
  SL_REPORT_CONFIG_t *sl_report_config = &mac->sl_report_config;
  int ue_idx = -1;
  for (int i = 0; i < MAX_REMOTE_UES; i++) {
    if (sl_report_config->remote_ue_mapping[i].is_active && sl_report_config->remote_ue_mapping[i].src_id == src_id) {
      ue_idx = i;
      break;
    }
  }
  if (ue_idx == -1) {
    for (int i = 0; i < MAX_REMOTE_UES; i++) {
      if (!sl_report_config->remote_ue_mapping[i].is_active) {
        sl_report_config->remote_ue_mapping[i].src_id = src_id;
        sl_report_config->remote_ue_mapping[i].is_active = true;
        ue_idx = i;
        LOG_D(NR_MAC, "Mapped Remote UE 0x%04x to UE Index %d\n", src_id, ue_idx);
        break;
      }
    }
  }
  if (ue_idx == -1) {
    LOG_E(NR_MAC, "No space for new Remote UE 0x%04x\n", src_id);
    return -1;
  }
  if (sl_harq_pid >= HARQ_BITS_PER_UE) {
    LOG_W(NR_MAC, "HARQ PID %d exceeds reserved bits per UE (%d)\n", sl_harq_pid, HARQ_BITS_PER_UE);
    return -1;
  }
  return (ue_idx * HARQ_BITS_PER_UE) + sl_harq_pid;
}

void nr_mac_process_sl_rx_data(NR_UE_MAC_INST_t *mac, uint16_t src_id, int8_t harq_id, uint8_t ack_nack)
{
  int bit_index = nr_mac_get_static_sl_report_bit_index(mac, src_id, harq_id);
  if (bit_index >= 0 && bit_index < MAX_SL_HARQ_PROCESSES) {
    SL_REPORT_CONFIG_t *sl_report_config = &mac->sl_report_config;
    if (sl_report_config->sl_harq_table[bit_index].is_active) {
      if (ack_nack)
        sl_report_config->sl_harq_table[bit_index].harq_status = true;
    } else {
      sl_report_config->sl_harq_table[bit_index].src_id = src_id;
      sl_report_config->sl_harq_table[bit_index].sl_harq_pid = harq_id;
      sl_report_config->sl_harq_table[bit_index].harq_status = ack_nack;
      sl_report_config->sl_harq_table[bit_index].is_active = true;
    }
    LOG_D(NR_MAC, "sl_harq_table Updated: Remote UE 0x%04x, HARQ PID %d -> Bit %d: %s\n",
          src_id, harq_id, bit_index, (ack_nack == 1 ? "ACK" : "NACK"));
  }
}

void abort_nr_ue_sl_harq(NR_UE_MAC_INST_t *mac, int8_t harq_pid, NR_SL_UE_info_t *UE_info)
{
  NR_SL_UE_sched_ctrl_t *sched_ctrl = &UE_info->UE_sched_ctrl;
  NR_UE_sl_harq_t *harq = &sched_ctrl->sl_harq_processes[harq_pid];
  harq->round = 0;
  UE_info->mac_sl_stats.sl.errors++;
  add_tail_nr_list(&sched_ctrl->available_sl_harq, harq_pid);
  // the transmission failed: retrieve the scheduled bytes so the next BSR reschedules them correctly.
  sched_ctrl->sched_sl_bytes -= harq->sched_pssch.tb_size;
  if (sched_ctrl->sched_sl_bytes < 0)
    sched_ctrl->sched_sl_bytes = 0;
}

// Process the HARQ ACK/NACK(s) decoded from a PSFCH for previously transmitted PSSCH(s).
void handle_nr_ue_sl_harq(module_id_t mod_id, frame_t frame, sub_frame_t slot, sl_nr_slsch_pdu_t *rx_slsch_pdu, uint16_t src_id)
{
  NR_UE_MAC_INST_t *mac = get_mac_inst(mod_id);
  NR_UE_SL_SCHED_LOCK(&mac->sl_sched_lock);
  NR_SL_UE_info_t **UE_SL_temp = (NR_SL_UE_info_t **)&mac->sl_info.list, *UE;
  // TODO: update for multiple UEs
  UE = *(UE_SL_temp);
  uint8_t num_ack_rcvd = rx_slsch_pdu->num_acks_rcvd;

  NR_SL_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
  NR_UE_sl_harq_t **matched_harqs = (NR_UE_sl_harq_t **)calloc(sched_ctrl->feedback_sl_harq.len, sizeof(NR_UE_sl_harq_t *));
  int k = find_current_slot_harqs(frame, slot, sched_ctrl, matched_harqs);
  LOG_D(NR_MAC, "Found %d matching HARQ processes vs. num. of received acks %d\n", k, num_ack_rcvd);
  /* matched_harqs[] holds only the k processes whose feedback is due this slot; num_ack_rcvd is the
     number of PSFCH occasions the PHY decoded (muxed PSFCH can exceed k under loss). Iterating up to
     num_ack_rcvd would dereference NULL matched_harqs[i>=k] -> segfault, so consume only k. */
  for (int i = 0; i < num_ack_rcvd && i < k; i++) {
    uint8_t ack_nack = rx_slsch_pdu->ack_nack_rcvd[i];
    uint8_t rx_harq_id = matched_harqs[i]->sl_harq_pid;
    int8_t harq_pid = sched_ctrl->feedback_sl_harq.head;
    while (rx_harq_id != harq_pid || harq_pid < 0) {
      LOG_W(NR_MAC, "Unexpected SLSCH HARQ PID %d (have %d) for src id %4d\n", rx_harq_id, harq_pid, src_id);
      if (harq_pid < 0) {
        NR_UE_SL_SCHED_UNLOCK(&mac->sl_sched_lock);
        free(matched_harqs);
        return;
      }
      remove_front_nr_list(&sched_ctrl->feedback_sl_harq);
      sched_ctrl->sl_harq_processes[harq_pid].is_waiting = false;
      if (sched_ctrl->sl_harq_processes[harq_pid].round >= (HARQ_ROUND_MAX - 1)) {
        abort_nr_ue_sl_harq(mac, harq_pid, UE);
      } else {
        sched_ctrl->sl_harq_processes[harq_pid].round++;
        add_tail_nr_list(&sched_ctrl->retrans_sl_harq, harq_pid);
      }
      harq_pid = sched_ctrl->feedback_sl_harq.head;
    }
    remove_front_nr_list(&sched_ctrl->feedback_sl_harq);
    NR_UE_sl_harq_t *harq = &sched_ctrl->sl_harq_processes[harq_pid];
    DevAssert(harq->is_waiting);
    harq->feedback_slot = -1;
    harq->is_waiting = false;
    if (!ack_nack) {
      UE->mac_sl_stats.cumul_round[harq->round]++;
#ifdef ENABLE_BLER_INSTRUMENTATION
      UE->mac_sl_stats.sl.rounds[harq->round]++;
      LOG_I(NR_MAC, "[HARQ_STATS] %u.%u PC5_HARQ_SUCCESS src_id=%d round=%d cumul_r0=%lu r1=%lu r2=%lu r3=%lu\n",
            frame, slot, src_id, harq->round,
            UE->mac_sl_stats.sl.rounds[0], UE->mac_sl_stats.sl.rounds[1],
            UE->mac_sl_stats.sl.rounds[2], UE->mac_sl_stats.sl.rounds[3]);
#endif
      harq->round = 0;
      harq->csi_req_pending = false; // link recovered: cancel any pending CSI-RS request
      LOG_D(NR_MAC, "%4u.%2u Slharq id %d crc passed for src id %4d\n", frame, slot, harq_pid, src_id);
      add_tail_nr_list(&sched_ctrl->available_sl_harq, harq_pid);
      if (get_softmodem_params()->is_relay_ue)
        nr_mac_process_sl_rx_data(mac, src_id, harq_pid, !ack_nack);
    } else if (harq->round >= (HARQ_ROUND_MAX - 1)) {
      UE->mac_sl_stats.cumul_round[HARQ_ROUND_MAX]++;
      LOG_D(NR_MAC, "src id %4d, Slharq id %d crc failed in all rounds\n", src_id, harq_pid);
      abort_nr_ue_sl_harq(mac, harq_pid, UE);
      if (get_softmodem_params()->is_relay_ue)
        nr_mac_process_sl_rx_data(mac, src_id, harq_pid, !ack_nack);
    } else {
      // NACK (round < HARQ_ROUND_MAX-1): schedule a HARQ retransmission. The buffered TB
      // (sl_harq_processes[pid].transportBlock) is re-sent by the TX scheduler when it drains
      // retrans_sl_harq (chase combining, rv=0).
      harq->round++;
      /* Aperiodic CSI-RS trigger: on sustained NACKs (round >= N) flag this process so the next
       * retransmission carries a CSI-RS (sci2.csi_req=1) to remeasure the degraded link. */
      if (harq->round >= SL_CSI_RS_NACK_TRIGGER_ROUNDS)
        harq->csi_req_pending = true;
      LOG_D(NR_MAC, "%4u.%2u Slharq id %d crc failed for src id %4d -> retx round %d\n", frame, slot, harq_pid, src_id, harq->round);
      add_tail_nr_list(&sched_ctrl->retrans_sl_harq, harq_pid);
    }
  }
  free(matched_harqs);
  NR_UE_SL_SCHED_UNLOCK(&mac->sl_sched_lock);
}

/* Copy the latest measured CSI (mac->csirs_measurements, populated by the PHY CSI-RS RX) into the
 * CSI report to be sent back to the peer. */
void set_csi_report_params(NR_UE_MAC_INST_t *mac, NR_SL_UE_sched_ctrl_t *sched_ctrl)
{
  SL_CSI_Report_t *csi_report = &sched_ctrl->sched_csi_report;
  csi_report->cqi = mac->csirs_measurements.cqi;
  csi_report->ri = mac->csirs_measurements.ri;
}

/* Periodic (debug-mode) CSI-RS occasion selection, ported from episys/sl-mode1-relay. Picks, per
 * assigned TDD period, the first PSFCH-free sidelink slot as the CSI-RS offset and derives the
 * periodicity from the UL-slot cadence. Only used when sl_csi_trigger_mode == SL_CSI_TRIGGER_PERIODIC;
 * production (aperiodic) mode never calls this. */
SL_CSI_Report_t *set_nr_ue_sl_csi_meas_periodicity(const NR_TDD_UL_DL_Pattern_t *tdd,
                                                   NR_SL_UE_sched_ctrl_t *sched_ctrl,
                                                   NR_UE_MAC_INST_t *mac,
                                                   int uid,
                                                   uint8_t psfch_period) {
  sl_nr_ue_mac_params_t *sl_mac = mac->SL_MAC_PARAMS;
  sl_nr_phy_config_request_t *sl_cfg = &sl_mac->sl_phy_config.sl_config_req;
  uint8_t mu = sl_cfg->sl_bwp_config.sl_scs;
  const int n_slots_frame = get_slots_per_frame_from_scs(mu);

  /* Build a develop-native frame_structure_t from the SL TDD config so we can reuse develop's
     get_first_ul_slot(fs, mixed) and its slots-per-period field (mirrors config_ue.c). */
  frame_structure_t fs = {0};
  if (tdd)
    config_frame_structure(mu, sl_mac->sl_TDD_config, tdd->dl_UL_TransmissionPeriodicity, TDD, &fs);

  const int n_ul_slots_period = tdd ? tdd->nrofUplinkSlots + (tdd->nrofUplinkSymbols > 0 ? 1 : 0) : n_slots_frame;
  const int nr_slots_period = tdd ? fs.numb_slots_period : n_slots_frame;

  /* UL slots repeat every nr_slots_period. CSI-RS must occur within the UL slot range, so use the
     UL-slot count as the ideal periodicity base. */
  const int ideal_period = n_ul_slots_period;

  const int first_ul_slot_period = tdd ? get_first_ul_slot(&fs, true) : 0;

  SL_CSI_Report_t *csi_report = &sched_ctrl->sched_csi_report;

  /* In Mode 2, each UE is assigned to a specific TDD period for transmission. Use uid to pick the
     period (inverted assignment observed in logs): uid=0 -> last period, uid=1 -> first period, etc. */
  const int nb_periods_per_frame = get_nb_periods_per_frame(tdd->dl_UL_TransmissionPeriodicity);
  const int period_index = (nb_periods_per_frame - 1 - (uid % nb_periods_per_frame));
  const int period_start = period_index * nr_slots_period;

  /* Adaptive CSI-RS slot selection: first PSFCH-free sidelink slot in THIS UE's own TX bitmap.
     Start at the uid-assigned period (preserves Mode-1 multi-UE spreading) but WRAP across the whole
     physical map: Mode-2 peers use mirrored TX/RX bitmaps ("0F" vs "F0"), so a UE's TX slots can fall
     entirely outside its assigned period; without wrapping only one direction ever sent CSI-RS. */
  SL_ResourcePool_params_t *sl_tx_rsrc_pool = sl_mac->sl_TxPool[0];
  size_t phy_map_sz = (sl_tx_rsrc_pool->phy_sl_bitmap.size << 3) - sl_tx_rsrc_pool->phy_sl_bitmap.bits_unused;
  int offset = first_ul_slot_period + period_start; // default: first UL slot of the assigned period
  bool slot_found = false;
  for (int i = 0; i < (int)phy_map_sz; i++) {
    int cand = (period_start + first_ul_slot_period + i) % (int)phy_map_sz;
    size_t bit_pos = sl_abs_slot_to_bit_pos((uint64_t)cand, phy_map_sz);
    // A candidate is usable if it is a sidelink slot that does not carry PSFCH.
    if (get_bit_from_map(sl_tx_rsrc_pool->phy_sl_bitmap.buf, bit_pos)
        && !sl_slot_carries_psfch(&sl_tx_rsrc_pool->phy_sl_bitmap, phy_map_sz, bit_pos, psfch_period)) {
      offset = cand;
      slot_found = true;
      break;
    }
  }
  // If the UE's TX bitmap has no PSFCH-free sidelink slot at all, do not schedule CSI-RS for this UE.
  csi_report->slot_valid = slot_found;
  if (!slot_found)
    LOG_D(NR_MAC, "No PSFCH-free sidelink slot in TX bitmap (psfch_period=%d): CSI-RS not scheduled for uid %d\n",
          psfch_period, uid);

  AssertFatal(offset < 320, "Not enough UL slots to accomodate all possible UEs. Need to rework the implementation\n");
  csi_report->slot_offset = offset;
  if (ideal_period < 5) {
    csi_report->slot_periodicity_offset = NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots4;
  } else if (ideal_period < 6) {
    csi_report->slot_periodicity_offset = NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots5;
  } else if (ideal_period < 9) {
    csi_report->slot_periodicity_offset = NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots8;
  } else if (ideal_period < 11) {
    csi_report->slot_periodicity_offset = NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots10;
  } else if (ideal_period < 17) {
    csi_report->slot_periodicity_offset = NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots16;
  } else if (ideal_period < 21) {
    csi_report->slot_periodicity_offset = NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots20;
  } else if (ideal_period < 41) {
    csi_report->slot_periodicity_offset = NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots40;
  } else if (ideal_period < 81) {
    csi_report->slot_periodicity_offset = NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots80;
  } else if (ideal_period < 161) {
    csi_report->slot_periodicity_offset = NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots160;
  } else {
    csi_report->slot_periodicity_offset = NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots320;
  }
  return csi_report;
}

/* Map the CSI-RS periodicity enum to its slot count (and echo the offset). Ported verbatim from
 * episys/sl-mode1-relay. */
void nr_ue_sl_csi_period_offset(SL_CSI_Report_t *sl_csi_report,
                                int *period,
                                int *offset) {
  *offset = sl_csi_report->slot_offset;
  switch(sl_csi_report->slot_periodicity_offset) {
    case NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots4:
      *period = 4;
      break;
    case NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots5:
      *period = 5;
      break;
    case NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots8:
      *period = 8;
      break;
    case NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots10:
      *period = 10;
      break;
    case NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots16:
      *period = 16;
      break;
    case NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots20:
      *period = 20;
      break;
    case NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots32:
      *period = 32;
      break;
    case NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots40:
      *period = 40;
      break;
    case NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots64:
      *period = 64;
      break;
    case NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots80:
      *period = 80;
      break;
    case NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots160:
      *period = 160;
      break;
    case NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots320:
      *period = 320;
      break;
    case NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots640:
      *period = 640;
      break;
    default:
      AssertFatal(1 == 0, "No periodicity and offset found in CSI resource");
  }
}

/* Schedule an SL CSI report MAC CE back to the peer. The report is packed opportunistically onto
 * the next new-data PSSCH (see nr_ue_scheduler_sl.c), so this only needs to confirm the peer has an
 * eligible PSFCH-bearing SL slot at/after rx_slot before arming the pending report. Derived from the
 * physical SL bitmap (get_feedback_abs_slot) so it stays aligned with the PSFCH-occasion model. A
 * pending report is overwritten (idempotent: latest CQI/RI only), which also stops a stranded report
 * wedging "active". */
void nr_ue_sl_csi_report_scheduling(NR_UE_MAC_INST_t *mac, NR_SL_UE_sched_ctrl_t *sched_ctrl, uint32_t rx_frame, uint32_t rx_slot)
{
  sl_nr_ue_mac_params_t *sl_mac = mac->SL_MAC_PARAMS;
  if (!sl_mac || !sl_mac->sl_TxPool[0] || !mac->sl_tx_res_pool)
    return;
  // psfch_period gates PSFCH-occasion eligibility; use the provisioned SL PSFCH period (or 1 if none).
  uint8_t psfch_period = 1;
  const uint8_t psfch_periods[] = {0, 1, 2, 4};
  if (mac->sl_tx_res_pool->sl_PSFCH_Config_r16
      && mac->sl_tx_res_pool->sl_PSFCH_Config_r16->choice.setup->sl_PSFCH_Period_r16) {
    psfch_period = psfch_periods[*mac->sl_tx_res_pool->sl_PSFCH_Config_r16->choice.setup->sl_PSFCH_Period_r16];
    if (psfch_period == 0)
      psfch_period = 1;
  }
  const uint8_t psfch_time_gaps[] = {2, 3};
  uint8_t min_time_gap = mac->sl_tx_res_pool->sl_PSFCH_Config_r16
                             ? psfch_time_gaps[*mac->sl_tx_res_pool->sl_PSFCH_Config_r16->choice.setup->sl_MinTimeGapPSFCH_r16]
                             : psfch_time_gaps[0];
  const uint8_t mu = sl_mac->sl_phy_config.sl_config_req.sl_bwp_config.sl_scs;
  SL_ResourcePool_params_t *sl_tx_rsrc_pool = sl_mac->sl_TxPool[0];
  size_t phy_map_sz = (sl_tx_rsrc_pool->phy_sl_bitmap.size << 3) - sl_tx_rsrc_pool->phy_sl_bitmap.bits_unused;
  frameslot_t rx_fs = {rx_frame, rx_slot};
  uint64_t rx_abs_slot = normalize(&rx_fs, mu);
  int64_t fb_abs = get_feedback_abs_slot(&sl_tx_rsrc_pool->phy_sl_bitmap, phy_map_sz, rx_abs_slot, min_time_gap, psfch_period);
  if (fb_abs < 0)
    return; // no eligible PSFCH-bearing SL slot for this rx_slot
  sched_ctrl->sched_csi_report.active = true;
}
