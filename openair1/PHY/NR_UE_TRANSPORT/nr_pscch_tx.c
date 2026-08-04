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

/*! \file PHY/NR_UE_TRANSPORT/nr_pscch_tx.c
* \brief Top-level routines for generating and decoding the PSCCH physical channel
* \author R. Knopp 
* \date 2023
* \version 0.1
* \company Eurecom
* \email:
* \note
* \warning
*/
//#include "PHY/defs.h"
#include "PHY/impl_defs_nr.h"
#include "PHY/defs_nr_common.h"
#include "PHY/defs_nr_UE.h"
//#include "PHY/extern.h"
#include "PHY/NR_UE_TRANSPORT/pucch_nr.h"
#include "PHY/NR_UE_TRANSPORT/nr_transport_proto_ue.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"
#include <openair1/PHY/CODING/nrSmallBlock/nr_small_block_defs.h>
#include "PHY/CODING/nrPolar_tools/nr_polar_defs.h"       // polar_encoder_fast, NR_POLAR_SCI_MESSAGE_TYPE
#include "PHY/CODING/nrPolar_tools/nr_polar_dci_defs.h"   // NR_POLAR_SCI_MESSAGE_TYPE
#include "PHY/CODING/coding_defs.h"                       // crc24c
#include "PHY/MODULATION/nr_modulation.h"                 // nr_modulation, DMRS_MOD_ORDER
#include "PHY/NR_REFSIG/nr_refsig.h"                       // nr_gold_pdcch, gold_cache
#include "PHY/NR_REFSIG/dmrs_nr.h"                          // get_dmrs_freq_idx_ul
#include "PHY/NR_TRANSPORT/nr_sch_dmrs.h"                   // get_Wt, get_Wf, get_delta
#include "PHY/NR_UE_TRANSPORT/nr_transport_ue.h"            // NR_UE_ULSCH_t
#include "common/utils/LOG/log.h"
#include "common/utils/LOG/vcd_signal_dumper.h"

#include "T.h"

// episys SL PHY helpers defined in nr_pscch_pssch_rx.c (non-static); forward-declared to avoid a header cycle.
uint32_t nr_sl_get_G(uint16_t nb_rb, uint16_t nb_symb_sch, uint8_t nb_re_dmrs, uint16_t length_dmrs,
                     uint8_t sci1_dmrs_overlap, uint16_t sci1_re, uint16_t sci1_rb, uint16_t sci2_re,
                     uint16_t csi_rs_re, uint8_t Qm, uint8_t Nl);
int nr_sl_get_NREsci2(int sci2_alpha, int sci2_payload_len, int sci2_beta_offset, int pssch_numsym,
                      int pscch_numsym, int pscch_numrbs, int l_subch, int subchannel_size,
                      int target_coderate, int mcs_table_index);
// develop UE-lib helpers (declared in nr_ulsch_coding.c / dmrs); forward-declared for the SL TX path.
int nr_ulsch_pre_encoding(PHY_VARS_NR_UE *ue, const NR_UE_ULSCH_t *ulsch, const uint32_t frame, const uint8_t slot,
                          const unsigned int *G, const int nb_ulsch, const uint8_t *ULSCH_ids);
uint8_t allowed_xlsch_re_in_dmrs_symbol(uint16_t k, uint16_t start_sc, uint16_t ofdm_symbol_size,
                                        uint8_t numDmrsCdmGrpsNoData, uint8_t dmrs_type);

// episys SL data-plane port: PSCCH DMRS/SCI scrambling (38.211 8.3.3.1), gold-XOR (mirrors the develop
// gNB nr_pdcch_scrambling; UE-native, no PHY_NR link). scrambling_RNTI is 1010 for sidelink PSCCH.
static void nr_pscch_scrambling(const uint32_t *in, uint32_t size, uint32_t Nid, uint32_t scrambling_RNTI, uint32_t *out)
{
  const int roundedSz = (size + 31) / 32;
  uint32_t *seq = gold_cache((scrambling_RNTI << 16) + Nid, roundedSz);
  for (int i = 0; i < roundedSz; i++)
    out[i] = in[i] ^ seq[i];
}

// UE-native PSCCH SCI-1A generation (episys SL data-plane port). Replaces the previous delegation to the
// gNB nr_generate_dci (PHY_NR, not linked into nr-uesoftmodem). Faithful to the episys UE/PC5 path in
// nr_generate_dci: polar encode (SCI message type) -> scramble -> QPSK -> PSCCH DMRS -> RE map onto the
// SL grid. Returns the PSCCH CRC (caller keeps the low 16 bits as the Nid for PSSCH DMRS + SLSCH scrambling).
uint32_t nr_generate_sci1(const PHY_VARS_NR_UE *ue,
                          c16_t *txdataF,
                          const NR_DL_FRAME_PARMS *frame_parms,
                          const int16_t amp,
                          const int nr_slot_tx,
                          const sl_nr_tx_config_pscch_pssch_pdu_t *pscch_pssch_pdu)
{
  const int rb_offset = pscch_pssch_pdu->startrb;   // PSCCH lowest RB (FreqDomainResource[0])
  const int n_rb = pscch_pssch_pdu->pscch_numrbs;
  const int start_symb = 1;                          // symbol 0 = AGC/guard
  const int dur = pscch_pssch_pdu->pscch_numsym;
  const uint16_t Nid = pscch_pssch_pdu->pscch_dmrs_scrambling_id;
  const uint16_t scrambling_RNTI = 1010;
  const int agg = pscch_pssch_pdu->pscch_numrbs * pscch_pssch_pdu->pscch_numsym; // "aggregation" = PRBs*symbols
  const uint16_t payload_bits = pscch_pssch_pdu->pscch_sci_payload_len;

  uint16_t cset_start_sc = frame_parms->first_carrier_offset + rb_offset * NR_NB_SC_PER_RB;
  if (cset_start_sc >= frame_parms->ofdm_symbol_size)
    cset_start_sc -= frame_parms->ofdm_symbol_size;

  // --- SCI-1 polar encode (SCI message type; n_RNTI = 0 for sidelink; CRC24C) ---
  uint64_t payload = *(const uint64_t *)pscch_pssch_pdu->pscch_sci_payload;
  uint32_t encoder_output[NR_MAX_DCI_SIZE_DWORD] = {0};
  polar_encoder_fast(&payload, (void *)encoder_output, 0 /*crcmask/n_RNTI*/, 1, NR_POLAR_SCI_MESSAGE_TYPE, payload_bits, agg);
  const uint32_t encoded_length = agg * 18; // 9 PSCCH data REs/PRB * 2 bits (QPSK)

  // --- scramble + QPSK modulate the SCI payload ---
  uint32_t scrambled_output[NR_MAX_DCI_SIZE_DWORD] = {0};
  nr_pscch_scrambling(encoder_output, encoded_length, Nid, scrambling_RNTI, scrambled_output);
  int16_t mod_sci[encoded_length] __attribute__((aligned(16)));
  nr_modulation(scrambled_output, encoded_length, DMRS_MOD_ORDER, mod_sci);

  // --- PSCCH DMRS (QPSK) per symbol, from the PDCCH gold generator scrambled by Nid ---
  const uint32_t dmrs_length = (n_rb + rb_offset) * 6; // 3 DMRS/RB * 2 bits (QPSK)
  int16_t mod_dmrs[start_symb + dur][((dmrs_length + 15) >> 4) << 4] __attribute__((aligned(16)));
  for (int symb = start_symb; symb < start_symb + dur; symb++) {
    const uint32_t *gold = nr_gold_pdcch(frame_parms->N_RB_DL, frame_parms->symbols_per_slot, Nid, nr_slot_tx, symb);
    nr_modulation(gold, dmrs_length, DMRS_MOD_ORDER, mod_dmrs[symb]);
  }

  // --- RE mapping: contiguous PSCCH RBs from cset_start_sc; per RB, DMRS at REs 1,5,9 and SCI on the rest ---
  int sci_idx = 0;
  const int num_regs = n_rb; // agg/dur
  for (int symbol_idx = 0; symbol_idx < dur; symbol_idx++) {
    const int l = start_symb + symbol_idx;
    int k = cset_start_sc;
    for (int reg = 0; reg < num_regs; reg++) {
      /* A PRB's DMRS are indexed by its ABSOLUTE position in the grid, not by its position within the
       * PSCCH allocation (38.211 7.4.1.3.2, reference point k = 0) - which is why the sequence above spans
       * rb_offset + n_rb PRBs. The receiver offsets into it the same way (nr_pscch_channel_estimation), as
       * does the reference implementation on both sides. No-op while rb_offset is 0, which is the case for
       * a single subchannel starting at RB 0. */
      int dmrs_idx = (rb_offset + reg) * 3;
      int k_prime = 0;
      for (int m = 0; m < NR_NB_SC_PER_RB; m++) {
        const int re = l * frame_parms->ofdm_symbol_size + k;
        if (m == (k_prime << 2) + 1) { // DMRS RE
          txdataF[re].r = (amp * mod_dmrs[l][dmrs_idx << 1]) >> 15;
          txdataF[re].i = (amp * mod_dmrs[l][(dmrs_idx << 1) + 1]) >> 15;
          dmrs_idx++;
          k_prime++;
        } else { // SCI payload RE
          txdataF[re].r = (amp * mod_sci[sci_idx << 1]) >> 15;
          txdataF[re].i = (amp * mod_sci[(sci_idx << 1) + 1]) >> 15;
          sci_idx++;
        }
        k++;
        if (k >= frame_parms->ofdm_symbol_size)
          k -= frame_parms->ofdm_symbol_size;
      }
    }
  }

  // PSCCH CRC -> Nid for PSSCH DMRS + SLSCH scrambling (38.211 8.3.1.1). crc24c() returns the 24-bit CRC in
  // the high bits. TODO(reconcile): confirm the exact Nid bit-extraction against the RX SCI1 decode + MAC.
  return crc24c((uint8_t *)&payload, payload_bits) >> 8;
}

// UE-native PSSCH data transmit (episys SL data-plane port). Encodes the SLSCH TB (reusing develop's UE
// UL-SCH encoder), polar-encodes SCI-2, scrambles/modulates both, layer-maps, and RE-maps DMRS + SCI-2 +
// SLSCH onto the SL grid (txdataF), skipping the PSCCH (SCI-1) region already written by nr_generate_sci1.
// Single-layer (SL mode-2). The TB payload is expected in ue->ul_harq_processes[harq_pid].payload_AB (MAC).
// NOTE: runtime RE-map consistency with the RX puncturing (nr_rx_pssch) is pending MAC (#9) + OTA validation.
void nr_ue_slsch_procedures(PHY_VARS_NR_UE *ue, uint32_t frame, uint8_t slot, nr_phy_data_tx_t *phy_data, c16_t **txdataF)
{
  const sl_nr_tx_config_pscch_pssch_pdu_t *pdu = &phy_data->nr_sl_pssch_pscch_pdu;
  NR_DL_FRAME_PARMS *fp = &ue->SL_UE_PHY_PARAMS.sl_frame_params;

  const int start_symbol = 1; // symbol 0 = AGC/guard
  const int number_of_symbols = pdu->pssch_numsym;
  const uint16_t dmrs_pos = pdu->dmrs_symbol_position;
  const int dmrs_type = pusch_dmrs_type1;
  const int start_rb = pdu->startrb;
  const int nb_rb = pdu->l_subch * pdu->subchannel_size;
  const int Nl = pdu->num_layers;
  const uint8_t mod_order = pdu->mod_order;
  const uint8_t cdm_grps_no_data = 1;
  const uint16_t Nid = phy_data->pscch_Nid;
  const uint8_t harq_pid = pdu->harq_pid;
  const int start_sc = fp->first_carrier_offset + start_rb * NR_NB_SC_PER_RB;

  AssertFatal(Nl == 1, "SL PSSCH TX currently supports single layer only (Nl=%d)\n", Nl);

  int number_dmrs_symbols = 0;
  for (int i = start_symbol; i < start_symbol + number_of_symbols; i++)
    if ((dmrs_pos >> i) & 1)
      number_dmrs_symbols++;

  // G + SCI-2 RE budget (same computation the RX uses, so counts match)
  static const int sl_mask[2] = {7, 15};
  const int sci1_dmrs_overlap = dmrs_pos & sl_mask[pdu->pscch_numsym - 2];
  const uint16_t sci1_re = pdu->pscch_numsym * pdu->pscch_numrbs * NR_NB_SC_PER_RB;
  const int sci2_re = nr_sl_get_NREsci2(pdu->sci2_alpha_times_100, pdu->sci2_payload_len, pdu->sci2_beta_offset,
                                        pdu->pssch_numsym, pdu->pscch_numsym, pdu->pscch_numrbs, pdu->l_subch,
                                        pdu->subchannel_size, pdu->target_coderate, pdu->mcs_table);
  unsigned int G = nr_sl_get_G(nb_rb, number_of_symbols, 6, number_dmrs_symbols, sci1_dmrs_overlap, sci1_re,
                               pdu->pscch_numrbs, sci2_re, 0, mod_order, Nl);

  // --- SLSCH TB encode via develop's UE UL-SCH encoder (TB already in ul_harq_processes[harq_pid].payload_AB) ---
  NR_UE_ULSCH_t ulsch = {0};
  nfapi_nr_ue_pusch_pdu_t *pp = &ulsch.pusch_pdu;
  pp->rb_size = nb_rb;
  pp->rb_start = start_rb;
  pp->bwp_start = 0;
  pp->nr_of_symbols = number_of_symbols;
  pp->start_symbol_index = start_symbol;
  pp->qam_mod_order = mod_order;
  pp->mcs_index = pdu->mcs;
  pp->nrOfLayers = Nl;
  pp->ul_dmrs_symb_pos = dmrs_pos;
  pp->dmrs_config_type = 0; // type1
  pp->num_dmrs_cdm_grps_no_data = cdm_grps_no_data;
  pp->pusch_data.tb_size = pdu->tb_size;
  pp->pusch_data.rv_index = pdu->rv_index;
  pp->pusch_data.harq_process_id = harq_pid;
  pp->tbslbrm = pdu->tbslbrm;
  pp->target_code_rate = pdu->target_coderate;
  pp->transform_precoding = transformPrecoder_disabled;
  // LDPC base graph: the develop UL-SCH encoder reads pp->ldpcBaseGraph (nr_ulsch_coding.c). It is otherwise 0
  // (invalid) here, so the TX would encode with a different base graph than the RX decodes (nr_slsch_decoding
  // computes BG from A + code rate) -> the RX recovers an all-zero TB. Compute BG with the SAME rule so they match.
  {
    const uint32_t A_bits = (uint32_t)pdu->tb_size << 3;
    const float Crate = (float)pp->target_code_rate / 10240.0f;
    pp->ldpcBaseGraph = ((A_bits <= 292) || ((A_bits <= 3824) && (Crate <= 0.6667f)) || (Crate <= 0.25f)) ? 2 : 1;
  }
  ulsch.status = NR_ACTIVE;

  // TB bridge (episys SL data-plane port): the MAC placed the SLSCH transport block in the TX PDU
  // (value-copied MAC->PHY via the fapi handler). Copy it into the UL HARQ payload buffer that the
  // develop UE UL-SCH encoder reads. See sl_nr_tx_config_pscch_pssch_pdu_t.slsch_payload.
  {
    NR_UL_UE_HARQ_t *txharq = &ue->ul_harq_processes[harq_pid];
    uint32_t tb_bytes = pdu->tb_size;
    if (pdu->slsch_payload_len && txharq->payload_AB)
      memcpy(txharq->payload_AB, pdu->slsch_payload, (pdu->slsch_payload_len < tb_bytes) ? pdu->slsch_payload_len : tb_bytes);
  }

  unsigned int Garr[1] = {G};
  uint8_t ULSCH_ids[1] = {0};
  if (nr_ulsch_pre_encoding(ue, &ulsch, frame, slot, Garr, 1, ULSCH_ids) != 0) {
    LOG_E(NR_PHY, "SLSCH pre-encoding failed\n");
    return;
  }
  if (nr_ulsch_encoding(ue, &ulsch, frame, slot, Garr, 1, ULSCH_ids) == -1) {
    LOG_E(NR_PHY, "SLSCH encoding failed\n");
    return;
  }
  NR_UL_UE_HARQ_t *harq = &ue->ul_harq_processes[harq_pid];

  // --- SCI-2 polar encode ---
  const uint32_t Gsci2 = (uint32_t)sci2_re * 2 * Nl;
  uint32_t sci2_encoded[(Gsci2 >> 5) + 2];
  memset(sci2_encoded, 0, sizeof(sci2_encoded));
  polar_encoder_fast((uint64_t *)pdu->sci2_payload, (void *)sci2_encoded, 0, 1, NR_POLAR_SCI2_MESSAGE_TYPE,
                     pdu->sci2_payload_len, sci2_re);

  // --- scramble SLSCH (matches RX nr_sl_unscrambling: q=0, n_RNTI=1010) + SCI-2 (gold XOR, 1010) ---
  uint32_t scrambled_slsch[(G >> 5) + 2];
  memset(scrambled_slsch, 0, sizeof(scrambled_slsch));
  nr_codeword_scrambling(harq->f, G, 0, Nid, 1010, scrambled_slsch);
  uint32_t scrambled_sci2[(Gsci2 >> 5) + 2];
  memset(scrambled_sci2, 0, sizeof(scrambled_sci2));
  nr_pscch_scrambling(sci2_encoded, Gsci2, Nid, 1010, scrambled_sci2);

  // --- modulate: combined stream d_mod = [SCI-2 QPSK symbols][SLSCH symbols] ---
  const int max_re = number_of_symbols * nb_rb * NR_NB_SC_PER_RB;
  c16_t d_mod[max_re] __attribute__((aligned(16)));
  memset(d_mod, 0, sizeof(d_mod));
  nr_modulation(scrambled_sci2, Gsci2, DMRS_MOD_ORDER, (int16_t *)d_mod);      // SCI-2 is QPSK
  nr_modulation(scrambled_slsch, G, mod_order, (int16_t *)(d_mod + sci2_re));  // SLSCH after SCI-2

  // --- layer mapping (Nl=1) ---
  const int n_data_re = G / mod_order + sci2_re;
  c16_t tx_layers[1][n_data_re] __attribute__((aligned(16)));
  memset(tx_layers, 0, sizeof(tx_layers));
  nr_ue_layer_mapping(d_mod, Nl, n_data_re, tx_layers);

  // --- RE mapping onto txdataF[0] (single layer/antenna): DMRS on DMRS REs, data (SCI-2 first via m) elsewhere,
  //     skipping the PSCCH (SCI-1) region in the PSCCH symbols (written by nr_generate_sci1). ---
  const int delta = get_delta(0, dmrs_type);
  int8_t Wf[2], Wt[2];
  get_Wf(Wf, 0, dmrs_type);
  get_Wt(Wt, 0, dmrs_type);
  int m = 0;
  for (int l = start_symbol; l < start_symbol + number_of_symbols; l++) {
    uint16_t k = start_sc;
    const int is_dmrs_sym = (dmrs_pos >> l) & 1;
    const int is_pscch_sym = (l < start_symbol + pdu->pscch_numsym);
    int dmrs_idx = start_rb * 6;
    int n = 0, k_prime = 0;
    int16_t mod_dmrs[((nb_rb + start_rb) * 6) << 1] __attribute__((aligned(16)));
    if (is_dmrs_sym) {
      /* PSSCH DMRS, 38.211 8.4.1.1.1. Must be the SAME function the receiver uses
       * (nr_pssch_channel_estimation -> nr_gold_pusch), not an equivalent one: the SL-local generator that
       * used to be here built c_init in 32-bit arithmetic, wrapping mod 2^32 where the spec takes mod 2^31,
       * so the two ends used different DMRS on every slot whose product had bit 31 set. */
      const uint32_t *pssch_dmrs = nr_gold_pusch(fp->N_RB_UL, fp->symbols_per_slot, Nid, 0 /* nscid */, slot, l);
      nr_modulation(pssch_dmrs, (nb_rb + start_rb) * 6 * 2, DMRS_MOD_ORDER, mod_dmrs);
    }
    for (int i = 0; i < nb_rb * NR_NB_SC_PER_RB; i++) {
      // Skip the PSCCH region at the start of the PSSCH allocation in PSCCH symbols.
      if (is_pscch_sym && i == 0) {
        i += pdu->pscch_numrbs * NR_NB_SC_PER_RB;
        k += pdu->pscch_numrbs * NR_NB_SC_PER_RB;
        if (is_dmrs_sym) {
          dmrs_idx += 6 * pdu->pscch_numrbs;
          n += 3 * pdu->pscch_numrbs;
        }
        if (i >= nb_rb * NR_NB_SC_PER_RB)
          break;
      }
      if (k >= fp->ofdm_symbol_size)
        k -= fp->ofdm_symbol_size;
      const int off = l * fp->ofdm_symbol_size + k;
      int is_dmrs = 0;
      if (is_dmrs_sym && k == ((start_sc + get_dmrs_freq_idx_ul(n, k_prime, delta, dmrs_type)) % fp->ofdm_symbol_size))
        is_dmrs = 1;
      if (is_dmrs) {
        txdataF[0][off].r = (Wt[0] * Wf[k_prime] * AMP * mod_dmrs[dmrs_idx << 1]) >> 15;
        txdataF[0][off].i = (Wt[0] * Wf[k_prime] * AMP * mod_dmrs[(dmrs_idx << 1) + 1]) >> 15;
        dmrs_idx++;
        k_prime++;
        k_prime &= 1;
        n += (k_prime) ? 0 : 1;
      } else if (!is_dmrs_sym
                 || allowed_xlsch_re_in_dmrs_symbol(k, start_sc, fp->ofdm_symbol_size, cdm_grps_no_data, dmrs_type)) {
        if (m < n_data_re)
          txdataF[0][off] = tx_layers[0][m++];
      }
      k++;
    }
  }
  ue->SL_UE_PHY_PARAMS.pssch.num_pssch_sci2_tx++;
  ue->SL_UE_PHY_PARAMS.pssch.num_pssch_tx++;
  LOG_D(NR_PHY, "%d.%d PSSCH TX: G %u sci2_re %d mapped %d data REs (rb %d, Qm %d)\n",
        frame, slot, G, sci2_re, m, nb_rb, mod_order);
}
