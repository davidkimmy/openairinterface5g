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

/* \file main_ue_nr.c
 * \brief top init of Layer 2
 * \author R. Knopp, K.H. HSU
 * \date 2018
 * \version 0.1
 * \company Eurecom / NTUST
 * \email: knopp@eurecom.fr, kai-hsiang.hsu@eurecom.fr
 * \note
 * \warning
 */

//#include "defs.h"
#include "mac_proto.h"
#include "radio/COMMON/common_lib.h"
//#undef MALLOC
#include "assertions.h"
#include "executables/softmodem-common.h"
#include "executables/nr-uesoftmodem.h"
#include "openair2/LAYER2/nr_pdcp/nr_pdcp.h"
#include "openair2/LAYER2/nr_pdcp/nr_pdcp_oai_api.h"
#include "nr_rlc/nr_rlc_oai_api.h"
#include "RRC/NR/MESSAGES/asn1_msg.h"
#include <pthread.h>

static NR_UE_MAC_INST_t *nr_ue_mac_inst; 

NR_UE_MAC_INST_t* nr_l2_init_ue(NR_UE_RRC_INST_t* rrc_inst, ueinfo_t* ueinfo) {

    //LOG_I(MAC, "[MAIN] MAC_INIT_GLOBAL_PARAM IN...\n");

    //LOG_I(MAC, "[MAIN] init UE MAC functions \n");
    
    //init mac here
    nr_ue_mac_inst = (NR_UE_MAC_INST_t *)calloc(sizeof(NR_UE_MAC_INST_t), NB_NR_UE_MAC_INST);

    for (int j = 0; j < NB_NR_UE_MAC_INST; j++)
      nr_ue_init_mac(j, ueinfo);

    int scs = get_softmodem_params()->sa ?
              get_softmodem_params()->numerology :
              rrc_inst && rrc_inst->scell_group_config ?
              *rrc_inst->scell_group_config->spCellConfig->reconfigurationWithSync->spCellConfigCommon->ssbSubcarrierSpacing :
              - 1;
    if (scs > -1)
      ue_init_config_request(nr_ue_mac_inst, scs);

    if (rrc_inst && rrc_inst->scell_group_config) {

      nr_rrc_mac_config_req_scg(0, 0, rrc_inst->scell_group_config);
      AssertFatal(rlc_module_init(0) == 0, "%s: Could not initialize RLC layer\n", __FUNCTION__);
      if (IS_SOFTMODEM_NOS1){
        // get default noS1 configuration
        NR_RadioBearerConfig_t *rbconfig = NULL;
        NR_RLC_BearerConfig_t *rlc_rbconfig = NULL;
        fill_nr_noS1_bearer_config(&rbconfig, &rlc_rbconfig);
        struct NR_CellGroupConfig__rlc_BearerToAddModList rlc_bearer_list = {
          .list = { .array = &rlc_rbconfig, .count = 1, .size = 1, }
        };

        // set up PDCP, RLC, MAC
        bool gNB_flag = false;
        nr_pdcp_layer_init(gNB_flag);
        nr_pdcp_add_drbs(ENB_FLAG_NO, nr_ue_mac_inst->crnti, 0, rbconfig->drb_ToAddModList, 0, NULL, NULL, &rlc_bearer_list, false); // noS1 mode, not Remote UE
        nr_rlc_add_drb(nr_ue_mac_inst->crnti, rbconfig->drb_ToAddModList->list.array[0]->drb_Identity, rlc_rbconfig);
        nr_ue_mac_inst->logicalChannelBearer_exist[0] = true;

        // free memory
        free_nr_noS1_bearer_config(&rbconfig, &rlc_rbconfig);
      }
    }
    else {
      LOG_I(MAC,"Running without CellGroupConfig\n");
      if(get_softmodem_params()->sa == 1) {
        AssertFatal(rlc_module_init(0) == 0, "%s: Could not initialize RLC layer\n", __FUNCTION__);
      }
    }

    reset_meas(&nr_ue_mac_inst->rlc_data_req);
    return (nr_ue_mac_inst);
}

NR_UE_MAC_INST_t *get_mac_inst(module_id_t module_id){
    return &nr_ue_mac_inst[(int)module_id];
}

size_t dump_mac_stats_sl(NR_UE_MAC_INST_t *mac, char *output, size_t strlen, bool reset_rsrp)
{
  const char *begin = output;
  const char *end = output + strlen;
  sl_nr_ue_mac_params_t *sl_mac = mac->SL_MAC_PARAMS;

  /* this function is called from gNB_dlsch_ulsch_scheduler(), so assumes the
   * scheduler to be locked*/
  // NR_UE_SL_SCHED_ENSURE_LOCKED(&mac->sl_sched_lock);

  NR_UE_SL_SCHED_LOCK(&mac->sl_info.mutex);
  SL_UE_iterator(mac->sl_info.list, UE) {
    NR_SL_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
    NR_UE_sl_mac_stats_t *stats = &UE->mac_sl_stats;

    if(sched_ctrl->print_csi_report) {
      output += snprintf(output,
                         end - output,
                         "UE %04x: CQI %d, RI %d\n",
                         UE->uid,
                         sched_ctrl->rx_csi_report.CQI,
                         sched_ctrl->rx_csi_report.RI + 1);
      sched_ctrl->print_csi_report = false;
    }

    output += snprintf(output, end - output, "%"PRIu64, stats->sl.rounds[0]);
    for (int i = 1; i < sl_mac->sl_bler.harq_round_max; i++)
      output += snprintf(output, end - output, "/%"PRIu64, stats->sl.rounds[i]);

    output += snprintf(output,
                       end - output,
                       ", slsch_errors %"PRIu64", BLER %.5f MCS %d\n",
                       stats->sl.errors,
                       sched_ctrl->sl_bler_stats.bler,
                       sched_ctrl->sl_bler_stats.mcs);

    output += snprintf(output,
                       end - output,
                       "UE %04x: slsch_total_bytes %"PRIu64"\n",
                       UE->uid, stats->sl.total_bytes);
    output += snprintf(output,
                       end - output,
                       "UE %04x: slsch_rounds ", UE->uid);
    output += snprintf(output, end - output, "%"PRIu64, stats->sl.rounds[0]);
    for (int i = 1; i < sl_mac->sl_bler.harq_round_max; i++)
      output += snprintf(output, end - output, "/%"PRIu64, stats->sl.rounds[i]);

    output += snprintf(output,
                       end - output,
                       ", slsch_DTX %d, slsch_errors %"PRIu64", BLER %.5f MCS %d\n",
                       stats->slsch_DTX,
                       stats->sl.errors,
                       sched_ctrl->sl_bler_stats.bler,
                       sched_ctrl->sl_bler_stats.mcs);
    output += snprintf(output,
                       end - output,
                       "UE %04x: slsch_total_bytes_scheduled %"PRIu64", slsch_total_bytes_received %"PRIu64"\n",
                       UE->uid,
                       stats->slsch_total_bytes_scheduled, stats->sl.total_bytes);

    for (int lc_id = 0; lc_id < 63; lc_id++) {
      if (stats->sl.lc_bytes[lc_id] > 0)
        output += snprintf(output,
                           end - output,
                           "UE %04x: LCID %d: %"PRIu64" bytes TX\n",
                           UE->uid,
                           lc_id,
                           stats->sl.lc_bytes[lc_id]);
      if (stats->sl.lc_bytes[lc_id] > 0)
        output += snprintf(output,
                           end - output,
                           "UE %04x: LCID %d: %"PRIu64" bytes RX\n",
                           UE->uid,
                           lc_id,
                           stats->sl.lc_bytes[lc_id]);
    }
  }
  NR_UE_SL_SCHED_UNLOCK(&mac->sl_info.mutex);
  return output - begin;
}