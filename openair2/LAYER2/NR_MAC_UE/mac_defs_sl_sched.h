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
 */

/* \file mac_defs_sl_sched.h
 * \brief Sidelink SL-SCH / SCI / CSI / HARQ scheduling data types for the NR UE MAC.
 *        Ported (episys SL data-plane port) from the episys branch's mac_defs.h SL cluster
 *        so develop's mac_defs.h stays a thin include. Depends on types in scope at the
 *        include point in mac_defs.h: nr_mac_common.h (frameslot_t, NR_bler_stats_t, NR_list_t,
 *        MAX_SL_* macros, NR_UE_sl_mac_stats_t), mac_defs_sl.h -> sidelink_nr_ue_interface.h
 *        (sl_config_grant_t, cg_type_t, NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR),
 *        platform_types.h (mac_rlc_status_resp_t).
 */

#ifndef __LAYER2_NR_MAC_UE_MAC_DEFS_SL_SCHED_H__
#define __LAYER2_NR_MAC_UE_MAC_DEFS_SL_SCHED_H__

#include "NR_MAC_UE/nr_ue_sci.h"
#include "common/utils/collection/linear_alloc.h"

// SL CSI report (38321 sec. 6.1.3.35), 1-byte packed.
typedef struct {
  uint8_t RI: 1; // 7th bit
  uint8_t CQI: 4; // 3-6 bits
  uint8_t R: 3; // 0-2 bits
} __attribute__ ((__packed__)) nr_sl_csi_report_t;

typedef struct NR_sched_pssch {
  int frame;
  int slot;
  int mu;

  /// RB allocation within active uBWP
  uint16_t rbSize;
  uint16_t rbStart;

  /// MCS
  uint8_t mcs;

  /// TBS-related info
  uint16_t R;
  uint8_t Qm;
  uint32_t tb_size;

  /// UL HARQ PID to use for this UE, or -1 for "any new"
  int8_t sl_harq_pid;

  uint8_t nrOfLayers;
  //NR_pusch_dmrs_t dmrs_info;
} NR_sched_pssch_t;

typedef struct {
  bool is_waiting;
  bool is_active;
  uint8_t ndi;
  uint8_t round;
  uint16_t feedback_slot;
  uint16_t feedback_frame;
  int8_t sl_harq_pid;

  // Transport block to be sent using this HARQ process, its size is in sched_pssch
  uint32_t transportBlock[38016]; // valid up to 4 layers
  uint32_t tb_size;

  /// sched_pusch keeps information on MCS etc used for the initial transmission
  NR_sched_pssch_t sched_pssch;
} NR_UE_sl_harq_t;

typedef struct SL_CSI_Report {
  uint8_t ri;
  int8_t cqi;
  uint8_t cqi_table;
  uint32_t frame;
  uint32_t slot;
  bool active;
  uint8_t slot_offset;
  uint8_t slot_periodicity;
  NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR slot_periodicity_offset;
} SL_CSI_Report_t;
  //

typedef struct SL_sched_feedback {
  int16_t feedback_slot;
  int16_t feedback_frame;
  int16_t harq_feedback;
  uint8_t dai_c;
  bool    active;
  uint8_t freq_hop_flag;
  uint8_t group_hop_flag;
  uint8_t sequence_hop_flag;
  uint16_t second_hop_prb;
  uint8_t nr_of_symbols;
  uint8_t start_symbol_index;
  uint8_t hopping_id;
  uint16_t prb;
  uint16_t sl_bwp_start;
  uint16_t initial_cyclic_shift;
  uint8_t mcs;
  uint8_t bit_len_harq;
} SL_sched_feedback_t;

typedef struct {

  // sidelink bytes that are currently scheduled
  int sched_sl_bytes;
  /// Sched PSSCH: scheduling decisions, copied into HARQ and cleared every TTI
  NR_sched_pssch_t sched_pssch;

  // Used on PSFCH transmitter
  SL_sched_feedback_t *sched_psfch;

  /// total amount of data awaiting for this UE
  uint32_t num_total_bytes;
  uint16_t sl_pdus_total;
  /// per-LC status data
  mac_rlc_status_resp_t rlc_status[NR_MAX_NUM_LCID];
  //
  NR_bler_stats_t sl_bler_stats;

  /// information about every UL HARQ process
  NR_UE_sl_harq_t sl_harq_processes[NR_MAX_HARQ_PROCESSES];
  /// UL HARQ processes that are free
  NR_list_t available_sl_harq;
  /// UL HARQ processes that await feedback
  NR_list_t feedback_sl_harq;
  /// UL HARQ processes that await retransmission
  NR_list_t retrans_sl_harq;
  //  NR_SLSCH
  // Used on CSI report transmitter
  SL_CSI_Report_t sched_csi_report;
  // To hold the CSI report values received from different users
  nr_sl_csi_report_t rx_csi_report;
  bool print_csi_report;
  /// UE-estimated maximum MCS (from CSI-RS)
  uint8_t sl_max_mcs;

} NR_SL_UE_sched_ctrl_t;

#define CUR_SL_UE_CONNECTIONS 1

#define MAX_SL_CSI_REPORTCONFIG MAX_SL_UE_CONNECTIONS

typedef struct {
  uid_t uid; // unique ID of this UE
  /// scheduling control info
  nr_sl_csi_report_t csi_report_template[MAX_SL_CSI_REPORTCONFIG];
  NR_SL_UE_sched_ctrl_t UE_sched_ctrl;
  NR_UE_sl_mac_stats_t mac_sl_stats;
  /* Per-HARQ-process NDI of the last TB already delivered upward for this source.
     SL uses blind HARQ retransmissions (RV cycle {0,2,3,1}); once a TB decodes, its
     remaining RVs also decode and would re-deliver the identical SDU, which PDCP then
     discards as duplicates ("discard NR PDU rcvd_count=N rx_deliv N+1"). Deliver a TB
     to RLC only on the first successful decode per (source, harq_pid, NDI). -1 = none. */
  int8_t sl_delivered_ndi[NR_MAX_HARQ_PROCESSES];
} NR_SL_UE_info_t;


typedef struct {

  NR_SL_UE_info_t *list[MAX_SL_UE_CONNECTIONS+1];
  uid_allocator_t ue_allocator;
  pthread_mutex_t mutex;
} NR_SL_UEs_t;

typedef struct {
  frameslot_t frame_slot;
  uint16_t rsvp; // The resource reservation period in ms
  uint8_t subch_len; // The total number of the sub-channel allocated
  uint8_t subch_start; // The index of the starting sub-channel allocated
  uint8_t prio; // The priority
  int16_t sl_rsrp; // The measured RSRP value over the used resource blocks
  uint8_t gap_re_tx1; // Gap for a first retransmission in absolute slots
  uint8_t subch_startre_tx1; // The index of the starting sub-channel allocated
                          // to first retransmission
  uint8_t gap_re_tx2; // Gap for a second retransmission in absolute slots
  uint8_t subch_startre_tx2; // The index of the starting sub-channel allocated
                          // to second retransmission
} sensing_data_t;

typedef struct {
  void* data;
  size_t element_size;
  size_t size;
  size_t capacity;
} List_t;

typedef struct {
  List_t* lists;
  size_t size;
  size_t capacity;
} vec_of_list_t;

typedef enum {
  c1, c2, c3, c4, c5, c6, c7
} allowed_rsc_selection_t;

typedef struct {
  uint16_t num_sl_pscch_rbs;
  uint16_t sl_pscch_sym_start;
  uint16_t sl_pscch_sym_len;
  uint16_t sl_pssch_sym_start;
  uint16_t sl_pssch_sym_len;
  uint16_t sl_subchan_size;
  uint16_t sl_max_num_per_reserve;
  uint8_t sl_psfch_period;
  uint8_t sl_min_time_gap_psfch;
  uint8_t sl_min_time_gap_processing;
  frameslot_t sfn;
  uint8_t sl_subchan_start;
  uint8_t sl_subchan_len;
  uint16_t sl_timeresource_cg_type1;
  uint16_t sl_freqresource_cg_type1;
  cg_type_t cg_type;
  bool slot_busy;
} sl_resource_info_t;

/**
 * \brief Structure to denote a future resource reserved by another UE
 *
 * This data structure represents resources excluded by step 6c) of the
 * TS 38.214 Sec. 8.1.4 sensing algorithm.
 */
typedef struct {
  frameslot_t sfn; // The SfnSf
  uint16_t rsvp; // The resource reservation period in ms
  uint8_t sb_ch_length; // The total number of the sub-channel allocated
  uint8_t sb_ch_start; // The index of the starting sub-channel allocated
  uint8_t prio; // The priority
  double sl_rsrp; // The measured RSRP value over the used resource blocks
} reserved_resource_t;

typedef struct {
  // PSCCH
  uint16_t num_sl_pscch_rbs; // Indicates the number of PRBs for PSCCH in a resource pool where it is not
                             // greater than the number PRBs of the subchannel.
  uint16_t sl_pscch_sym_start; // Indicates the starting symbol used for sidelink PSCCH in a slot
  uint16_t sl_pscch_sym_len; // Indicates the total number of symbols available for sidelink PSCCH
  // PSSCH
  uint16_t sl_pssch_sym_start; // Indicates the starting symbol used for sidelink PSSCH in a slot
  uint16_t sl_pssch_sym_len; // Indicates the total number of symbols available for sidelink PSSCH
  bool sl_has_psfch; // Indicates whether PSFCH is present in the slot
                     // subchannel size in RBs
  uint16_t sl_sub_chan_size; // Indicates the subchannel size in number of RBs
  uint16_t sl_max_num_per_reserve; // The maximum number of reserved PSCCH/PSSCH resources
                                   // that can be indicated by an SCI.
  uint64_t abs_slot_index; // Indicates the the absolute slot index
  uint32_t slot_offset; // Indicates the positive offset between two slots
} slot_info_t;

typedef struct {
  uint8_t bwp_id;
  sl_config_grant_t *sl_cg[MAX_GRANTS];
} sl_config_grant_bwp_t;

typedef struct {
  uint16_t src_id;
  uint8_t sl_harq_pid;
  uint8_t harq_status;   // 1: ACK, 0: NACK
  bool is_active;
} SL_HARQ_ENTRY_t;

typedef struct {
  uint16_t src_id;       // Remote UE Identity
  bool is_active;        // Slot is occupied by a known UE
} SL_REMOTE_UE_MAP_t;

typedef struct {
  SL_HARQ_ENTRY_t sl_harq_table[MAX_SL_HARQ_PROCESSES];
  SL_REMOTE_UE_MAP_t remote_ue_mapping[MAX_REMOTE_UES];
  uint8_t active_sl_harq_count;
  uint32_t sl_pucch_period;
  uint32_t next_pucch_slot;
} SL_REPORT_CONFIG_t;

#endif /* __LAYER2_NR_MAC_UE_MAC_DEFS_SL_SCHED_H__ */
