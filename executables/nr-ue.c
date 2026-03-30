/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.0  (the "License"); you may not use this file
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

#define _GNU_SOURCE // For pthread_setname_np
#include <pthread.h>
#include <openair1/PHY/impl_defs_top.h>
#include "executables/nr-uesoftmodem.h"
#include "PHY/phy_extern_nr_ue.h"
#include "PHY/INIT/nr_phy_init.h"
#include "NR_MAC_UE/mac_proto.h"
#include "RRC/NR_UE/rrc_proto.h"
#include "SCHED_NR_UE/phy_frame_config_nr.h"
#include "SCHED_NR_UE/defs.h"
#include "PHY/NR_UE_TRANSPORT/nr_transport_proto_ue.h"
#include "executables/softmodem-common.h"
#include "PHY/NR_REFSIG/refsig_defs_ue.h"
#include "radio/COMMON/common_lib.h"
#include "LAYER2/nr_pdcp/nr_pdcp_oai_api.h"

/*
 *  NR SLOT PROCESSING SEQUENCE
 *
 *  Processing occurs with following steps for connected mode:
 *
 *  - Rx samples for a slot are received,
 *  - PDCCH processing (including DCI extraction for downlink and uplink),
 *  - PDSCH processing (including transport blocks decoding),
 *  - PUCCH/PUSCH (transmission of acknowledgements, CSI, ... or data).
 *
 *  Time between reception of the slot and related transmission depends on UE processing performance.
 *  It is defined by the value NR_UE_CAPABILITY_SLOT_RX_TO_TX.
 *
 *  In NR, network gives the duration between Rx slot and Tx slot in the DCI:
 *  - for reception of a PDSCH and its associated acknowledgment slot (with a PUCCH or a PUSCH),
 *  - for reception of an uplink grant and its associated PUSCH slot.
 *
 *  So duration between reception and it associated transmission depends on its transmission slot given in the DCI.
 *  NR_UE_CAPABILITY_SLOT_RX_TO_TX means the minimum duration but higher duration can be given by the network because UE can support it.
 *
 *                                                                                                    Slot k
 *                                                                                  -------+------------+--------
 *                Frame                                                                    | Tx samples |
 *                Subframe                                                                 |   buffer   |
 *                Slot n                                                            -------+------------+--------
 *       ------ +------------+--------                                                     |
 *              | Rx samples |                                                             |
 *              |   buffer   |                                                             |
 *       -------+------------+--------                                                     |
 *                           |                                                             |
 *                           V                                                             |
 *                           +------------+                                                |
 *                           |   PDCCH    |                                                |
 *                           | processing |                                                |
 *                           +------------+                                                |
 *                           |            |                                                |
 *                           |            v                                                |
 *                           |            +------------+                                   |
 *                           |            |   PDSCH    |                                   |
 *                           |            | processing | decoding result                   |
 *                           |            +------------+    -> ACK/NACK of PDSCH           |
 *                           |                         |                                   |
 *                           |                         v                                   |
 *                           |                         +-------------+------------+        |
 *                           |                         | PUCCH/PUSCH | Tx samples |        |
 *                           |                         |  processing | transfer   |        |
 *                           |                         +-------------+------------+        |
 *                           |                                                             |
 *                           |/___________________________________________________________\|
 *                            \  duration between reception and associated transmission   /
 *
 * Remark: processing is done slot by slot, it can be distribute on different threads which are executed in parallel.
 * This is an architecture optimization in order to cope with real time constraints.
 * By example, for LTE, subframe processing is spread over 4 different threads.
 *
 */


#define RX_JOB_ID 0x1010
#define TX_JOB_ID 100
typedef struct {
  int abs_slot;
  pthread_cond_t c;
  pthread_mutex_t m;
  int ready;
} sync_notif_t;
sync_notif_t sync_notif;
sync_notif_t sync_notif_sl;

typedef enum {
  pss = 0,
  pbch = 1,
  si = 2,
  psbch = 3
} sync_mode_t;

static void *NRUE_phy_stub_standalone_pnf_task(void *arg);

static size_t dump_L1_UE_meas_stats(PHY_VARS_NR_UE *ue, char *output, size_t max_len)
{
  const char *begin = output;
  const char *end = output + max_len;
  output += print_meas_log(&ue->phy_proc_tx, "L1 TX processing", NULL, NULL, output, end - output);
  output += print_meas_log(&ue->ulsch_encoding_stats, "ULSCH encoding", NULL, NULL, output, end - output);
  output += print_meas_log(&ue->phy_proc_rx, "L1 RX processing", NULL, NULL, output, end - output);
  output += print_meas_log(&ue->ue_ul_indication_stats, "UL Indication", NULL, NULL, output, end - output);
  output += print_meas_log(&ue->rx_pdsch_stats, "PDSCH receiver", NULL, NULL, output, end - output);
  output += print_meas_log(&ue->dlsch_decoding_stats, "PDSCH decoding", NULL, NULL, output, end - output);
  output += print_meas_log(&ue->dlsch_deinterleaving_stats, " -> Deinterleive", NULL, NULL, output, end - output);
  output += print_meas_log(&ue->dlsch_rate_unmatching_stats, " -> Rate Unmatch", NULL, NULL, output, end - output);
  output += print_meas_log(&ue->dlsch_ldpc_decoding_stats, " ->  LDPC Decode", NULL, NULL, output, end - output);
  output += print_meas_log(&ue->dlsch_unscrambling_stats, "PDSCH unscrambling", NULL, NULL, output, end - output);
  output += print_meas_log(&ue->dlsch_rx_pdcch_stats, "PDCCH handling", NULL, NULL, output, end - output);
  return output - begin;
}

static void *nrL1_UE_stats_thread(void *param)
{
  PHY_VARS_NR_UE *ue = (PHY_VARS_NR_UE *) param;
  const int max_len = 16384;
  char output[max_len];
  char filename[30];
  snprintf(filename, 29, "nrL1_UE_stats-%d.log", ue->Mod_id);
  filename[29] = 0;
  FILE *fd = fopen(filename, "w");
  AssertFatal(fd != NULL, "Cannot open %s\n", filename);

  while (!oai_exit) {
    sleep(1);
    const int len = dump_L1_UE_meas_stats(ue, output, max_len);
    AssertFatal(len < max_len, "exceeded length\n");
    fwrite(output, len + 1, 1, fd); // + 1 for terminating NULL byte
    fflush(fd);
    fseek(fd, 0, SEEK_SET);
  }
  fclose(fd);

  return NULL;
}

void init_nr_ue_vars(PHY_VARS_NR_UE *ue,
                     uint8_t UE_id,
                     uint8_t abstraction_flag,
                     uint8_t sl_mode)
{

  int nb_connected_gNB = 1;

  ue->Mod_id      = UE_id;
  ue->if_inst     = nr_ue_if_module_init(0);
  ue->dci_thres   = 0;
  ue->target_Nid_cell = -1;
  bool is_sl = sl_mode > 0 ? true : false;
  // initialize all signal buffers
  init_nr_ue_signal(ue, nb_connected_gNB, is_sl);

  if (ue->sl_mode)
    sl_ue_phy_init(ue);

  // intialize transport
  init_nr_ue_transport(ue);

  // init N_TA offset
  init_N_TA_offset(ue);
}

void init_nrUE_standalone_thread(int ue_idx)
{
  int standalone_tx_port = 3611 + ue_idx * 2;
  int standalone_rx_port = 3612 + ue_idx * 2;
  nrue_init_standalone_socket(standalone_tx_port, standalone_rx_port);

  NR_UE_MAC_INST_t *mac = get_mac_inst(0);
  pthread_mutex_init(&mac->mutex_dl_info, NULL);

  pthread_t thread;
  if (pthread_create(&thread, NULL, nrue_standalone_pnf_task, NULL) != 0) {
    LOG_E(NR_MAC, "pthread_create failed for calling nrue_standalone_pnf_task");
  }
  pthread_setname_np(thread, "oai:nrue-stand");
  pthread_t phy_thread;
  if (pthread_create(&phy_thread, NULL, NRUE_phy_stub_standalone_pnf_task, NULL) != 0) {
    LOG_E(NR_MAC, "pthread_create failed for calling NRUE_phy_stub_standalone_pnf_task");
  }
  pthread_setname_np(phy_thread, "oai:nrue-stand-phy");
}

static void process_queued_nr_nfapi_msgs(NR_UE_MAC_INST_t *mac, int sfn_slot)
{
  nfapi_nr_rach_indication_t *rach_ind = unqueue_matching(&nr_rach_ind_queue, MAX_QUEUE_SIZE, sfn_slot_matcher, &sfn_slot);
  nfapi_nr_dl_tti_request_t *dl_tti_request = get_queue(&nr_dl_tti_req_queue);
  nfapi_nr_ul_dci_request_t *ul_dci_request = get_queue(&nr_ul_dci_req_queue);

  for (int i = 0; i < NR_MAX_HARQ_PROCESSES; i++) {
    LOG_D(NR_MAC, "Try to get a ul_tti_req by matching CRC active SFN %d/SLOT %d from queue with %lu items\n",
            NFAPI_SFNSLOT2SFN(mac->nr_ue_emul_l1.harq[i].active_ul_harq_sfn_slot),
            NFAPI_SFNSLOT2SLOT(mac->nr_ue_emul_l1.harq[i].active_ul_harq_sfn_slot), nr_ul_tti_req_queue.num_items);
    nfapi_nr_ul_tti_request_t *ul_tti_request_crc = unqueue_matching(&nr_ul_tti_req_queue, MAX_QUEUE_SIZE, sfn_slot_matcher, &mac->nr_ue_emul_l1.harq[i].active_ul_harq_sfn_slot);
    if (ul_tti_request_crc && ul_tti_request_crc->n_pdus > 0) {
      check_and_process_dci(NULL, NULL, NULL, ul_tti_request_crc);
      free_and_zero(ul_tti_request_crc);
    }
  }

  if (rach_ind && rach_ind->number_of_pdus > 0) {
      NR_UL_IND_t UL_INFO = {
        .rach_ind = *rach_ind,
      };
      send_nsa_standalone_msg(&UL_INFO, rach_ind->header.message_id);
      for (int i = 0; i < rach_ind->number_of_pdus; i++)
      {
        free_and_zero(rach_ind->pdu_list[i].preamble_list);
      }
      free_and_zero(rach_ind->pdu_list);
      free_and_zero(rach_ind);
  }
  if (dl_tti_request) {
    int dl_tti_sfn_slot = NFAPI_SFNSLOT2HEX(dl_tti_request->SFN, dl_tti_request->Slot);
    nfapi_nr_tx_data_request_t *tx_data_request = unqueue_matching(&nr_tx_req_queue, MAX_QUEUE_SIZE, sfn_slot_matcher, &dl_tti_sfn_slot);
    if (!tx_data_request) {
      LOG_E(NR_MAC, "[%d %d] No corresponding tx_data_request for given dl_tti_request sfn/slot\n",
            NFAPI_SFNSLOT2SFN(dl_tti_sfn_slot), NFAPI_SFNSLOT2SLOT(dl_tti_sfn_slot));
      if (get_softmodem_params()->nsa)
        save_nr_measurement_info(dl_tti_request);
      free_and_zero(dl_tti_request);
    }
    else if (dl_tti_request->dl_tti_request_body.nPDUs > 0 && tx_data_request->Number_of_PDUs > 0) {
      if (get_softmodem_params()->nsa)
        save_nr_measurement_info(dl_tti_request);
      check_and_process_dci(dl_tti_request, tx_data_request, NULL, NULL);
      free_and_zero(dl_tti_request);
      free_and_zero(tx_data_request);
    }
    else {
      AssertFatal(false, "We dont have PDUs in either dl_tti %d or tx_req %d\n",
                  dl_tti_request->dl_tti_request_body.nPDUs, tx_data_request->Number_of_PDUs);
    }
  }
  if (ul_dci_request && ul_dci_request->numPdus > 0) {
    check_and_process_dci(NULL, NULL, ul_dci_request, NULL);
    free_and_zero(ul_dci_request);
  }
}

static void *NRUE_phy_stub_standalone_pnf_task(void *arg)
{
  LOG_I(MAC, "Clearing Queues\n");
  reset_queue(&nr_rach_ind_queue);
  reset_queue(&nr_rx_ind_queue);
  reset_queue(&nr_crc_ind_queue);
  reset_queue(&nr_uci_ind_queue);
  reset_queue(&nr_dl_tti_req_queue);
  reset_queue(&nr_tx_req_queue);
  reset_queue(&nr_ul_dci_req_queue);
  reset_queue(&nr_ul_tti_req_queue);

  int last_sfn_slot = -1;
  uint16_t sfn_slot = 0;

  module_id_t mod_id = 0;
  NR_UE_MAC_INST_t *mac = get_mac_inst(mod_id);
  for (int i = 0; i < NR_MAX_HARQ_PROCESSES; i++) {
      mac->nr_ue_emul_l1.harq[i].active = false;
      mac->nr_ue_emul_l1.harq[i].active_ul_harq_sfn_slot = -1;
  }

  while (!oai_exit) {
    if (sem_wait(&sfn_slot_semaphore) != 0) {
      LOG_E(NR_MAC, "sem_wait() error\n");
      abort();
    }
    uint16_t *slot_ind = get_queue(&nr_sfn_slot_queue);
    nr_phy_channel_params_t *ch_info = get_queue(&nr_chan_param_queue);
    if (!slot_ind && !ch_info) {
      LOG_D(MAC, "get nr_sfn_slot_queue and nr_chan_param_queue == NULL!\n");
      continue;
    }
    if (slot_ind) {
      sfn_slot = *slot_ind;
      free_and_zero(slot_ind);
    }
    else if (ch_info) {
      sfn_slot = ch_info->sfn_slot;
      free_and_zero(ch_info);
    }

    frame_t frame = NFAPI_SFNSLOT2SFN(sfn_slot);
    int slot = NFAPI_SFNSLOT2SLOT(sfn_slot);
    if (sfn_slot == last_sfn_slot) {
      LOG_D(NR_MAC, "repeated sfn_sf = %d.%d\n",
            frame, slot);
      continue;
    }
    last_sfn_slot = sfn_slot;

    LOG_D(NR_MAC, "The received sfn/slot [%d %d] from proxy\n",
          frame, slot);

    if (get_softmodem_params()->sa && mac->mib == NULL) {
      LOG_D(NR_MAC, "We haven't gotten MIB. Lets see if we received it\n");
      nr_ue_dl_indication(&mac->dl_info);
      process_queued_nr_nfapi_msgs(mac, sfn_slot);
    }

    bool only_dl = false;
    if (mac->scc == NULL && mac->scc_SIB == NULL)
      only_dl = true;

    int CC_id = 0;
    uint8_t gNB_id = 0;
    nr_uplink_indication_t ul_info;
    int slots_per_frame = 20; //30 kHZ subcarrier spacing
    int slot_ahead = 3; // TODO: Make this dynamic
    ul_info.cc_id = CC_id;
    ul_info.gNB_index = gNB_id;
    ul_info.module_id = mod_id;
    ul_info.frame_rx = frame;
    ul_info.slot_rx = slot;
    ul_info.slot_tx = (slot + slot_ahead) % slots_per_frame;
    ul_info.frame_tx = (ul_info.slot_rx + slot_ahead >= slots_per_frame) ? ul_info.frame_rx + 1 : ul_info.frame_rx;

    if (pthread_mutex_lock(&mac->mutex_dl_info)) abort();

    if (ch_info) {
      mac->nr_ue_emul_l1.pmi = ch_info->csi[0].pmi;
      mac->nr_ue_emul_l1.ri = ch_info->csi[0].ri;
      mac->nr_ue_emul_l1.cqi = ch_info->csi[0].cqi;
      free_and_zero(ch_info);
    }

    if (only_dl ||
        is_nr_DL_slot(get_softmodem_params()->nsa ?
                      mac->scc->tdd_UL_DL_ConfigurationCommon :
                      mac->scc_SIB->tdd_UL_DL_ConfigurationCommon,
                      ul_info.slot_rx)) {
      memset(&mac->dl_info, 0, sizeof(mac->dl_info));
      mac->dl_info.cc_id = CC_id;
      mac->dl_info.gNB_index = gNB_id;
      mac->dl_info.module_id = mod_id;
      mac->dl_info.frame = frame;
      mac->dl_info.slot = slot;
      mac->dl_info.dci_ind = NULL;
      mac->dl_info.rx_ind = NULL;
      nr_ue_dl_indication(&mac->dl_info);
    }

    if (pthread_mutex_unlock(&mac->mutex_dl_info)) abort();

    if (!only_dl &&
        is_nr_UL_slot(get_softmodem_params()->nsa ?
                      mac->scc->tdd_UL_DL_ConfigurationCommon :
                      mac->scc_SIB->tdd_UL_DL_ConfigurationCommon,
                      ul_info.slot_tx, mac->frame_type)) {
      LOG_D(NR_MAC, "Slot %d. calling nr_ue_ul_ind()\n", ul_info.slot_tx);
      nr_ue_ul_scheduler(&ul_info);
    }
    process_queued_nr_nfapi_msgs(mac, sfn_slot);
  }
  return NULL;
}


/*!
 * It performs band scanning and synchonization.
 * \param arg is a pointer to a \ref PHY_VARS_NR_UE structure.
 */

typedef nr_rxtx_thread_data_t syncData_t;

static void UE_synch_sl(void *arg) {
  syncData_t *syncD = (syncData_t *) arg;
  PHY_VARS_NR_UE *UE = syncD->UE;
  UE->is_synchronized_sl = 0;

  LOG_I(PHY, "[UE thread Synch] Running Sidelink Initial Synch \n");
  NR_DL_FRAME_PARMS *fp = &UE->SL_UE_PHY_PARAMS.sl_frame_params;
  uint64_t dl_carrier = fp->sl_CarrierFreq;
  uint64_t ul_carrier = fp->sl_CarrierFreq;

  static int freq_offset = 0;
  if (sl_nr_slss_search(UE, &syncD->proc, 16) == 0) {
    /* PC5 FFO belongs in sidelink sync state; common_vars.freq_offset is for the Uu link. */
    freq_offset = UE->SL_UE_PHY_PARAMS.sync_params.freq_offset;
    int hw_slot_offset = ((UE->rx_offset << 1) / fp->samples_per_subframe * fp->slots_per_subframe) +
                      round((float)((UE->rx_offset << 1) % fp->samples_per_subframe) / fp->samples_per_slot0);

    nr_rf_card_config_freq(&openair0_cfg[UE->rf_map_sl.card], ul_carrier, dl_carrier, freq_offset);

    LOG_I(PHY,"Got synch sidelink: hw_slot_offset %d, carrier off %d Hz, rxgain %f (DL %f Hz, UL %f Hz)\n",
          hw_slot_offset,
          freq_offset,
          openair0_cfg[UE->rf_map_sl.card].rx_gain[0],
          openair0_cfg[UE->rf_map_sl.card].rx_freq[0],
          openair0_cfg[UE->rf_map_sl.card].tx_freq[0]);

    UE->rfdevice_sl.trx_set_freq_func(&UE->rfdevice_sl, &openair0_cfg[UE->rf_map_sl.card]);
    if (UE->UE_scan_carrier == 1) {
      UE->UE_scan_carrier = 0;
    } else {
      UE->is_synchronized_sl = 1;
    }
  } else if (UE->UE_scan_carrier == 1) {
    if (freq_offset >= 0)
      freq_offset += 100;
    freq_offset *= -1;
    nr_rf_card_config_freq(&openair0_cfg[UE->rf_map_sl.card], ul_carrier, dl_carrier, freq_offset);
    LOG_I(NR_PHY, "Sidelink Initial sync failed: trying carrier off %d Hz\n", freq_offset);
    UE->rfdevice_sl.trx_set_freq_func(&UE->rfdevice_sl, &openair0_cfg[UE->rf_map_sl.card]);
  }
}

static void UE_synch(void *arg) {
  syncData_t *syncD=(syncData_t *) arg;
  int i, hw_slot_offset;
  PHY_VARS_NR_UE *UE = syncD->UE;
  sync_mode_t sync_mode = pbch;
  //int CC_id = UE->CC_id;
  static int freq_offset=0;
  UE->is_synchronized = 0;

  if (UE->UE_scan == 0) {

    for (i=0; i<openair0_cfg[UE->rf_map.card].rx_num_channels; i++) {

      LOG_I( PHY, "[SCHED][UE] Check absolute frequency DL %f, UL %f (RF card %d, oai_exit %d, channel %d, rx_num_channels %d)\n",
        openair0_cfg[UE->rf_map.card].rx_freq[UE->rf_map.chain+i],
        openair0_cfg[UE->rf_map.card].tx_freq[UE->rf_map.chain+i],
        UE->rf_map.card,
        oai_exit,
        i,
        openair0_cfg[UE->rf_map.card].rx_num_channels);

    }

    sync_mode = pbch;
  } else {
    LOG_E(PHY,"Fixme!\n");
    /*
    for (i=0; i<openair0_cfg[UE->rf_map.card].rx_num_channels; i++) {
      downlink_frequency[UE->rf_map.card][UE->rf_map.chain+i] = bands_to_scan.band_info[CC_id].dl_min;
      uplink_frequency_offset[UE->rf_map.card][UE->rf_map.chain+i] =
        bands_to_scan.band_info[CC_id].ul_min-bands_to_scan.band_info[CC_id].dl_min;
      openair0_cfg[UE->rf_map.card].rx_freq[UE->rf_map.chain+i] = downlink_frequency[CC_id][i];
      openair0_cfg[UE->rf_map.card].tx_freq[UE->rf_map.chain+i] =
        downlink_frequency[CC_id][i]+uplink_frequency_offset[CC_id][i];
      openair0_cfg[UE->rf_map.card].rx_gain[UE->rf_map.chain+i] = UE->rx_total_gain_dB;
    }
    */
  }

  if (UE->target_Nid_cell != -1) {
    LOG_W(NR_PHY, "Starting re-sync detection for target Nid_cell %i\n", UE->target_Nid_cell);
  } else {
    LOG_W(NR_PHY, "Starting sync detection\n");
  }

  switch (sync_mode) {
    /*
    case pss:
      LOG_I(PHY,"[SCHED][UE] Scanning band %d (%d), freq %u\n",bands_to_scan.band_info[current_band].band, current_band,bands_to_scan.band_info[current_band].dl_min+current_offset);
      //lte_sync_timefreq(UE,current_band,bands_to_scan.band_info[current_band].dl_min+current_offset);
      current_offset += 20000000; // increase by 20 MHz

      if (current_offset > bands_to_scan.band_info[current_band].dl_max-bands_to_scan.band_info[current_band].dl_min) {
        current_band++;
        current_offset=0;
      }

      if (current_band==bands_to_scan.nbands) {
        current_band=0;
        oai_exit=1;
      }

      for (i=0; i<openair0_cfg[UE->rf_map.card].rx_num_channels; i++) {
        downlink_frequency[UE->rf_map.card][UE->rf_map.chain+i] = bands_to_scan.band_info[current_band].dl_min+current_offset;
        uplink_frequency_offset[UE->rf_map.card][UE->rf_map.chain+i] = bands_to_scan.band_info[current_band].ul_min-bands_to_scan.band_info[0].dl_min + current_offset;
        openair0_cfg[UE->rf_map.card].rx_freq[UE->rf_map.chain+i] = downlink_frequency[CC_id][i];
        openair0_cfg[UE->rf_map.card].tx_freq[UE->rf_map.chain+i] = downlink_frequency[CC_id][i]+uplink_frequency_offset[CC_id][i];
        openair0_cfg[UE->rf_map.card].rx_gain[UE->rf_map.chain+i] = UE->rx_total_gain_dB;

        if (UE->UE_scan_carrier) {
          openair0_cfg[UE->rf_map.card].autocal[UE->rf_map.chain+i] = 1;
        }
      }

      break;
    */
    case pbch:
      LOG_I(PHY, "[UE thread Synch] Running Initial Synch \n");

      uint64_t dl_carrier, ul_carrier;
      nr_get_carrier_frequencies(UE, &dl_carrier, &ul_carrier);

      if (nr_initial_sync(&syncD->proc, UE, 2, get_softmodem_params()->sa) == 0) {
        freq_offset = UE->common_vars.freq_offset; // frequency offset computed with pss in initial sync
        hw_slot_offset = ((UE->rx_offset<<1) / UE->frame_parms.samples_per_subframe * UE->frame_parms.slots_per_subframe) +
                         round((float)((UE->rx_offset<<1) % UE->frame_parms.samples_per_subframe)/UE->frame_parms.samples_per_slot0);

        // rerun with new cell parameters and frequency-offset
        // todo: the freq_offset computed on DL shall be scaled before being applied to UL
        nr_rf_card_config_freq(&openair0_cfg[UE->rf_map.card], ul_carrier, dl_carrier, freq_offset);

        LOG_I(PHY,"Got synch: hw_slot_offset %d, carrier off %d Hz, rxgain %f (DL %f Hz, UL %f Hz)\n",
              hw_slot_offset,
              freq_offset,
              openair0_cfg[UE->rf_map.card].rx_gain[0],
              openair0_cfg[UE->rf_map.card].rx_freq[0],
              openair0_cfg[UE->rf_map.card].tx_freq[0]);

        UE->rfdevice.trx_set_freq_func(&UE->rfdevice,&openair0_cfg[UE->rf_map.card]);
        if (UE->UE_scan_carrier == 1) {
          UE->UE_scan_carrier = 0;
        } else {
          UE->is_synchronized = 1;
        }
      } else {

        if (UE->UE_scan_carrier == 1) {

          if (freq_offset >= 0)
            freq_offset += 100;

          freq_offset *= -1;

          nr_rf_card_config_freq(&openair0_cfg[UE->rf_map.card], ul_carrier, dl_carrier, freq_offset);

          LOG_I(PHY, "Initial sync failed: trying carrier off %d Hz\n", freq_offset);

          UE->rfdevice.trx_set_freq_func(&UE->rfdevice,&openair0_cfg[UE->rf_map.card]);
        }
      }
      break;

    case si:
    default:
      break;

  }
}

static void RU_write(nr_rxtx_thread_data_t *rxtxD, bool sl_tx_action) {

  PHY_VARS_NR_UE *UE = rxtxD->UE;
  UE_nr_rxtx_proc_t *proc = &rxtxD->proc;
  NR_DL_FRAME_PARMS *fp = (rxtxD->intf_type == PC5) ? &UE->SL_UE_PHY_PARAMS.sl_frame_params : &UE->frame_parms;
  void *txp[NB_ANTENNAS_TX];
  if (rxtxD->intf_type == UU) {
    for (int i = 0; i < fp->nb_antennas_tx; i++)
      txp[i] = (void *)&UE->common_vars.txData[i][fp->get_samples_slot_timestamp(proc->nr_slot_tx, fp, 0)];
  } else {
    for (int i = 0; i < fp->nb_antennas_tx; i++)
      txp[i] = (void *)&UE->common_vars.txDataSl[i][fp->get_samples_slot_timestamp(proc->nr_slot_tx, fp, 0)];
  }

  radio_tx_burst_flag_t flags = TX_BURST_INVALID;

  NR_UE_MAC_INST_t *mac = get_mac_inst(0);
  duplex_mode_t duplex_mode = (rxtxD->intf_type == UU) ? openair0_cfg[UE->rf_map.card].duplex_mode : openair0_cfg[UE->rf_map_sl.card].duplex_mode;
  if (mac->phy_config_request_sent &&
      duplex_mode == duplex_mode_TDD &&
      !get_softmodem_params()->continuous_tx) {

    //Perform USRP write only in case SL Txn needs to be done.
    if (rxtxD->intf_type == PC5) {
      flags = sl_tx_action ? TX_BURST_START_AND_END
                           : TX_BURST_INVALID;
    } else {

      uint8_t tdd_period = mac->phy_config.config_req.tdd_table.tdd_period_in_slots;
      int nrofUplinkSlots, nrofUplinkSymbols;
      if (mac->scc) {
        nrofUplinkSlots = mac->scc->tdd_UL_DL_ConfigurationCommon->pattern1.nrofUplinkSlots;
        nrofUplinkSymbols = mac->scc->tdd_UL_DL_ConfigurationCommon->pattern1.nrofUplinkSymbols;
      }
      else {
        nrofUplinkSlots = mac->scc_SIB->tdd_UL_DL_ConfigurationCommon->pattern1.nrofUplinkSlots;
        nrofUplinkSymbols = mac->scc_SIB->tdd_UL_DL_ConfigurationCommon->pattern1.nrofUplinkSymbols;
      }

      int slot_tx_usrp = proc->nr_slot_tx;
      uint8_t  num_UL_slots = nrofUplinkSlots + (nrofUplinkSymbols != 0);
      uint8_t first_tx_slot = tdd_period - num_UL_slots;

      if (slot_tx_usrp % tdd_period == first_tx_slot)
        flags = TX_BURST_START;
      else if (slot_tx_usrp % tdd_period == first_tx_slot + num_UL_slots - 1)
        flags = TX_BURST_END;
      else if (slot_tx_usrp % tdd_period > first_tx_slot)
        flags = TX_BURST_MIDDLE;
    }
  } else {
    flags = TX_BURST_MIDDLE;
  }

  openair0_device *rfdevice = (rxtxD->intf_type == UU) ? &UE->rfdevice : &UE->rfdevice_sl;
  if (flags || IS_SOFTMODEM_RFSIM)
    AssertFatal(rxtxD->writeBlockSize ==
                rfdevice->trx_write_func(rfdevice,
                                         proc->timestamp_tx,
                                         txp,
                                         rxtxD->writeBlockSize,
                                         fp->nb_antennas_tx,
                                         flags),"");

  for (int i = 0; i < fp->nb_antennas_tx; i++)
    memset(txp[i], 0, rxtxD->writeBlockSize);

}

void processSlotTX(void *arg) {

  nr_rxtx_thread_data_t *rxtxD = (nr_rxtx_thread_data_t *) arg;
  UE_nr_rxtx_proc_t *proc = &rxtxD->proc;
  PHY_VARS_NR_UE    *UE   = rxtxD->UE;
  nr_phy_data_tx_t phy_data = {0};
  bool sl_tx_action = false;

  if (proc->tx_slot_type == NR_SIDELINK_SLOT) {

   // trigger L2 to run ue_scheduler thru IF module
    if(UE->if_inst != NULL && UE->if_inst->sl_indication != NULL) {
      start_meas(&UE->ue_ul_indication_stats);
      nr_sidelink_indication_t sl_indication;
      memset((void*)&sl_indication, 0, sizeof(sl_indication));

      sl_indication.module_id = UE->Mod_id;
      sl_indication.gNB_index = proc->gNB_id;
      sl_indication.cc_id     = UE->CC_id;
      sl_indication.frame_rx  = proc->frame_rx;
      sl_indication.slot_rx   = proc->nr_slot_rx;
      sl_indication.frame_tx  = proc->frame_tx;
      sl_indication.slot_tx   = proc->nr_slot_tx;
      sl_indication.phy_data  = &phy_data;
      sl_indication.slot_type = SIDELINK_SLOT_TYPE_TX;

      LOG_D(NR_PHY,"Sending SL indication RX %d.%d TX %d.%d\n",proc->frame_rx,proc->nr_slot_rx,proc->frame_tx,proc->nr_slot_tx);
      UE->if_inst->sl_indication(&sl_indication);

      stop_meas(&UE->ue_ul_indication_stats);
    }

    sl_tx_action = phy_procedures_nrUE_SL_TX(UE, proc, &phy_data);
  } else {
    if (proc->tx_slot_type == NR_UPLINK_SLOT || proc->tx_slot_type == NR_MIXED_SLOT){

      // wait for rx slots to send indication (if any) that DLSCH decoding is finished
      for(int i=0; i < rxtxD->tx_wait_for_dlsch; i++) {
        notifiedFIFO_elt_t *res = pullNotifiedFIFO(UE->tx_resume_ind_fifo[proc->nr_slot_tx]);
        delNotifiedFIFO_elt(res);
      }

      // trigger L2 to run ue_scheduler thru IF module
      // [TODO] mapping right after NR initial sync
      if(UE->if_inst != NULL && UE->if_inst->ul_indication != NULL) {
        start_meas(&UE->ue_ul_indication_stats);
        nr_uplink_indication_t ul_indication;
        memset((void*)&ul_indication, 0, sizeof(ul_indication));

        ul_indication.module_id = UE->Mod_id;
        ul_indication.gNB_index = proc->gNB_id;
        ul_indication.cc_id     = UE->CC_id;
        ul_indication.frame_rx  = proc->frame_rx;
        ul_indication.slot_rx   = proc->nr_slot_rx;
        ul_indication.frame_tx  = proc->frame_tx;
        ul_indication.slot_tx   = proc->nr_slot_tx;
        ul_indication.phy_data      = &phy_data;

        UE->if_inst->ul_indication(&ul_indication);
        stop_meas(&UE->ue_ul_indication_stats);
      }

      phy_procedures_nrUE_TX(UE, proc, &phy_data);
    }
  }

  RU_write(rxtxD, sl_tx_action);
}

nr_phy_data_t UE_dl_preprocessing(PHY_VARS_NR_UE *UE, UE_nr_rxtx_proc_t *proc,  nr_intf_type_t intf_type)
{
  nr_phy_data_t phy_data = {0};
  NR_DL_FRAME_PARMS *fp = intf_type == UU ? &UE->frame_parms : &UE->SL_UE_PHY_PARAMS.sl_frame_params;

  if (IS_SOFTMODEM_NOS1 || get_softmodem_params()->sa) {

    // Start synchronization with a target gNB
    if (UE->synch_request.received_synch_request == 1 && UE->target_Nid_cell == -1) {
      UE->is_synchronized = 0;
      UE->is_synchronized_sl = 0;
      UE->target_Nid_cell = UE->synch_request.synch_req.target_Nid_cell;
      clean_UE_ulsch(UE, proc->gNB_id);
    } else if (UE->synch_request.received_synch_request == 1 && UE->target_Nid_cell != -1) {
      UE->synch_request.received_synch_request = 0;
      UE->target_Nid_cell = -1;
    }

    /* send tick to RLC and PDCP every ms */
    if (proc->nr_slot_rx % fp->slots_per_subframe == 0) {
      void nr_rlc_tick(int frame, int subframe);
      void nr_pdcp_tick(int frame, int subframe);
      nr_rlc_tick(proc->frame_rx, proc->nr_slot_rx / fp->slots_per_subframe);
      nr_pdcp_tick(proc->frame_rx, proc->nr_slot_rx / fp->slots_per_subframe);
    }
  }

  if (intf_type == PC5) {
    if (proc->rx_slot_type == NR_SIDELINK_SLOT) {
      if(UE->if_inst != NULL && UE->if_inst->sl_indication != NULL) {
        nr_sidelink_indication_t sl_indication;
        nr_fill_sl_indication(&sl_indication, NULL, NULL, proc, UE, &phy_data);
        UE->if_inst->sl_indication(&sl_indication);
      }
      uint64_t a=rdtsc_oai();
      psbch_pscch_pssch_processing(UE, proc, &phy_data);
      LOG_D(PHY, "In %s: slot %d:%d, time %llu\n", __FUNCTION__, proc->frame_rx, proc->nr_slot_rx, (rdtsc_oai()-a)/3500);
    }
  } else {
    /* Skip Uu PBCH/PDCCH/DLSCH when in SL Mode 2 (sidelink-only; no gNB link) */
    if (proc->rx_slot_type == NR_DOWNLINK_SLOT || proc->rx_slot_type == NR_MIXED_SLOT){

      if(UE->if_inst != NULL && UE->if_inst->dl_indication != NULL) {
        nr_downlink_indication_t dl_indication;
        nr_fill_dl_indication(&dl_indication, NULL, NULL, proc, UE, &phy_data);
        UE->if_inst->dl_indication(&dl_indication);
      }

      uint64_t a=rdtsc_oai();
      pbch_pdcch_processing(UE, proc, &phy_data);
      if (phy_data.dlsch[0].active) {
        // indicate to tx thread to wait for DLSCH decoding
        const int ack_nack_slot = (proc->nr_slot_rx + phy_data.dlsch[0].dlsch_config.k1_feedback) % UE->frame_parms.slots_per_frame;
        UE->tx_wait_for_dlsch[ack_nack_slot]++;
      }

      LOG_D(PHY, "In %s: slot %d, time %llu\n", __FUNCTION__, proc->nr_slot_rx, (rdtsc_oai()-a)/3500);
    }
    ue_ta_procedures(UE, proc->nr_slot_tx, proc->frame_tx);
  }
  return phy_data;
}

void UE_dl_processing(void *arg) {
  nr_rxtx_thread_data_t *rxtxD = (nr_rxtx_thread_data_t *) arg;
  UE_nr_rxtx_proc_t *proc = &rxtxD->proc;
  PHY_VARS_NR_UE    *UE   = rxtxD->UE;
  nr_phy_data_t *phy_data = &rxtxD->phy_data;

  if (UE->sl_mode == 0 || UE->sl_mode == 1)
    pdsch_processing(UE, proc, phy_data);
}

void dummyWrite(PHY_VARS_NR_UE *UE, openair0_timestamp timestamp, int writeBlockSize, nr_intf_type_t intf_type) {

  NR_DL_FRAME_PARMS *fp = (intf_type == UU) ? &UE->frame_parms : &UE->SL_UE_PHY_PARAMS.sl_frame_params;
  openair0_device *rfdevice = (intf_type == UU) ? &UE->rfdevice : &UE->rfdevice_sl;
  void *dummy_tx[fp->nb_antennas_tx];
  int16_t dummy_tx_data[fp->nb_antennas_tx][2 * writeBlockSize]; // 2 because the function we call use pairs of int16_t implicitly as complex numbers

  memset(dummy_tx_data, 0, sizeof(dummy_tx_data));

  for (int i = 0; i < fp->nb_antennas_tx; i++)
    dummy_tx[i] = dummy_tx_data[i];

  AssertFatal(writeBlockSize ==
              rfdevice->trx_write_func(rfdevice,
              timestamp,
              dummy_tx,
              writeBlockSize,
              fp->nb_antennas_tx,
              4), "");

}

void readFrame(PHY_VARS_NR_UE *UE,  openair0_timestamp *timestamp, nr_intf_type_t intf_type, bool toTrash) {

  //In Sidelink worst case SL-SSB can be sent once in 16 frames
  int num_frames = (intf_type == UU) ? 2 : SL_NR_PSBCH_REPETITION_IN_FRAMES;
  NR_DL_FRAME_PARMS *fp = (intf_type == UU) ? &UE->frame_parms : &UE->SL_UE_PHY_PARAMS.sl_frame_params;
  openair0_device *rfdevice = (intf_type == UU) ? &UE->rfdevice : &UE->rfdevice_sl;
  c16_t **rxdata = (intf_type == UU) ? UE->common_vars.rxdata : UE->common_vars.rxdata_sl;

  void *rxp[NB_ANTENNAS_RX];

  for(int x = 0; x < num_frames * NR_NUMBER_OF_SUBFRAMES_PER_FRAME; x++) {  // two frames for initial sync
    for (int slot = 0; slot < fp->slots_per_subframe; slot++) {
      for (int i = 0; i < fp->nb_antennas_rx; i++) {
        if (toTrash)
          rxp[i] = malloc16(fp->get_samples_per_slot(slot, fp) * 4);
        else
          rxp[i] = ((void *)&rxdata[i][0]) +
                   4 * ((x * fp->samples_per_subframe) +
                   fp->get_samples_slot_timestamp(slot, fp, 0));
      }

      AssertFatal(fp->get_samples_per_slot(slot, fp) ==
                  rfdevice->trx_read_func(rfdevice,
                  timestamp,
                  rxp,
                  fp->get_samples_per_slot(slot, fp),
                  fp->nb_antennas_rx), "");

      if (IS_SOFTMODEM_RFSIM)
        dummyWrite(UE, *timestamp, fp->get_samples_per_slot(slot, fp), intf_type);
      if (toTrash)
        for (int i = 0; i < fp->nb_antennas_rx; i++)
          free(rxp[i]);
    }
  }

}

void syncInFrame(PHY_VARS_NR_UE *UE, openair0_timestamp *timestamp, nr_intf_type_t intf_type) {

  NR_DL_FRAME_PARMS *fp = (intf_type == UU) ? &UE->frame_parms : &UE->SL_UE_PHY_PARAMS.sl_frame_params;
  openair0_device *rfdevice = (intf_type == UU) ? &UE->rfdevice : &UE->rfdevice_sl;
  c16_t **rxdata = (intf_type == UU) ? UE->common_vars.rxdata : UE->common_vars.rxdata_sl;

  LOG_I(PHY, "Resynchronizing RX by %d samples\n", UE->rx_offset);

  if (IS_SOFTMODEM_IQPLAYER || IS_SOFTMODEM_IQRECORDER) {
    // Resynchonize by slot (will work with numerology 1 only)
    for ( int size = UE->rx_offset; size > 0; size -= fp->samples_per_subframe / 2 ) {
      int unitTransfer = size > fp->samples_per_subframe / 2 ? fp->samples_per_subframe / 2 : size;
      AssertFatal(unitTransfer ==
                  rfdevice->trx_read_func(rfdevice,
                  timestamp,
                  (void **)rxdata,
                  unitTransfer,
                  fp->nb_antennas_rx), "");
    }
  } else {
    *timestamp += fp->get_samples_per_slot(1, fp);
    for ( int size = UE->rx_offset; size > 0; size -= fp->samples_per_subframe ) {
      int unitTransfer = size>fp->samples_per_subframe ? fp->samples_per_subframe : size;
      // we write before read because gNB waits for UE to write and both executions halt
      // this happens here as the read size is samples_per_subframe which is very much larger than samp_per_slot
      if (IS_SOFTMODEM_RFSIM) dummyWrite(UE, *timestamp, unitTransfer, intf_type);
      AssertFatal(unitTransfer ==
                  rfdevice->trx_read_func(rfdevice,
                  timestamp,
                  (void **)rxdata,
                  unitTransfer,
                  fp->nb_antennas_rx), "");
      *timestamp += unitTransfer; // this does not affect the read but needed for RFSIM write
    }
  }
}

static inline int get_firstSymSamp(uint16_t slot, NR_DL_FRAME_PARMS *fp) {
  if (fp->numerology_index == 0)
    return fp->nb_prefix_samples0 + fp->ofdm_symbol_size;
  int num_samples = (slot%(fp->slots_per_subframe/2)) ? fp->nb_prefix_samples : fp->nb_prefix_samples0;
  num_samples += fp->ofdm_symbol_size;
  return num_samples;
}

static inline int get_readBlockSize(uint16_t slot, NR_DL_FRAME_PARMS *fp) {
  int rem_samples = fp->get_samples_per_slot(slot, fp) - get_firstSymSamp(slot, fp);
  int next_slot_first_symbol = 0;
  if (slot < (fp->slots_per_frame-1))
    next_slot_first_symbol = get_firstSymSamp(slot+1, fp);
  return rem_samples + next_slot_first_symbol;
}


void *UE_thread_sl(void *arg)
{
  PHY_VARS_NR_UE *UE = (PHY_VARS_NR_UE *) arg;
  openair0_timestamp timestamp;
  int start_rx_stream = 0;
  NR_DL_FRAME_PARMS *fp = &UE->SL_UE_PHY_PARAMS.sl_frame_params;
  AssertFatal(UE->sl_mode != 0, "Should not enter this function if not in SL mode 1 or 2.");
  openair0_cfg[UE->rf_map_sl.card].gpio_controller = RU_GPIO_CONTROL_GENERIC;

  AssertFatal(0 == openair0_device_load(&(UE->rfdevice_sl), &openair0_cfg[UE->rf_map_sl.card]), "");
  UE->rfdevice_sl.host_type = RAU_HOST;
  UE->is_synchronized_sl = 0;
  LOG_I(NR_PHY,"Launching UE_thread_sl\n");
  AssertFatal(UE->rfdevice_sl.trx_start_func_sl != NULL, "Undefined function: trx_start_func_sl\n");
  AssertFatal(UE->rfdevice_sl.trx_start_func_sl(&UE->rfdevice_sl) == 0, "Could not start the device\n");

  /* Do NOT call init_context_synchro_nr(..., PC5) here: it replaces global Uu PSS/SSS
   * buffers used by the gNB DL chain (PBCH/PDCCH). Sidelink SLSS search uses
   * SL_UE_PHY_PARAMS.init_params.sl_pss_for_correlation (see sl_generate_pss_ifft_samples). */

  notifiedFIFO_t nf;
  initNotifiedFIFO(&nf);

  bool syncRunning = false;
  const int nb_slot_frame = fp->slots_per_frame;
  int absolute_slot = 0, decoded_frame_rx = INT_MAX, trashed_frames = 0;

  if (get_nrUE_params()->sync_ref) {
      UE->is_synchronized_sl = 1;
      start_rx_stream = -1;
  }

  while (!oai_exit) {

    if (syncRunning) {
      notifiedFIFO_elt_t *res = tryPullTpool(&nf, &(get_nrUE_params()->Tpool));

      if (res) {
        syncRunning = false;
        if (UE->is_synchronized_sl) {
          decoded_frame_rx = UE->SL_UE_PHY_PARAMS.sync_params.DFN;
          LOG_I(NR_PHY, "UE synchronized decoded_frame_rx = %d UE->init_sync_frame = %d trashed_frames = %d\n",
                decoded_frame_rx,
                UE->init_sync_frame,
                trashed_frames);
          // shift the frame index with all the frames we trashed meanwhile we perform the synch search
          decoded_frame_rx = (decoded_frame_rx + UE->init_sync_frame + trashed_frames) % MAX_FRAME_NUMBER;
        }
        delNotifiedFIFO_elt(res);
        start_rx_stream = 0;
      } else {
        if (IS_SOFTMODEM_IQPLAYER || IS_SOFTMODEM_IQRECORDER) {
          // For IQ recorder/player we force synchronization to happen in 280 ms
          while (trashed_frames != 28) {
            readFrame(UE, &timestamp, PC5, true);
            trashed_frames += 2;
          }
        } else {
          readFrame(UE, &timestamp, PC5, true);
          trashed_frames += SL_NR_PSBCH_REPETITION_IN_FRAMES;
        }
        continue;
      }
    }

    AssertFatal( !syncRunning, "At this point synchronization can't be running\n");

    if (!UE->is_synchronized_sl) {
      readFrame(UE, &timestamp, PC5, false);
      notifiedFIFO_elt_t *Msg = newNotifiedFIFO_elt(sizeof(syncData_t), 0, &nf, UE_synch_sl);
      syncData_t *syncMsg = (syncData_t *)NotifiedFifoData(Msg);
      syncMsg->UE = UE;
      memset(&syncMsg->proc, 0, sizeof(syncMsg->proc));
      pushTpool(&(get_nrUE_params()->Tpool), Msg);
      trashed_frames = 0;
      syncRunning = true;
      continue;
    }

    if (start_rx_stream == 0) {
      start_rx_stream = 1;
      syncInFrame(UE, &timestamp, PC5);
      UE->rx_offset = 0;
      UE->time_sync_cell = 0;
      // read in first symbol
      AssertFatal (fp->ofdm_symbol_size + fp->nb_prefix_samples0 ==
                   UE->rfdevice_sl.trx_read_func(&UE->rfdevice_sl,
                                              &timestamp,
                                              (void **)UE->common_vars.rxdata_sl,
                                              fp->ofdm_symbol_size + fp->nb_prefix_samples0,
                                              fp->nb_antennas_rx),"");

      // we have the decoded frame index in the return of the synch process
      // and we shifted above to the first slot of next frame
      decoded_frame_rx++;
      // we do ++ first in the regular processing, so it will be begin of frame;
      absolute_slot = decoded_frame_rx * nb_slot_frame - 1;
      if (UE->sl_mode == 2) {
        //Set to the slot where the SL-SSB was decoded
        absolute_slot += UE->SL_UE_PHY_PARAMS.sync_params.slot_offset;
      }
      continue;
    }
    if (UE->is_synchronized_sl) {
      if ((UE->sl_mode == 1) || (UE->sl_mode == 2)) {
        pthread_mutex_lock(&sync_notif_sl.m);
        sync_notif_sl.abs_slot = absolute_slot;
        sync_notif_sl.ready = 1;
        pthread_cond_signal(&sync_notif_sl.c); // Notify the waiting thread
        pthread_mutex_unlock(&sync_notif_sl.m);
      }
      break;
    }
  } // while !oai_exit

  return NULL;
}

void *UE_thread(void *arg)
{
  //this thread should be over the processing thread to keep in real time
  PHY_VARS_NR_UE *UE = (PHY_VARS_NR_UE *) arg;
  //  int tx_enabled = 0;
  openair0_timestamp timestamp;
  int start_rx_stream = 0;
  NR_DL_FRAME_PARMS *fp = &UE->frame_parms;

  AssertFatal(0 == openair0_device_load(&(UE->rfdevice), &openair0_cfg[UE->rf_map.card]), "");
  UE->rfdevice.host_type = RAU_HOST;
  UE->is_synchronized = 0;
  AssertFatal(UE->rfdevice.trx_start_func(&UE->rfdevice) == 0, "Could not start the device\n");

  notifiedFIFO_t nf;
  initNotifiedFIFO(&nf);

  NR_UE_MAC_INST_t *mac = get_mac_inst(0);

  bool syncRunning=false;
  const int nb_slot_frame = fp->slots_per_frame;
  int absolute_slot = 0, decoded_frame_rx = INT_MAX, trashed_frames = 0;

  while (!oai_exit) {

    if (syncRunning) {
      notifiedFIFO_elt_t *res=tryPullTpool(&nf,&(get_nrUE_params()->Tpool));

      if (res) {
        syncRunning=false;
        if (UE->is_synchronized) {
          decoded_frame_rx = mac->mib_frame;
          LOG_I(PHY,"UE synchronized decoded_frame_rx=%d UE->init_sync_frame=%d trashed_frames=%d\n",
                decoded_frame_rx,
                UE->init_sync_frame,
                trashed_frames);
          // shift the frame index with all the frames we trashed meanwhile we perform the synch search
          decoded_frame_rx = (decoded_frame_rx + UE->init_sync_frame + trashed_frames) % MAX_FRAME_NUMBER;
        }
        delNotifiedFIFO_elt(res);
        start_rx_stream = 0;
      } else {
        if (IS_SOFTMODEM_IQPLAYER || IS_SOFTMODEM_IQRECORDER) {
          // For IQ recorder/player we force synchronization to happen in 280 ms
          while (trashed_frames != 28) {
            readFrame(UE, &timestamp, UU, true);
            trashed_frames += 2;
          }
        } else {
          readFrame(UE, &timestamp, UU, true);
          trashed_frames += 2;
        }
        continue;
      }
    }

    AssertFatal( !syncRunning, "At this point synchronization can't be running\n");

    if (!UE->is_synchronized) {
      readFrame(UE, &timestamp, UU, false);
      notifiedFIFO_elt_t *Msg = newNotifiedFIFO_elt(sizeof(syncData_t), 0, &nf, UE_synch);
      syncData_t *syncMsg = (syncData_t *)NotifiedFifoData(Msg);
      syncMsg->UE = UE;
      memset(&syncMsg->proc, 0, sizeof(syncMsg->proc));
      pushTpool(&(get_nrUE_params()->Tpool), Msg);
      trashed_frames = 0;
      syncRunning = true;
      continue;
    }

    if (start_rx_stream == 0) {
      start_rx_stream = 1;
      syncInFrame(UE, &timestamp, UU);
      UE->rx_offset = 0;
      UE->time_sync_cell = 0;
      // read in first symbol
      AssertFatal (fp->ofdm_symbol_size + fp->nb_prefix_samples0 ==
                   UE->rfdevice.trx_read_func(&UE->rfdevice,
                                              &timestamp,
                                              (void **)UE->common_vars.rxdata,
                                              fp->ofdm_symbol_size + fp->nb_prefix_samples0,
                                              fp->nb_antennas_rx), "");

      // we have the decoded frame index in the return of the synch process
      // and we shifted above to the first slot of next frame
      decoded_frame_rx++;
      // we do ++ first in the regular processing, so it will be begin of frame;
      absolute_slot = decoded_frame_rx * nb_slot_frame - 1;
      continue;
    }
    if (UE->is_synchronized) {
      if (!sync_notif.ready) {
        pthread_mutex_lock(&sync_notif.m);
        sync_notif.ready = 1;
        sync_notif.abs_slot = absolute_slot;
        pthread_cond_signal(&sync_notif.c); // Notify the waiting thread
        pthread_mutex_unlock(&sync_notif.m);
        if (UE->sl_mode == 0)
          break;
      }
      if (UE->sl_mode == 1 && mac->state == UE_CONNECTED) {
        pthread_t threads_sl;
        threadCreate(&threads_sl, UE_thread_sl, (void *)UE, "UE_thread_sl", -1, OAI_PRIORITY_RT_MAX);
        threadCreate(&threads_sl, UE_RU_thread_sl, (void *)UE, "UE_RU_thread_sl", -1, OAI_PRIORITY_RT_MAX);
        break;
      }
    }
  } // while !oai_exit

  return NULL;
}

void *UE_RU_thread_sl(void *arg)
{
  //this thread should be over the processing thread to keep in real time
  PHY_VARS_NR_UE *UE = (PHY_VARS_NR_UE *) arg;
  openair0_timestamp timestampSl, writeTimestampSl;

  NR_DL_FRAME_PARMS *fp = &UE->SL_UE_PHY_PARAMS.sl_frame_params;

  notifiedFIFO_t txFifoSl;
  initNotifiedFIFO(&txFifoSl);

  int timing_advance_sl = UE->timing_advance_sl;

  const int nb_slot_frame = fp->slots_per_frame;
  int absolute_slot = 0;

  pthread_mutex_lock(&sync_notif_sl.m);
  while (!sync_notif_sl.ready) {
    pthread_cond_wait(&sync_notif_sl.c, &sync_notif_sl.m);
    absolute_slot = sync_notif_sl.abs_slot;
    LOG_W(PHY, "Waiting for Sync for SL\n");
  }
  pthread_mutex_unlock(&sync_notif_sl.m);
  LOG_I(PHY, "Sync Achieved with absolute_slot = %d\n", absolute_slot);

  while (!oai_exit) {

    absolute_slot++;
    if (absolute_slot % 1270 == 0)
      LOG_D(PHY, "RU_thread_sl %d\n", absolute_slot);

    int slot_nr = absolute_slot % nb_slot_frame;
    nr_rxtx_thread_data_t curMsgSl = {0};
    int readBlockSizeSl, writeBlockSizeSl;

    update_curMsg(UE, &curMsgSl, absolute_slot, nb_slot_frame, PC5);
    rfdevice_trx(UE, &UE->rfdevice_sl, UE->common_vars.rxdata_sl, slot_nr, &readBlockSizeSl, &writeBlockSizeSl, &timestampSl, &writeTimestampSl, &timing_advance_sl, PC5);
    UE_processing(UE, &curMsgSl, readBlockSizeSl, writeBlockSizeSl, writeTimestampSl, &txFifoSl, PC5);
  } // while !oai_exit

  return NULL;
}

void *UE_RU_thread(void *arg)
{
  //this thread should be over the processing thread to keep in real time
  PHY_VARS_NR_UE *UE = (PHY_VARS_NR_UE *) arg;
  openair0_timestamp timestamp, writeTimestamp;

  NR_DL_FRAME_PARMS *fp = &UE->frame_parms;

  notifiedFIFO_t txFifo;
  initNotifiedFIFO(&txFifo);

  int timing_advance = UE->timing_advance;

  const int nb_slot_frame = fp->slots_per_frame;
  int absolute_slot = 0;
  initNotifiedFIFO(&UE->phy_config_ind);

  int num_ind_fifo = nb_slot_frame;
  for(int i = 0; i < num_ind_fifo; i++) {
    UE->tx_wait_for_dlsch[num_ind_fifo] = 0;
    UE->tx_resume_ind_fifo[i] = malloc(sizeof(*UE->tx_resume_ind_fifo[i]));
    initNotifiedFIFO(UE->tx_resume_ind_fifo[i]);
  }

  pthread_mutex_lock(&sync_notif.m);
  while (!sync_notif.ready) {
    pthread_cond_wait(&sync_notif.c, &sync_notif.m);
    absolute_slot = sync_notif.abs_slot;
    LOG_I(PHY, "Waiting for Sync\n");
  }
  pthread_mutex_unlock(&sync_notif.m);
  LOG_I(PHY, "Sync Achieved with absolute_slot = %d\n", absolute_slot);

  while (!oai_exit) {

    absolute_slot++;
    if (absolute_slot % 1270 == 0)
      LOG_D(PHY, "RU_thread  %d\n", absolute_slot);

    int slot_nr = absolute_slot % nb_slot_frame;
    nr_rxtx_thread_data_t curMsg = {0};
    int readBlockSize, writeBlockSize;

    update_curMsg(UE, &curMsg, absolute_slot, nb_slot_frame, UU);
    rfdevice_trx(UE, &UE->rfdevice, UE->common_vars.rxdata, slot_nr, &readBlockSize, &writeBlockSize, &timestamp, &writeTimestamp, &timing_advance, UU);
    UE_processing(UE, &curMsg, readBlockSize, writeBlockSize, writeTimestamp, &txFifo, UU);
  } // while !oai_exit

  return NULL;
}

void update_curMsg(PHY_VARS_NR_UE *UE, nr_rxtx_thread_data_t *curMsg, int absolute_slot, const int nb_slot_frame,
  nr_intf_type_t intf_type) {
  // update thread index for received subframe
  int slot_nr    = absolute_slot % nb_slot_frame;
  fapi_nr_config_request_t *cfg = &UE->nrUE_config;
  curMsg->UE = UE;
  curMsg->proc.nr_slot_rx  = slot_nr;
  curMsg->proc.nr_slot_tx  = (absolute_slot + DURATION_RX_TO_TX) % nb_slot_frame;
  curMsg->proc.frame_rx    = (absolute_slot / nb_slot_frame) % MAX_FRAME_NUMBER;
  curMsg->proc.frame_tx    = ((absolute_slot + DURATION_RX_TO_TX) / nb_slot_frame) % MAX_FRAME_NUMBER;
  if (UE->phy_config_request_sent) {
    if (intf_type == PC5) {
      NR_UE_MAC_INST_t *mac = get_mac_inst(UE->Mod_id);
      uint8_t pool_id = 0;
      // Temporarily setting this to this initial NON_NR_SIDELINK_SLOT slot type.
      // Later we should properly determine if the current slot is an NR_DOWNLINK_SLOT, NR_UPLINK_SLOT, or NR_MIXED_SLOT
      curMsg->proc.tx_slot_type = NON_NR_SIDELINK_SLOT;
      curMsg->proc.rx_slot_type = NON_NR_SIDELINK_SLOT;

      SL_ResourcePool_params_t *sl_tx_rsrc_pool = mac->SL_MAC_PARAMS->sl_TxPool[pool_id];
      uint16_t phy_map_sz_tx = ((sl_tx_rsrc_pool->phy_sl_bitmap.size << 3) - sl_tx_rsrc_pool->phy_sl_bitmap.bits_unused);
      bool sl_tx_slot = is_sl_slot(mac, &sl_tx_rsrc_pool->phy_sl_bitmap, phy_map_sz_tx, absolute_slot + DURATION_RX_TO_TX);
      if (sl_tx_slot) {
        frameslot_t frame_slot_tx;
        frame_slot_tx.frame = curMsg->proc.frame_tx;
        frame_slot_tx.slot = curMsg->proc.nr_slot_tx;
        LOG_D(NR_PHY, "Setting 1 SL slot type to TX %d.%d %d\n",
            curMsg->proc.frame_tx, curMsg->proc.nr_slot_tx, curMsg->proc.tx_slot_type);
        if (is_selected_sl_slot(true, false, mac->SL_MAC_PARAMS->sl_TDD_config, frame_slot_tx))
          curMsg->proc.tx_slot_type = NR_SIDELINK_SLOT;
      }

      SL_ResourcePool_params_t *sl_rx_rsrc_pool = mac->SL_MAC_PARAMS->sl_RxPool[pool_id];
      uint16_t phy_map_sz_rx = ((sl_rx_rsrc_pool->phy_sl_bitmap.size << 3) - sl_rx_rsrc_pool->phy_sl_bitmap.bits_unused);
      bool sl_rx_slot = is_sl_slot(mac, &sl_rx_rsrc_pool->phy_sl_bitmap, phy_map_sz_rx, absolute_slot);
      if (sl_rx_slot) {
        frameslot_t frame_slot_rx;
        frame_slot_rx.frame = curMsg->proc.frame_rx;
        frame_slot_rx.slot = curMsg->proc.nr_slot_rx;
        LOG_D(NR_PHY, "Setting 2 SL slot type to RX %d.%d %d\n",
            curMsg->proc.frame_rx, curMsg->proc.nr_slot_rx, curMsg->proc.rx_slot_type);
        if (is_selected_sl_slot(false, true, mac->SL_MAC_PARAMS->sl_TDD_config, frame_slot_rx))
          curMsg->proc.rx_slot_type = NR_SIDELINK_SLOT;
      }

      LOG_D(NR_PHY, "Setting SL slot type to TX %d.%d %d, RX %d.%d %d\n",
            curMsg->proc.frame_tx, curMsg->proc.nr_slot_tx, curMsg->proc.tx_slot_type, curMsg->proc.frame_rx, curMsg->proc.nr_slot_rx, curMsg->proc.rx_slot_type);
    } else {
      curMsg->proc.rx_slot_type = nr_ue_slot_select(cfg, curMsg->proc.frame_rx, curMsg->proc.nr_slot_rx);
      curMsg->proc.tx_slot_type = nr_ue_slot_select(cfg, curMsg->proc.frame_tx, curMsg->proc.nr_slot_tx);
    }
  }
  else {
    curMsg->proc.rx_slot_type = NR_DOWNLINK_SLOT;
    curMsg->proc.tx_slot_type = NR_DOWNLINK_SLOT;
  }
}

void rfdevice_trx(PHY_VARS_NR_UE *UE, openair0_device *rfdevice, c16_t **rxdata, int slot_nr, int *rdBlkSz, int *wtBlkSz, openair0_timestamp *ts, openair0_timestamp *wts, int *ta, nr_intf_type_t intf_type) {
  void *rxp[NB_ANTENNAS_RX];
  openair0_timestamp timestamp, writeTimestamp;
  int timing_advance = *ta;
  NR_DL_FRAME_PARMS *fp = (intf_type == UU) ? &UE->frame_parms : &UE->SL_UE_PHY_PARAMS.sl_frame_params;
  bool apply_timing_offset = (intf_type == UU) ? UE->apply_timing_offset : UE->apply_timing_offset_sl;
  const int nb_slot_frame = fp->slots_per_frame;

  int firstSymSamp = get_firstSymSamp(slot_nr, fp);
  for (int i=0; i<fp->nb_antennas_rx; i++)
    rxp[i] = (void *)&rxdata[i][firstSymSamp+
                          fp->get_samples_slot_timestamp(slot_nr, fp, 0)];

  int readBlockSize, writeBlockSize;

  readBlockSize = get_readBlockSize(slot_nr, fp);
  writeBlockSize = fp->get_samples_per_slot((slot_nr + DURATION_RX_TO_TX) % nb_slot_frame, fp);
  if (apply_timing_offset && (slot_nr == nb_slot_frame - 1)) {
    const int sampShift = -(UE->rx_offset>>1);
    readBlockSize -= sampShift;
    writeBlockSize -= sampShift;
    if (intf_type == PC5)
      UE->apply_timing_offset_sl = false;
    else
      UE->apply_timing_offset = false;
  }

  LOG_D(PHY, "fn rfdevice_trx line %u\n", __LINE__);
  AssertFatal(readBlockSize ==
              rfdevice->trx_read_func(rfdevice,
                                      &timestamp,
                                      rxp,
                                      readBlockSize,
                                      fp->nb_antennas_rx), "");
  if(slot_nr == (nb_slot_frame - 1)) {
    // read in first symbol of next frame and adjust for timing drift
    int first_symbols = fp->ofdm_symbol_size + fp->nb_prefix_samples0; // first symbol of every frames

    if (first_symbols > 0) {
      openair0_timestamp ignore_timestamp;
      AssertFatal(first_symbols ==
                  rfdevice->trx_read_func(rfdevice,
                                          &ignore_timestamp,
                                          (void **)rxdata,
                                          first_symbols,
                                          fp->nb_antennas_rx), "");
    } else
      LOG_E(PHY,"can't compensate: diff =%d\n", first_symbols);
  }

  if(intf_type == UU) {
    // use previous timing_advance value to compute writeTimestamp
    writeTimestamp = timestamp +
      fp->get_samples_slot_timestamp(slot_nr, fp, DURATION_RX_TO_TX)
      - firstSymSamp - openair0_cfg[UE->rf_map.card].tx_sample_advance -
      UE->N_TA_offset - timing_advance;

    // but use current UE->timing_advance value to compute writeBlockSize
    if (UE->timing_advance != timing_advance) {
      writeBlockSize -= UE->timing_advance - timing_advance;
      timing_advance = UE->timing_advance;
    }
  }
  else {
    // use previous timing_advance value to compute writeTimestamp
    writeTimestamp = timestamp +
      fp->get_samples_slot_timestamp(slot_nr, fp, DURATION_RX_TO_TX)
      - firstSymSamp - openair0_cfg[UE->rf_map_sl.card].tx_sample_advance -
      UE->N_TA_offset_sl - timing_advance;

    // but use current UE->timing_advance value to compute writeBlockSize
    if (UE->timing_advance_sl != timing_advance) {
      writeBlockSize -= UE->timing_advance_sl - timing_advance;
      timing_advance = UE->timing_advance_sl;
    }
  }
  *rdBlkSz = readBlockSize;
  *wtBlkSz = writeBlockSize;
  *ts = timestamp;
  *wts = writeTimestamp;
  *ta = timing_advance;
}

void UE_processing(PHY_VARS_NR_UE *UE, nr_rxtx_thread_data_t *curMsg, int readBlockSize, int writeBlockSize, openair0_timestamp writeTimestamp, notifiedFIFO_t *txFifo, nr_intf_type_t intf_type) {
  nr_ue_rrc_timer_trigger(UE->Mod_id, curMsg->proc.frame_tx, curMsg->proc.nr_slot_tx, curMsg->proc.gNB_id);

  // Start TX slot processing here. It runs in parallel with RX slot processing
  notifiedFIFO_elt_t *newElt = newNotifiedFIFO_elt(sizeof(nr_rxtx_thread_data_t), curMsg->proc.nr_slot_tx, txFifo, processSlotTX);
  nr_rxtx_thread_data_t *curMsgTx = (nr_rxtx_thread_data_t *) NotifiedFifoData(newElt);
  curMsgTx->proc = curMsg->proc;
  curMsgTx->writeBlockSize = writeBlockSize;
  curMsgTx->proc.timestamp_tx = writeTimestamp;
  curMsgTx->UE = UE;
  curMsgTx->tx_wait_for_dlsch = UE->tx_wait_for_dlsch[curMsgTx->proc.nr_slot_tx];
  curMsgTx->intf_type = intf_type;
  UE->tx_wait_for_dlsch[curMsgTx->proc.nr_slot_tx] = 0;
  pushTpool(&(get_nrUE_params()->Tpool), newElt);

  // RX slot processing. We launch and forget.
  newElt = newNotifiedFIFO_elt(sizeof(nr_rxtx_thread_data_t), curMsg->proc.nr_slot_rx, NULL, UE_dl_processing);
  nr_rxtx_thread_data_t *curMsgRx = (nr_rxtx_thread_data_t *) NotifiedFifoData(newElt);
  curMsgRx->proc = curMsg->proc;
  curMsgRx->UE = UE;
  curMsgRx->phy_data = UE_dl_preprocessing(UE, &curMsg->proc, intf_type);
  pushTpool(&(get_nrUE_params()->Tpool), newElt);

  // Wait for TX slot processing to finish
  notifiedFIFO_elt_t *res;
  res = pullTpool(txFifo, &(get_nrUE_params()->Tpool));
  if (res == NULL)
    LOG_E(PHY, "Tpool has been aborted\n");
  else
    delNotifiedFIFO_elt(res);
}

void init_NR_UE(int nb_inst,
                char* uecap_file,
                char* rrc_config_path,
                ueinfo_t* ueinfo) {
  int inst;
  NR_UE_MAC_INST_t *mac_inst;
  NR_UE_RRC_INST_t* rrc_inst;

  for (inst=0; inst < nb_inst; inst++) {
    AssertFatal((rrc_inst = nr_l3_init_ue(uecap_file,rrc_config_path)) != NULL, "can not initialize RRC module\n");
    AssertFatal((mac_inst = nr_l2_init_ue(rrc_inst, ueinfo)) != NULL, "can not initialize L2 module\n");
    AssertFatal((mac_inst->if_module = nr_ue_if_module_init(inst)) != NULL, "can not initialize IF module\n");
  }
  if (get_softmodem_params()->sl_mode) {
    configure_NR_SL_Preconfig(0, get_nrUE_params()->sync_ref);
  }
}

void init_NR_UE_threads(int nb_inst) {
  int inst;

  pthread_t threads[nb_inst];
  pthread_t threads_sl[nb_inst];
  uint8_t sl_mode = get_softmodem_params()->sl_mode;

  for (inst = 0; inst < nb_inst; inst++) {
    PHY_VARS_NR_UE *UE = PHY_vars_UE_g[inst][0];

    LOG_I(PHY,"Intializing UE Threads for instance %d (%p,%p)...\n",inst,PHY_vars_UE_g[inst],PHY_vars_UE_g[inst][0]);
    if (sl_mode == 0 || sl_mode == 1) {
      threadCreate(&threads[inst], UE_thread, (void *)UE, "UE_thread", -1, OAI_PRIORITY_RT_MAX);
      threadCreate(&threads[inst], UE_RU_thread, (void *)UE, "UE_RU_thread", -1, OAI_PRIORITY_RT_MAX);
    }
    else {
      threadCreate(&threads_sl[inst], UE_thread_sl, (void *)UE, "UE_thread_sl", -1, OAI_PRIORITY_RT_MAX);
      threadCreate(&threads_sl[inst], UE_RU_thread_sl, (void *)UE, "UE_RU_thread_sl", -1, OAI_PRIORITY_RT_MAX);
    }
    if (!IS_SOFTMODEM_NOSTATS_BIT) {
      pthread_t stat_pthread;
      threadCreate(&stat_pthread, nrL1_UE_stats_thread, UE, "L1_UE_stats", -1, OAI_PRIORITY_RT_LOW);
    }
  }
}
