/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/* SL mode-2 sensing-based resource selection (TS 38.214 8.1.4).
 *
 * Why: our SL scheduler transmits on the lowest subchannel the moment it has data
 * (nr_ue_scheduler_sl.c: `tti_action = (tx_bytes > 0) ? TX : RX`), with no coordination between UEs.
 * "SyncRef" is only the SLSS/PSBCH timing role and confers no transmit turn, so two UEs with traffic
 * pick the same slot. Measured on the 2026-07-31 rfsim mode-2 run: 22 and 12 transmissions with
 * 6 in the SAME frame:slot, which accounted for the whole receive shortfall in one direction. A UE
 * cannot receive while it transmits, so every collision destroys a TB in both directions.
 *
 * The fix is the standard sensing chain: record what other UEs reserved (learned from their SCI-1A),
 * exclude those resources, and select from what is left. This file holds the port, kept separate from
 * nr_ue_scheduler_sl.c so it can be reviewed and reverted on its own.
 *
 * Staged deliberately — the data model (List_t, sensing_data_t, sl_resource_info_t, reserved_resource_t
 * in mac_defs_sl_sched.h, and mac->sl_sensing_data / sl_transmit_history / sl_candidate_resources in
 * mac_defs.h) was already ported previously; only the algorithms were missing.
 *
 *   Stage 1 (this file, now): list container + sensing/history ageing. Additive: nothing calls the
 *                             selector yet, so transmit behaviour is unchanged and no test can regress.
 *   Stage 2: SCI-1A out of shadow mode -> sci_ind -> MAC, so sl_sensing_data actually gets entries.
 *   Stage 3: remove_old_sensing_data / remove_old_transmit_history driven per slot.
 *   Stage 4: exclude_reserved_resources + get_candidate_resources (still not wired to TX).
 *   Stage 5: get_resource_element wired into the TX path — the first stage that changes when a UE
 *            transmits, and the only one that can move the collision count or regress rfsim/vrtsim.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "mac_defs.h"
#include "mac_proto.h"
#include "utils.h" // max() used by the reselection-counter bounds
#include "common/utils/LOG/log.h"
#include "common/utils/nr/nr_common.h"
#include "NR_SL-ResourcePool-r16.h"           // NR_SL_ResourcePool_r16_t + PSCCH/PSFCH sub-configs
#include "NR_SL-BWP-Generic-r16.h"            // NR_SL_BWP_Generic_r16_t (sl_StartSymbol / sl_LengthSymbols)
#include "NR_SL-UE-SelectedConfigRP-r16.h"    // sl_MaxNumPerReserve_r16

// develop keeps nr_slots_per_frame file-local to nr_mac_common_tdd.c; use the same standard formula
// the rest of the SL MAC uses (nr_ue_procedures_sl.c).
#define SL_SLOTS_PER_FRAME(mu) (10 << (mu))

// Defined in nr_ue_procedures_sl.c / nr_ue_sci_slsch.c; neither is declared in a header yet.
uint16_t sl_get_subchannel_size(NR_SL_ResourcePool_r16_t *rpool);
extern const int pscch_rb_table[5];
extern const int pscch_tda[2];

/* ---- generic list container -------------------------------------------------------------------
 * A plain growable array: the sensing store and the transmit history are both append-at-the-end /
 * drop-from-the-end, which is all the algorithm needs.
 */

void init_list(List_t *list, size_t element_size, size_t initial_capacity)
{
  AssertFatal(element_size > 0 && initial_capacity > 0, "init_list: bad element_size %zu capacity %zu\n",
              element_size, initial_capacity);
  list->data = calloc(1, element_size * initial_capacity);
  AssertFatal(list->data != NULL, "init_list: out of memory (%zu x %zu)\n", element_size, initial_capacity);
  list->element_size = element_size;
  list->size = 0;
  list->capacity = initial_capacity;
}

void push_back(List_t *list, void *element)
{
  if (list->size == list->capacity) {
    // capacity 0 would double to 0 and realloc(p, 0) forever; only reachable if init_list was skipped.
    list->capacity = list->capacity ? list->capacity * 2 : 1;
    void *grown = realloc(list->data, list->element_size * list->capacity);
    AssertFatal(grown != NULL, "push_back: realloc to %zu bytes failed\n", list->element_size * list->capacity);
    list->data = grown;
  }
  void *target = (char *)list->data + (list->size * list->element_size);
  memcpy(target, element, list->element_size);
  list->size++;
}

void pop_back(List_t *list)
{
  if (list->size > 0)
    list->size--;
}

void free_list_mem(List_t *list)
{
  free(list->data);
  list->data = NULL;
  list->size = 0;
  list->capacity = 0;
}

/* ---- sensing / transmit-history ageing ---------------------------------------------------------
 * Both lists are ordered newest-first, so entries younger than T_proc0 (the UE's own processing time,
 * during which a resource cannot be acted on) are dropped from the end. The modulo arithmetic over
 * SL_SLOTS_PER_FRAME(mu) * 1024 handles the SFN wrap.
 */

uint16_t time_to_slots(uint8_t mu, uint16_t time)
{
  const uint8_t slots_per_ms = (uint8_t)pow(2, mu); // subframe is of 1 ms
  return time * slots_per_ms;
}

uint8_t get_tproc0(sl_nr_ue_mac_params_t *sl_mac, uint16_t pool_id)
{
  AssertFatal(sl_mac->sl_TxPool[pool_id] != NULL, "get_tproc0: no TX pool %u\n", pool_id);
  return sl_mac->sl_TxPool[pool_id]->tproc0;
}

void update_sensing_data(List_t *sensing_data, frameslot_t *frame_slot, sl_nr_ue_mac_params_t *sl_mac, uint16_t pool_id)
{
  const uint8_t mu = sl_mac->sl_phy_config.sl_config_req.sl_bwp_config.sl_scs;
  const int64_t num_max_slots = (int64_t)SL_SLOTS_PER_FRAME(mu) * 1024;
  const uint8_t tproc0 = get_tproc0(sl_mac, pool_id);

  while (sensing_data->size > 0) {
    const sensing_data_t *last =
        (const sensing_data_t *)((char *)sensing_data->data + (sensing_data->size - 1) * sensing_data->element_size);
    const int64_t diff =
        (normalize(frame_slot, mu) - normalize((frameslot_t *)&last->frame_slot, mu) + num_max_slots) % num_max_slots;
    if (diff <= tproc0)
      pop_back(sensing_data);
    else
      break;
  }
}

/* ---- sensing-window trim -----------------------------------------------------------------------
 * Called on every SCI-1A reception. The list is ordered
 * oldest-first here, so entries older than the sensing window T0 are dropped from the FRONT by counting
 * the stale prefix and memmove-ing the remainder down. The loop breaks at the first in-window entry.
 */
void remove_old_sensing_data(frameslot_t *frame_slot,
                             uint16_t sensing_window,
                             List_t *sensing_data,
                             sl_nr_ue_mac_params_t *sl_mac)
{
  int new_size = 0;
  const int mu = sl_mac->sl_phy_config.sl_config_req.sl_bwp_config.sl_scs;
  const int64_t num_max_slots = (int64_t)SL_SLOTS_PER_FRAME(mu) * 1024;

  for (int i = 0; i < sensing_data->size; i++) {
    sensing_data_t *data = (sensing_data_t *)((char *)sensing_data->data + i * sensing_data->element_size);
    const int64_t diff =
        (normalize(frame_slot, mu) - normalize(&data->frame_slot, mu) + num_max_slots) % num_max_slots;
    if (diff <= sensing_window)
      break;
    new_size++;
  }

  if (new_size > 0) {
    LOG_D(NR_MAC, "sensing data: size %ld, element_size %ld, dropping %d stale entries\n",
          sensing_data->size, sensing_data->element_size, new_size);
    memmove(sensing_data->data,
            (char *)sensing_data->data + new_size * sensing_data->element_size,
            (sensing_data->size - new_size) * sensing_data->element_size);
    sensing_data->size -= new_size;
  }
}

/* ---- reselection counter (TS 38.321 5.22.1.1) ---------------------------------------------------
 * NOTE: the LOWER bound is returned rather than a random draw in the range, matching the reference
 * implementation this was ported from (its rand() call is commented out). Kept as-is so behaviour matches.
 */
uint8_t get_lower_bound_resel_counter(uint16_t p_rsrv)
{
  AssertFatal(p_rsrv < 100, "Resource reservation must be less than 100 ms\n");
  return (uint8_t)(5 * ceil(100 / (double)(max(20, p_rsrv))));
}

uint8_t get_upper_bound_resel_counter(uint16_t p_rsrv)
{
  AssertFatal(p_rsrv < 100, "Resource reservation must be less than 100 ms\n");
  return (uint8_t)(15 * ceil(100 / (double)(max(20, p_rsrv))));
}

uint8_t get_random_reselection_counter(uint16_t rri)
{
  uint8_t min_res_cntr = 0;
  uint8_t max_res_cntr = 0;

  switch (rri) {
    case 100: case 150: case 200: case 250: case 300:
    case 350: case 400: case 450: case 500: case 550:
    case 600: case 700: case 750: case 800: case 850:
    case 900: case 950: case 1000:
      min_res_cntr = 5;
      max_res_cntr = 15;
      break;
    default:
      if (rri < 100) {
        min_res_cntr = get_lower_bound_resel_counter(rri);
        max_res_cntr = get_upper_bound_resel_counter(rri);
      } else {
        LOG_E(NR_MAC, "Resource reservation interval %d not supported!\n", rri);
      }
      break;
  }

  LOG_D(NR_MAC, "Range to choose random reselection counter. min: %d max: %d\n", min_res_cntr, max_res_cntr);
  // Deterministic lower bound, matching the reference implementation (its rand() draw is commented out).
  (void)max_res_cntr;
  return min_res_cntr;
}

void update_transmit_history(List_t *transmit_history,
                             frameslot_t *frame_slot,
                             sl_nr_ue_mac_params_t *sl_mac,
                             uint16_t pool_id)
{
  const uint8_t mu = sl_mac->sl_phy_config.sl_config_req.sl_bwp_config.sl_scs;
  const int64_t num_max_slots = (int64_t)SL_SLOTS_PER_FRAME(mu) * 1024;
  const uint8_t tproc0 = get_tproc0(sl_mac, pool_id);

  while (transmit_history->size > 0) {
    frameslot_t *last =
        (frameslot_t *)((char *)transmit_history->data + (transmit_history->size - 1) * transmit_history->element_size);
    const int64_t diff = (normalize(frame_slot, mu) - normalize(last, mu) + num_max_slots) % num_max_slots;
    if (diff <= tproc0)
      pop_back(transmit_history);
    else
      break;
  }
}

/* ---- Stage 3/4 helpers -------------------------------------------------------------------------- */

void remove_old_transmit_history(frameslot_t *frame_slot,
                                 uint16_t sensing_window,
                                 List_t *transmit_history,
                                 sl_nr_ue_mac_params_t *sl_mac)
{
  int new_size = 0;
  const int mu = sl_mac->sl_phy_config.sl_config_req.sl_bwp_config.sl_scs;
  const int64_t num_max_slots = (int64_t)SL_SLOTS_PER_FRAME(mu) * 1024;

  for (int i = 0; i < transmit_history->size; i++) {
    frameslot_t *fs = (frameslot_t *)((char *)transmit_history->data + i * transmit_history->element_size);
    const int64_t diff = (normalize(frame_slot, mu) - normalize(fs, mu) + num_max_slots) % num_max_slots;
    if (diff <= sensing_window)
      break;
    new_size++;
  }
  if (new_size > 0) {
    memmove(transmit_history->data,
            (char *)transmit_history->data + new_size * transmit_history->element_size,
            (transmit_history->size - new_size) * transmit_history->element_size);
    transmit_history->size -= new_size;
  }
}

void delete_at(List_t *list, size_t index)
{
  if (index >= list->size) {
    LOG_E(NR_MAC, "delete_at: index %zu out of bounds (size %zu)\n", index, list->size);
    return;
  }
  char *element_ptr = (char *)list->data + index * list->element_size;
  memmove(element_ptr, element_ptr + list->element_size, (list->size - index - 1) * list->element_size);
  list->size--;
}

frameslot_t add_to_sfn(frameslot_t *sfn, uint16_t slot_n, uint8_t mu)
{
  frameslot_t temp_sfn;
  temp_sfn.frame = (sfn->frame + ((sfn->slot + slot_n) / SL_SLOTS_PER_FRAME(mu))) % 1024;
  temp_sfn.slot = (sfn->slot + slot_n) % SL_SLOTS_PER_FRAME(mu);
  return temp_sfn;
}

bool overlapped_resource(uint8_t first_start, uint8_t first_length, uint8_t second_start, uint8_t second_length)
{
  AssertFatal(first_length && second_length, "Length should not be zero\n");
  return (max(first_start, second_start) < min(first_start + first_length, second_start + second_length));
}

bool check_t1_within_tproc1(uint8_t mu, uint16_t t1_slots)
{
  return ((mu == 0 && t1_slots <= 3) || (mu == 1 && t1_slots <= 5) || (mu == 2 && t1_slots <= 9)
          || (mu == 3 && t1_slots <= 17));
}

uint16_t get_T2_min(uint16_t pool_id, sl_nr_ue_mac_params_t *sl_mac, uint8_t mu)
{
  return sl_mac->sl_TxPool[pool_id]->t2min * pow(2, mu);
}

uint16_t get_t2(uint16_t pool_id, uint8_t mu, nr_sl_transmission_params_t *sl_tx_params, sl_nr_ue_mac_params_t *sl_mac)
{
  if (sl_tx_params->packet_delay_budget_ms != 0) {
    const uint16_t pdb_slots = time_to_slots(mu, sl_tx_params->packet_delay_budget_ms);
    return min(pdb_slots, sl_mac->sl_TxPool[pool_id]->t2);
  }
  const uint16_t t2min = get_T2_min(pool_id, sl_mac, mu);
  return max(t2min, sl_mac->sl_TxPool[pool_id]->t2);
}

/* ---- vector-of-lists (one list of projected reservations per sensed SCI) ------------------------ */

void init_vector(vec_of_list_t *vec, size_t initial_capacity)
{
  vec->size = 0;
  vec->capacity = initial_capacity;
  vec->lists = (List_t *)calloc(initial_capacity, sizeof(List_t));
  AssertFatal(vec->lists != NULL, "init_vector: out of memory\n");
}

void push_back_list(vec_of_list_t *vec, List_t *new_list)
{
  if (vec->size == vec->capacity) {
    vec->capacity *= 2;
    List_t *grown = realloc(vec->lists, vec->capacity * sizeof(List_t));
    AssertFatal(grown != NULL, "push_back_list: realloc failed\n");
    vec->lists = grown;
  }
  vec->lists[vec->size] = *new_list;
  vec->size++;
}

void free_vector_mem(vec_of_list_t *vec)
{
  for (size_t i = 0; i < vec->size; i++)
    free_list_mem(&vec->lists[i]);
  free(vec->lists);
  vec->lists = NULL;
  vec->size = 0;
  vec->capacity = 0;
}

/* ---- Stage 4: candidate generation + reservation exclusion (TS 38.214 8.1.4) --------------------- */

/* Step 4: the SL slots inside the selection window [n+T1, n+T2] that this pool may use. */
static List_t get_nr_sl_comm_opportunities(NR_UE_MAC_INST_t *mac,
                                           uint64_t abs_idx_cur_slot,
                                           uint16_t mu,
                                           uint16_t pool_id,
                                           uint8_t t1,
                                           uint16_t t2,
                                           uint8_t psfch_period)
{
  List_t slot_info_list;
  init_list(&slot_info_list, sizeof(slot_info_t), 1);

  sl_nr_ue_mac_params_t *sl_mac = mac->SL_MAC_PARAMS;
  const NR_SL_ResourcePool_r16_t *respool = sl_mac->sl_TxPool[pool_id]->respool;
  const struct NR_SL_BWP_Generic_r16 *sl_bwp_generic = sl_mac->sl_bwp_generic;

  const uint64_t first_abs_slot_ind = abs_idx_cur_slot + t1;
  const uint64_t last_abs_slot_ind = abs_idx_cur_slot + t2;

  for (uint64_t i = first_abs_slot_ind; i <= last_abs_slot_ind; i++) {
    if (!is_sl_slot(mac, i))
      continue;

    const uint8_t num_sl_pscch_rbs = pscch_rb_table[*respool->sl_PSCCH_Config_r16->choice.setup->sl_FreqResourcePSCCH_r16];
    const uint16_t num_sl_pscch_sym = pscch_tda[*respool->sl_PSCCH_Config_r16->choice.setup->sl_TimeResourcePSCCH_r16];
    const uint8_t start_sl_pscch_sym = 1;
    const uint16_t sl_pssch_sym_start = *sl_bwp_generic->sl_StartSymbol_r16;

    int num_psfch_symbols = 0;
    bool sl_has_psfch = false;
    if (psfch_period == 1) {
      num_psfch_symbols = 3;
    } else if (psfch_period == 2 || psfch_period == 4) {
      sl_has_psfch = slot_has_psfch(mac, i, psfch_period);
      if (sl_has_psfch)
        num_psfch_symbols = 3;
    }
    // PSFCH costs an additional 3 symbols
    const uint16_t sl_pssch_sym_len = 7 + *sl_bwp_generic->sl_LengthSymbols_r16 - num_psfch_symbols - 2;

    slot_info_t slot_info = {.sl_pscch_sym_start = start_sl_pscch_sym,
                             .sl_pscch_sym_len = num_sl_pscch_sym,
                             .num_sl_pscch_rbs = num_sl_pscch_rbs,
                             .sl_pssch_sym_start = sl_pssch_sym_start,
                             .sl_pssch_sym_len = sl_pssch_sym_len,
                             .slot_offset = (uint32_t)(i - abs_idx_cur_slot),
                             .abs_slot_index = i,
                             .sl_max_num_per_reserve = *respool->sl_UE_SelectedConfigRP_r16->sl_MaxNumPerReserve_r16,
                             .sl_sub_chan_size = sl_get_subchannel_size((NR_SL_ResourcePool_r16_t *)respool),
                             .sl_has_psfch = sl_has_psfch};
    push_back(&slot_info_list, &slot_info);
  }

  LOG_D(NR_MAC, "SL slots available in the selection window = %ld\n", slot_info_list.size);
  return slot_info_list;
}

/* Expand each usable slot into one candidate per subchannel position. */
static List_t *get_candidate_resources_from_slots(frameslot_t *sfn,
                                                  uint8_t psfch_period,
                                                  uint8_t min_time_gap_psfch,
                                                  uint16_t l_subch,
                                                  uint16_t total_subch,
                                                  List_t *slot_info,
                                                  uint8_t mu)
{
  List_t *nr_resource_list = calloc(1, sizeof(*nr_resource_list));
  AssertFatal(nr_resource_list != NULL, "get_candidate_resources_from_slots: out of memory\n");
  init_list(nr_resource_list, sizeof(sl_resource_info_t), 1);

  for (int s = 0; s < slot_info->size; s++) {
    const slot_info_t *s_info = (const slot_info_t *)((char *)slot_info->data + s * slot_info->element_size);
    for (uint16_t i = 0; i + l_subch <= total_subch; i += l_subch) {
      frameslot_t frame_slot;
      de_normalize(normalize(sfn, mu) + s_info->slot_offset, mu, &frame_slot);

      sl_resource_info_t rsrc_info = {0};
      rsrc_info.num_sl_pscch_rbs = s_info->num_sl_pscch_rbs;
      rsrc_info.sl_pscch_sym_start = s_info->sl_pscch_sym_start;
      rsrc_info.sl_pscch_sym_len = s_info->sl_pscch_sym_len;
      rsrc_info.sl_pssch_sym_start = s_info->sl_pssch_sym_start;
      rsrc_info.sl_pssch_sym_len = s_info->sl_pssch_sym_len;
      rsrc_info.sl_subchan_size = s_info->sl_sub_chan_size;
      rsrc_info.sl_subchan_start = i;
      rsrc_info.sl_subchan_len = l_subch;
      rsrc_info.sl_max_num_per_reserve = s_info->sl_max_num_per_reserve;
      rsrc_info.sfn = frame_slot;
      rsrc_info.sl_psfch_period = psfch_period;
      rsrc_info.sl_min_time_gap_psfch = min_time_gap_psfch;
      push_back(nr_resource_list, &rsrc_info);
    }
  }
  return nr_resource_list;
}

/* Step 6c: project one sensed SCI's reservation forward into the selection window. */
static List_t exclude_reserved_resources(sensing_data_t *sensed_data,
                                         float slot_period_ms,
                                         uint16_t resv_period_slots,
                                         uint16_t t1,
                                         uint16_t t2,
                                         uint8_t mu)
{
  List_t resource_list;
  init_list(&resource_list, sizeof(reserved_resource_t), 1);
  AssertFatal(slot_period_ms <= 1, "Slot length can not exceed 1 ms\n");

  // slot range is [n + T1, n + T2], both endpoints included
  const uint16_t window_slots = (t2 - t1) + 1;
  const double t_scal_ms = window_slots * slot_period_ms; // T_scal
  const double p_rsvp_ms = (double)sensed_data->rsvp;     // Pprime_rsvp_rx
  uint16_t q = 0;                                         // Q

  if (sensed_data->rsvp != 0)
    q = (p_rsvp_ms < t_scal_ms) ? (uint16_t)ceil(t_scal_ms / p_rsvp_ms) : 1;

  for (uint16_t i = 1; i <= q; i++) {
    reserved_resource_t resource = {.sfn = sensed_data->frame_slot,
                                    .rsvp = sensed_data->rsvp,
                                    .sb_ch_length = sensed_data->subch_len,
                                    .sb_ch_start = sensed_data->subch_start,
                                    .prio = sensed_data->prio,
                                    .sl_rsrp = sensed_data->sl_rsrp};
    resource.sfn = add_to_sfn(&resource.sfn, resv_period_slots, mu);
    push_back(&resource_list, &resource);

    if (sensed_data->gap_re_tx1 != 0 && sensed_data->gap_re_tx1 != 0xFF) {
      reserved_resource_t re_tx1_slot = resource;
      re_tx1_slot.sfn = add_to_sfn(&re_tx1_slot.sfn, sensed_data->gap_re_tx1, mu);
      re_tx1_slot.sb_ch_length = sensed_data->subch_len;
      re_tx1_slot.sb_ch_start = sensed_data->subch_startre_tx1;
      push_back(&resource_list, &re_tx1_slot);
    }
    if (sensed_data->gap_re_tx1 != 0 && sensed_data->gap_re_tx2 != 0xFF) {
      reserved_resource_t re_tx2_slot = resource;
      re_tx2_slot.sfn = add_to_sfn(&re_tx2_slot.sfn, sensed_data->gap_re_tx2, mu);
      re_tx2_slot.sb_ch_length = sensed_data->subch_len;
      re_tx2_slot.sb_ch_start = sensed_data->subch_startre_tx2;
      push_back(&resource_list, &re_tx2_slot);
    }
  }
  return resource_list;
}

/* Step 4-7 of TS 38.214 8.1.4: build the candidate set, then remove every candidate that collides in
 * time AND frequency with a projected reservation whose RSRP exceeds the threshold. If that leaves fewer
 * than sl_res_ratio of the original candidates, the threshold is raised 3 dB and the pass repeats.
 *
 * Returns a list owned by the caller's mac->sl_candidate_resources, or NULL when the selection window
 * holds no usable SL slot. */
List_t *get_candidate_resources(frameslot_t *frame_slot,
                                NR_UE_MAC_INST_t *mac,
                                List_t *sensing_data,
                                List_t *transmit_history)
{
  const uint16_t pool_id = 0;
  sl_nr_ue_mac_params_t *sl_mac = mac->SL_MAC_PARAMS;
  const uint8_t mu = sl_mac->sl_phy_config.sl_config_req.sl_bwp_config.sl_scs;
  nr_sl_transmission_params_t *sl_tx_params = &sl_mac->mac_tx_params;
  const uint8_t t1 = sl_mac->sl_TxPool[pool_id]->t1;
  const uint8_t tproc1 = sl_mac->sl_TxPool[pool_id]->tproc1;
  const uint16_t t2 = get_t2(pool_id, mu, sl_tx_params, sl_mac);

  AssertFatal(check_t1_within_tproc1(mu, t1), "Configured t1 %d exceeds tproc1 %d for numerology %d\n", t1, tproc1, mu);

  const uint64_t abs_slot_ind = normalize(frame_slot, mu);
  // (T2-T1+1) x (1/2^mu) must not exceed the reservation period, else the window wraps onto itself.
  const uint16_t num_slots_mul_s_dur_ms = (t2 - t1 + 1) * (1 / pow(2, mu));
  const uint16_t rsvpMs = sl_tx_params->rri;
  if (rsvpMs == 0 || num_slots_mul_s_dur_ms > rsvpMs) {
    LOG_E(NR_MAC,
          "SL sensing: selection window (T2-T1+1)x(1/2^mu)=%d exceeds reservation period %d ms - check "
          "sl_SelectionWindow / sl_ResourceReservePeriod\n",
          num_slots_mul_s_dur_ms, rsvpMs);
    return NULL;
  }

  const uint16_t l_subch = 1;
  const uint16_t total_subch = *mac->sl_tx_res_pool->sl_NumSubchannel_r16;
  const uint8_t psfch_time_gaps[] = {2, 3};
  const uint8_t min_time_gap_psfch =
      mac->sl_tx_res_pool->sl_PSFCH_Config_r16
          ? psfch_time_gaps[*mac->sl_tx_res_pool->sl_PSFCH_Config_r16->choice.setup->sl_MinTimeGapPSFCH_r16]
          : 0;
  const uint8_t psfch_periods[] = {0, 1, 2, 4};
  const uint8_t psfch_period = (mac->sl_tx_res_pool->sl_PSFCH_Config_r16
                                && mac->sl_tx_res_pool->sl_PSFCH_Config_r16->choice.setup->sl_PSFCH_Period_r16)
                                   ? psfch_periods[*mac->sl_tx_res_pool->sl_PSFCH_Config_r16->choice.setup
                                                        ->sl_PSFCH_Period_r16]
                                   : 0;

  // step 4: SL slots inside the selection window
  List_t candidate_slots = get_nr_sl_comm_opportunities(mac, abs_slot_ind, mu, pool_id, t1, t2, psfch_period);
  if (candidate_slots.size == 0) {
    free_list_mem(&candidate_slots);
    return NULL;
  }

  List_t *candidate_resources =
      get_candidate_resources_from_slots(frame_slot, psfch_period, min_time_gap_psfch, l_subch, total_subch,
                                         &candidate_slots, mu);
  free_list_mem(&candidate_slots);
  const uint64_t m_total = candidate_resources->size;

  // Nothing sensed and nothing transmitted yet: every candidate stands.
  if (sensing_data->size == 0 && transmit_history->size == 0) {
    LOG_D(NR_MAC, "SL sensing: no history, %ld candidates\n", m_total);
    return candidate_resources;
  }

  // Trim both stores to [n - T0, n - Tproc0): entries newer than Tproc0 cannot be acted on.
  update_sensing_data(sensing_data, frame_slot, sl_mac, pool_id);
  update_transmit_history(transmit_history, frame_slot, sl_mac, pool_id);

  List_t *remaining_candidates = candidate_resources;

  // step 6: project every sensed SCI's reservation into the selection window, one list per SCI.
  vec_of_list_t sensing_data_projections;
  init_vector(&sensing_data_projections, 1);
  const uint8_t nr_slots_per_subframe = pow(2, mu);
  const float slot_duration_ms = 1.0f / nr_slots_per_subframe;

  for (int k = 0; k < sensing_data->size; k++) {
    sensing_data_t *itr_sdata = (sensing_data_t *)((char *)sensing_data->data + k * sensing_data->element_size);
    const uint16_t resv_period_slots = time_to_slots(mu, itr_sdata->rsvp);
    itr_sdata->gap_re_tx1 = 0;
    itr_sdata->gap_re_tx2 = 0;
    List_t temp_rsrc_list =
        exclude_reserved_resources(itr_sdata, slot_duration_ms, resv_period_slots, t1, t2, mu);
    push_back_list(&sensing_data_projections, &temp_rsrc_list);
  }

  int rsrp_threshold = sl_tx_params->sl_thresh_rsrp;
  const uint16_t p_prime_rsvp_tx = time_to_slots(mu, sl_tx_params->rri);

  do {
    int k = 0;
    while (k < remaining_candidates->size) {
      sl_resource_info_t *itr_rsrc =
          (sl_resource_info_t *)((char *)remaining_candidates->data + k * remaining_candidates->element_size);
      bool erased = false;
      itr_rsrc->slot_busy = false;

      // every future transmission this candidate implies, over the reselection counter
      List_t resource_info_list;
      init_list(&resource_info_list, sizeof(sl_resource_info_t), 1);
      for (uint16_t i = 0; i < sl_tx_params->resel_counter; i++) {
        sl_resource_info_t sl_resource_info = *itr_rsrc;
        frameslot_t fs = itr_rsrc->sfn;
        sl_resource_info.sfn = add_to_sfn(&fs, p_prime_rsvp_tx, mu);
        push_back(&resource_info_list, &sl_resource_info);
      }

      for (size_t i = 0; i < sensing_data_projections.size && !erased; i++) {
        List_t proj = sensing_data_projections.lists[i];
        for (int j = 0; j < resource_info_list.size && !erased; j++) {
          sl_resource_info_t *future_cand_info =
              (sl_resource_info_t *)((char *)resource_info_list.data + j * resource_info_list.element_size);
          for (int l = 0; l < proj.size; l++) {
            reserved_resource_t *rsrvd_rsc = (reserved_resource_t *)((char *)proj.data + l * proj.element_size);
            if (normalize(&future_cand_info->sfn, mu) != normalize(&rsrvd_rsc->sfn, mu))
              continue; // no time overlap
            if (rsrvd_rsc->sl_rsrp <= rsrp_threshold)
              continue; // too weak to matter
            if (overlapped_resource(rsrvd_rsc->sb_ch_start, rsrvd_rsc->sb_ch_length, itr_rsrc->sl_subchan_start,
                                    itr_rsrc->sl_subchan_len)) {
              LOG_D(NR_MAC, "SL sensing: erase candidate %d.%d [%d,%d] (rsrp %.0f > thr %d)\n",
                    itr_rsrc->sfn.frame, itr_rsrc->sfn.slot, itr_rsrc->sl_subchan_start,
                    itr_rsrc->sl_subchan_start + itr_rsrc->sl_subchan_len - 1, rsrvd_rsc->sl_rsrp, rsrp_threshold);
              delete_at(remaining_candidates, k);
              erased = true;
              break;
            }
            // overlaps in time but not in frequency
            future_cand_info->slot_busy = true;
          }
        }
      }
      free_list_mem(&resource_info_list);
      if (!erased)
        k++; // on erase, index k already points at the next entry
    }

    // step 7: too few left - retry with the threshold raised 3 dB
    rsrp_threshold += 3;
    if (rsrp_threshold > 0) {
      // 0 dBm is the ceiling: everything overlaps and is strong, so nothing can be selected
      LOG_D(NR_MAC, "SL sensing: reached maximum RSRP threshold, no resource selectable\n");
      remaining_candidates->size = 0;
      break;
    }
  } while (remaining_candidates->size < (sl_tx_params->sl_res_ratio * m_total));

  free_vector_mem(&sensing_data_projections);
  LOG_D(NR_MAC, "SL sensing: %ld of %ld candidate resources survive\n", remaining_candidates->size, m_total);
  return remaining_candidates;
}

/* Stage 5: is this exact slot one of the selected resources? NULL = stay silent and listen. */
sl_resource_info_t *get_resource_element(List_t *resource_list, frameslot_t sfn)
{
  for (int i = 0; i < resource_list->size; i++) {
    sl_resource_info_t *itr_rsrc =
        (sl_resource_info_t *)((char *)resource_list->data + i * resource_list->element_size);
    if (itr_rsrc->sfn.frame == sfn.frame && itr_rsrc->sfn.slot == sfn.slot)
      return itr_rsrc;
  }
  return NULL;
}
