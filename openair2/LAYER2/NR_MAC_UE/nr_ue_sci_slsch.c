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

/*! \file openair2/LAYER2/NR_MAC_UE/nr_ue_sci_slsch.c
 * \brief Sidelink SCI / SLSCH MAC helpers (episys SL data-plane port, F1 minimal subset).
 *        Resource-index math (FRIV/TRIV) now; SCI-size + bitpacking + PDU fill + RX config to follow.
 */

#include <stdint.h>
#include <math.h>
#include "assertions.h"
#include "NR_MAC_UE/nr_ue_sci.h"
#include "NR_MAC_UE/mac_proto.h"                           // NR_UE_MAC_INST_t, sl_nr_sci_indication_pdu_t (SCI-2 RX)
#include "common/utils/LOG/log.h"
#include "executables/nr-uesoftmodem.h"                  // get_nrUE_params (nb_antennas_tx)
#include "oai_asn1.h"
#include "NR_SL-ResourcePool-r16.h"                       // NR_SL_ResourcePool_r16_t + sub-configs
#include "NR_SL-BWP-Generic-r16.h"                         // NR_SL_BWP_Generic_r16_t
#include "NR_SL-UE-SelectedConfigRP-r16.h" // NR_SL_UE_SelectedConfigRP_r16__sl_MaxNumPerReserve_r16_n2/n3
#include "common/utils/nr/nr_common.h"                     // NRRIV2BW
#include "NR_MAC_COMMON/nr_mac_common.h"                   // nr_get_Qm_ul, nr_get_code_rate_ul, nr_compute_tbs_sl, nr_compute_tbslbrm
#include "executables/softmodem-common.h"                  // get_softmodem_params (sl_mode)
#include "sidelink_nr_ue_interface.h"                      // sl_nr_tx_config_pscch_pssch_pdu_t

// TS 38.213 9.3.2 beta-offset table for the 2nd-stage SCI RE count (local copy; matches the PHY-side table).
#define MAX_EL_213_9_3_2 19
static const float tab38_213_9_3_2[MAX_EL_213_9_3_2] =
  {1.125,1.250,1.375,1.625,1.750,2.000,2.250,2.500,2.875,3.125,3.500,4.000,5.000,6.250,8.000,10.000,12.625,15.875,20.000};

// PSCCH/PSSCH resource-pool lookup tables (episys SL data-plane port; RRC index -> value).
const int sl_dmrs_mask2[2][8] = {{34, 34, 34, 264, 264, 1032, 1032, 1032}, {34, 34, 34, 272, 272, 1040, 1040, 1040}};
const int sl_dmrs_mask3[5] = {146, 146, 546, 546, 2114};
const int sl_dmrs_mask4[3] = {1170, 1170, 1170};
const int pscch_rb_table[5] = {10, 12, 15, 20, 25};
const int pscch_tda[2] = {2, 3};
const int subch_to_rb[8] = {10, 12, 15, 20, 25, 50, 75, 100};

// TS 38.212 8.1.5 — frequency resource indicator value (FRIV) from the chosen subchannel allocation.
// sl_max_num_per_reserve selects the n2 (2 reservations) or n3 (3) encoding. L_sub_chan = number of
// contiguous subchannels; n_start_subch1/2 = starting subchannel index(es); N_sl_subch = pool subchannels.
uint32_t compute_FRIV(uint8_t sl_max_num_per_reserve,
                      uint8_t L_sub_chan,
                      uint8_t n_start_subch1,
                      uint8_t n_start_subch2,
                      uint8_t N_sl_subch)
{
  uint32_t friv = 0;
  int sum = 0;
  if (sl_max_num_per_reserve == NR_SL_UE_SelectedConfigRP_r16__sl_MaxNumPerReserve_r16_n2) {
    for (int i = 1; i < L_sub_chan; i++)
      sum += N_sl_subch + 1 - i;
    friv = n_start_subch1 + sum;
  } else if (sl_max_num_per_reserve == NR_SL_UE_SelectedConfigRP_r16__sl_MaxNumPerReserve_r16_n3) {
    for (int i = 1; i < L_sub_chan; i++)
      sum += (N_sl_subch + 1 - i) * (N_sl_subch + 1 - i);
    friv = n_start_subch1 + n_start_subch2 * (N_sl_subch + 1 - L_sub_chan) + sum;
  } else {
    AssertFatal(1 == 0, "sl_MaxNumPerReserve is configured with incorrect value");
  }
  return friv;
}

// TS 38.212 8.1.5 — time resource indicator value (TRIV) from the reservation offsets.
uint32_t compute_TRIV(uint8_t N, uint8_t t1, uint8_t t2)
{
  int32_t triv = 0;
  if (N == 1) {
    triv = 0;
  } else if (N == 2) {
    triv = t1;
  } else {
    if ((t2 - t1 - 1) <= 15)
      triv = 30 * (t2 - t1 - 1) + t1 + 31;
    else
      triv = 30 * (31 - t2 + t1) + 62 - t1;
  }
  return triv;
}

// Inverse of compute_FRIV: recover the subchannel length (Lsc) + start(s) from a received FRIV.
void convNRFRIV(int FRIV, int N_subch, long sl_MaxNumPerReserve, uint16_t *Lsc, uint16_t *startsc, uint16_t *startsc2)
{
  if (sl_MaxNumPerReserve == NR_SL_UE_SelectedConfigRP_r16__sl_MaxNumPerReserve_r16_n2) {
    *Lsc = 1;
    int prevN = 0;
    int N = N_subch;
    while (FRIV > N) {
      *Lsc = *Lsc + 1;
      prevN = N;
      N += (N_subch - *Lsc + 1);
    }
    if (startsc)
      *startsc = FRIV - prevN;
  } else if (sl_MaxNumPerReserve == NR_SL_UE_SelectedConfigRP_r16__sl_MaxNumPerReserve_r16_n3) {
    *Lsc = 1;
    int prevN = 0;
    int N = N_subch;
    while (FRIV > N) {
      *Lsc = *Lsc + 1;
      prevN = N;
      N += ((N_subch - *Lsc + 1) * (N_subch - *Lsc + 1));
    }
    int tmp1 = FRIV - prevN; // startsc1 + startsc2*(N_subch - *Lsc + 1)
    if (startsc2)
      *startsc2 = tmp1 / (N_subch - *Lsc + 1);
    if (startsc)
      *startsc = tmp1 % (N_subch - *Lsc + 1);
  } else {
    AssertFatal(1 == 0, "sl_MaxNumPerReserve is configured with incorrect value");
  }
}

// TS 38.212 8.3/8.4 — total bit length of an SCI (format 1A on PSCCH, or 2A/2B/2C on PSSCH), and side-effect
// of filling the per-field nbits in sci_pdu. Derived from the resource-pool ASN.1 config. (episys SL port)
uint32_t nr_sci_size(const struct NR_SL_ResourcePool_r16 *sl_res_pool, nr_sci_pdu_t *sci_pdu, const nr_sci_format_t format)
{
  int size = 0;
  switch (format) {
    case NR_SL_SCI_FORMAT_1A: {
      size += 3; // priority
      long Nsc = *sl_res_pool->sl_NumSubchannel_r16;
      if (sl_res_pool->sl_UE_SelectedConfigRP_r16 && sl_res_pool->sl_UE_SelectedConfigRP_r16->sl_MaxNumPerReserve_r16
          && *sl_res_pool->sl_UE_SelectedConfigRP_r16->sl_MaxNumPerReserve_r16
                 == NR_SL_UE_SelectedConfigRP_r16__sl_MaxNumPerReserve_r16_n2)
        sci_pdu->frequency_resource_assignment.nbits = (uint8_t)ceil(log2((Nsc * (Nsc + 1)) >> 1));
      else
        sci_pdu->frequency_resource_assignment.nbits = (uint8_t)ceil(log2((Nsc * (Nsc + 1) * (2 * Nsc + 1)) / 6));
      size += sci_pdu->frequency_resource_assignment.nbits;

      if (*sl_res_pool->sl_UE_SelectedConfigRP_r16->sl_MaxNumPerReserve_r16
          == NR_SL_UE_SelectedConfigRP_r16__sl_MaxNumPerReserve_r16_n2)
        sci_pdu->time_resource_assignment.nbits = 5;
      else
        sci_pdu->time_resource_assignment.nbits = 9;
      size += sci_pdu->time_resource_assignment.nbits;

      sci_pdu->resource_reservation_period.nbits = 0; // sl-MultiReserveResource not modelled (R17)
      size += sci_pdu->resource_reservation_period.nbits;

      int dmrs_pattern_num = sl_res_pool->sl_PSSCH_Config_r16->choice.setup->sl_PSSCH_DMRS_TimePatternList_r16->list.count;
      sci_pdu->dmrs_pattern.nbits = (uint8_t)ceil(log2(dmrs_pattern_num));
      size += sci_pdu->dmrs_pattern.nbits;

      size += 2; // second_stage_sci_format
      size += 2; // beta_offset_indicator
      size += 1; // number_of_dmrs_port
      size += 5; // mcs

      if (sl_res_pool->sl_Additional_MCS_Table_r16)
        sci_pdu->additional_mcs.nbits = (*sl_res_pool->sl_Additional_MCS_Table_r16 < 2) ? 1 : 2;
      else
        sci_pdu->additional_mcs.nbits = 0;
      size += sci_pdu->additional_mcs.nbits;

      if (sl_res_pool->sl_PSFCH_Config_r16 && sl_res_pool->sl_PSFCH_Config_r16->choice.setup->sl_PSFCH_Period_r16
          && *sl_res_pool->sl_PSFCH_Config_r16->choice.setup->sl_PSFCH_Period_r16 > 1)
        sci_pdu->psfch_overhead.nbits = 1;
      else
        sci_pdu->psfch_overhead.nbits = 0;
      size += sci_pdu->psfch_overhead.nbits;

      AssertFatal(sl_res_pool->sl_PSCCH_Config_r16 && sl_res_pool->sl_PSCCH_Config_r16->choice.setup
                      && sl_res_pool->sl_PSCCH_Config_r16->choice.setup->sl_NumReservedBits_r16,
                  "sl_PSCCH_Config_r16 sl_NumReservedBits_r16 not configured\n");
      sci_pdu->reserved.nbits = *sl_res_pool->sl_PSCCH_Config_r16->choice.setup->sl_NumReservedBits_r16;
      size += sci_pdu->reserved.nbits;

      sci_pdu->conflict_information_receiver.nbits = 0; // R17 sl-IndicationUE-B not modelled
      size += sci_pdu->conflict_information_receiver.nbits;
      break;
    }
    case NR_SL_SCI_FORMAT_2A:
    case NR_SL_SCI_FORMAT_2B:
    case NR_SL_SCI_FORMAT_2C:
      size += (4 + 1 + 2 + 8 + 16 + 1); // harq_pid, ndi, rv, source_id, dest_id, harq_feedback
      if (format == NR_SL_SCI_FORMAT_2A)
        size += 2; // cast_type
      if (format == NR_SL_SCI_FORMAT_2C || format == NR_SL_SCI_FORMAT_2A)
        size += 1; // csi_req
      if (format == NR_SL_SCI_FORMAT_2B) {
        size += 12; // zone_id (communication_range R17 not modelled)
      } else if (format == NR_SL_SCI_FORMAT_2C) {
        size += 1; // providing_req_ind
        size += 8; // first_resource_location
        size += 1; // resource_set_type
      }
      break;
  }
  return size;
}

// Average number of DMRS REs per RB across the configured PSSCH DMRS time patterns (6 REs/RB each). (episys SL port)
int get_nREDMRS(const NR_SL_ResourcePool_r16_t *sl_res_pool)
{
  int cnt = sl_res_pool->sl_PSSCH_Config_r16->choice.setup->sl_PSSCH_DMRS_TimePatternList_r16->list.count;
  int nREDMRS = 0;
  for (int i = 0; i < cnt; i++)
    nREDMRS += *sl_res_pool->sl_PSSCH_Config_r16->choice.setup->sl_PSSCH_DMRS_TimePatternList_r16->list.array[i] * 6;
  return (nREDMRS / cnt);
}

// Number of REs occupied by CSI-RS in a PSSCH slot (F3; 0 on the F1 minimal path). (episys SL port)
int get_nRECSI_RS(uint8_t freq_density, uint16_t nr_of_rbs)
{
  AssertFatal(freq_density > 0, "freq_density must be greater than 0\n");
  uint8_t nr_rbs_w_csi_rs = nr_of_rbs / freq_density;
  uint8_t subcarriers_used = get_nrUE_params()->nb_antennas_tx > 2 ? 2 : get_nrUE_params()->nb_antennas_tx;
  return nr_rbs_w_csi_rs * subcarriers_used;
}

// MSB-first bit-packer for one SCI field: place the low `nbits` of `val` at the running position `*pos`
// within an `sci_size`-bit payload (38.212 bit ordering). Advances `*pos`. (episys SL data-plane port)
static inline void pack_sci_field(uint64_t *payload, int sci_size, int *pos, uint64_t val, int nbits)
{
  for (int i = 0; i < nbits; i++)
    *payload |= (((val >> (nbits - i - 1)) & 1) << (sci_size - (*pos)++ - 1));
}

// Pack SCI-1A (PSCCH) fields of sci_pdu into `payload` (MSB-first). sci_size = nr_sci_size(...,FORMAT_1A).
// Per-field nbits must already be set (by nr_sci_size). (episys SL data-plane port)
void nr_pack_sci1(nr_sci_pdu_t *sci, int sci_size, uint64_t *payload)
{
  int pos = 0;
  *payload = 0;
  pack_sci_field(payload, sci_size, &pos, sci->priority, 3);
  pack_sci_field(payload, sci_size, &pos, sci->frequency_resource_assignment.val, sci->frequency_resource_assignment.nbits);
  pack_sci_field(payload, sci_size, &pos, sci->time_resource_assignment.val, sci->time_resource_assignment.nbits);
  pack_sci_field(payload, sci_size, &pos, sci->resource_reservation_period.val, sci->resource_reservation_period.nbits);
  pack_sci_field(payload, sci_size, &pos, sci->dmrs_pattern.val, sci->dmrs_pattern.nbits);
  pack_sci_field(payload, sci_size, &pos, sci->second_stage_sci_format, 2);
  pack_sci_field(payload, sci_size, &pos, sci->beta_offset_indicator, 2);
  pack_sci_field(payload, sci_size, &pos, sci->number_of_dmrs_port, 1);
  pack_sci_field(payload, sci_size, &pos, sci->mcs, 5);
  pack_sci_field(payload, sci_size, &pos, sci->additional_mcs.val, sci->additional_mcs.nbits);
  pack_sci_field(payload, sci_size, &pos, sci->psfch_overhead.val, sci->psfch_overhead.nbits);
  pack_sci_field(payload, sci_size, &pos, sci->reserved.val, sci->reserved.nbits);
  pack_sci_field(payload, sci_size, &pos, sci->conflict_information_receiver.val, sci->conflict_information_receiver.nbits);
}

// Pack SCI-2 (PSSCH: 2A/2B/2C) fields of sci2 into `payload` (MSB-first). sci2_size = nr_sci_size(...,format).
// (episys SL data-plane port)
void nr_pack_sci2(nr_sci_pdu_t *sci2, int sci2_size, nr_sci_format_t format, uint64_t *payload)
{
  int pos = 0;
  *payload = 0;
  pack_sci_field(payload, sci2_size, &pos, sci2->harq_pid, 4);
  pack_sci_field(payload, sci2_size, &pos, sci2->ndi, 1);
  pack_sci_field(payload, sci2_size, &pos, sci2->rv_index, 2);
  pack_sci_field(payload, sci2_size, &pos, sci2->source_id, 8);
  pack_sci_field(payload, sci2_size, &pos, sci2->dest_id, 16);
  pack_sci_field(payload, sci2_size, &pos, sci2->harq_feedback, 1);
  if (format == NR_SL_SCI_FORMAT_2A)
    pack_sci_field(payload, sci2_size, &pos, sci2->cast_type, 2);
  if (format == NR_SL_SCI_FORMAT_2A || format == NR_SL_SCI_FORMAT_2C)
    pack_sci_field(payload, sci2_size, &pos, sci2->csi_req, 1);
  if (format == NR_SL_SCI_FORMAT_2B) {
    pack_sci_field(payload, sci2_size, &pos, sci2->zone_id, 12);
  } else if (format == NR_SL_SCI_FORMAT_2C) {
    pack_sci_field(payload, sci2_size, &pos, sci2->providing_req_ind, 1);
    pack_sci_field(payload, sci2_size, &pos, sci2->first_resource_location, 8);
    pack_sci_field(payload, sci2_size, &pos, sci2->resource_set_type, 1);
  }
}

// episys SL PSFCH port (4c-A): unpack a decoded SCI-2 (format 2A) payload into mac->sci_pdu_rx. Exact
// inverse of nr_pack_sci2 (MSB-first at sci2_size). Provides harq_feedback/source_id/cast_type the MAC
// needs to decide the PSFCH HARQ ACK/NACK.
void extract_pssch_sci_pdu(uint64_t *sci2_payload,
                           int len,
                           const struct NR_SL_BWP_ConfigCommon_r16 *sl_bwp,
                           const struct NR_SL_ResourcePool_r16 *sl_res_pool,
                           nr_sci_pdu_t *sci_pdu)
{
  (void)sl_bwp;
  int pos = 0, fsize;
  int sci2_size = nr_sci_size(sl_res_pool, sci_pdu, NR_SL_SCI_FORMAT_2A);
  if (sci2_size != len)
    LOG_W(NR_MAC, "SCI2A size %d != indicated len %d\n", sci2_size, len);
  fsize = 4;  pos += fsize; sci_pdu->harq_pid      = (*sci2_payload >> (sci2_size - pos)) & ((1 << fsize) - 1);
  fsize = 1;  pos += fsize; sci_pdu->ndi           = (*sci2_payload >> (sci2_size - pos)) & ((1 << fsize) - 1);
  fsize = 2;  pos += fsize; sci_pdu->rv_index      = (*sci2_payload >> (sci2_size - pos)) & ((1 << fsize) - 1);
  fsize = 8;  pos += fsize; sci_pdu->source_id     = (*sci2_payload >> (sci2_size - pos)) & ((1 << fsize) - 1);
  fsize = 16; pos += fsize; sci_pdu->dest_id       = (*sci2_payload >> (sci2_size - pos)) & ((1 << fsize) - 1);
  fsize = 1;  pos += fsize; sci_pdu->harq_feedback = (*sci2_payload >> (sci2_size - pos)) & ((1 << fsize) - 1);
  fsize = 2;  pos += fsize; sci_pdu->cast_type     = (*sci2_payload >> (sci2_size - pos)) & ((1 << fsize) - 1);
  fsize = 1;  pos += fsize; sci_pdu->csi_req       = (*sci2_payload >> (sci2_size - pos)) & ((1 << fsize) - 1);
}

// episys SL PSFCH port (4c-A): MAC handler for a decoded SCI-2 indication -> populate mac->sci_pdu_rx.
int nr_ue_process_sci2_indication_pdu(NR_UE_MAC_INST_t *mac, module_id_t mod_id, int cc_id, frame_t frame,
                                      int slot, sl_nr_sci_indication_pdu_t *sci, void *phy_data)
{
  (void)mod_id; (void)cc_id; (void)frame; (void)slot; (void)phy_data;
  nr_sci_pdu_t *sci_pdu = &mac->sci_pdu_rx;
  const NR_SL_ResourcePool_r16_t *sl_res_pool = mac->sl_rx_res_pool ? mac->sl_rx_res_pool : mac->sl_tx_res_pool;
  extract_pssch_sci_pdu((uint64_t *)sci->sci_payloadBits, sci->sci_payloadlen, mac->sl_bwp, sl_res_pool, sci_pdu);
  LOG_D(NR_MAC, "SCI2A rx: harq_pid %d ndi %d RV %d SRC %x DST %x HARQ_FB %d Cast %d CSI %d\n",
        sci_pdu->harq_pid, sci_pdu->ndi, sci_pdu->rv_index, sci_pdu->source_id, sci_pdu->dest_id,
        sci_pdu->harq_feedback, sci_pdu->cast_type, sci_pdu->csi_req);
  return 0;
}

// TS 38.213 9.3.2 — number of REs occupied by the 2nd-stage SCI on PSSCH (MAC-side, mcs->coderate). (episys SL port)
int get_NREsci2(const int sci2_alpha, const int sci2_payload_len, const int sci2_beta_offset, const int pssch_numsym,
                const int pscch_numsym, const int pscch_numrbs, const int l_subch, const int subchannel_size,
                const int mcs, const int mcs_tb_ind)
{
  float Osci2 = (float)sci2_payload_len;
  AssertFatal(sci2_beta_offset < MAX_EL_213_9_3_2, "illegal sci2_beta_offset %d\n", sci2_beta_offset);
  float beta_offset_sci2 = tab38_213_9_3_2[sci2_beta_offset];
  uint32_t R10240 = nr_get_code_rate_ul(mcs, mcs_tb_ind);
  uint32_t tmp = (uint32_t)ceil((Osci2 + 24) * beta_offset_sci2 / ((float)R10240 / 5120));
  float tmp2 = 12.0 * pssch_numsym;
  int N_REsci1 = 12 * pscch_numrbs * pscch_numsym;
  tmp2 *= l_subch * subchannel_size;
  tmp2 -= N_REsci1;
  tmp2 *= ((float)sci2_alpha / 100.0);
  int min_val = min(tmp, (int)ceil(tmp2));
  uint8_t gamma = 12 - (min_val % 12);
  return min_val + (gamma % 12);
}

// Assemble the PSCCH+PSSCH TX PDU (episys SL data-plane port, F1 minimal: resource-pool-default resource,
// no CSI-RS, no PSFCH-driven resizing beyond the pool's PSFCH period). Fills the SCI-1A + SCI-2 payloads
// (nr_sci_size + nr_pack_sci1/2), computes the SLSCH TB size, and derives PSSCH DMRS symbol positions.
void fill_pssch_pscch_pdu(sl_nr_tx_config_pscch_pssch_pdu_t *pdu,
                          const NR_SL_BWP_Generic_r16_t *sl_bwp_generic,
                          const struct NR_SL_ResourcePool_r16 *sl_res_pool,
                          nr_sci_pdu_t *sci_pdu,
                          nr_sci_pdu_t *sci2_pdu,
                          const nr_sci_format_t format1,
                          const nr_sci_format_t format2)
{
  uint64_t *sci_payload = (uint64_t *)pdu->pscch_sci_payload;
  uint64_t *sci2_payload = (uint64_t *)pdu->sci2_payload;
  pdu->pscch_sci_payload_len = nr_sci_size(sl_res_pool, sci_pdu, format1);
  pdu->sci2_payload_len = nr_sci_size(sl_res_pool, sci2_pdu, format2);
  pdu->harq_pid = sci2_pdu->harq_pid;

  pdu->startrb = *sl_res_pool->sl_StartRB_Subchannel_r16;
  pdu->pscch_numsym = pscch_tda[*sl_res_pool->sl_PSCCH_Config_r16->choice.setup->sl_TimeResourcePSCCH_r16];
  pdu->pscch_numrbs = pscch_rb_table[*sl_res_pool->sl_PSCCH_Config_r16->choice.setup->sl_FreqResourcePSCCH_r16];
  pdu->pscch_dmrs_scrambling_id = *sl_res_pool->sl_PSCCH_Config_r16->choice.setup->sl_DMRS_ScrambleID_r16;
  pdu->num_subch = *sl_res_pool->sl_NumSubchannel_r16;
  pdu->subchannel_size = subch_to_rb[*sl_res_pool->sl_SubchannelSize_r16];
  convNRFRIV(sci_pdu->frequency_resource_assignment.val, pdu->num_subch,
             *sl_res_pool->sl_UE_SelectedConfigRP_r16->sl_MaxNumPerReserve_r16, &pdu->l_subch, NULL, NULL);

  // PSFCH period (symbols removed from PSSCH region). Minimal path: only the pool-configured period matters.
  const uint8_t psfch_periods[] = {0, 1, 2, 4};
  long psfch_period = 0;
  if (sl_res_pool->sl_PSFCH_Config_r16 && sl_res_pool->sl_PSFCH_Config_r16->choice.setup
      && sl_res_pool->sl_PSFCH_Config_r16->choice.setup->sl_PSFCH_Period_r16) {
    long idx = *sl_res_pool->sl_PSFCH_Config_r16->choice.setup->sl_PSFCH_Period_r16;
    psfch_period = (idx < 4) ? psfch_periods[idx] : 0;
  }
  int num_psfch_symbols = 0;
  if (psfch_period == 1)
    num_psfch_symbols = 3;
  else if ((psfch_period == 2 || psfch_period == 4) && sci_pdu->psfch_overhead.nbits > 0 && sci_pdu->psfch_overhead.val == 1)
    num_psfch_symbols = 3;
  int base_pssch_numsym = 7 + *sl_bwp_generic->sl_LengthSymbols_r16 - 2; // total SL symbols minus AGC/guard
  pdu->pssch_numsym = base_pssch_numsym - num_psfch_symbols;

  pdu->sci2_beta_offset =
      *sl_res_pool->sl_PSSCH_Config_r16->choice.setup->sl_BetaOffsets2ndSCI_r16->list.array[sci_pdu->beta_offset_indicator];
  if (sl_res_pool->sl_PowerControl_r16) {
    pdu->sci2_alpha_times_100 = 50 + (*sl_res_pool->sl_PowerControl_r16->sl_Alpha_PSSCH_PSCCH_r16) * 15;
    if (*sl_res_pool->sl_PowerControl_r16->sl_Alpha_PSSCH_PSCCH_r16 == 3)
      pdu->sci2_alpha_times_100 = 100;
  } else {
    pdu->sci2_alpha_times_100 = 100;
  }

  // Pack SCI-1A into the PSCCH payload.
  nr_pack_sci1(sci_pdu, pdu->pscch_sci_payload_len, sci_payload);

  // SLSCH TB size (F1: no CSI-RS REs).
  int mcs_tb_ind = (sci_pdu->additional_mcs.nbits > 0) ? sci_pdu->additional_mcs.val : 0;
  pdu->mcs = sci_pdu->mcs;
  int nohPRB = (sl_res_pool->sl_X_Overhead_r16) ? 3 * (*sl_res_pool->sl_X_Overhead_r16) : 0;
  int nREDMRS = get_nREDMRS(sl_res_pool);
  int N_REprime = 12 * pdu->pssch_numsym - nohPRB - nREDMRS;
  int N_REsci1 = 12 * pdu->pscch_numrbs * pdu->pscch_numsym;
  int N_REsci2 = get_NREsci2(pdu->sci2_alpha_times_100, pdu->sci2_payload_len, pdu->sci2_beta_offset, pdu->pssch_numsym,
                             pdu->pscch_numsym, pdu->pscch_numrbs, pdu->l_subch, pdu->subchannel_size,
                             get_softmodem_params()->sl_mode ? 1 : pdu->mcs, mcs_tb_ind);
  int N_RE = N_REprime * pdu->l_subch * pdu->subchannel_size - N_REsci1 - N_REsci2;

  pdu->mod_order = nr_get_Qm_ul(sci_pdu->mcs, mcs_tb_ind);
  pdu->target_coderate = nr_get_code_rate_ul(sci_pdu->mcs, mcs_tb_ind);
  pdu->num_layers = sci_pdu->number_of_dmrs_port + 1;
  pdu->tb_size = (nr_compute_tbs_sl(pdu->mod_order, pdu->target_coderate, N_RE, pdu->num_layers) + 7) >> 3;
  pdu->tbslbrm = 0; // SL: servingCellConfig rate-matching disabled (matches episys) -> full circular buffer
  pdu->mcs_table = mcs_tb_ind;
  pdu->rv_index = sci2_pdu->rv_index;
  pdu->ndi = sci2_pdu->ndi;

  // PSSCH DMRS symbol positions from the selected time pattern.
  int num_dmrs_symbols = *sl_res_pool->sl_PSSCH_Config_r16->choice.setup->sl_PSSCH_DMRS_TimePatternList_r16->list
                              .array[sci_pdu->dmrs_pattern.val];
  if (num_dmrs_symbols == 2)
    pdu->dmrs_symbol_position = sl_dmrs_mask2[pdu->pscch_numsym - 2][pdu->pssch_numsym - 6];
  else if (num_dmrs_symbols == 3)
    pdu->dmrs_symbol_position = sl_dmrs_mask3[pdu->pssch_numsym - 9];
  else if (num_dmrs_symbols == 4)
    pdu->dmrs_symbol_position = sl_dmrs_mask4[pdu->pssch_numsym - 11];

  // Pack SCI-2 into the PSSCH SCI payload.
  nr_pack_sci2(sci2_pdu, pdu->sci2_payload_len, format2, sci2_payload);

  LOG_D(NR_MAC, "PSSCH TX PDU: startrb %d pscch(%d sym,%d rb) num_subch %d subch_size %d l_subch %d pssch_numsym %d "
                "mcs %d Qm %d R %d Nl %d N_RE %d tb_size %d sci1_len %d sci2_len %d\n",
        pdu->startrb, pdu->pscch_numsym, pdu->pscch_numrbs, pdu->num_subch, pdu->subchannel_size, pdu->l_subch,
        pdu->pssch_numsym, pdu->mcs, pdu->mod_order, pdu->target_coderate, pdu->num_layers, N_RE, pdu->tb_size,
        pdu->pscch_sci_payload_len, pdu->sci2_payload_len);
}

// Helper: PSFCH symbols reserved in a PSSCH slot from the pool period + SCI overhead bit. (episys SL port)
static int sl_num_psfch_symbols(const struct NR_SL_ResourcePool_r16 *sl_res_pool, const nr_sci_pdu_t *sci_pdu)
{
  const uint8_t psfch_periods[] = {0, 1, 2, 4};
  long psfch_period = 0;
  if (sl_res_pool->sl_PSFCH_Config_r16 && sl_res_pool->sl_PSFCH_Config_r16->choice.setup
      && sl_res_pool->sl_PSFCH_Config_r16->choice.setup->sl_PSFCH_Period_r16) {
    long idx = *sl_res_pool->sl_PSFCH_Config_r16->choice.setup->sl_PSFCH_Period_r16;
    psfch_period = (idx < 4) ? psfch_periods[idx] : 0;
  }
  if (psfch_period == 1)
    return 3;
  if ((psfch_period == 2 || psfch_period == 4) && sci_pdu->psfch_overhead.nbits && sci_pdu->psfch_overhead.val == 1)
    return 3;
  return 0;
}

// Build the SLSCH RX transport-block config from a (decoded or preconfigured) SCI-1 PDU (episys SL port, F1: no CSI).
void config_pssch_slsch_pdu_rx(sl_nr_rx_config_pssch_pdu_t *nr_sl_pssch_pdu,
                               nr_sci_pdu_t *sci_pdu,
                               const NR_SL_BWP_Generic_r16_t *sl_bwp_generic,
                               const NR_SL_ResourcePool_r16_t *sl_res_pool)
{
  nr_sl_pssch_pdu->target_coderate = nr_get_code_rate_ul(sci_pdu->mcs, sci_pdu->additional_mcs.val);
  nr_sl_pssch_pdu->harq_pid = sci_pdu->harq_pid;
  nr_sl_pssch_pdu->mod_order = nr_get_Qm_ul(sci_pdu->mcs, sci_pdu->additional_mcs.val);
  nr_sl_pssch_pdu->mcs = sci_pdu->mcs;
  nr_sl_pssch_pdu->mcs_table = sci_pdu->additional_mcs.val;
  nr_sl_pssch_pdu->num_layers = 1 + (sci_pdu->number_of_dmrs_port & 1);
  nr_sl_pssch_pdu->rv_index = sci_pdu->rv_index;
  nr_sl_pssch_pdu->ndi = sci_pdu->ndi;

  int num_psfch_symbols = sl_num_psfch_symbols(sl_res_pool, sci_pdu);
  int pssch_numsym = 7 + *sl_bwp_generic->sl_LengthSymbols_r16 - num_psfch_symbols - 2;
  uint16_t l_subch;
  convNRFRIV(sci_pdu->frequency_resource_assignment.val, *sl_res_pool->sl_NumSubchannel_r16,
             *sl_res_pool->sl_UE_SelectedConfigRP_r16->sl_MaxNumPerReserve_r16, &l_subch, NULL, NULL);
  int subchannel_size = subch_to_rb[*sl_res_pool->sl_SubchannelSize_r16];
  int nohPRB = (sl_res_pool->sl_X_Overhead_r16) ? 3 * (*sl_res_pool->sl_X_Overhead_r16) : 0;
  int nREDMRS = get_nREDMRS(sl_res_pool);
  int pscch_numsym = pscch_tda[*sl_res_pool->sl_PSCCH_Config_r16->choice.setup->sl_TimeResourcePSCCH_r16];
  int pscch_numrbs = pscch_rb_table[*sl_res_pool->sl_PSCCH_Config_r16->choice.setup->sl_FreqResourcePSCCH_r16];
  int N_REprime = 12 * pssch_numsym - nohPRB - nREDMRS; // F1: no CSI-RS REs
  int N_REsci1 = 12 * pscch_numrbs * pscch_numsym;
  int sci2_beta_offset =
      *sl_res_pool->sl_PSSCH_Config_r16->choice.setup->sl_BetaOffsets2ndSCI_r16->list.array[sci_pdu->beta_offset_indicator];
  int sci2_alpha_times_100 = 100;
  if (sl_res_pool->sl_PowerControl_r16) {
    sci2_alpha_times_100 = 50 + (*sl_res_pool->sl_PowerControl_r16->sl_Alpha_PSSCH_PSCCH_r16) * 15;
    if (*sl_res_pool->sl_PowerControl_r16->sl_Alpha_PSSCH_PSCCH_r16 == 3)
      sci2_alpha_times_100 = 100;
  }
  int sci2_payload_len = nr_sci_size(sl_res_pool, sci_pdu, NR_SL_SCI_FORMAT_2A);
  int N_REsci2 = get_NREsci2(sci2_alpha_times_100, sci2_payload_len, sci2_beta_offset, pssch_numsym, pscch_numsym,
                             pscch_numrbs, l_subch, subchannel_size,
                             get_softmodem_params()->sl_mode ? 1 : nr_sl_pssch_pdu->mcs, nr_sl_pssch_pdu->mcs_table);
  int N_RE = N_REprime * l_subch * subchannel_size - N_REsci1 - N_REsci2;
  nr_sl_pssch_pdu->tb_size =
      (nr_compute_tbs_sl(nr_sl_pssch_pdu->mod_order, nr_sl_pssch_pdu->target_coderate, N_RE, nr_sl_pssch_pdu->num_layers) + 7) >> 3;
  nr_sl_pssch_pdu->tbslbrm = 0; // SL: servingCellConfig rate-matching disabled (matches episys)
}

// Build the SCI-2/PSSCH RX config (blind decode: l_subch=1, sense_pssch=0) from a SCI-1 PDU + PSCCH Nid.
// Returns 0 on success, -1 on an invalid DMRS pattern / symbol count. (episys SL port, F1)
int config_pssch_sci_pdu_rx(sl_nr_rx_config_pssch_sci_pdu_t *nr_sl_pssch_sci_pdu,
                            nr_sci_format_t sci2_format,
                            nr_sci_pdu_t *sci_pdu,
                            uint32_t pscch_Nid,
                            int pscch_subchannel_index,
                            const NR_SL_BWP_Generic_r16_t *sl_bwp_generic,
                            const NR_SL_ResourcePool_r16_t *sl_res_pool)
{
  AssertFatal(sci2_format > NR_SL_SCI_FORMAT_1A, "cannot use format 1A with this function\n");
  nr_sl_pssch_sci_pdu->sci2_len = nr_sci_size(sl_res_pool, sci_pdu, sci2_format);
  nr_sl_pssch_sci_pdu->sci2_beta_offset =
      *sl_res_pool->sl_PSSCH_Config_r16->choice.setup->sl_BetaOffsets2ndSCI_r16->list.array[sci_pdu->beta_offset_indicator];
  nr_sl_pssch_sci_pdu->sci2_alpha_times_100 = 100;
  if (sl_res_pool->sl_PowerControl_r16) {
    nr_sl_pssch_sci_pdu->sci2_alpha_times_100 = 50 + (*sl_res_pool->sl_PowerControl_r16->sl_Alpha_PSSCH_PSCCH_r16) * 15;
    if (*sl_res_pool->sl_PowerControl_r16->sl_Alpha_PSSCH_PSCCH_r16 == 3)
      nr_sl_pssch_sci_pdu->sci2_alpha_times_100 = 100;
  }
  int mcs_tb_ind = (sci_pdu->additional_mcs.nbits > 0) ? sci_pdu->additional_mcs.val : 0;
  nr_sl_pssch_sci_pdu->targetCodeRate = nr_get_code_rate_ul(sci_pdu->mcs, mcs_tb_ind);
  nr_sl_pssch_sci_pdu->mod_order = nr_get_Qm_ul(sci_pdu->mcs, mcs_tb_ind);
  nr_sl_pssch_sci_pdu->num_layers = 1 + sci_pdu->number_of_dmrs_port;
  nr_sl_pssch_sci_pdu->Nid = pscch_Nid; // 38.211 8.3.1.1: from PSCCH CRC
  nr_sl_pssch_sci_pdu->startrb = pscch_subchannel_index * 12 * (*sl_res_pool->sl_SubchannelSize_r16);
  nr_sl_pssch_sci_pdu->pscch_numsym = pscch_tda[*sl_res_pool->sl_PSCCH_Config_r16->choice.setup->sl_TimeResourcePSCCH_r16];
  nr_sl_pssch_sci_pdu->pscch_numrbs = pscch_rb_table[*sl_res_pool->sl_PSCCH_Config_r16->choice.setup->sl_FreqResourcePSCCH_r16];
  nr_sl_pssch_sci_pdu->num_subch = *sl_res_pool->sl_NumSubchannel_r16;
  nr_sl_pssch_sci_pdu->subchannel_size = subch_to_rb[*sl_res_pool->sl_SubchannelSize_r16];
  nr_sl_pssch_sci_pdu->l_subch = 1; // blind decode every subchannel

  int num_psfch_symbols = sl_num_psfch_symbols(sl_res_pool, sci_pdu);
  nr_sl_pssch_sci_pdu->pssch_numsym = 7 + *sl_bwp_generic->sl_LengthSymbols_r16 - num_psfch_symbols - 2;

  if (sci_pdu->dmrs_pattern.val >= sl_res_pool->sl_PSSCH_Config_r16->choice.setup->sl_PSSCH_DMRS_TimePatternList_r16->list.count) {
    LOG_W(NR_MAC, "dmrs.pattern %d out of bounds\n", sci_pdu->dmrs_pattern.val);
    sci_pdu->dmrs_pattern.val = 0;
    return -1;
  }
  int num_dmrs_symbols =
      *sl_res_pool->sl_PSSCH_Config_r16->choice.setup->sl_PSSCH_DMRS_TimePatternList_r16->list.array[sci_pdu->dmrs_pattern.val];
  if (num_dmrs_symbols == 2) {
    if (nr_sl_pssch_sci_pdu->pssch_numsym <= 5)
      return -1;
    nr_sl_pssch_sci_pdu->dmrs_symbol_position = sl_dmrs_mask2[nr_sl_pssch_sci_pdu->pscch_numsym - 2][nr_sl_pssch_sci_pdu->pssch_numsym - 6];
  } else if (num_dmrs_symbols == 3) {
    if (nr_sl_pssch_sci_pdu->pssch_numsym <= 8)
      return -1;
    nr_sl_pssch_sci_pdu->dmrs_symbol_position = sl_dmrs_mask3[nr_sl_pssch_sci_pdu->pssch_numsym - 9];
  } else if (num_dmrs_symbols == 4) {
    if (nr_sl_pssch_sci_pdu->pssch_numsym <= 10)
      return -1;
    nr_sl_pssch_sci_pdu->dmrs_symbol_position = sl_dmrs_mask4[nr_sl_pssch_sci_pdu->pssch_numsym - 11];
  }
  nr_sl_pssch_sci_pdu->sense_pssch = 0;
  return 0;
}

// Minimal F1 SLSCH scheduler: populate SCI-1 (sci_pdu, 1st stage) + SCI-2 (sci2_pdu, 2nd stage) field values
// for a single fixed-resource, fixed-MCS broadcast PSSCH grant (no sensing/CSI/BLER). The caller then runs
// nr_sci_size (to set per-field nbits) + fill_pssch_pscch_pdu. (episys SL data-plane port, F1 minimal)
void nr_schedule_slsch(const NR_SL_ResourcePool_r16_t *sl_tx_res_pool,
                       nr_sci_pdu_t *sci_pdu,
                       nr_sci_pdu_t *sci2_pdu,
                       uint8_t harq_pid,
                       uint8_t ndi,
                       uint8_t rv,
                       uint16_t src_id,
                       uint16_t dest_id,
                       uint8_t mcs)
{
  const uint8_t sl_num_subch = *sl_tx_res_pool->sl_NumSubchannel_r16;
  const uint16_t sl_max_num_reserve = *sl_tx_res_pool->sl_UE_SelectedConfigRP_r16->sl_MaxNumPerReserve_r16;
  const uint8_t l_subch = 1;        // single subchannel (minimal)
  const uint8_t n_start_subch1 = 0; // lowest subchannel
  const uint8_t N = 1;              // one reserved resource (no time hops)

  // ---- SCI-1A (1st stage) ----
  sci_pdu->priority = 0;
  sci_pdu->frequency_resource_assignment.val = compute_FRIV(sl_max_num_reserve, l_subch, n_start_subch1, 0, sl_num_subch);
  sci_pdu->time_resource_assignment.val = compute_TRIV(N, 0, 0);
  sci_pdu->resource_reservation_period.val = 0;
  sci_pdu->dmrs_pattern.val = 0;
  sci_pdu->second_stage_sci_format = 0; // SCI format 2A
  sci_pdu->beta_offset_indicator = 0;
  sci_pdu->number_of_dmrs_port = 0; // single layer
  sci_pdu->mcs = mcs;
  if (sl_tx_res_pool->sl_Additional_MCS_Table_r16) {
    long table_idx = *sl_tx_res_pool->sl_Additional_MCS_Table_r16;
    sci_pdu->additional_mcs.nbits = (table_idx < 2) ? 1 : 2;
    sci_pdu->additional_mcs.val = (uint8_t)table_idx;
  } else {
    sci_pdu->additional_mcs.nbits = 0;
    sci_pdu->additional_mcs.val = 0;
  }
  sci_pdu->psfch_overhead.val = 0;

  // ---- SCI-2A (2nd stage) ----
  sci2_pdu->harq_pid = harq_pid;
  sci2_pdu->ndi = ndi;
  sci2_pdu->rv_index = rv;
  sci2_pdu->source_id = src_id & 0xFF;
  sci2_pdu->dest_id = dest_id & 0xFFFF;
  sci2_pdu->harq_feedback = 0; // no PSFCH on the F1 path
  sci2_pdu->cast_type = 2;     // broadcast
  sci2_pdu->csi_req = 0;
  // SCI-2 carries a copy of the MCS-table selection so the RX config path can reuse sci_pdu fields.
  sci2_pdu->additional_mcs = sci_pdu->additional_mcs;
  sci2_pdu->mcs = mcs;
  sci2_pdu->number_of_dmrs_port = 0;
}
