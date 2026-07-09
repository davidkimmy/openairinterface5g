/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#define _GNU_SOURCE

#include "PHY/defs_nr_UE.h"
#include "PHY/nr_phy_common/inc/nr_sl_decode_defs.h"  // episys SL port: shared LDPC decode struct for SLSCH RX fill
#include <openair1/PHY/TOOLS/phy_scope_interface.h>
#include "common/utils/LOG/log.h"
#include "UTIL/OPT/opt.h"
#include "intertask_interface.h"
#include "T.h"
#include "PHY/MODULATION/modulation_UE.h"
#include "PHY/NR_UE_ESTIMATION/nr_estimation.h"
#include "PHY/NR_UE_TRANSPORT/nr_transport_proto_ue.h"
#include "PHY/CODING/nrPolar_tools/nr_polar_psbch_defs.h"
#include "openair1/PHY/nr_phy_common/inc/nr_phy_common.h"

void nr_fill_sl_indication(nr_sidelink_indication_t *sl_ind,
                           sl_nr_rx_indication_t *rx_ind,
                           sl_nr_sci_indication_t *sci_ind,
                           const UE_nr_rxtx_proc_t *proc,
                           PHY_VARS_NR_UE *ue,
                           void *phy_data)
{
  memset((void *)sl_ind, 0, sizeof(nr_sidelink_indication_t));

  sl_ind->gNB_index = proc->gNB_id;
  sl_ind->module_id = ue->Mod_id;
  sl_ind->cc_id = ue->CC_id;
  sl_ind->hfn_rx = proc->hfn_rx;
  sl_ind->frame_rx = proc->frame_rx;
  sl_ind->slot_rx = proc->nr_slot_rx;
  sl_ind->hfn_tx = proc->hfn_tx;
  sl_ind->frame_tx = proc->frame_tx;
  sl_ind->slot_tx = proc->nr_slot_tx;
  sl_ind->phy_data = phy_data;
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

void nr_fill_sl_rx_indication(sl_nr_rx_indication_t *rx_ind,
                              uint8_t pdu_type,
                              PHY_VARS_NR_UE *ue,
                              uint16_t n_pdus,
                              void *typeSpecific,
                              uint16_t rx_slss_id)
{
  if (n_pdus > 1) {
    LOG_E(NR_PHY, "In %s: multiple number of SL PDUs not supported yet...\n", __FUNCTION__);
  }

  sl_nr_ue_phy_params_t *sl_phy_params = &ue->SL_UE_PHY_PARAMS;

  switch (pdu_type) {
    case SL_NR_RX_PDU_TYPE_SLSCH:
    case SL_NR_RX_PDU_TYPE_SLSCH_PSFCH: {
      // episys SL data-plane port: deliver a decoded SLSCH (PSSCH) transport block up to MAC.
      sl_nr_slsch_pdu_t *rx_slsch_pdu = &rx_ind->rx_indication_body[n_pdus - 1].rx_slsch_pdu;
      slsch_status_t *slsch_status = (slsch_status_t *)typeSpecific;
      rx_slsch_pdu->pdu        = slsch_status->rdata->ulsch_harq->b;
      rx_slsch_pdu->pdu_length = slsch_status->rdata->ulsch_harq->TBS;
      rx_slsch_pdu->harq_pid   = slsch_status->rdata->harq_pid;
      rx_slsch_pdu->ack_nack   = (slsch_status->rxok == true) ? 1 : 0;
      LOG_D(NR_MAC, "%4d.%2d Received %s SLSCH\n", rx_ind->sfn, rx_ind->slot, rx_slsch_pdu->ack_nack ? "Correct" : "Incorrect");
      if (slsch_status->rxok == true) sl_phy_params->pssch.rx_ok++;
      else                            sl_phy_params->pssch.rx_errors[0]++;
    } break;
    case FAPI_NR_RX_PDU_TYPE_SSB: {
      sl_nr_ssb_pdu_t *ssb_pdu = &rx_ind->rx_indication_body[n_pdus - 1].ssb_pdu;
      if (typeSpecific) {
        uint8_t *psbch_decoded_output = (uint8_t *)typeSpecific;
        memcpy(ssb_pdu->psbch_payload, psbch_decoded_output, 4); // 4 bytes of PSBCH payload bytes
        ssb_pdu->rsrp_dbm = sl_phy_params->psbch.rsrp_dBm_per_RE;
        ssb_pdu->rx_slss_id = rx_slss_id;
        ssb_pdu->decode_status = true;
        LOG_D(NR_PHY,
              "SL-IND: SSB to MAC. rsrp:%d, slssid:%d, payload:%x\n",
              ssb_pdu->rsrp_dbm,
              ssb_pdu->rx_slss_id,
              *((uint32_t *)(ssb_pdu->psbch_payload)));
      } else
        ssb_pdu->decode_status = false;
    } break;
    default:
      break;
  }

  rx_ind->rx_indication_body[n_pdus - 1].pdu_type = pdu_type;
  rx_ind->number_pdus = n_pdus;
}

static void nr_psbch_symbol_process(PHY_VARS_NR_UE *ue,
                                    const UE_nr_rxtx_proc_t *proc,
                                    const int symbol,
                                    const c16_t rxdataF[][ue->frame_parms.ofdm_symbol_size],
                                    int *psbch_e_rx_offset,
                                    int16_t psbch_e_rx[SL_NR_POLAR_PSBCH_E_NORMAL_CP + 2],
                                    int16_t psbch_unClipped[SL_NR_POLAR_PSBCH_E_NORMAL_CP + 2],
                                    c16_t dl_ch_estimates_time[ue->frame_parms.nb_antennas_rx][ue->frame_parms.ofdm_symbol_size])
{
  sl_nr_ue_phy_params_t *sl_phy_params = &ue->SL_UE_PHY_PARAMS;
  NR_DL_FRAME_PARMS *fp = &sl_phy_params->sl_frame_params;
  int slss_id = sl_phy_params->sl_config.sl_sync_source.rx_slss_id;

  __attribute__((aligned(32))) c16_t dl_ch_estimates[fp->nb_antennas_rx][fp->ofdm_symbol_size];
  start_meas(&sl_phy_params->channel_estimation_stats);
  for (int aarx = 0; aarx < fp->nb_antennas_rx; aarx++) {
    nr_pbch_channel_estimation(fp,
                               &ue->SL_UE_PHY_PARAMS,
                               dl_ch_estimates[aarx],
                               proc,
                               symbol,
                               0,
                               0,
                               fp->ssb_start_subcarrier,
                               rxdataF[aarx],
                               true,
                               slss_id);
    if (symbol == 12) {
      freq2time(ue->frame_parms.ofdm_symbol_size, (int16_t *)&dl_ch_estimates[aarx], (int16_t *)&dl_ch_estimates_time[aarx]);
    }
  }
  stop_meas(&sl_phy_params->channel_estimation_stats);

  if (symbol == 12)
    UEscopeCopy(ue,
                psbchDlChEstimateTime,
                (void *)dl_ch_estimates_time,
                sizeof(c16_t),
                fp->nb_antennas_rx,
                fp->ofdm_symbol_size,
                0);

  nr_generate_psbch_llr(fp, rxdataF, dl_ch_estimates, symbol, psbch_e_rx_offset, psbch_e_rx, psbch_unClipped);

  ue->adjust_rxgain = nr_sl_psbch_rsrp_measurements(ue, sl_phy_params, fp, symbol, rxdataF, false);
}

static unsigned int get_psbch_symbol_bitmap(const int num_symbols)
{
  unsigned int b = 0;
  for (int s = 0; s < num_symbols;) {
    b |= (0x1 << s);
    s = (s == 0) ? 5 : s + 1;
  }
  return b;
}

static bool is_psbch_symbol(const unsigned bitmap, const unsigned int symbol)
{
  return ((bitmap >> symbol) == 1);
}

static int nr_psbch_process(PHY_VARS_NR_UE *ue,
                            nr_phy_data_t *phy_data,
                            const UE_nr_rxtx_proc_t *proc,
                            const int symbol,
                            const c16_t rxdataF[][ue->frame_parms.ofdm_symbol_size],
                            int *psbch_e_rx_offset,
                            int16_t psbch_e_rx[SL_NR_POLAR_PSBCH_E_NORMAL_CP + 2],
                            int16_t psbch_unClipped[SL_NR_POLAR_PSBCH_E_NORMAL_CP + 2],
                            c16_t dl_ch_estimates_time[ue->frame_parms.nb_antennas_rx][ue->frame_parms.ofdm_symbol_size])
{
  sl_nr_ue_phy_params_t *sl_phy_params = &ue->SL_UE_PHY_PARAMS;
  NR_DL_FRAME_PARMS *fp = &sl_phy_params->sl_frame_params;
  const unsigned int numsymb = (fp->Ncp) ? SL_NR_NUM_SYMBOLS_SSB_EXT_CP : SL_NR_NUM_SYMBOLS_SSB_NORMAL_CP;
  const unsigned int symbol_bitmap = get_psbch_symbol_bitmap(numsymb);
  if (!is_psbch_symbol(symbol_bitmap, symbol)) {
    return 0;
  }

  const unsigned int last_symbol = numsymb - 1;
  int sampleShift = 0;

  nr_psbch_symbol_process(ue, proc, symbol, rxdataF, psbch_e_rx_offset, psbch_e_rx, psbch_unClipped, dl_ch_estimates_time);

  if (symbol == last_symbol) {
    const int slss_id = sl_phy_params->sl_config.sl_sync_source.rx_slss_id;
    uint8_t decoded_pdu[4] = {0};
    const int psbchSuccess = nr_psbch_decode(ue, psbch_e_rx, proc, *psbch_e_rx_offset, slss_id, phy_data, decoded_pdu);

    /* SV: Is this needed? */
    if (ue->no_timing_correction == 0 && psbchSuccess == 0) {
      LOG_D(NR_PHY, "start adjust sync slot = %d no timing %d\n", proc->nr_slot_rx, ue->no_timing_correction);
      sampleShift = nr_adjust_synch_ue(fp, ue, dl_ch_estimates_time, proc->frame_rx, proc->nr_slot_rx, 16384);
    }
  }

  UEscopeCopy(ue, psbchRxdataF_comp, psbch_unClipped, sizeof(c16_t), fp->nb_antennas_rx, *psbch_e_rx_offset / 2, 0);
  UEscopeCopy(ue, psbchLlr, psbch_e_rx, sizeof(int16_t), fp->nb_antennas_rx, *psbch_e_rx_offset, 0);

  return sampleShift;
}

int psbch_pscch_processing(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc, nr_phy_data_t *phy_data)
{
  int frame_rx = proc->frame_rx;
  int nr_slot_rx = proc->nr_slot_rx;
  sl_nr_ue_phy_params_t *sl_phy_params = &ue->SL_UE_PHY_PARAMS;
  NR_DL_FRAME_PARMS *fp = &sl_phy_params->sl_frame_params;
  int sampleShift = INT_MAX;

  start_meas(&sl_phy_params->phy_proc_sl_rx);

  LOG_D(NR_PHY, " ****** Sidelink RX-Chain for Frame.Slot %d.%d ******  \n", frame_rx % 1024, nr_slot_rx);

  const uint32_t rxdataF_sz = fp->samples_per_slot_wCP;
  __attribute__((aligned(32))) c16_t rxdataF[fp->nb_antennas_rx][rxdataF_sz];

  // Dual-card relay (mode-1): the PC5 device fills rxdata_sl; single-card (mode-2) SL reuses rxdata.
  c16_t **sl_rxdata = ue->sl_dual_card ? ue->common_vars.rxdata_sl : ue->common_vars.rxdata;

  if (phy_data->sl_rx_action == SL_NR_CONFIG_TYPE_RX_PSBCH) {
    LOG_D(NR_PHY, " ----- PSBCH RX TTI: frame.slot %d.%d ------  \n", frame_rx % 1024, nr_slot_rx);

    __attribute__((aligned(32))) struct complex16 dl_ch_estimates_time[fp->nb_antennas_rx][fp->ofdm_symbol_size];

    int16_t psbch_e_rx[SL_NR_POLAR_PSBCH_E_NORMAL_CP + 2] = {0};
    int16_t psbch_unClippled[SL_NR_POLAR_PSBCH_E_NORMAL_CP + 2] = {0};
    int e_rx_offset = 0;
    /* TODO: Remove loop over symbols in later commit. */
    for (int sym = 0; sym < NR_SYMBOLS_PER_SLOT; sym++) {
      nr_slot_fep(ue, fp, proc->nr_slot_rx, sym, rxdataF, link_type_sl, 0, sl_rxdata);
      __attribute__((aligned(32))) c16_t rxdataF_symb[fp->nb_antennas_rx][fp->ofdm_symbol_size];
      for (int aarx = 0; aarx < fp->nb_antennas_rx; aarx++) {
        /* TODO: Remove this buffer reshaping in later commit after rxdataF is in right format */
        memcpy(rxdataF_symb[aarx], &rxdataF[aarx][sym * fp->ofdm_symbol_size], sizeof(c16_t) * fp->ofdm_symbol_size);
      }
      sampleShift =
          nr_psbch_process(ue, phy_data, proc, sym, rxdataF_symb, &e_rx_offset, psbch_e_rx, psbch_unClippled, dl_ch_estimates_time);
    }

    if (frame_rx % 64 == 0) {
      LOG_I(NR_PHY, "============================================\n");

      LOG_I(NR_PHY,
            "[UE%d] %d:%d PSBCH Stats: TX %d, RX ok %d, RX not ok %d\n",
            ue->Mod_id,
            frame_rx,
            nr_slot_rx,
            sl_phy_params->psbch.num_psbch_tx,
            sl_phy_params->psbch.rx_ok,
            sl_phy_params->psbch.rx_errors);

      // PC5 PHY TX/RX stats (episys port). PSCCH RX-ok is 0 for now (SCI-1 decode deferred: blind PSSCH
      // RX); PSFCH TX is 0 (PSFCH TX not wired on this branch). PSSCH/SCI2 counters are live.
      LOG_I(NR_PHY,
            "[UE%d] %d:%d PSCCH Stats: TX %u, RX ok %u\n",
            ue->Mod_id, frame_rx, nr_slot_rx,
            sl_phy_params->pscch.num_pscch_tx,
            sl_phy_params->pscch.rx_ok);

      LOG_I(NR_PHY,
            "[UE%d] %d:%d PSSCH/SCI2 Stats: TX %u, RX ok %u, RX not ok %u\n",
            ue->Mod_id, frame_rx, nr_slot_rx,
            sl_phy_params->pssch.num_pssch_sci2_tx,
            sl_phy_params->pssch.rx_sci2_ok,
            sl_phy_params->pssch.rx_sci2_errors);

      LOG_I(NR_PHY,
            "[UE%d] %d:%d PSSCH Stats: TX %u, RX ok %u, RX not ok (%u/%u/%u/%u)\n",
            ue->Mod_id, frame_rx, nr_slot_rx,
            sl_phy_params->pssch.num_pssch_tx,
            sl_phy_params->pssch.rx_ok,
            sl_phy_params->pssch.rx_errors[0],
            sl_phy_params->pssch.rx_errors[1],
            sl_phy_params->pssch.rx_errors[2],
            sl_phy_params->pssch.rx_errors[3]);

      LOG_I(NR_PHY,
            "[UE%d] %d:%d PSFCH Stats: TX %u, RX %u\n",
            ue->Mod_id, frame_rx, nr_slot_rx,
            sl_phy_params->psfch.num_psfch_tx,
            sl_phy_params->psfch.num_psfch_rx);

      LOG_I(NR_PHY, "============================================\n");
    }
  }
  // episys SL data-plane port: PSSCH (SLSCH) receive. develop's SL was sync-only; this is the data plane.
  else if (phy_data->sl_rx_action == SL_NR_CONFIG_TYPE_RX_PSSCH_SCI
           || phy_data->sl_rx_action == SL_NR_CONFIG_TYPE_RX_PSSCH_SLSCH
           || phy_data->sl_rx_action == SL_NR_CONFIG_TYPE_RX_PSSCH_SLSCH_PSFCH) {
    sl_nr_rx_config_pssch_sci_pdu_t *pssch_pdu = &phy_data->nr_sl_pssch_sci_pdu;
    LOG_D(NR_PHY, " ----- PSSCH RX TTI: frame.slot %d.%d pssch_numsym %d ------\n",
          frame_rx % 1024, nr_slot_rx, pssch_pdu->pssch_numsym);

    NR_gNB_PUSCH *pssch_vars = ue->pssch_vars;
    // A PSFCH-only feedback slot carries no valid PSSCH config (pssch_numsym 0, or garbage). Only run the
    // PSSCH demod when the config is valid; otherwise skip it (DTX) — the PSFCH decode below runs regardless.
    bool valid_pssch = pssch_pdu->pssch_numsym >= 1 && pssch_pdu->pssch_numsym <= NR_SYMBOLS_PER_SLOT - 1
                       && pssch_pdu->sci2_beta_offset < 19;
    if (!valid_pssch) {
      pssch_vars->DTX = 1;
    } else {
      // OFDM front-end for the PSSCH symbols (symbol 0 is AGC/guard).
      for (int sym = 1; sym <= pssch_pdu->pssch_numsym; sym++)
        nr_slot_fep(ue, fp, proc->nr_slot_rx, sym, rxdataF, link_type_sl, 0, sl_rxdata);

      // UE-native PSSCH demod -> ue->pssch_vars[0].llr_layers + per-antenna RX/noise power.
      nr_rx_pssch(ue, proc, fp, phy_data, rxdataF_sz, rxdataF, 0);

      pssch_vars->ulsch_power_tot = 0;
      pssch_vars->ulsch_noise_power_tot = 0;
      for (int aarx = 0; aarx < fp->nb_antennas_rx; aarx++) {
        pssch_vars->ulsch_power_tot += pssch_vars->ulsch_power[aarx];
        pssch_vars->ulsch_noise_power_tot += pssch_vars->ulsch_noise_power[aarx];
      }
      bool detected = dB_fixed_x10(pssch_vars->ulsch_power_tot)
                      >= dB_fixed_x10(pssch_vars->ulsch_noise_power_tot) + ue->pssch_thres;
      if (!detected) {
        pssch_vars->DTX = 1;
        LOG_D(NR_PHY, "%d.%d PSSCH not detected (pwr %d < noise %d + thr %d)\n", frame_rx, nr_slot_rx,
              dB_fixed_x10(pssch_vars->ulsch_power_tot), dB_fixed_x10(pssch_vars->ulsch_noise_power_tot), ue->pssch_thres);
      } else {
        pssch_vars->DTX = 0;
        int ret = nr_slsch_procedures(ue, proc, phy_data, 0);
        LOG_D(NR_PHY, "%d.%d PSSCH SLSCH decode returned %d (pwr %d noise %d)\n", frame_rx, nr_slot_rx, ret,
              dB_fixed_x10(pssch_vars->ulsch_power_tot), dB_fixed_x10(pssch_vars->ulsch_noise_power_tot));
      }
    }

    // episys SL PSFCH port (Stage 2 PHY RX): decode HARQ ACK/NACK feedback on this slot's PSFCH resources.
    // phy_data->psfch_pdu_list is populated by the MAC HARQ scheduler (Stages 3-4); until then
    // num_psfch_pdus is 0 so this loop is dormant. The decoded ack_nack_rcvd[] is consumed at Stage 3.
    if (phy_data->sl_rx_action == SL_NR_CONFIG_TYPE_RX_PSSCH_SLSCH_PSFCH && phy_data->num_psfch_pdus) {
      int8_t *ack_nack_rcvd = calloc(phy_data->num_psfch_pdus, sizeof(*ack_nack_rcvd));
      LOG_D(NR_PHY, "%d.%d PSFCH RX: num_psfch_pdus %d\n", frame_rx, nr_slot_rx, phy_data->num_psfch_pdus);
      for (int k = 0; k < phy_data->num_psfch_pdus; k++) {
        sl_nr_tx_rx_config_psfch_pdu_t *psfch_pdu = &phy_data->psfch_pdu_list[k];
        // OFDM front-end for the PSFCH symbol(s) (PUCCH-format-0 layout). Guard the symbol index — a bad
        // start_symbol_index/nr_of_symbols must not drive nr_slot_fep out of the slot (asserts otherwise).
        if (psfch_pdu->start_symbol_index + psfch_pdu->nr_of_symbols > NR_SYMBOLS_PER_SLOT
            || psfch_pdu->nr_of_symbols == 0) {
          LOG_W(NR_PHY, "%d.%d PSFCH RX skip: bad start_sym %d + nsym %d (>%d)\n", frame_rx, nr_slot_rx,
                psfch_pdu->start_symbol_index, psfch_pdu->nr_of_symbols, NR_SYMBOLS_PER_SLOT);
          ack_nack_rcvd[k] = 1; // treat as NACK (can't decode)
          continue;
        }
        for (int sym = psfch_pdu->start_symbol_index; sym < psfch_pdu->start_symbol_index + psfch_pdu->nr_of_symbols; sym++)
          nr_slot_fep(ue, fp, proc->nr_slot_rx, sym, rxdataF, link_type_sl, 0, sl_rxdata);
        ack_nack_rcvd[k] = nr_ue_decode_psfch0(ue, frame_rx, nr_slot_rx, rxdataF, psfch_pdu);
        sl_phy_params->psfch.num_psfch_rx++;
        LOG_D(NR_PHY, "%d.%d PSFCH[%d] HARQ %s\n", frame_rx, nr_slot_rx, k,
              ack_nack_rcvd[k] == 0 ? "ACK" : "NACK");
      }
      // episys SL PSFCH port (Stage 4d): deliver the decoded ACK/NACK to the MAC (handle_nr_ue_sl_harq)
      // via an SLSCH_PSFCH rx indication. sl_indication is synchronous, so free after it returns.
      sl_nr_rx_indication_t rx_ind = {0};
      rx_ind.sfn = frame_rx;
      rx_ind.slot = nr_slot_rx;
      rx_ind.number_pdus = 1;
      rx_ind.rx_indication_body[0].pdu_type = SL_NR_RX_PDU_TYPE_SLSCH_PSFCH;
      rx_ind.rx_indication_body[0].rx_slsch_pdu.ack_nack_rcvd = (uint8_t *)ack_nack_rcvd;
      rx_ind.rx_indication_body[0].rx_slsch_pdu.num_acks_rcvd = phy_data->num_psfch_pdus;
      nr_sidelink_indication_t sl_indication;
      nr_fill_sl_indication(&sl_indication, &rx_ind, NULL, proc, ue, phy_data);
      if (ue->if_inst && ue->if_inst->sl_indication)
        ue->if_inst->sl_indication(&sl_indication);
      free(ack_nack_rcvd);
      free(phy_data->psfch_pdu_list);
      phy_data->psfch_pdu_list = NULL;
      phy_data->num_psfch_pdus = 0;
    }
  }
  return sampleShift;
}

void phy_procedures_nrUE_SL_TX(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc, nr_phy_data_tx_t *phy_data, c16_t **txp)
{
  int slot_tx = proc->nr_slot_tx;
  int frame_tx = proc->frame_tx;
  int tx_action = 0;

  sl_nr_ue_phy_params_t *sl_phy_params = &ue->SL_UE_PHY_PARAMS;
  NR_DL_FRAME_PARMS *fp = &sl_phy_params->sl_frame_params;

  const int samplesF_per_slot = NR_SYMBOLS_PER_SLOT * fp->ofdm_symbol_size;
  c16_t txdataF_buf[fp->nb_antennas_tx * samplesF_per_slot] __attribute__((aligned(32)));
  memset(txdataF_buf, 0, sizeof(txdataF_buf));
  c16_t *txdataF[fp->nb_antennas_tx]; /* workaround to be compatible with current txdataF usage in all tx procedures. */
  for (int i = 0; i < fp->nb_antennas_tx; ++i)
    txdataF[i] = &txdataF_buf[i * samplesF_per_slot];

  LOG_D(NR_PHY, "****** start Sidelink TX-Chain for AbsSubframe %d.%d ******\n", frame_tx, slot_tx);

  start_meas(&sl_phy_params->phy_proc_sl_tx);

  if (phy_data->sl_tx_action == SL_NR_CONFIG_TYPE_TX_PSBCH) {
    sl_nr_tx_config_psbch_pdu_t *psbch_vars = &phy_data->psbch_vars;
    nr_tx_psbch(ue, frame_tx, slot_tx, psbch_vars, txdataF);
    sl_phy_params->psbch.num_psbch_tx++;

    if (frame_tx % 64 == 0) {
      LOG_I(NR_PHY, "============================================\n");

      LOG_I(NR_PHY,
            "[UE%d] %d:%d PSBCH Stats: TX %d, RX ok %d, RX not ok %d\n",
            ue->Mod_id,
            frame_tx,
            slot_tx,
            sl_phy_params->psbch.num_psbch_tx,
            sl_phy_params->psbch.rx_ok,
            sl_phy_params->psbch.rx_errors);

      LOG_I(NR_PHY, "============================================\n");
    }
    tx_action = 1;
  }
  // episys SL data-plane port: PSCCH+PSSCH transmit. PSCCH (SCI-1) is encoded UE-native (nr_generate_sci1).
  else if (phy_data->sl_tx_action == SL_NR_CONFIG_TYPE_TX_PSCCH_PSSCH) {
    LOG_D(NR_PHY, "(%d.%d) Sidelink TX PSCCH(+PSSCH)\n", frame_tx, slot_tx);
    // PSCCH SCI-1 (PC5). nr_generate_sci1 writes the PSCCH and returns its CRC (spec: low 16 bits would be the
    // PSSCH DMRS/SLSCH-scrambling Nid). F1 BRING-UP: the RX does a blind PSSCH config with a FIXED Nid=0
    // (nr_ue_scheduler_sl.c), so force the TX to the same fixed Nid=0 here to align PSSCH DMRS + SLSCH + SCI-2
    // scrambling on both sides. TODO(reconcile): compute crc24c(SCI1)>>8 on BOTH ends for spec/multi-UE.
    nr_generate_sci1(ue, txdataF[0], fp, AMP, slot_tx, &phy_data->nr_sl_pssch_pscch_pdu);
    sl_phy_params->pscch.num_pscch_tx++; // PC5 PHY stats: PSCCH (SCI-1) TX
    phy_data->pscch_Nid = 0;
    // PSSCH data: SLSCH encode + SCI-2 polar encode + PSSCH DMRS + SL RE map (SCI-1 REs already written above).
    nr_ue_slsch_procedures(ue, frame_tx, slot_tx, phy_data, txdataF);
    tx_action = 1;
  }
  // episys SL PSFCH port: standalone PSFCH TX action (no PSSCH data this slot). Generation is done by the
  // common block below (shared with the MUXED case where PSFCH rides a TX_PSCCH_PSSCH slot's last symbol).
  else if (phy_data->sl_tx_action == SL_NR_CONFIG_TYPE_TX_PSFCH) {
    LOG_D(NR_PHY, "(%d.%d) Sidelink standalone TX PSFCH: %d pdu(s)\n", frame_tx, slot_tx, phy_data->num_psfch_pdus);
  }

  // episys SL PSFCH port (Stage 1/4c PHY TX): generate HARQ-feedback PSFCH(s) on PC5. Runs for BOTH the
  // standalone TX_PSFCH action and the MUXED TX_PSCCH_PSSCH slot (PSFCH in the last symbol, no data
  // preemption). nr_generate_psfch0 writes each PSFCH (PUCCH-format-0 sequence) onto the SL grid.
  if (phy_data->num_psfch_pdus && phy_data->psfch_pdu_list) {
    for (int k = 0; k < phy_data->num_psfch_pdus; k++)
      nr_generate_psfch0(ue, txdataF, fp, AMP, slot_tx, &phy_data->psfch_pdu_list[k]);
    sl_phy_params->psfch.num_psfch_tx++;
    tx_action = 1;
    free(phy_data->psfch_pdu_list); // MAC-allocated (nr_ue_sl_psfch_scheduler); PHY owns it after handoff
    phy_data->psfch_pdu_list = NULL;
    phy_data->num_psfch_pdus = 0;
  }

  bool was_symbol_used[NR_SYMBOLS_PER_SLOT];
  for (int i = 0; i < 14; i++)
    was_symbol_used[i] = true;
  if (tx_action) {
    LOG_D(NR_PHY, "Sending Uplink data \n");
    nr_tx_rotation_and_ofdm_mod(proc->nr_slot_tx,
                                fp,
                                fp->nb_antennas_tx,
                                txdataF,
                                txp,
                                link_type_sl,
                                was_symbol_used,
                                ue->no_phase_pre_comp);
  }

  LOG_D(NR_PHY, "****** end Sidelink TX-Chain for AbsSubframe %d.%d ******\n", frame_tx, slot_tx);
  stop_meas(&sl_phy_params->phy_proc_sl_tx);
}
