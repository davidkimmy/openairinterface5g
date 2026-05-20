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
#include "NR_IF_Module.h"
#include "openair1/SCHED_NR_UE/defs.h"
#include "PHY/NR_UE_TRANSPORT/nr_transport_proto_ue.h"
#include "common/utils/LOG/vcd_signal_dumper.h"

void nr_fill_sl_indication(nr_sidelink_indication_t *sl_ind,
                           sl_nr_rx_indication_t *rx_ind,
                           sl_nr_sci_indication_t *sci_ind,
                           UE_nr_rxtx_proc_t *proc,
                           PHY_VARS_NR_UE *ue,
                           void *phy_data)
{
  memset((void*)sl_ind, 0, sizeof(nr_sidelink_indication_t));

  sl_ind->gNB_index = proc->gNB_id;
  sl_ind->module_id = ue->Mod_id;
  sl_ind->cc_id     = ue->CC_id;
  sl_ind->frame_rx  = proc->frame_rx;
  sl_ind->slot_rx   = proc->nr_slot_rx;
  sl_ind->frame_tx  = proc->frame_tx;
  sl_ind->slot_tx   = proc->nr_slot_tx;
  sl_ind->phy_data  = phy_data;
  sl_ind->slot_type = SIDELINK_SLOT_TYPE_RX;

  if (rx_ind) {
    sl_ind->rx_ind = rx_ind; //  hang on rx_ind instance
    sl_ind->sci_ind = NULL;
  }
  if (sci_ind) {
    sl_ind->rx_ind = NULL;
    sl_ind->sci_ind = sci_ind;
  }
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
      s = lte_gold_generic(&x1, &x2, reset);
      reset = 0;
    }

    if (((s >> (i % 32)) & 1) == 1)
      z2[i] = -e_rx[i];
    else
      z2[i]=e_rx[i];
  }
}

void nr_fill_sl_rx_indication(sl_nr_rx_indication_t *rx_ind,
                              uint8_t pdu_type,
                              PHY_VARS_NR_UE *ue,
                              uint16_t n_pdus,
                              UE_nr_rxtx_proc_t *proc,
                              void *typeSpecific,
                              uint16_t rx_slss_id)
{

  if (n_pdus > 1){
    LOG_E(PHY, "In %s: multiple number of SL PDUs not supported yet...\n", __FUNCTION__);
  }

  sl_nr_ue_phy_params_t *sl_phy_params = &ue->SL_UE_PHY_PARAMS;

  switch (pdu_type){
    case SL_NR_RX_PDU_TYPE_SLSCH:
    case SL_NR_RX_PDU_TYPE_SLSCH_PSFCH: {
        sl_nr_slsch_pdu_t *rx_slsch_pdu = &rx_ind->rx_indication_body[n_pdus - 1].rx_slsch_pdu;
        slsch_status_t *slsch_status = (slsch_status_t *)typeSpecific;
        rx_slsch_pdu->pdu        = slsch_status->rdata->ulsch_harq->b;
        rx_slsch_pdu->pdu_length = slsch_status->rdata->ulsch_harq->TBS;
        rx_slsch_pdu->harq_pid   = slsch_status->rdata->harq_pid;
        rx_slsch_pdu->ack_nack   = (slsch_status->rxok==true) ? 1 : 0;

        LOG_D(NR_MAC, "%4d.%2d Received %s SLSCH\n", rx_ind->sfn, rx_ind->slot, rx_slsch_pdu->ack_nack ? "Correct" : "Incorrect");
        if (slsch_status->rxok==true) ue->SL_UE_PHY_PARAMS.pssch.rx_ok++;
        else                          ue->SL_UE_PHY_PARAMS.pssch.rx_errors[0]++;
      }
      break;
    case FAPI_NR_RX_PDU_TYPE_SSB: {
        sl_nr_ssb_pdu_t *ssb_pdu = &rx_ind->rx_indication_body[n_pdus - 1].ssb_pdu;
        if(typeSpecific) {
          uint8_t *psbch_decoded_output = (uint8_t *)typeSpecific;
          memcpy(ssb_pdu->psbch_payload, psbch_decoded_output, sizeof(4));//4 bytes of PSBCH payload bytes
          ssb_pdu->rsrp_dbm = sl_phy_params->psbch.rsrp_dBm_per_RE;
          ssb_pdu->rx_slss_id = rx_slss_id;
          ssb_pdu->decode_status = true;
          LOG_D(PHY, "SL-IND: SSB to MAC. rsrp:%d, slssid:%d, payload:%x\n",
                                                    ssb_pdu->rsrp_dbm,ssb_pdu->rx_slss_id,
                                                    *((uint32_t *)(ssb_pdu->psbch_payload)) );
        }
        else
          ssb_pdu->decode_status = false;
      }
      break;
    default:
      break;
  }

  rx_ind->rx_indication_body[n_pdus -1].pdu_type = pdu_type;
  rx_ind->number_pdus = n_pdus;

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
    sl_rx_indication.rx_indication_body[0].rx_slsch_pdu.ack_nack_rcvd = calloc(num_acks, sizeof(uint8_t));
    memcpy((void*)sl_rx_indication.rx_indication_body[0].rx_slsch_pdu.ack_nack_rcvd, (void*)ack_nack_rcvd,
          num_acks * sizeof(uint8_t));
    sl_rx_indication.rx_indication_body[0].rx_slsch_pdu.num_acks_rcvd = num_acks;
    uint8_t pdu_type = phy_data->sl_rx_action == SL_NR_CONFIG_TYPE_RX_PSSCH_SLSCH_PSFCH ? SL_NR_RX_PDU_TYPE_SLSCH_PSFCH : SL_NR_RX_PDU_TYPE_SLSCH;
    nr_fill_sl_rx_indication(&sl_rx_indication, pdu_type, UE, 1, proc, (void*)&slsch_status, 0);
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