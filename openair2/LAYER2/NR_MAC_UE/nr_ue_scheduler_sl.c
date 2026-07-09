/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "mac_defs.h"
#include "mac_proto.h"
#include "nr_ue_sci.h"                                  // nr_schedule_slsch, fill_pssch_pscch_pdu, config_pssch_*_rx
#include "openair2/LAYER2/nr_rlc/nr_rlc_oai_api.h"      // nr_mac_rlc_status_ind_sl / data_req_sl (SL DRB)
#include "executables/softmodem-common.h"               // SL_MCS (--mcs cmdline override)

// episys SL data-plane port: F1 minimal — SL DRB is drb_id 1; broadcast dest id. MCS comes from --mcs
// (SL_MCS) when set, else the default below. Both TX and RX must use the same MCS (same SCI-1).
#define SL_F1_DRB_ID 1
#define SL_F1_BROADCAST_DEST 0xFFFF
#define SL_F1_DEFAULT_MCS 9

// episys SL PSFCH port (Stage 4b): RV sequence by HARQ round (TS 38.212 / episys nr_slsch_scheduler.c).
static const uint8_t nr_rv_round_map[4] = {0, 2, 3, 1};

// Effective SL MCS: cmdline --mcs if provided (>=0), otherwise the F1 default.
static inline uint8_t sl_effective_mcs(void)
{
  int mcs = get_softmodem_params()->mcs;
  return (mcs >= 0) ? (uint8_t)mcs : SL_F1_DEFAULT_MCS;
}

static uint16_t sl_adjust_ssb_indices(sl_ssb_timealloc_t *ssb_timealloc, uint32_t slot_in_16frames, uint16_t *ssb_slot_ptr)
{
  uint16_t ssb_slot = ssb_timealloc->sl_TimeOffsetSSB;
  uint16_t numssb = 0;
  *ssb_slot_ptr = 0;

  if (ssb_timealloc->sl_NumSSB_WithinPeriod == 0) {
    *ssb_slot_ptr = 0;
    return 0;
  }

  while (slot_in_16frames > ssb_slot) {
    numssb = numssb + 1;
    if (numssb < ssb_timealloc->sl_NumSSB_WithinPeriod)
      ssb_slot = ssb_slot + ssb_timealloc->sl_TimeInterval;
    else
      break;
  }

  *ssb_slot_ptr = ssb_slot;

  return numssb;
}

static uint8_t sl_get_elapsed_slots(uint32_t slot, uint32_t sl_slot_bitmap)
{
  uint8_t elapsed_slots = 0;

  for (int i = 0; i < slot; i++) {
    if (sl_slot_bitmap & (1 << i))
      elapsed_slots++;
  }

  return elapsed_slots;
}

/*
 * This function determines if the mixed slot is a Sidelink slot
 */
static uint8_t sl_determine_if_sidelink_slot(uint8_t sl_startsym, uint8_t sl_lensym, uint8_t num_ulsym)
{
  uint8_t ul_startsym = NR_SYMBOLS_PER_SLOT - num_ulsym;

  if ((sl_startsym >= ul_startsym) && (sl_lensym <= NR_SYMBOLS_PER_SLOT)) {
    LOG_D(MAC,
          "MIXED SLOT is a SIDELINK SLOT. Sidelink Symbols: %d-%d, Uplink Symbols: %d-%d\n",
          sl_startsym,
          sl_lensym - 1,
          ul_startsym,
          ul_startsym + num_ulsym - 1);
    return NR_SIDELINK_SLOT;
  } else {
    LOG_D(MAC,
          "MIXED SLOT is NOT SIDELINK SLOT. Sidelink Symbols: %d-%d, Uplink Symbols: %d-%d\n",
          sl_startsym,
          sl_lensym - 1,
          ul_startsym,
          ul_startsym + num_ulsym - 1);
    return 0;
  }
}

/*
 * This function determines if the Slot is a SIDELINK SLOT
 * Every Uplink Slot is a Sidelink slot
 * Mixed Slot is a sidelink slot if the uplink symbols in Mixed slot
 * overlaps with Sidelink start symbol and number of symbols.
 */
int sl_nr_ue_slot_select(const sl_nr_phy_config_request_t *cfg, int slot, uint8_t frame_duplex_type)
{
  int ul_sym = 0, slot_type = 0;

  // All PC5 bands are TDD bands , hence handling only TDD in this function.
  AssertFatal(frame_duplex_type == TDD, "No Sidelink operation defined for FDD in 3GPP rel16\n");

  if (cfg->tdd_table.max_tdd_periodicity_list == NULL) { // this happens before receiving TDD configuration
    return slot_type;
  }

  int period = cfg->tdd_table.tdd_period_in_slots;
  int rel_slot = slot % period;
  const fapi_nr_tdd_table_t *tdd_table = &cfg->tdd_table;

  const fapi_nr_max_tdd_periodicity_t *current_slot = &tdd_table->max_tdd_periodicity_list[rel_slot];

  for (int symbol_count = 0; symbol_count < NR_SYMBOLS_PER_SLOT; symbol_count++) {
    if (current_slot->max_num_of_symbol_per_slot_list[symbol_count].slot_config == 1) {
      ul_sym++;
    }
  }

  if (ul_sym == NR_SYMBOLS_PER_SLOT) {
    slot_type = NR_SIDELINK_SLOT;
  } else if (ul_sym) {
    slot_type = sl_determine_if_sidelink_slot(cfg->sl_bwp_config.sl_start_symbol, cfg->sl_bwp_config.sl_num_symbols, ul_sym);
  }

  return slot_type;
}

static void sl_determine_slot_bitmap(sl_nr_ue_mac_params_t *sl_mac, int ue_id)
{

  sl_nr_phy_config_request_t *sl_cfg = &sl_mac->sl_phy_config.sl_config_req;

  uint8_t sl_scs = sl_cfg->sl_bwp_config.sl_scs;
  uint8_t num_slots_per_frame = 10 * (1 << sl_scs);
  uint8_t slot_type = 0;
  for (int i = 0; i < num_slots_per_frame; i++) {
    slot_type = sl_nr_ue_slot_select(sl_cfg, i, TDD);
    if (slot_type == NR_SIDELINK_SLOT) {
      sl_mac->N_SL_SLOTS_perframe += 1;
      sl_mac->sl_slot_bitmap |= (1 << i);
    }
  }

  sl_mac->future_ttis = calloc(num_slots_per_frame, sizeof(sl_stored_tti_req_t));

  LOG_I(NR_MAC,
        "[UE%d] SL-MAC: N_SL_SLOTS_perframe:%d, SL SLOT bitmap:%x\n",
        ue_id,
        sl_mac->N_SL_SLOTS_perframe,
        sl_mac->sl_slot_bitmap);
}

/* This function determines the number of sidelink slots in 1024 frames - DFN cycle
 * which can be used for determining reserved slots and REsource pool slots according to bitmap.
 * Sidelink slots are the uplink and mixed slots with sidelink support except the SSB slots.
 */
static uint32_t sl_determine_num_sidelink_slots(sl_nr_ue_mac_params_t *sl_mac, int ue_id, uint16_t *N_SSB_16frames)
{

  uint32_t N_SSB_1024frames = 0;
  uint32_t N_SL_SLOTS = 0;
  *N_SSB_16frames = 0;

  if (sl_mac->rx_sl_bch.status) {
    sl_ssb_timealloc_t *ssb_timealloc = &sl_mac->rx_sl_bch.ssb_time_alloc;
    *N_SSB_16frames += ssb_timealloc->sl_NumSSB_WithinPeriod;
    LOG_D(NR_MAC, "RX SSB Slots:%d\n", *N_SSB_16frames);
  }

  if (sl_mac->tx_sl_bch.status) {
    sl_ssb_timealloc_t *ssb_timealloc = &sl_mac->tx_sl_bch.ssb_time_alloc;
    *N_SSB_16frames += ssb_timealloc->sl_NumSSB_WithinPeriod;
    LOG_D(NR_MAC, "TX SSB Slots:%d\n", *N_SSB_16frames);
  }

  // Total SSB slots in SFN cycle (1024 frames)
  N_SSB_1024frames = SL_FRAME_NUMBER_CYCLE / SL_NR_SSB_REPETITION_IN_FRAMES * (*N_SSB_16frames);

  // Determine total number of Valid Sidelink slots which can be used for Respool in a SFN cycle (1024 frames)
  N_SL_SLOTS = (sl_mac->N_SL_SLOTS_perframe * SL_FRAME_NUMBER_CYCLE) - N_SSB_1024frames;

  LOG_I(NR_MAC,
        "[UE%d]SL-MAC:SSB slots in 1024 frames:%d, N_SL_SLOTS_perframe:%d, N_SL_SLOTs in 1024 frames:%d, SL SLOT bitmap:%x\n",
        ue_id,
        N_SSB_1024frames,
        sl_mac->N_SL_SLOTS_perframe,
        N_SL_SLOTS,
        sl_mac->sl_slot_bitmap);

  return N_SL_SLOTS;
}

/**
 * DETERMINE IF SLOT IS MARKED AS SSB SLOT
 * ACCORDING TO THE SSB TIME ALLOCATION PARAMETERS.
 * sl_numSSB_withinPeriod - NUM SSBS in 16frames
 * sl_timeoffset_SSB - time offset for first SSB at start of 16 frames cycle
 * sl_timeinterval - distance in slots between 2 SSBs
 */
uint8_t sl_determine_if_SSB_slot(uint16_t frame, uint16_t slot, uint16_t slots_per_frame, sl_bch_params_t *sl_bch)
{
  uint16_t frame_16 = frame % SL_NR_SSB_REPETITION_IN_FRAMES;
  uint32_t slot_in_16frames = (frame_16 * slots_per_frame) + slot;
  uint16_t sl_NumSSB_WithinPeriod = sl_bch->ssb_time_alloc.sl_NumSSB_WithinPeriod;
  uint16_t sl_TimeOffsetSSB = sl_bch->ssb_time_alloc.sl_TimeOffsetSSB;
  uint16_t sl_TimeInterval = sl_bch->ssb_time_alloc.sl_TimeInterval;
  uint16_t num_ssb = sl_bch->num_ssb, ssb_slot = sl_bch->ssb_slot;

#ifdef SL_DEBUG
  LOG_D(NR_MAC,
        "%d:%d. num_ssb:%d,ssb_slot:%d, %d-%d-%d, status:%d\n",
        frame,
        slot,
        sl_bch->num_ssb,
        sl_bch->ssb_slot,
        sl_NumSSB_WithinPeriod,
        sl_TimeOffsetSSB,
        sl_TimeInterval,
        sl_bch->status);
#endif

  if (sl_NumSSB_WithinPeriod && sl_bch->status) {
    if (slot_in_16frames == sl_TimeOffsetSSB) {
      num_ssb = 0;
      ssb_slot = sl_TimeOffsetSSB;
    }

    if (num_ssb < sl_NumSSB_WithinPeriod && slot_in_16frames == ssb_slot) {
      num_ssb += 1;
      ssb_slot = (num_ssb < sl_NumSSB_WithinPeriod) ? (ssb_slot + sl_TimeInterval) : sl_TimeOffsetSSB;

      sl_bch->ssb_slot = ssb_slot;
      sl_bch->num_ssb = num_ssb;

      LOG_D(NR_MAC, "%d:%d is a PSBCH SLOT. Next PSBCH Slot:%d, num_ssb:%d\n", frame, slot, sl_bch->ssb_slot, sl_bch->num_ssb);

      return 1;
    }
  }

  LOG_D(NR_MAC, "%d:%d is NOT a PSBCH SLOT. Next PSBCH Slot:%d, num_ssb:%d\n", frame, slot, sl_bch->ssb_slot, sl_bch->num_ssb);
  return 0;
}

static uint8_t sl_psbch_scheduler(sl_nr_ue_mac_params_t *sl_mac_params, int ue_id, int frame, int slot, int slots_per_frame)
{
  uint8_t config_type = 0, is_psbch_rx_slot = 0, is_psbch_tx_slot = 0;
  if (sl_mac_params->rx_sl_bch.status) {
    is_psbch_rx_slot = sl_determine_if_SSB_slot(frame, slot, slots_per_frame, &sl_mac_params->rx_sl_bch);

    if (is_psbch_rx_slot)
      config_type = SL_NR_CONFIG_TYPE_RX_PSBCH;

  } else if (sl_mac_params->tx_sl_bch.status) {
    is_psbch_tx_slot = sl_determine_if_SSB_slot(frame, slot, slots_per_frame, &sl_mac_params->tx_sl_bch);

    if (is_psbch_tx_slot)
      config_type = SL_NR_CONFIG_TYPE_TX_PSBCH;
  }

  sl_mac_params->future_ttis[slot].frame = frame;
  sl_mac_params->future_ttis[slot].slot = slot;
  sl_mac_params->future_ttis[slot].sl_action = config_type;

  LOG_D(NR_MAC, "[UE%d] SL-PSBCH SCHEDULER: %d:%d, config type:%d\n", ue_id, frame, slot, config_type);
  return config_type;
}

/*
 * This function calculates the indices based on the new timing (frame,slot)
 * acquired by the UE.
 * NUM SSB, SLOT_SSB needs to be calculated based on current timing
 */
static void sl_adjust_indices_based_on_timing(sl_nr_ue_mac_params_t *sl_mac,
                                              int ue_id,
                                              int frame, int slot,
                                              int slots_per_frame)
{

  uint8_t elapsed_slots = 0;

  elapsed_slots = sl_get_elapsed_slots(slot, sl_mac->sl_slot_bitmap);
  AssertFatal(elapsed_slots <= sl_mac->N_SL_SLOTS_perframe,
              "Elapsed slots cannot be > N_SL_SLOTS_perframe %d,%d\n",
              elapsed_slots,
              sl_mac->N_SL_SLOTS_perframe);

  uint16_t frame_16 = frame % SL_NR_SSB_REPETITION_IN_FRAMES;
  uint32_t slot_in_16frames = (frame_16 * slots_per_frame) + slot;
  LOG_I(NR_MAC,
        "[UE%d]PSBCH params adjusted based on current timing %d:%d. frame_16:%d, slot_in_16frames:%d\n",
        ue_id,
        frame,
        slot,
        frame_16,
        slot_in_16frames);

  // Adjust PSBCH Indices based on current timing
  if (sl_mac->rx_sl_bch.status) {
    sl_ssb_timealloc_t *ssb_timealloc = &sl_mac->rx_sl_bch.ssb_time_alloc;
    sl_mac->rx_sl_bch.num_ssb = sl_adjust_ssb_indices(ssb_timealloc, slot_in_16frames, &sl_mac->rx_sl_bch.ssb_slot);

    LOG_I(NR_MAC,
          "[UE%d]PSBCH RX params adjusted. NumSSB:%d, ssb_slot:%d\n",
          ue_id,
          sl_mac->rx_sl_bch.num_ssb,
          sl_mac->rx_sl_bch.ssb_slot);
  }

  if (sl_mac->tx_sl_bch.status) {
    sl_ssb_timealloc_t *ssb_timealloc = &sl_mac->tx_sl_bch.ssb_time_alloc;
    sl_mac->tx_sl_bch.num_ssb = sl_adjust_ssb_indices(ssb_timealloc, slot_in_16frames, &sl_mac->tx_sl_bch.ssb_slot);

    LOG_I(NR_MAC,
          "[UE%d]PSBCH TX params adjusted. NumSSB:%d, ssb_slot:%d\n",
          ue_id,
          sl_mac->tx_sl_bch.num_ssb,
          sl_mac->tx_sl_bch.ssb_slot);
  }
}

// Adjust indices as new timing is acquired
static void sl_actions_after_new_timing(sl_nr_ue_mac_params_t *sl_mac, int ue_id, int frame, int slot, int slots_per_frame)
{
  sl_determine_slot_bitmap(sl_mac, ue_id);
  sl_mac->N_SL_SLOTS = sl_determine_num_sidelink_slots(sl_mac, ue_id, &sl_mac->N_SSB_16frames);
  sl_adjust_indices_based_on_timing(sl_mac, ue_id, frame, slot, slots_per_frame);
}

static void sl_schedule_rx_actions(nr_sidelink_indication_t *sl_ind, NR_UE_MAC_INST_t *mac)
{

  sl_nr_ue_mac_params_t *sl_mac = mac->SL_MAC_PARAMS;
  int ue_id = mac->ue_id;
  int rx_action = 0;

  sl_nr_rx_config_request_t rx_config;
  rx_config.number_pdus = 0;
  rx_config.sfn = sl_ind->frame_rx;
  rx_config.slot = sl_ind->slot_rx;

  if (sl_ind->sci_ind != NULL) {
    // TBD..
  } else {
    rx_action = sl_mac->future_ttis[sl_ind->slot_rx].sl_action;
  }

  if (rx_action == SL_NR_CONFIG_TYPE_RX_PSBCH) {
    rx_config.number_pdus = 1;
    rx_config.sl_rx_config_list[0].pdu_type = rx_action;

    LOG_D(NR_MAC, "[UE%d] %d:%d CMD to PHY: RX PSBCH \n", ue_id, sl_ind->frame_rx, sl_ind->slot_rx);

  } else if (rx_action >= SL_NR_CONFIG_TYPE_RX_PSCCH && rx_action <= SL_NR_CONFIG_TYPE_RX_PSSCH_SLSCH) {
    // episys SL data-plane port (F1 minimal): blind PSSCH RX config. Both UEs share the pool config, so we
    // build the SCI-1 the transmitter would use and derive the demod (SCI-2) + SLSCH transport RX configs.
    const NR_SL_ResourcePool_r16_t *respool =
        (sl_mac->sl_RxPool[0] && sl_mac->sl_RxPool[0]->respool) ? sl_mac->sl_RxPool[0]->respool : mac->sl_tx_res_pool;
    const struct NR_SL_BWP_Generic_r16 *bwp_gen = sl_mac->sl_bwp_generic;
    if (respool && bwp_gen) {
      nr_sci_pdu_t sci1 = {0}, sci2 = {0};
      nr_schedule_slsch(respool, &sci1, &sci2, 0, 0, 0, 0, SL_F1_BROADCAST_DEST, sl_effective_mcs());
      nr_sci_size(respool, &sci1, NR_SL_SCI_FORMAT_1A); // fill 1st-stage nbits used by the RX config builders
      // merge the 2nd-stage fields the RX config reads into the single combined SCI-1 PDU
      sci1.harq_pid = sci2.harq_pid;
      sci1.ndi = sci2.ndi;
      sci1.rv_index = sci2.rv_index;
      uint32_t pscch_Nid = 0; // TODO(OTA): must equal the TX Nid (CRC-derived in nr_generate_sci1) — reconcile.
      config_pssch_sci_pdu_rx(&rx_config.sl_rx_config_list[0].rx_sci2_config_pdu, NR_SL_SCI_FORMAT_2A, &sci1,
                              pscch_Nid, 0, bwp_gen, respool);
      config_pssch_slsch_pdu_rx(&rx_config.sl_rx_config_list[1].rx_pssch_config_pdu, &sci1, bwp_gen, respool);
      rx_config.number_pdus = 2;
      rx_config.sl_rx_config_list[0].pdu_type = SL_NR_CONFIG_TYPE_RX_PSSCH_SLSCH;
      rx_config.sl_rx_config_list[1].pdu_type = SL_NR_CONFIG_TYPE_RX_PSSCH_SLSCH;
      LOG_D(NR_MAC, "[UE%d] %d:%d CMD to PHY: RX PSSCH/SLSCH (Nid=%u)\n", ue_id, sl_ind->frame_rx, sl_ind->slot_rx, pscch_Nid);
    }

  } else if (rx_action == SL_NR_CONFIG_TYPE_RX_PSFCH) {
    // TBD (F2 PSFCH)
  }

  // episys SL PSFCH port (Stage 4d): if a HARQ process is awaiting feedback this slot, attach the PSFCH
  // (HARQ ACK/NACK) decode config to the (blind) PSSCH RX config already built above and upgrade its type.
  configure_psfch_params_rx(ue_id, mac, sl_ind->frame_rx, sl_ind->slot_rx, &rx_config);

  if (rx_config.number_pdus) {
    AssertFatal(sl_ind->slot_type == SIDELINK_SLOT_TYPE_RX || sl_ind->slot_type == SIDELINK_SLOT_TYPE_BOTH,
                "RX action cannot be scheduled in non Sidelink RX slot\n");

    nr_scheduled_response_t scheduled_response = {.sl_rx_config = &rx_config,
                                                  .module_id = sl_ind->module_id,
                                                  .CC_id = sl_ind->cc_id,
                                                  .phy_data = sl_ind->phy_data,
                                                  .mac = mac};

    sl_mac->future_ttis[sl_ind->slot_rx].sl_action = 0;

    if ((mac->if_module != NULL) && (mac->if_module->scheduled_response != NULL))
      mac->if_module->scheduled_response(&scheduled_response);
  }
}

// episys SL PSFCH port (Stage 4c): if any pending HARQ-feedback resource is scheduled for (frame,slot),
// build the PSFCH TX config from the stored sched_psfch[] entries (filled at RX by configure_psfch_params_tx).
// Returns the number of PSFCH PDUs scheduled (0 => nothing to send this slot).
static int nr_ue_sl_psfch_scheduler(NR_UE_MAC_INST_t *mac, int frame, int slot, sl_nr_tx_config_request_t *tx_config)
{
  sl_nr_ue_mac_params_t *sl_mac = mac->SL_MAC_PARAMS;
  if (!mac->sl_info.list[0] || !mac->sl_tx_res_pool || !mac->sl_tx_res_pool->sl_PSFCH_Config_r16
      || !sl_mac || !sl_mac->sl_TDD_config)
    return 0;
  NR_SL_UE_sched_ctrl_t *sc = &mac->sl_info.list[0]->UE_sched_ctrl;
  NR_TDD_UL_DL_Pattern_t *tdd = &sl_mac->sl_TDD_config->pattern1;
  int n_ul = tdd->nrofUplinkSlots + (tdd->nrofUplinkSymbols > 0 ? 1 : 0);
  uint16_t num_subch = (sl_mac->sl_TxPool[0] && sl_mac->sl_TxPool[0]->num_subch) ? sl_mac->sl_TxPool[0]->num_subch : 1;
  int nfb = n_ul * num_subch;
  sl_nr_tx_rx_config_psfch_pdu_t *list = NULL;
  int k = 0;
  for (int i = 0; i < nfb; i++) {
    SL_sched_feedback_t *sp = &sc->sched_psfch[i];
    if (sp->feedback_slot == slot && sp->feedback_frame == frame) {
      if (!list)
        list = calloc(nfb, sizeof(*list));
      list[k].nr_of_symbols = 1;
      list[k].start_symbol_index = sp->start_symbol_index;
      list[k].hopping_id = sp->hopping_id;
      list[k].prb = sp->prb;
      list[k].sl_bwp_start = sp->sl_bwp_start;
      list[k].initial_cyclic_shift = sp->initial_cyclic_shift;
      list[k].mcs = sp->mcs;
      list[k].bit_len_harq = sp->bit_len_harq;
      list[k].freq_hop_flag = sp->freq_hop_flag;
      list[k].group_hop_flag = sp->group_hop_flag;
      list[k].sequence_hop_flag = sp->sequence_hop_flag;
      list[k].second_hop_prb = sp->second_hop_prb;
      LOG_D(NR_MAC, "SL PSFCH TX %d.%d ics=%d mcs=%d prb=%d ssym=%d hop=%d nsym=%d bit_len=%d\n",
            frame, slot, list[k].initial_cyclic_shift, list[k].mcs, list[k].prb, list[k].start_symbol_index,
            list[k].hopping_id, list[k].nr_of_symbols, list[k].bit_len_harq);
      k++;
      sp->feedback_slot = -1;
      sp->feedback_frame = -1;
    }
  }
  if (k > 0) {
    // MUX: attach the PSFCH to whatever TX config was already built for this slot. If PSSCH data is being
    // sent (number_pdus>0, pdu_type TX_PSCCH_PSSCH), the PSFCH rides in the last symbol of the SAME slot
    // (no data preemption). If no data, this is a standalone PSFCH TX. Either way psfch_pdu_list lives on
    // list[0].tx_pscch_pssch_config_pdu and the PHY generates PSFCH after (any) PSSCH.
    if (tx_config->number_pdus == 0) {
      tx_config->number_pdus = 1;
      tx_config->tx_config_list[0].pdu_type = SL_NR_CONFIG_TYPE_TX_PSFCH;
    }
    tx_config->tx_config_list[0].tx_pscch_pssch_config_pdu.psfch_pdu_list = list;
    tx_config->tx_config_list[0].tx_pscch_pssch_config_pdu.num_psfch_pdus = k;
    LOG_I(NR_MAC, "[UE%d] %d:%d CMD to PHY: TX PSFCH %d pdu(s) (muxed=%d)\n", mac->ue_id, frame, slot, k,
          tx_config->tx_config_list[0].pdu_type == SL_NR_CONFIG_TYPE_TX_PSCCH_PSSCH);
  }
  return k;
}

static void sl_schedule_tx_actions(nr_sidelink_indication_t *sl_ind, NR_UE_MAC_INST_t *mac)
{

  sl_nr_ue_mac_params_t *sl_mac = mac->SL_MAC_PARAMS;
  int ue_id = mac->ue_id;

  int tx_action = 0;
  sl_nr_tx_config_request_t tx_config;
  tx_config.number_pdus = 0;
  tx_config.sfn = sl_ind->frame_tx;
  tx_config.slot = sl_ind->slot_tx;

  tx_action = sl_mac->future_ttis[sl_ind->slot_tx].sl_action;

  if (tx_action == SL_NR_CONFIG_TYPE_TX_PSBCH) {
    tx_config.number_pdus = 1;
    tx_config.tx_config_list[0].pdu_type = tx_action;
    tx_config.tx_config_list[0].tx_psbch_config_pdu.tx_slss_id = sl_mac->tx_sl_bch.slss_id;
    tx_config.tx_config_list[0].tx_psbch_config_pdu.psbch_tx_power = 0; // TBD...
    memcpy(tx_config.tx_config_list[0].tx_psbch_config_pdu.psbch_payload, sl_mac->tx_sl_bch.sl_mib, 4);

    LOG_D(NR_MAC, "[UE%d] %d:%d CMD to PHY: TX PSBCH \n", ue_id, sl_ind->frame_tx, sl_ind->slot_tx);

  } else if (tx_action == SL_NR_CONFIG_TYPE_TX_PSCCH_PSSCH) {
    // episys SL data-plane port (F1 minimal): build the PSCCH+PSSCH grant and pull the SLSCH TB from the SL DRB.
    const NR_SL_ResourcePool_r16_t *respool = mac->sl_tx_res_pool;
    const struct NR_SL_BWP_Generic_r16 *bwp_gen = sl_mac->sl_bwp_generic;
    if (respool && bwp_gen) {
      sl_nr_tx_config_pscch_pssch_pdu_t *pdu = &tx_config.tx_config_list[0].tx_pscch_pssch_config_pdu;
      memset(pdu, 0, sizeof(*pdu));
      nr_sci_pdu_t sci1 = {0}, sci2 = {0};
      static uint8_t sl_ndi = 0;
      sl_ndi ^= 1;
      const uint8_t mcs = sl_effective_mcs();

      // episys SL PSFCH port (Stage 4b): HARQ-aware TX. When PSFCH is configured, allocate a HARQ
      // process (retransmission first, else a fresh one), arm it to expect feedback, and request
      // harq_feedback in the SCI. Falls back to the blind path (harq_pid 0, no feedback) otherwise.
      int8_t harq_pid = 0;
      uint8_t rv = 0;
      bool arm_feedback = false;
      bool is_retx = false;
      long psfch_period = 0;
      if (respool->sl_PSFCH_Config_r16 && respool->sl_PSFCH_Config_r16->choice.setup->sl_PSFCH_Period_r16
          && mac->sl_info.list[0]) {
        const uint8_t psfch_periods[] = {0, 1, 2, 4};
        psfch_period = psfch_periods[*respool->sl_PSFCH_Config_r16->choice.setup->sl_PSFCH_Period_r16];
        NR_SL_UE_sched_ctrl_t *sc = &mac->sl_info.list[0]->UE_sched_ctrl;
        // HARQ process selection: a NACKed process on retrans_sl_harq (round>0) is retransmitted FIRST
        // (re-sending the buffered TB, see the TB-bridge below); otherwise take a fresh process from
        // available_sl_harq for new data.
        int rp = sc->retrans_sl_harq.head;
        int ap = sc->available_sl_harq.head;
        if (rp >= 0) {
          harq_pid = rp;
          remove_nr_list(&sc->retrans_sl_harq, rp);
          is_retx = true;
        } else if (ap >= 0) {
          harq_pid = ap;
          remove_front_nr_list(&sc->available_sl_harq);
        } else {
          psfch_period = 0; // no HARQ process free -> blind TX this slot
        }
        if (psfch_period) {
          NR_UE_sl_harq_t *h = &sc->sl_harq_processes[harq_pid];
          // develop's blind SL RX has no soft-combining buffer, so incremental-redundancy RVs (2,3,1) are
          // undecodable. Force rv=0 -> chase combining: each (re)transmission is a self-decodable copy of
          // the SAME buffered TB; the RX decodes each attempt independently until one passes CRC.
          rv = 0;
          (void)nr_rv_round_map;
          // The peer (receiver) sends the PSFCH in ITS TX half = my RX half. If I am SyncRef my RX half
          // is {16-19} (use_first_half=false); if Nearby, {6-9} (true). So pass !sync_ref.
          int fb_slot = get_feedback_slot(psfch_period, sl_ind->slot_tx, !get_softmodem_params()->sync_ref);
          if (fb_slot >= 0) {
            int fb_frame = sl_ind->frame_tx;
            if (fb_slot < sl_ind->slot_tx)
              fb_frame = (fb_frame + 1) & 1023;
            h->feedback_slot = fb_slot;
            h->feedback_frame = fb_frame;
            h->is_waiting = true;
            h->sl_harq_pid = harq_pid;
            if (!is_retx)
              h->ndi ^= 1; // toggle NDI only for new data; a retx keeps the same NDI
            h->sched_pssch.slot = sl_ind->slot_tx; // PSSCH TX slot -> matching PSFCH RX PRB index (4d)
            add_tail_nr_list(&sc->feedback_sl_harq, harq_pid);
            arm_feedback = true;
            LOG_D(NR_MAC, "[UE%d] %d:%d SL TX HARQ pid %d round %d rv %d -> feedback %d:%d\n",
                  ue_id, sl_ind->frame_tx, sl_ind->slot_tx, harq_pid, h->round, rv, fb_frame, fb_slot);
          } else {
            // feedback slot not resolvable for this TX slot -> release pid, blind TX
            add_tail_nr_list(&sc->available_sl_harq, harq_pid);
            LOG_D(NR_MAC, "[UE%d] %d:%d SL TX no PSFCH feedback slot (tx_slot %d) -> blind\n",
                  ue_id, sl_ind->frame_tx, sl_ind->slot_tx, sl_ind->slot_tx);
          }
        }
      }
      // populate SCI-1/SCI-2 field values, then assemble the PDU (nr_sci_size + packing + TB size inside).
      nr_schedule_slsch(respool, &sci1, &sci2, harq_pid, sl_ndi, rv, mac->src_id, SL_F1_BROADCAST_DEST, mcs);
      if (arm_feedback)
        sci2.harq_feedback = 1;
      fill_pssch_pscch_pdu(pdu, bwp_gen, respool, &sci1, &sci2, NR_SL_SCI_FORMAT_1A, NR_SL_SCI_FORMAT_2A);
      // TB bridge: on a NEW transmission pull one SLSCH RLC PDU from the SL DRB (prefixed by the 2-byte
      // SL-SCH subheader = RLC-PDU length) and BUFFER it in the HARQ process. On a HARQ RETRANSMISSION
      // (is_retx) re-send the buffered TB instead (chase combining, rv=0) so the NACKed packet is actually
      // resent rather than dropped.
      NR_UE_sl_harq_t *htx = (psfch_period && mac->sl_info.list[0])
                                 ? &mac->sl_info.list[0]->UE_sched_ctrl.sl_harq_processes[harq_pid] : NULL;
      tbs_size_t len = 0;
      if (is_retx && htx && htx->tb_size > 0) {
        uint32_t cap = htx->tb_size <= SL_NR_MAX_SLSCH_PAYLOAD_BYTES ? htx->tb_size : SL_NR_MAX_SLSCH_PAYLOAD_BYTES;
        memcpy(pdu->slsch_payload, htx->transportBlock, cap);
        pdu->slsch_payload_len = cap;
        len = (cap > SL_SCH_SUBHEADER_LEN) ? (tbs_size_t)(cap - SL_SCH_SUBHEADER_LEN) : 0;
        LOG_D(NR_MAC, "[UE%d] %d:%d SL HARQ RETX pid %d (buffered TB %d bytes)\n",
              ue_id, sl_ind->frame_tx, sl_ind->slot_tx, harq_pid, cap);
      } else {
        uint32_t tb = pdu->tb_size > SL_NR_MAX_SLSCH_PAYLOAD_BYTES ? SL_NR_MAX_SLSCH_PAYLOAD_BYTES : pdu->tb_size;
        uint32_t room = (tb > SL_SCH_SUBHEADER_LEN) ? tb - SL_SCH_SUBHEADER_LEN : 0;
        len = nr_mac_rlc_data_req_sl(mac->src_id, SL_F1_DRB_ID, room, (char *)pdu->slsch_payload + SL_SCH_SUBHEADER_LEN);
        if (len > 0) {
          pdu->slsch_payload[0] = ((uint32_t)len >> 8) & 0xff;
          pdu->slsch_payload[1] = (uint32_t)len & 0xff;
          pdu->slsch_payload_len = (uint32_t)len + SL_SCH_SUBHEADER_LEN;
          // Buffer the TB (with subheader) for a possible HARQ retransmission on NACK.
          if (htx) {
            uint32_t cp = pdu->slsch_payload_len <= sizeof(htx->transportBlock) ? pdu->slsch_payload_len : sizeof(htx->transportBlock);
            memcpy(htx->transportBlock, pdu->slsch_payload, cp);
            htx->tb_size = cp;
          }
        } else {
          pdu->slsch_payload_len = 0;
        }
      }
      tx_config.number_pdus = 1;
      tx_config.tx_config_list[0].pdu_type = tx_action;
      LOG_I(NR_MAC, "[UE%d] %d:%d CMD to PHY: TX PSCCH/PSSCH tb_size %d (rlc %d) mcs %d\n",
            ue_id, sl_ind->frame_tx, sl_ind->slot_tx, pdu->tb_size, (int)len, mcs);
    }

  } else if (tx_action == SL_NR_CONFIG_TYPE_TX_PSFCH) {
    // TBD (F2 PSFCH)
  }

  // episys SL PSFCH port (Stage 4c/mux): attach any pending HARQ-feedback PSFCH to this slot's TX. If PSSCH
  // data was scheduled above, the PSFCH rides the SAME slot (last symbol) with NO data preemption; if not,
  // this becomes a standalone PSFCH TX. Must run AFTER the data branch so it muxes rather than replaces.
  nr_ue_sl_psfch_scheduler(mac, sl_ind->frame_tx, sl_ind->slot_tx, &tx_config);

  if (tx_config.number_pdus == 1) {
    AssertFatal(sl_ind->slot_type == SIDELINK_SLOT_TYPE_TX || sl_ind->slot_type == SIDELINK_SLOT_TYPE_BOTH,
                "TX action cannot be scheduled in non Sidelink TX slot\n");

    nr_scheduled_response_t scheduled_response = {.sl_tx_config = &tx_config,
                                                  .module_id = sl_ind->module_id,
                                                  .CC_id = sl_ind->cc_id,
                                                  .phy_data = sl_ind->phy_data,
                                                  .mac = mac};

    sl_mac->future_ttis[sl_ind->slot_tx].sl_action = 0;

    if ((mac->if_module != NULL) && (mac->if_module->scheduled_response != NULL))
      mac->if_module->scheduled_response(&scheduled_response);
  }
}

void nr_ue_sidelink_scheduler(nr_sidelink_indication_t *sl_ind, NR_UE_MAC_INST_t *mac)
{
  AssertFatal(sl_ind != NULL, "sl_indication cannot be NULL\n");
  sl_nr_ue_mac_params_t *sl_mac = mac->SL_MAC_PARAMS;
  int ue_id = mac->ue_id;

  LOG_D(NR_MAC,
        "[UE%d]SL-SCHEDULER: RX %d-%d- TX %d-%d. slot_type:%d\n",
        ue_id,
        sl_ind->frame_rx,
        sl_ind->slot_rx,
        sl_ind->frame_tx,
        sl_ind->slot_tx,
        sl_ind->slot_type);

  // Adjust indices as new timing is acquired
  if (sl_mac->timing_acquired) {
    sl_actions_after_new_timing(sl_mac, ue_id, sl_ind->frame_tx, sl_ind->slot_tx, mac->frame_structure.numb_slots_frame);
    sl_mac->timing_acquired = false;
  }

  if (sl_ind->slot_type == SIDELINK_SLOT_TYPE_TX || sl_ind->slot_type == SIDELINK_SLOT_TYPE_BOTH) {
    int frame = sl_ind->frame_tx;
    int slot = sl_ind->slot_tx;
    int is_sl_slot = 0;
    is_sl_slot = sl_mac->sl_slot_bitmap & (1 << slot);

    if (is_sl_slot) {
      uint8_t tti_action = 0;

      // Check if PSBCH slot and PSBCH should be transmitted or Received
      tti_action = sl_psbch_scheduler(sl_mac, ue_id, frame, slot, mac->frame_structure.numb_slots_frame);

      // episys SL data-plane port (F1 minimal): if this SL slot isn't a PSBCH slot, arbitrate the PSSCH data
      // plane. Half-duplex rule: transmit PSSCH when the SL DRB has data buffered, otherwise listen (RX PSSCH).
      // (Bidirectional/half-duplex refinement + sensing are follow-ups.)
      if (!tti_action && mac->sl_tx_res_pool && sl_mac->sl_bwp_generic) {
        mac_rlc_status_resp_t st = nr_mac_rlc_status_ind_sl(mac->src_id, SL_F1_DRB_ID, frame);
        tti_action = (st.bytes_in_buffer > 0) ? SL_NR_CONFIG_TYPE_TX_PSCCH_PSSCH : SL_NR_CONFIG_TYPE_RX_PSSCH_SLSCH;
        sl_mac->future_ttis[slot].sl_action = tti_action;
        LOG_D(NR_MAC, "[UE%d] %d:%d SL-SCHED data-plane: status_ind_sl(src_id=0x%x drb=%d)=%d bytes -> action %d\n",
              ue_id, frame, slot, mac->src_id, SL_F1_DRB_ID, st.bytes_in_buffer, tti_action);
      } else if (!tti_action) {
        static int warned = 0;
        if (!warned) { warned = 1;
          LOG_W(NR_MAC, "[UE%d] SL-SCHED data-plane branch SKIPPED: sl_tx_res_pool=%p sl_bwp_generic=%p\n",
                ue_id, (void *)mac->sl_tx_res_pool, (void *)sl_mac->sl_bwp_generic); }
      }

      LOG_D(NR_MAC, "[UE%d]SL-SCHED: TTI - %d:%d scheduled action:%d\n", ue_id, frame, slot, tti_action);

    } else {
      AssertFatal(1 == 0, "TX SLOT not a sidelink slot. Should not occur\n");
    }

    // Schedule the Tx actions if any
    sl_schedule_tx_actions(sl_ind, mac);
  }

  if (sl_ind->slot_type == SIDELINK_SLOT_TYPE_RX || sl_ind->slot_type == SIDELINK_SLOT_TYPE_BOTH)
    sl_schedule_rx_actions(sl_ind, mac);
}
