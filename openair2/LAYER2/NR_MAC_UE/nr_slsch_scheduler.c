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
  for (int i = 0; i < num_ack_rcvd; i++) {
    uint8_t ack_nack = rx_slsch_pdu->ack_nack_rcvd[i];
    LOG_D(NR_MAC, "%u.%u handle harq: matched %d acks %d, ack_nack_rcvd[%d]=%d (%s)\n",
          frame, slot, k, num_ack_rcvd, i, ack_nack, ack_nack ? "NACK" : "ACK");
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
      LOG_D(NR_MAC, "%4u.%2u Slharq id %d crc failed for src id %4d -> retx round %d\n", frame, slot, harq_pid, src_id, harq->round);
      add_tail_nr_list(&sched_ctrl->retrans_sl_harq, harq_pid);
    }
  }
  free(matched_harqs);
  NR_UE_SL_SCHED_UNLOCK(&mac->sl_sched_lock);
}
