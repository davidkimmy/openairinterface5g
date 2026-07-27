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

#define _GNU_SOURCE

#include "PHY/defs_nr_UE.h"
#include "PHY/nr_phy_common/inc/nr_sl_decode_defs.h"  // episys SL port: shared LDPC decode/HARQ/ULSCH/PUSCH structs (no gNB coupling)
#include "PHY/gold.h"       // episys SL port: gold_generic (was lte_gold_generic on the old branch)
#include "NR_IF_Module.h"
#include "openair1/SCHED_NR_UE/defs.h"
#include "PHY/NR_UE_TRANSPORT/nr_transport_proto_ue.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"  // nr_segmentation, nr_get_E, nr_get_R_ldpc_decoder, lenWithCrc
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h"  // nrLDPC_TB/slot decoding params + common decoder
#include "executables/nr-uesoftmodem.h"                        // get_nrUE_params (Tpool)
#include "common/utils/LOG/vcd_signal_dumper.h"
#include "PHY/NR_UE_ESTIMATION/nr_estimation.h"                 // nr_pssch_channel_estimation (UE-native SL DMRS ch-est)
#include "PHY/nr_phy_common/inc/nr_phy_common.h"                // nr_scale_channel, nr_channel_level, nr_compute_llr
#include "PHY/nr_phy_common/inc/nr_channel_compensation.h"      // nr_channel_compensation

/* episys SL data-plane port (develop-way reconciliation): nr_fill_sl_indication and
   nr_fill_sl_rx_indication are provided by develop's SCHED_NR_UE/phy_procedures_nr_ue_sl.c
   (already wired into develop's SL threads + PSBCH/SSB path). The SLSCH-fill logic that used
   to live in episys's nr_fill_sl_rx_indication has been grafted into develop's version there.
   Only the PSSCH-specific helpers (nr_pdcch_unscrambling, nr_postDecode_slsch) remain here. */

// nr_get_code_rate_ul lives in LAYER2/NR_MAC_COMMON/nr_mac_common.h; forward-declare here to avoid a PHY->MAC include.
extern uint32_t nr_get_code_rate_ul(uint8_t Imcs, uint8_t table_idx);

/* episys SL data-plane port: small SL PHY helpers (UE-native, ported from episys nr_common.c /
   nr_tbs_tools.c / nr_ulsch.c). Named nr_sl_* to avoid clashing with any future gNB ULSCH symbols. */
#define SL_MAX_EL_213_9_3_2 19
static const float sl_tab38_213_9_3_2[SL_MAX_EL_213_9_3_2] =
  {1.125,1.250,1.375,1.625,1.750,2.000,2.250,2.500,2.875,3.125,3.500,4.000,5.000,6.250,8.000,10.000,12.625,15.875,20.000};
static const int sl_dmrs_pscch_mask[2] = {7, 15};

// TS 38.213 9.3.2: number of REs occupied by 2nd-stage SCI on PSSCH.
int nr_sl_get_NREsci2(int sci2_alpha, int sci2_payload_len, int sci2_beta_offset,
                      int pssch_numsym, int pscch_numsym, int pscch_numrbs,
                      int l_subch, int subchannel_size, int target_coderate, int mcs_table_index)
{
  float Osci2 = (float)sci2_payload_len;
  AssertFatal(sci2_beta_offset < SL_MAX_EL_213_9_3_2, "illegal sci2_beta_offset %d\n", sci2_beta_offset);
  float beta_offset_sci2 = sl_tab38_213_9_3_2[sci2_beta_offset];
  uint32_t R10240 = get_softmodem_params()->sl_mode ? nr_get_code_rate_ul(1, mcs_table_index) : target_coderate;
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

// TS 38.212: number of coded bits G available for the SLSCH on PSSCH.
uint32_t nr_sl_get_G(uint16_t nb_rb, uint16_t nb_symb_sch, uint8_t nb_re_dmrs, uint16_t length_dmrs,
                     uint8_t sci1_dmrs_overlap, uint16_t sci1_re, uint16_t sci1_rb, uint16_t sci2_re,
                     uint16_t csi_rs_re, uint8_t Qm, uint8_t Nl)
{
  uint32_t slsch_re = ((NR_NB_SC_PER_RB * nb_symb_sch) - (nb_re_dmrs * length_dmrs)) * nb_rb;
  if (sci1_dmrs_overlap > 0) slsch_re += (nb_re_dmrs * sci1_rb);
  slsch_re -= sci1_re;
  slsch_re -= sci2_re;
  slsch_re -= csi_rs_re;
  return slsch_re * Qm * Nl;
}

// Layer de-mapping of PSSCH LLRs (TS 38.211 6.3.1.3), UE-native (was gNB nr_ulsch_layer_demapping).
void nr_sl_layer_demapping(int16_t *llr_cw, uint8_t Nl, uint8_t mod_order, uint32_t length, int16_t **llr_layers)
{
  switch (Nl) {
    case 1:
      memcpy((void *)llr_cw, (void *)llr_layers[0], length * sizeof(int16_t));
      break;
    case 2: case 3: case 4:
      for (int i = 0; i < (length / Nl / mod_order); i++)
        for (int l = 0; l < Nl; l++)
          for (int m = 0; m < mod_order; m++)
            llr_cw[i * Nl * mod_order + l * mod_order + m] = llr_layers[l][i * mod_order + m];
      break;
    default:
      AssertFatal(0, "Unsupported number of SL layers %d\n", Nl);
  }
}

// PSSCH LLR descrambling (reuses develop's common nr_codeword_unscrambling).
void nr_sl_unscrambling(int16_t *llr, uint32_t size, uint32_t Nid, uint32_t n_RNTI)
{
  nr_codeword_unscrambling(llr, size, 0, Nid, n_RNTI);
}

void nr_pdcch_unscrambling(int16_t *e_rx,
                           uint16_t scrambling_RNTI,
                           uint32_t length,
                           uint16_t pdcch_DMRS_scrambling_id,
                           int16_t *z2,
                           int sci_flag) {
  int i;
  uint8_t reset;
  uint32_t x1 = 0, x2 = 0, s = 0;
  uint16_t n_id; //{0,1,...,65535}
  uint32_t rnti = (uint32_t) scrambling_RNTI;
  reset = 1;
  // x1 is set in first call to lte_gold_generic
  n_id = pdcch_DMRS_scrambling_id;
  x2 = sci_flag == 0 ? ((rnti<<16) + n_id) : ((n_id<<15) + 1010); //mod 2^31 is implicit //this is c_init in 38.211 v15.1.0 Section 7.3.2.3

  LOG_D(PHY,"PDCCH Unscrambling x2 %x : scrambling_RNTI %x\n", x2, rnti);

  for (i = 0; i < length; i++) {
    if ((i & 0x1f) == 0) {
      s = gold_generic(&x1, &x2, reset);
      reset = 0;
    }

    if (((s >> (i % 32)) & 1) == 1)
      z2[i] = -e_rx[i];
    else
      z2[i]=e_rx[i];
  }
}

void nr_postDecode_slsch(PHY_VARS_NR_UE *UE, notifiedFIFO_elt_t *req,UE_nr_rxtx_proc_t *proc,nr_phy_data_t *phy_data, int8_t *ack_nack_rcvd, uint8_t num_acks)
{
  ldpcDecode_t *rdata = (ldpcDecode_t*) NotifiedFifoData(req);
  NR_UL_gNB_HARQ_t *slsch_harq = rdata->ulsch_harq;
  NR_gNB_ULSCH_t *slsch = rdata->ulsch;
  int r = rdata->segment_r;
  sl_nr_rx_config_pssch_pdu_t *slsch_pdu = &phy_data->nr_sl_pssch_pdu;//UE->slsch[rdata->ulsch_id].harq_process->slsch_pdu;
  bool decodeSuccess = (rdata->decodeIterations <= rdata->decoderParms.numMaxIter);
  slsch_harq->processedSegments++;
  LOG_D(NR_PHY,
        "processing result of segment: %d, processed %d/%d\n",
        rdata->segment_r,
        slsch_harq->processedSegments,
        rdata->nbSegments);
  if (decodeSuccess) {
    memcpy(slsch_harq->b + rdata->offset, slsch_harq->c[r], rdata->Kr_bytes - (slsch_harq->F >> 3) - ((slsch_harq->C > 1) ? 3 : 0));

  } else {
    LOG_D(NR_PHY, "ULSCH %d in error\n", rdata->ulsch_id);
  }

  //int dumpsig=0;
  // if all segments are done
  if (rdata->nbSegments == slsch_harq->processedSegments) {
    sl_nr_rx_indication_t sl_rx_indication;
    nr_sidelink_indication_t sl_indication;
    slsch_status_t slsch_status;
    if (!check_abort(&slsch_harq->abort_decode) && !UE->pssch_vars[rdata->ulsch_id].DTX) {
      LOG_D(NR_PHY,
            "[UE] SLSCH %d: Setting ACK for SFN/SF %d.%d (pid %d, ndi %d, status %d, round %d, TBS %d, Max interation "
            "(all seg) %d)\n",
            rdata->ulsch_id,
            proc->frame_rx,
            proc->nr_slot_rx,
            rdata->harq_pid,
            slsch_pdu->ndi,
            slsch->active,
            slsch_harq->round,
            slsch_harq->TBS,
            rdata->decodeIterations);
      slsch->active = false;
      slsch_harq->round = 0;
      LOG_D(NR_PHY, "%4d.%2d SLSCH received ok \n", proc->frame_rx, proc->nr_slot_rx);
      slsch_status.rdata = rdata;
      slsch_status.rxok = true;
      //dumpsig=1;
    } else {
      LOG_E(NR_PHY,
            "[UE] SLSCH %d in error: Setting NAK for SFN/SF %d/%d (pid %d, ndi %d, status %d, round %d, RV %d, prb_start %d, subchannel_size %d, "
            "TBS %d) r %d\n",
            rdata->ulsch_id,
            proc->frame_rx,
            proc->nr_slot_rx,
            rdata->harq_pid,
            slsch_pdu->ndi,
            slsch->active,
            slsch_harq->round,
            slsch_harq->slsch_pdu ? slsch_harq->slsch_pdu->rv_index : slsch_harq->ulsch_pdu.pusch_data.rv_index,
            slsch_harq->pssch_pdu->startrb,
            slsch_harq->pssch_pdu->subchannel_size,
            slsch_harq->TBS,
            r);
      slsch->handled = 1;
      LOG_D(NR_PHY, "%4d.%2d SLSCH %d in error\n", proc->frame_rx, proc->nr_slot_rx, rdata->ulsch_id);
      slsch_status.rdata = rdata;
      slsch_status.rxok = false;
      //      dumpsig=1;
    }
    slsch->last_iteration_cnt = rdata->decodeIterations;

#ifdef ENABLE_BLER_INSTRUMENTATION
    // Log LDPC iterations with MCS (SNR derived from noise power in test)
    uint8_t mcs = slsch_harq->slsch_pdu ? slsch_harq->slsch_pdu->mcs : 0;
    LOG_I(NR_PHY, "[LDPC_STATS] %d.%d PC5_LDPC_ITERATIONS mcs=%u iterations=%u max=%u success=%d\n",
          proc->frame_rx, proc->nr_slot_rx,
          mcs, rdata->decodeIterations, rdata->decoderParms.numMaxIter, decodeSuccess);
#endif

    sl_rx_indication.sfn = proc->frame_rx;
    sl_rx_indication.slot = proc->nr_slot_rx;
#if 0 // episys SL data-plane port: PSFCH HARQ-ACK feedback delivery deferred (F2). sl_nr_slsch_pdu_t
      // aliases fapi_nr_pdsch_pdu_t on develop and has no ack_nack_rcvd/num_acks_rcvd; re-enable with PSFCH.
    sl_rx_indication.rx_indication_body[0].rx_slsch_pdu.ack_nack_rcvd = calloc(num_acks, sizeof(uint8_t));
    memcpy((void*)sl_rx_indication.rx_indication_body[0].rx_slsch_pdu.ack_nack_rcvd, (void*)ack_nack_rcvd,
          num_acks * sizeof(uint8_t));
    sl_rx_indication.rx_indication_body[0].rx_slsch_pdu.num_acks_rcvd = num_acks;
#endif
    (void)ack_nack_rcvd; (void)num_acks;
    uint8_t pdu_type = phy_data->sl_rx_action == SL_NR_CONFIG_TYPE_RX_PSSCH_SLSCH_PSFCH ? SL_NR_RX_PDU_TYPE_SLSCH_PSFCH : SL_NR_RX_PDU_TYPE_SLSCH;
    nr_fill_sl_rx_indication(&sl_rx_indication, pdu_type, UE, 1, (void*)&slsch_status, 0);
    nr_fill_sl_indication(&sl_indication,&sl_rx_indication,NULL,proc,UE,phy_data);
    if (UE->if_inst && UE->if_inst->sl_indication)
      UE->if_inst->sl_indication(&sl_indication);
#ifdef DEBUG_SLSCH
        if (ulsch_harq->ulsch_pdu.mcs_index == 0 && dumpsig==1) {
          int off = ((ulsch_harq->ulsch_pdu.rb_size&1) == 1)? 4:0;

          LOG_M("rxsigF0.m","rxsF0",&gNB->common_vars.rxdataF[0][(ulsch_harq->slot&3)*gNB->frame_parms.ofdm_symbol_size*gNB->frame_parms.symbols_per_slot],gNB->frame_parms.ofdm_symbol_size*gNB->frame_parms.symbols_per_slot,1,1);
          LOG_M("rxsigF0_ext.m","rxsF0_ext",
                 &gNB->pusch_vars[0].rxdataF_ext[0][ulsch_harq->ulsch_pdu.start_symbol_index*NR_NB_SC_PER_RB *
       ulsch_harq->ulsch_pdu.rb_size],ulsch_harq->ulsch_pdu.nr_of_symbols*(off+(NR_NB_SC_PER_RB *
       ulsch_harq->ulsch_pdu.rb_size)),1,1); LOG_M("chestF0.m","chF0",
                &gNB->pusch_vars[0].ul_ch_estimates[0][ulsch_harq->ulsch_pdu.start_symbol_index*gNB->frame_parms.ofdm_symbol_size],gNB->frame_parms.ofdm_symbol_size,1,1);
          LOG_M("chestF0_ext.m","chF0_ext",
                &gNB->pusch_vars[0]->ul_ch_estimates_ext[0][(ulsch_harq->ulsch_pdu.start_symbol_index+1)*(off+(NR_NB_SC_PER_RB *
       ulsch_harq->ulsch_pdu.rb_size))], (ulsch_harq->ulsch_pdu.nr_of_symbols-1)*(off+(NR_NB_SC_PER_RB *
       ulsch_harq->ulsch_pdu.rb_size)),1,1); LOG_M("rxsigF0_comp.m","rxsF0_comp",
                &gNB->pusch_vars[0].rxdataF_comp[0][ulsch_harq->ulsch_pdu.start_symbol_index*(off+(NR_NB_SC_PER_RB *
       ulsch_harq->ulsch_pdu.rb_size))],ulsch_harq->ulsch_pdu.nr_of_symbols*(off+(NR_NB_SC_PER_RB *
       ulsch_harq->ulsch_pdu.rb_size)),1,1); LOG_M("rxsigF0_llr.m","rxsF0_llr",
                &gNB->pusch_vars[0].llr[0],(ulsch_harq->ulsch_pdu.nr_of_symbols-1)*NR_NB_SC_PER_RB * ulsch_harq->ulsch_pdu.rb_size *
       ulsch_harq->ulsch_pdu.qam_mod_order,1,0); if (gNB->frame_parms.nb_antennas_rx > 1) {

            LOG_M("rxsigF1_ext.m","rxsF0_ext",
                   &gNB->pusch_vars[0].rxdataF_ext[1][ulsch_harq->ulsch_pdu.start_symbol_index*NR_NB_SC_PER_RB *
       ulsch_harq->ulsch_pdu.rb_size],ulsch_harq->ulsch_pdu.nr_of_symbols*(off+(NR_NB_SC_PER_RB *
       ulsch_harq->ulsch_pdu.rb_size)),1,1); LOG_M("chestF1.m","chF1",
                  &gNB->pusch_vars[0].ul_ch_estimates[1][ulsch_harq->ulsch_pdu.start_symbol_index*gNB->frame_parms.ofdm_symbol_size],gNB->frame_parms.ofdm_symbol_size,1,1);
            LOG_M("chestF1_ext.m","chF1_ext",
                  &gNB->pusch_vars[0].ul_ch_estimates_ext[1][(ulsch_harq->ulsch_pdu.start_symbol_index+1)*(off+(NR_NB_SC_PER_RB *
       ulsch_harq->ulsch_pdu.rb_size))], (ulsch_harq->ulsch_pdu.nr_of_symbols-1)*(off+(NR_NB_SC_PER_RB *
       ulsch_harq->ulsch_pdu.rb_size)),1,1); LOG_M("rxsigF1_comp.m","rxsF1_comp",
                  &gNB->pusch_vars[0].rxdataF_comp[1][ulsch_harq->ulsch_pdu.start_symbol_index*(off+(NR_NB_SC_PER_RB *
       ulsch_harq->ulsch_pdu.rb_size))],ulsch_harq->ulsch_pdu.nr_of_symbols*(off+(NR_NB_SC_PER_RB *
       ulsch_harq->ulsch_pdu.rb_size)),1,1);
          }
          exit(-1);

        }
#endif
    slsch->last_iteration_cnt = rdata->decodeIterations;
    VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME(VCD_SIGNAL_DUMPER_FUNCTIONS_PHY_gNB_ULSCH_DECODING,0);
  }
}

/* episys SL data-plane port: UE-native PSSCH (SLSCH) transport-block decode.
 * Mirrors the UE nr_dlsch_decoding orchestration for the sidelink shared channel: it builds the
 * COMMON LDPC decode parameters and calls the SHARED decoder (ue->nrLDPC_coding_interface) -- no
 * gNB coupling, no duplication of the gNB nr_ulsch_decoding. Scalar decode params are supplied by
 * the caller (nr_slsch_procedures) so this stays decoupled from the exact SCI/PSSCH PDU layout.
 * On success the decoded TB (harq->b) is delivered to MAC via develop's SL RX indication path.
 */
int nr_slsch_decoding(PHY_VARS_NR_UE *ue,
                      const UE_nr_rxtx_proc_t *proc,
                      NR_UL_gNB_HARQ_t *harq,
                      int16_t *llr,
                      int G,
                      int nb_rb,
                      uint8_t Qm,
                      uint8_t mcs,
                      uint8_t nb_layers,
                      uint32_t A,               // transport block size in bits
                      uint8_t rv_index,
                      uint32_t target_coderate, // x10240
                      uint32_t tbslbrm,
                      int harq_pid)
{
  nrLDPC_TB_decoding_parameters_t TB = {0};
  nrLDPC_slot_decoding_parameters_t slot = {
    .frame = proc->frame_rx,
    .slot = proc->nr_slot_rx,
    .nb_TBs = 1,
    .threadPool = &get_nrUE_params()->Tpool,
    .TBs = &TB
  };

  float Coderate = (float)target_coderate / 10240.0f;
  uint8_t BG = ((A <= 292) || ((A <= 3824) && (Coderate <= 0.6667f)) || (Coderate <= 0.25f)) ? 2 : 1;

  TB.harq_unique_pid = harq_pid;
  TB.G = G;
  TB.nb_rb = nb_rb;
  TB.Qm = Qm;
  TB.mcs = mcs;
  TB.nb_layers = nb_layers;
  TB.BG = BG;
  TB.A = A;
  TB.processedSegments = &harq->processedSegments;

  bool new_rx = harq->new_rx;
  if (new_rx) {
    nr_segmentation(NULL, NULL, lenWithCrc(1, A), &TB.C, &TB.K, &TB.Z, &TB.F, BG);
    harq->C = TB.C; harq->K = TB.K; harq->Z = TB.Z; harq->F = TB.F;
    harq->processedSegments = 0;
  } else {
    TB.C = harq->C; TB.K = harq->K; TB.Z = harq->Z; TB.F = harq->F;
  }

  TB.max_ldpc_iterations = 8;
  TB.rv_index = rv_index;
  TB.tbslbrm = tbslbrm;
  TB.abort_decode = &harq->abort_decode;
  set_abort(&harq->abort_decode, false);
  TB.llr = llr;
  TB.c = harq->c;
  TB.d = harq->d;
  TB.E = nr_get_E(TB.G, TB.C, TB.Qm, TB.nb_layers, 0);
  TB.E2 = TB.E;
  TB.first_rE2 = TB.C;
  TB.R = nr_get_R_ldpc_decoder(TB.rv_index, TB.E, TB.BG, TB.Z, &harq->llrLen, harq->round);
  TB.d_to_be_cleared = new_rx;
  for (int r = 0; r < TB.C; r++)
    TB.decodeSuccess[r] = false;

  int ret = ue->nrLDPC_coding_interface.nrLDPC_coding_decoder(&slot);
  if (ret != 0) {
    LOG_E(NR_PHY, "SLSCH nrLDPC_coding_decoder returned error %d\n", ret);
    return ret;
  }

  bool crcok = true;
  for (int r = 0; r < TB.C; r++)
    if (!TB.decodeSuccess[r]) { crcok = false; break; }

  if (crcok) {
    uint32_t offset = 0, r_offset = 0;
    for (int r = 0; r < TB.C; r++) {
      int cbyte = (harq->K >> 3) - (harq->F >> 3) - ((harq->C > 1) ? 3 : 0);
      memcpy(harq->b + offset, harq->c + r_offset, cbyte);
      offset += cbyte;
      r_offset += (harq->K >> 3);
    }
    harq->round = 0;
  } else {
    harq->round++;
  }
  harq->TBS = A >> 3;

  // Deliver decoded TB to MAC through develop's SL RX indication plumbing (SLSCH case fills from slsch_status).
  sl_nr_rx_indication_t rx_ind = {0};
  nr_sidelink_indication_t sl_ind;
  ldpcDecode_t rdata = {0};
  rdata.ulsch_harq = harq;
  rdata.harq_pid = harq_pid;
  slsch_status_t st = { .rdata = &rdata, .rxok = crcok };
  rx_ind.sfn = proc->frame_rx;
  rx_ind.slot = proc->nr_slot_rx;
  nr_fill_sl_rx_indication(&rx_ind, SL_NR_RX_PDU_TYPE_SLSCH, ue, 1, (void *)&st, 0);
  nr_fill_sl_indication(&sl_ind, &rx_ind, NULL, proc, ue, NULL);
  if (ue->if_inst && ue->if_inst->sl_indication)
    ue->if_inst->sl_indication(&sl_ind);

  return crcok ? 0 : -1;
}

// UE-native PSSCH RE extraction for one RX antenna (episys SL data-plane port). Packs the data
// REs of one OFDM symbol into rxF_ext and the matching (single-DMRS-symbol) channel estimates into
// ch_ext, contiguously. On a PSSCH DMRS symbol (config type 1, 1 CDM group) the DMRS occupies the
// even REs of each RB, so only the 6 odd REs/RB carry data. The channel estimate is always taken
// from the DMRS symbol (ch_est_offset), since a single front-loaded DMRS covers the whole PSSCH.
static int nr_sl_extract_rbs(int rxFsize,
                             c16_t rxdataF[][rxFsize],
                             int32_t **ul_ch_estimates,
                             int rx_symbol_offset,
                             int ch_est_offset,
                             c16_t *rxF_ext,
                             c16_t *ch_ext,
                             const NR_DL_FRAME_PARMS *fp,
                             int bwp_start_subcarrier,
                             int rb_size,
                             bool dmrs_symbol_flag,
                             int aarx)
{
  const c16_t *rxF = &rxdataF[aarx][rx_symbol_offset];
  const c16_t *ul_ch = (const c16_t *)&ul_ch_estimates[aarx][ch_est_offset];
  int j = 0;
  for (int re = 0; re < rb_size * NR_NB_SC_PER_RB; re++) {
    if (dmrs_symbol_flag && ((re & 1) == 0))
      continue; // type-1 PSSCH DMRS occupies the even REs -> not data
    const int k = (bwp_start_subcarrier + re) % fp->ofdm_symbol_size;
    rxF_ext[j] = rxF[k];      // raw rxdataF is at the absolute subcarrier (bwp_start_subcarrier + re)
    ch_ext[j] = ul_ch[re];    // channel estimate is stored allocation-relative (contiguous from 0)
    j++;
  }
  return j;
}

// UE-native sidelink PSSCH demodulator (episys SL data-plane port). Models develop's UE receiver
// nr_rx_pdsch and reuses the common (UE-linked) DSP (nr_scale_channel / nr_channel_level /
// nr_channel_compensation / nr_compute_llr) — NO gNB PHY (nr_rx_pusch / nr_pusch_channel_estimation)
// is linked, per the established UE-native principle. PSSCH is single-layer, so the MMSE/ML/PTRS/
// transform-precoding branches of nr_rx_pusch are not needed. Produces ue->pssch_vars[].llr_layers
// (per-layer SLSCH LLRs, SCI1/SCI2 REs punctured) which nr_slsch_procedures then layer-demaps,
// unscrambles and LDPC-decodes. It also fills ulsch_power/ulsch_noise_power for the caller's DTX test.
void nr_rx_pssch(PHY_VARS_NR_UE *ue,
                 const UE_nr_rxtx_proc_t *proc,
                 NR_DL_FRAME_PARMS *fp,
                 nr_phy_data_t *phy_data,
                 int rxFsize,
                 c16_t rxdataF[][rxFsize],
                 uint8_t slsch_id)
{
  sl_nr_rx_config_pssch_sci_pdu_t *pssch_pdu = &phy_data->nr_sl_pssch_sci_pdu;
  sl_nr_rx_config_pssch_pdu_t *slsch_pdu = &phy_data->nr_sl_pssch_pdu;
  NR_gNB_PUSCH *pssch_vars = &ue->pssch_vars[slsch_id];

  const int nbRx = fp->nb_antennas_rx;
  const int nl = 1; // sidelink PSSCH: single layer
  const int rb_size = pssch_pdu->num_subch * pssch_pdu->subchannel_size;
  const int rb_start = pssch_pdu->startrb;
  const uint8_t Qm = pssch_pdu->mod_order;
  const int start_symbol = 1; // symbol 0 = AGC/guard
  const int nr_of_symbols = pssch_pdu->pssch_numsym;
  const uint16_t Nid = pssch_pdu->Nid;
  const uint16_t dmrs_pos = pssch_pdu->dmrs_symbol_position;
  const int sci1_re_per_symb = pssch_pdu->pscch_numrbs * NR_NB_SC_PER_RB;
  const int bwp_start_subcarrier = (rb_start * NR_NB_SC_PER_RB + fp->first_carrier_offset) % fp->ofdm_symbol_size;
  const int buf_len = rb_size * NR_NB_SC_PER_RB;

  // 2nd-stage SCI RE budget (punctured from the SLSCH LLR stream; TS 38.212/38.213 9.3).
  int sci2_left = nr_sl_get_NREsci2(pssch_pdu->sci2_alpha_times_100,
                                    pssch_pdu->sci2_len,
                                    pssch_pdu->sci2_beta_offset,
                                    pssch_pdu->pssch_numsym,
                                    pssch_pdu->pscch_numsym,
                                    pssch_pdu->pscch_numrbs,
                                    pssch_pdu->l_subch,
                                    pssch_pdu->subchannel_size,
                                    pssch_pdu->targetCodeRate,
                                    slsch_pdu->mcs_table);

  // episys SL PSFCH port (4c-A): collect the SCI-2 (format 2A) QPSK LLRs across the PSSCH symbols so they
  // can be descrambled + polar-decoded after the LLR loop (-> mac->sci_pdu_rx, needed to trigger PSFCH).
  const int sci2_re_total = sci2_left;
  int16_t sci2_llrs[(sci2_re_total > 0 ? sci2_re_total * 2 : 1)];
  int sci2_llr_cnt = 0;

  // ---- 1) SL DMRS channel estimation (per DMRS symbol) + RX/noise power for the DTX test ----
  uint32_t nvar = 0, nvar_cnt = 0;
  int dmrs_symbol = -1;
  for (int aarx = 0; aarx < nbRx; aarx++) {
    pssch_vars->ulsch_power[aarx] = 0;
    pssch_vars->ulsch_noise_power[aarx] = 0;
  }
  for (int sym = start_symbol; sym < start_symbol + nr_of_symbols; sym++) {
    if (((dmrs_pos >> sym) & 1) == 0)
      continue;
    if (dmrs_symbol < 0)
      dmrs_symbol = sym;
    uint32_t nvar_tmp = 0;
    nr_pssch_channel_estimation(ue, proc, fp, Nid, rb_start, rb_size, sym,
                                pssch_vars->ul_ch_estimates, rxFsize, rxdataF, &nvar_tmp);
    nvar += nvar_tmp;
    nvar_cnt++;
    for (int aarx = 0; aarx < nbRx; aarx++)
      // NFAPI_NR_DMRS_TYPE1_linear_interp stores the estimate allocation-relative (contiguous from the symbol
      // base), NOT at bwp_start_subcarrier, so read from the symbol base here.
      pssch_vars->ulsch_power[aarx] +=
          signal_energy_nodc((c16_t *)&pssch_vars->ul_ch_estimates[aarx][sym * fp->ofdm_symbol_size], buf_len);
  }
  if (dmrs_symbol < 0) {
    LOG_E(NR_PHY, "%d.%d PSSCH: no DMRS symbol in mask %x\n", proc->frame_rx, proc->nr_slot_rx, dmrs_pos);
    return;
  }
  if (nvar_cnt)
    nvar /= nvar_cnt;
  for (int aarx = 0; aarx < nbRx; aarx++)
    pssch_vars->ulsch_noise_power[aarx] = nvar;
  const int ch_est_offset = dmrs_symbol * fp->ofdm_symbol_size;

  // ---- 2) per-symbol demod: extract -> scale -> level -> compensate -> LLR (SCI1/SCI2 punctured) ----
  int32_t log2_maxh = 0;
  bool cl_done = false;
  uint32_t llr_offset = 0;   // running SLSCH RE offset into llr_layers[0]
  uint64_t data_re_energy = 0; // total data-RE energy (rxF_ext) across symbols, feeds the DTX gate below
  for (int sym = start_symbol; sym < start_symbol + nr_of_symbols; sym++) {
    const bool dmrs_flag = (dmrs_pos >> sym) & 1;
    int nb_re = dmrs_flag ? rb_size * 6 : rb_size * NR_NB_SC_PER_RB; // type1 DMRS: 6 data REs/RB
    if (nb_re == 0)
      continue;

    __attribute__((aligned(32))) c16_t rxF_ext[nbRx][buf_len];
    __attribute__((aligned(32))) c16_t ch_ext[nl][nbRx][buf_len];
    memset(rxF_ext, 0, sizeof(rxF_ext));
    memset(ch_ext, 0, sizeof(ch_ext));

    for (int aarx = 0; aarx < nbRx; aarx++)
      nr_sl_extract_rbs(rxFsize, rxdataF, pssch_vars->ul_ch_estimates,
                        sym * fp->ofdm_symbol_size, ch_est_offset,
                        rxF_ext[aarx], ch_ext[0][aarx], fp, bwp_start_subcarrier,
                        rb_size, dmrs_flag, aarx);
    data_re_energy += signal_energy_nodc(rxF_ext[0], nb_re); // data-RE energy this symbol (DTX gate)

    nr_scale_channel(buf_len, (int(*)[buf_len])ch_ext, 0, nb_re, nl, nbRx, 0);

    if (!cl_done) {
      int32_t avg[nl * nbRx];
      nr_channel_level(0, buf_len, (const c16_t(*)[buf_len])ch_ext, nbRx, nl, avg, nb_re);
      int avgs = 0;
      for (int i = 0; i < nl * nbRx; i++)
        avgs = cmax(avgs, avg[i]);
      log2_maxh = (log2_approx(avgs) >> 1) + 1 + log2_approx(nbRx >> 1);
      if (log2_maxh < 0)
        log2_maxh = 0;
      cl_done = true;
    }

    __attribute__((aligned(32))) c16_t ch_maga[nl][buf_len];
    __attribute__((aligned(32))) c16_t ch_magb[nl][buf_len];
    __attribute__((aligned(32))) c16_t ch_magc[nl][buf_len];
    __attribute__((aligned(32))) c16_t rxComp_buf[nl][buf_len];
    memset(ch_maga, 0, sizeof(ch_maga));
    memset(ch_magb, 0, sizeof(ch_magb));
    memset(ch_magc, 0, sizeof(ch_magc));
    memset(rxComp_buf, 0, sizeof(rxComp_buf));
    c16_t *rxComp[nl];
    for (int l = 0; l < nl; l++)
      rxComp[l] = rxComp_buf[l];

    // develop added pdsch_buf_size_max (ch_mag buffer size) as the 2nd arg; here the ch_mag buffers are
    // [nl][buf_len], so it equals buf_len (same as buffer_length), matching develop's own callers.
    nr_channel_compensation(buf_len, buf_len, nbRx, nl, rxF_ext, ch_ext, ch_maga, ch_magb, ch_magc,
                            rxComp, NULL, Qm, 0, log2_maxh);

    // SCI region accounting: SCI1 occupies the first sci1_re_per_symb REs of the PSCCH symbols;
    // SCI2 REs are punctured from the start of the PSSCH data region until sci2_left is exhausted.
    // Puncturing rxComp and ch_mag together by the same offset keeps per-RE alignment for all QAM.
    int off = 0;
    if (sym <= pssch_pdu->pscch_numsym) {
      nb_re -= sci1_re_per_symb;
      off += sci1_re_per_symb;
    }
    if (sci2_left > 0) {
      const int take = (nb_re < sci2_left) ? nb_re : sci2_left;
      // episys SL PSFCH port (4c-A): collect the SCI-2 QPSK LLRs (channel-compensated) for decode after the
      // loop. These same REs are still punctured from the SLSCH LLR stream (off/nb_re advance) so the SLSCH
      // stays transmitter-aligned.
      nr_compute_llr(&rxComp[0][off], &ch_maga[0][off], &ch_magb[0][off], &ch_magc[0][off],
                     &sci2_llrs[sci2_llr_cnt], take, 0, 2 /*QPSK, 2 LLRs/RE*/);
      sci2_llr_cnt += take * 2;
      off += take;
      nb_re -= take;
      sci2_left -= take;
    }
    if (nb_re <= 0)
      continue;

    nr_compute_llr(&rxComp[0][off], &ch_maga[0][off], &ch_magb[0][off], &ch_magc[0][off],
                   &pssch_vars->llr_layers[0][llr_offset * Qm], nb_re, 0, Qm);
    llr_offset += nb_re;
  }
  pssch_vars->DTX = 0;

  // F1 blind-RX gate: without SCI-1 decode (deferred), the RX tries PSSCH on every SL RX slot. On slots that
  // carry DMRS/PSBCH energy but no PSSCH data (data REs empty), the all-zero TB would falsely pass CRC and
  // flood PDCP. Reject when the SLSCH data-RE energy is not meaningfully above noise -> mark DTX (no decode).
  const int n_data_sym = (nr_of_symbols > 0) ? nr_of_symbols : 1;
  const uint32_t avg_data_re = (uint32_t)(data_re_energy / n_data_sym);
  if (avg_data_re <= nvar) {
    pssch_vars->DTX = 1;
    for (int aarx = 0; aarx < nbRx; aarx++)
      pssch_vars->ulsch_power[aarx] = 0; // force the caller's DTX test to treat this as "not detected"
  }
  LOG_D(NR_PHY, "%d.%d PSSCH demod: %u SLSCH REs -> llr_layers[0] (Qm %d, rb %d, sym %d) data_re_e=%llu nvar=%u\n",
        proc->frame_rx, proc->nr_slot_rx, llr_offset, Qm, rb_size, nr_of_symbols, (unsigned long long)data_re_energy, nvar);

  // episys SL PSFCH port (4c-A): descramble + polar-decode the collected SCI-2 (format 2A) ONLY on a
  // detected PSSCH (gated by !DTX — running polar decode on every blind RX slot overruns real-time). On
  // CRC OK, deliver to MAC (-> mac->sci_pdu_rx: harq_feedback/source_id/cast_type) so the SLSCH rx_ind can
  // trigger the PSFCH HARQ feedback. Descramble MUST use the TX gold seq gold_cache((1010<<16)+Nid).
  if (!pssch_vars->DTX && sci2_re_total > 0 && sci2_llr_cnt == sci2_re_total * 2) {
    const int Gsci2 = sci2_re_total * 2;
    const int roundedSz = (Gsci2 + 31) / 32;
    uint32_t *seq = gold_cache((1010u << 16) + Nid, roundedSz);
    int16_t unscrambled_sci2[Gsci2];
    for (int j = 0; j < Gsci2; j++) {
      const int bit = (seq[j >> 5] >> (j & 31)) & 1;
      unscrambled_sci2[j] = bit ? -sci2_llrs[j] : sci2_llrs[j];
    }
    uint64_t sci_estimation[2] = {0};
    uint16_t crc = polar_decoder_int16(unscrambled_sci2, sci_estimation, 1, NR_POLAR_SCI2_MESSAGE_TYPE,
                                       pssch_pdu->sci2_len, sci2_re_total);
    if (crc == 0) {
      ue->SL_UE_PHY_PARAMS.pssch.rx_sci2_ok++;
      sl_nr_sci_indication_t sci_ind = {0};
      sci_ind.sfn = proc->frame_rx;
      sci_ind.slot = proc->nr_slot_rx;
      sci_ind.number_of_SCIs = 1;
      sci_ind.sci_pdu.sci_payloadlen = pssch_pdu->sci2_len;
      sci_ind.sci_pdu.Nid = Nid;
      memcpy(sci_ind.sci_pdu.sci_payloadBits, sci_estimation, sizeof(sci_ind.sci_pdu.sci_payloadBits)); // 8 bytes
      nr_sidelink_indication_t sl_indication;
      nr_fill_sl_indication(&sl_indication, NULL, &sci_ind, proc, ue, phy_data);
      if (ue->if_inst && ue->if_inst->sl_indication)
        ue->if_inst->sl_indication(&sl_indication);
      LOG_D(NR_PHY, "%d.%d SCI-2 decoded OK (len %d, %d REs)\n", proc->frame_rx, proc->nr_slot_rx,
            pssch_pdu->sci2_len, sci2_re_total);
    } else {
      ue->SL_UE_PHY_PARAMS.pssch.rx_sci2_errors++;
    }
  }
}

// SLSCH receive-decode entry (episys SL data-plane port). Assumes nr_rx_pssch already produced the
// per-layer LLRs in ue->pssch_vars[slsch_id].llr_layers. Computes the coded-bit count G, layer-demaps
// and unscrambles into the codeword LLR buffer, then LDPC-decodes via the UE-native nr_slsch_decoding
// (common LDPC, no gNB coupling). Returns 0 on CRC pass.
int nr_slsch_procedures(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc, nr_phy_data_t *phy_data, uint8_t slsch_id)
{
  sl_nr_ue_phy_params_t *sl_phy_params = &ue->SL_UE_PHY_PARAMS;
  sl_nr_rx_config_pssch_pdu_t *slsch_pdu = &phy_data->nr_sl_pssch_pdu;
  sl_nr_rx_config_pssch_sci_pdu_t *pssch_pdu = &phy_data->nr_sl_pssch_sci_pdu;
  NR_gNB_PUSCH *pssch_vars = &ue->pssch_vars[slsch_id];
  NR_UL_gNB_HARQ_t *harq = ue->slsch[slsch_id].harq_process;

  const int harq_pid = slsch_pdu->harq_pid;
  const uint16_t start_symbol = 1;
  const uint16_t number_symbols = pssch_pdu->pssch_numsym;
  uint8_t number_dmrs_symbols = 0;
  for (int l = start_symbol; l < start_symbol + number_symbols; l++)
    number_dmrs_symbols += (pssch_pdu->dmrs_symbol_position >> l) & 0x01;
  const uint16_t nb_re_dmrs = 6; // PSSCH DMRS type1, 1 CDM group

  const uint32_t rb_size = pssch_pdu->num_subch * pssch_pdu->subchannel_size;
  const int sci1_dmrs_overlap = pssch_pdu->dmrs_symbol_position & sl_dmrs_pscch_mask[pssch_pdu->pscch_numsym - 2];
  const int sci2_re = nr_sl_get_NREsci2(pssch_pdu->sci2_alpha_times_100,
                                        pssch_pdu->sci2_len,
                                        pssch_pdu->sci2_beta_offset,
                                        pssch_pdu->pssch_numsym,
                                        pssch_pdu->pscch_numsym,
                                        pssch_pdu->pscch_numrbs,
                                        pssch_pdu->l_subch,
                                        pssch_pdu->subchannel_size,
                                        pssch_pdu->targetCodeRate,
                                        slsch_pdu->mcs_table);
  const uint16_t sci1_re = pssch_pdu->pscch_numsym * pssch_pdu->pscch_numrbs * NR_NB_SC_PER_RB;
  uint32_t G = nr_sl_get_G(rb_size,
                           number_symbols,
                           nb_re_dmrs,
                           number_dmrs_symbols,
                           sci1_dmrs_overlap,
                           sci1_re,
                           pssch_pdu->pscch_numrbs,
                           sci2_re,
                           0 /* CSI-RS REs: F3, deferred */,
                           pssch_pdu->mod_order,
                           pssch_pdu->num_layers);
  AssertFatal(G > 0, "SLSCH G is 0: rb_size %u nsym %d ndmrs %d Qm %d Nl %d\n",
              rb_size, number_symbols, number_dmrs_symbols, pssch_pdu->mod_order, pssch_pdu->num_layers);

  // per-layer LLRs (from nr_rx_pssch) -> codeword LLRs -> unscramble (Nid, n_RNTI = 1010)
  nr_sl_layer_demapping(pssch_vars->llr, pssch_pdu->num_layers, pssch_pdu->mod_order, G, pssch_vars->llr_layers);
  nr_sl_unscrambling(pssch_vars->llr, G, pssch_pdu->Nid, 1010);

  // Sidelink SL-DRB is UM (no HARQ soft-combining) -> every reception is a fresh decode.
  harq->new_rx = true;
  harq->round = 0;
  const uint32_t A = (uint32_t)slsch_pdu->tb_size << 3;
  int ret = nr_slsch_decoding(ue,
                              proc,
                              harq,
                              pssch_vars->llr,
                              G,
                              rb_size,
                              pssch_pdu->mod_order,
                              slsch_pdu->mcs,
                              pssch_pdu->num_layers,
                              A,
                              slsch_pdu->rv_index,
                              slsch_pdu->target_coderate,
                              slsch_pdu->tbslbrm >> 3, // bytes, matching the common decoder convention
                              harq_pid);
  if (ret == 0)
    sl_phy_params->pssch.rx_ok++;
  else
    sl_phy_params->pssch.rx_errors[0]++;
  return ret;
}