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

/*! \file rrc_gNB.c
 * \brief rrc procedures for gNB
 * \author Navid Nikaein and  Raymond Knopp , WEI-TAI CHEN
 * \date 2011 - 2014 , 2018
 * \version 1.0
 * \company Eurecom, NTUST
 * \email: navid.nikaein@eurecom.fr and raymond.knopp@eurecom.fr, kroempa@gmail.com
 */
#define RRC_GNB_C
#define RRC_GNB_C

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "nr_rrc_config.h"
#include "nr_rrc_defs.h"
#include "nr_rrc_extern.h"
#include "assertions.h"
#include "common/ran_context.h"
#include "oai_asn1.h"
#include "rrc_gNB_radio_bearers.h"

#include "RRC/L2_INTERFACE/openair_rrc_L2_interface.h"
#include "LAYER2/RLC/rlc.h"
#include "LAYER2/NR_MAC_gNB/mac_proto.h"
#include "LAYER2/NR_MAC_COMMON/nr_mac_common.h"
#include "common/utils/LOG/log.h"
#include "COMMON/mac_rrc_primitives.h"
#include "RRC/NR/MESSAGES/asn1_msg.h"

#include "NR_BCCH-BCH-Message.h"
#include "NR_UL-DCCH-Message.h"
#include "NR_DL-DCCH-Message.h"
#include "NR_DL-CCCH-Message.h"
#include "NR_UL-CCCH-Message.h"
#include "NR_RRCReject.h"
#include "NR_RejectWaitTime.h"
#include "NR_RRCSetup.h"

#include "NR_CellGroupConfig.h"
#include "NR_MeasResults.h"
#include "NR_UL-CCCH-Message.h"
#include "NR_RRCSetupRequest-IEs.h"
#include "NR_RRCSetupComplete-IEs.h"
#include "NR_RRCReestablishmentRequest-IEs.h"
#include "NR_MIB.h"
#include "uper_encoder.h"
#include "uper_decoder.h"

#include "rlc.h"
#include "platform_types.h"
#include "common/utils/LOG/vcd_signal_dumper.h"

#include "T.h"

#include "RRC/NAS/nas_config.h"
#include "RRC/NAS/rb_config.h"

#include "openair3/SECU/secu_defs.h"

#include "rrc_gNB_NGAP.h"

#include "rrc_gNB_GTPV1U.h"

#include "nr_pdcp/nr_pdcp_entity.h"
#include "nr_pdcp/nr_pdcp_oai_api.h"

#include "LAYER2/nr_srap/nr_srap_header.h"

#include "intertask_interface.h"
#include "SIMULATION/TOOLS/sim.h" // for taus

#include "executables/softmodem-common.h"
#include <openair2/RRC/NR/rrc_gNB_UE_context.h>
#include <openair2/X2AP/x2ap_eNB.h>
#include <openair3/SECU/key_nas_deriver.h>
#include <openair3/NAS/NR_UE/nr_nas_msg_sim.h>  // For NAS message-type macros (REGISTRATION_ACCEPT)
#include <openair3/ocp-gtpu/gtp_itf.h>
#include <openair2/RRC/NR/nr_rrc_proto.h>
#include "openair2/LAYER2/nr_pdcp/nr_pdcp_e1_api.h"
#include "openair2/F1AP/f1ap_common.h"
#include "openair2/SDAP/nr_sdap/nr_sdap_entity.h"
#include "cucp_cuup_if.h"

#include "BIT_STRING.h"
#include "assertions.h"

//#define XER_PRINT

extern RAN_CONTEXT_t RC;

static inline uint64_t bitStr_to_uint64(BIT_STRING_t *asn);

// Forward declaration
int rrc_gNB_decode_dcch(const protocol_ctxt_t *const ctxt_pP,
                        const rb_id_t Srb_id,
                        const uint8_t *const Rx_sdu,
                        const sdu_size_t sdu_sizeP);

mui_t rrc_gNB_mui = 0;

///---------------------------------------------------------------------------------------------------------------///
///---------------------------------------------------------------------------------------------------------------///

NR_DRB_ToAddModList_t *fill_DRB_configList(gNB_RRC_UE_t *ue)
{
  gNB_RRC_INST *rrc = RC.nrrrc[0];
  if (ue->nb_of_pdusessions == 0)
    return NULL;
  int nb_drb_to_setup = rrc->configuration.drbs;

  /* Remote UE: Only create one DRB (no duplicate for relay forwarding)
     Remote UE uses SRAP layer for relay, doesn't need multiple DRBs per PDU session */
  if (ue->is_remote_ue && nb_drb_to_setup > 1) {
    LOG_D(NR_RRC, "[Remote UE] Limiting nb_drb_to_setup from %d to 1 (Remote UE uses SRAP for relay)\n", nb_drb_to_setup);
    nb_drb_to_setup = 1;
  }

  /* Relay UE: Force 2 DRBs if relay_type is U2N
     DRB 1: Relay UE's own traffic (LCID 4)
     DRB 2: Remote UE relayed traffic (LCID 5)
     Check if THIS UE is a Relay UE (not Remote UE, even if gNB is in relay mode) */
  bool is_relay_ue = (!ue->is_remote_ue && get_softmodem_params()->relay_type == U2N);

  if (is_relay_ue && nb_drb_to_setup < 2) {
    LOG_D(NR_RRC, "[Relay UE] RNTI 0x%04x: Forcing nb_drb_to_setup to 2 for L2 relay (was %d)\n",
          ue->rnti, nb_drb_to_setup);
    nb_drb_to_setup = 2;
  }

  long drb_priority[MAX_DRBS_PER_UE] = {0};
  uint8_t drb_id_to_setup_start = 0;
  NR_DRB_ToAddModList_t *DRB_configList = CALLOC(sizeof(*DRB_configList), 1);
  for (int i = 0; i < ue->nb_of_pdusessions; i++) {
    if (ue->pduSession[i].status >= PDU_SESSION_STATUS_DONE) {
      continue;
    }
    LOG_D(NR_RRC, "adding rnti %x pdusession %d, nb drb %d, nb_qos %d\n", ue->rnti, ue->pduSession[i].param.pdusession_id, nb_drb_to_setup, ue->pduSession[i].param.nb_qos);

    /* For Relay UE: process each DRB ID sequentially (1, then 2)
       For normal UE: use drb_iterations count from nb_drb_to_setup */
    int drb_iterations = is_relay_ue ? 2 : nb_drb_to_setup;

    for (long drb_id_add = 1; drb_id_add <= drb_iterations; drb_id_add++) {
      uint8_t drb_id;

      // Relay UE: DRB 1 = own traffic (from QoS), DRB 2 = Remote UE relay (explicit)
      if (is_relay_ue && drb_id_add == 2) {
        // Skip QoS flow processing for DRB 2, will create explicitly after loop
        continue;
      }

      // Reference TS23501 Table 5.7.4-1: Standardized 5QI to QoS characteristics mapping
      for (int qos_flow_index = 0; qos_flow_index < ue->pduSession[i].param.nb_qos; qos_flow_index++) {
        switch (ue->pduSession[i].param.qos[qos_flow_index].fiveQI) {
          case 1 ... 4: /* GBR */
            drb_id = next_available_drb(ue, &ue->pduSession[i], GBR_FLOW);
            break;
          case 5 ... 9: /* Non-GBR */
            if (rrc->configuration.drbs > 1) { /* Force the creation from gNB Conf file */
              LOG_W(NR_RRC, "Adding %d DRBs, from gNB config file (not decided by 5GC\n", rrc->configuration.drbs);
              drb_id = next_available_drb(ue, &ue->pduSession[i], GBR_FLOW);
            } else {
              drb_id = next_available_drb(ue, &ue->pduSession[i], NONGBR_FLOW);
            }
            break;

          default:
            LOG_E(NR_RRC, "not supported 5qi %lu\n", ue->pduSession[i].param.qos[qos_flow_index].fiveQI);
            ue->pduSession[i].status = PDU_SESSION_STATUS_FAILED;
            continue;
        }
        drb_priority[drb_id - 1] = ue->pduSession[i].param.qos[qos_flow_index].allocation_retention_priority.priority_level;
        if (drb_priority[drb_id - 1] < 0 || drb_priority[drb_id - 1] > NGAP_PRIORITY_LEVEL_NO_PRIORITY) {
          LOG_E(NR_RRC, "invalid allocation_retention_priority.priority_level %ld set to _NO_PRIORITY\n", drb_priority[drb_id - 1]);
          drb_priority[drb_id - 1] = NGAP_PRIORITY_LEVEL_NO_PRIORITY;
        }

        if (drb_is_active(ue, drb_id)) { /* Non-GBR flow using the same DRB or a GBR flow with no available DRBs*/
          nb_drb_to_setup--;
        } else {
          generateDRB(ue,
                      drb_id,
                      &ue->pduSession[i],
                      rrc->configuration.enable_sdap,
                      rrc->security.do_drb_integrity,
                      rrc->security.do_drb_ciphering);
          NR_DRB_ToAddMod_t *DRB_config = generateDRB_ASN1(&ue->established_drbs[drb_id - 1]);
          if (drb_id_to_setup_start == 0)
            drb_id_to_setup_start = DRB_config->drb_Identity;
          asn1cSeqAdd(&DRB_configList->list, DRB_config);
        }
        LOG_D(RRC, "DRB Priority %ld\n", drb_priority[drb_id]); // To supress warning for now
      }
    }

  }

  // Relay UE: Explicitly create DRB 2 for Remote UE traffic (outside PDU session loop to avoid duplicates)
  if (is_relay_ue && !drb_is_active(ue, 2) && DRB_configList->list.count > 0) {
    // Use first PDU session for DRB 2 configuration
    int pdu_idx = 0;
    for (int i = 0; i < ue->nb_of_pdusessions; i++) {
      if (ue->pduSession[i].status < PDU_SESSION_STATUS_DONE) {
        pdu_idx = i;
        break;
      }
    }

    LOG_D(NR_RRC, "[Relay UE] RNTI 0x%04x: Creating DRB 2 for Remote UE relayed traffic\n", ue->rnti);
    drb_priority[1] = ue->pduSession[pdu_idx].param.qos[0].allocation_retention_priority.priority_level;
    generateDRB(ue,
                2,  // DRB ID = 2
                &ue->pduSession[pdu_idx],
                rrc->configuration.enable_sdap,
                rrc->security.do_drb_integrity,
                rrc->security.do_drb_ciphering);

    /* DRB 2 is for relay forwarding via SRAP, NOT for QFI-based routing
       Clear QoS flow mappings to prevent SDAP from routing to DRB 2 */
    for (int qfi_idx = 0; qfi_idx < QOSFLOW_MAX_VALUE; qfi_idx++) {
      ue->established_drbs[1].cnAssociation.sdap_config.mappedQoS_FlowsToAdd[qfi_idx] = 0;
    }
    LOG_D(NR_RRC, "[Relay UE] RNTI 0x%04x: Cleared QoS flow mappings for DRB 2 (relay forwarding only)\n", ue->rnti);

    NR_DRB_ToAddMod_t *DRB_config = generateDRB_ASN1(&ue->established_drbs[1]);
    asn1cSeqAdd(&DRB_configList->list, DRB_config);

    // Also add the corresponding RLC bearer to masterCellGroup
    if (ue->masterCellGroup && ue->masterCellGroup->rlc_BearerToAddModList) {
      // Use AM mode for relay DRB (same as DRB 1)
      const NR_RLC_Config_PR rlc_conf = NR_RLC_Config_PR_am;
      NR_RLC_BearerConfig_t *rlc_BearerConfig = get_DRB_RLC_BearerConfig(5, 2, rlc_conf, drb_priority[1]);
      asn1cSeqAdd(&ue->masterCellGroup->rlc_BearerToAddModList->list, rlc_BearerConfig);
      LOG_D(NR_RRC, "[Relay UE] RNTI 0x%04x: Added RLC bearer for DRB 2 (LCID 5) to masterCellGroup\n", ue->rnti);
    }
  }
  if (DRB_configList->list.count == 0) {
    free(DRB_configList);
    return NULL;
  }
  return DRB_configList;
}

static void freeDRBlist(NR_DRB_ToAddModList_t *list)
{
  //ASN_STRUCT_FREE(asn_DEF_NR_DRB_ToAddModList, list);
  return;
}
static void nr_rrc_addmod_srbs(int rnti,
                               const NR_SRB_INFO_TABLE_ENTRY *srb_list,
                               const int nb_srb,
                               const struct NR_CellGroupConfig__rlc_BearerToAddModList *bearer_list)
{
  if (srb_list == NULL || bearer_list == NULL)
    return;

  for (int i = 0; i < nb_srb; i++) {
    if (srb_list[i].Active)
      for (int j = 0; j < bearer_list->list.count; j++) {
        const NR_RLC_BearerConfig_t *bearer = bearer_list->list.array[j];
        if (bearer->servedRadioBearer != NULL
            && bearer->servedRadioBearer->present == NR_RLC_BearerConfig__servedRadioBearer_PR_srb_Identity
            && i == bearer->servedRadioBearer->choice.srb_Identity) {
          nr_rlc_add_srb(rnti, i, bearer);
        }
      }
  }
}

static void nr_rrc_addmod_drbs(int rnti,
                               const NR_DRB_ToAddModList_t *drb_list,
                               const struct NR_CellGroupConfig__rlc_BearerToAddModList *bearer_list)
{
  if (drb_list == NULL || bearer_list == NULL)
    return;

  // Check if this is a Relay UE
  bool is_relay_ue = (get_softmodem_params()->relay_type == U2N);

  for (int i = 0; i < drb_list->list.count; i++) {
    const NR_DRB_ToAddMod_t *drb = drb_list->list.array[i];
    for (int j = 0; j < bearer_list->list.count; j++) {
      const NR_RLC_BearerConfig_t *bearer = bearer_list->list.array[j];
      if (bearer->servedRadioBearer != NULL
          && bearer->servedRadioBearer->present == NR_RLC_BearerConfig__servedRadioBearer_PR_drb_Identity
          && drb->drb_Identity == bearer->servedRadioBearer->choice.drb_Identity) {

        /* Configure DRB at RLC layer
           For Relay UE DRB2, runtime routing in deliver_sdu handles SRAP forwarding */
        bool created = nr_rlc_add_drb(rnti, drb->drb_Identity, bearer);

        /* Mark that Relay UE DRB2 needs RRCReconfiguration to reset RLC entity
           Only if entity was newly created (prevents infinite loop) */
        if (is_relay_ue && drb->drb_Identity == 2 && created) {
          rrc_gNB_ue_context_t *ue_context = rrc_gNB_get_ue_context_by_rnti(RC.nrrrc[0], rnti);
          if (ue_context) {
            ue_context->ue_context.relay_drb2_needs_reconfig = true;
            LOG_D(NR_RRC, "[Relay UE] RNTI 0x%04x: DRB 2 entity created - marked for RLC entity reset notification\n", rnti);
          }
        }
      }
    }
  }
}


///---------------------------------------------------------------------------------------------------------------///
///---------------------------------------------------------------------------------------------------------------///

static void init_NR_SI(gNB_RRC_INST *rrc, gNB_RrcConfigurationReq *configuration)
{

  LOG_D(RRC,"%s()\n\n\n\n",__FUNCTION__);
  if (NODE_IS_DU(rrc->node_type) || NODE_IS_MONOLITHIC(rrc->node_type))
    rrc->carrier.mib = get_new_MIB_NR(rrc->carrier.servingcellconfigcommon);

  if((get_softmodem_params()->sa) && ( (NODE_IS_DU(rrc->node_type) || NODE_IS_MONOLITHIC(rrc->node_type)))) {
    NR_BCCH_DL_SCH_Message_t *sib1 = get_SIB1_NR(configuration);
    rrc->carrier.SIB1 = calloc(NR_MAX_SIB_LENGTH / 8, sizeof(*rrc->carrier.SIB1));
    AssertFatal(rrc->carrier.SIB1 != NULL, "out of memory\n");
    rrc->carrier.sizeof_SIB1 = encode_SIB1_NR(sib1, rrc->carrier.SIB1, NR_MAX_SIB_LENGTH / 8);
    rrc->carrier.siblock1 = sib1;
    nr_mac_config_sib1(RC.nrmac[rrc->module_id], sib1);
  }

  if (!NODE_IS_DU(rrc->node_type)) {
    rrc->carrier.SIB23 = (uint8_t *) malloc16(100);
    AssertFatal(rrc->carrier.SIB23 != NULL, "cannot allocate memory for SIB");
    rrc->carrier.sizeof_SIB23 = do_SIB23_NR(&rrc->carrier, configuration);
    LOG_I(NR_RRC,"do_SIB23_NR, size %d \n ", rrc->carrier.sizeof_SIB23);
    AssertFatal(rrc->carrier.sizeof_SIB23 != 255,"FATAL, RC.nrrrc[mod].carrier[CC_id].sizeof_SIB23 == 255");
  }

  LOG_I(NR_RRC,"Done init_NR_SI\n");

  if (NODE_IS_MONOLITHIC(rrc->node_type) || NODE_IS_DU(rrc->node_type)){
    // update SI info
    nr_mac_config_scc(RC.nrmac[rrc->module_id],
                      rrc->configuration.pdsch_AntennaPorts,
                      rrc->configuration.pusch_AntennaPorts,
                      rrc->configuration.sib1_tda,
                      rrc->configuration.minRXTXTIME,
                      rrc->carrier.servingcellconfigcommon);
    nr_mac_config_mib(RC.nrmac[rrc->module_id], rrc->carrier.mib);
  }

  /* set flag to indicate that cell information is configured. This is required
   * in DU to trigger F1AP_SETUP procedure */
  pthread_mutex_lock(&rrc->cell_info_mutex);
  rrc->cell_info_configured=1;
  pthread_mutex_unlock(&rrc->cell_info_mutex);

  if (get_softmodem_params()->phy_test > 0 || get_softmodem_params()->do_ra > 0) {
    rrc_gNB_ue_context_t *ue_context_p = rrc_gNB_allocate_new_ue_context(rrc);
    gNB_RRC_UE_t *UE = &ue_context_p->ue_context;
    UE->spCellConfig = calloc(1, sizeof(struct NR_SpCellConfig));
    UE->spCellConfig->spCellConfigDedicated = configuration->scd;
    LOG_I(NR_RRC,"Adding new user (%p)\n",ue_context_p);
    if (!NODE_IS_CU(RC.nrrrc[0]->node_type)) {
      rrc_add_nsa_user(rrc,ue_context_p,NULL);
    }
  }
}

static void rrc_gNB_CU_DU_init(gNB_RRC_INST *rrc)
{
  switch (rrc->node_type) {
    case ngran_gNB_CUCP:
      mac_rrc_dl_f1ap_init(&rrc->mac_rrc);
      cucp_cuup_message_transfer_e1ap_init(rrc);
      break;
    case ngran_gNB_CU:
      mac_rrc_dl_f1ap_init(&rrc->mac_rrc);
      cucp_cuup_message_transfer_direct_init(rrc);
      break;
    case ngran_gNB:
      mac_rrc_dl_direct_init(&rrc->mac_rrc);
      cucp_cuup_message_transfer_direct_init(rrc);
       break;
    case ngran_gNB_DU:
      /* silently drop this, as we currently still need the RRC at the DU. As
       * soon as this is not the case anymore, we can add the AssertFatal() */
      //AssertFatal(1==0,"nothing to do for DU\n");
      break;
    default:
      AssertFatal(0 == 1, "Unknown node type %d\n", rrc->node_type);
      break;
  }
}

static void openair_rrc_gNB_configuration(const module_id_t gnb_mod_idP, gNB_RrcConfigurationReq *configuration)
{
  protocol_ctxt_t      ctxt = { 0 };
  gNB_RRC_INST         *rrc=RC.nrrrc[gnb_mod_idP];
  PROTOCOL_CTXT_SET_BY_MODULE_ID(&ctxt, gnb_mod_idP, GNB_FLAG_YES, NOT_A_RNTI, 0, 0,gnb_mod_idP);
  LOG_I(NR_RRC,
        PROTOCOL_NR_RRC_CTXT_FMT" Init...\n",
        PROTOCOL_NR_RRC_CTXT_ARGS(&ctxt));
  AssertFatal(rrc != NULL, "RC.nrrrc not initialized!");
  AssertFatal(NUMBER_OF_UE_MAX < (module_id_t)0xFFFFFFFFFFFFFFFF, " variable overflow");
  AssertFatal(configuration!=NULL,"configuration input is null\n");
  rrc->module_id = gnb_mod_idP;
  rrc_gNB_CU_DU_init(rrc);
  uid_linear_allocator_init(&rrc->uid_allocator);
  RB_INIT(&rrc->rrc_ue_head);
  rrc->configuration = *configuration;
  rrc->carrier.servingcellconfigcommon = configuration->scc;
  nr_rrc_config_ul_tda(configuration->scc,configuration->minRXTXTIME);
   /// System Information INIT
  pthread_mutex_init(&rrc->cell_info_mutex,NULL);
  rrc->cell_info_configured = 0;
  LOG_I(NR_RRC, PROTOCOL_NR_RRC_CTXT_FMT" Checking release \n",PROTOCOL_NR_RRC_CTXT_ARGS(&ctxt));
  init_NR_SI(rrc, configuration);
  return;
} // END openair_rrc_gNB_configuration

static void rrc_gNB_process_AdditionRequestInformation(const module_id_t gnb_mod_idP, x2ap_ENDC_sgnb_addition_req_t *m)
{
  struct NR_CG_ConfigInfo *cg_configinfo = NULL;
  asn_dec_rval_t dec_rval = uper_decode_complete(NULL,
                            &asn_DEF_NR_CG_ConfigInfo,
                            (void **)&cg_configinfo,
                            (uint8_t *)m->rrc_buffer,
                            (int) m->rrc_buffer_size);//m->rrc_buffer_size);
  gNB_RRC_INST         *rrc=RC.nrrrc[gnb_mod_idP];

  if ((dec_rval.code != RC_OK) && (dec_rval.consumed == 0)) {
    AssertFatal(1==0,"NR_UL_DCCH_MESSAGE decode error\n");
    // free the memory
    SEQUENCE_free(&asn_DEF_NR_CG_ConfigInfo, cg_configinfo, 1);
    return;
  }

  xer_fprint(stdout,&asn_DEF_NR_CG_ConfigInfo, cg_configinfo);
  // recreate enough of X2 EN-DC Container
  AssertFatal(cg_configinfo->criticalExtensions.choice.c1->present == NR_CG_ConfigInfo__criticalExtensions__c1_PR_cg_ConfigInfo,
              "ueCapabilityInformation not present\n");
  parse_CG_ConfigInfo(rrc,cg_configinfo,m);
  LOG_A(NR_RRC, "Successfully parsed CG_ConfigInfo of size %zu bits. (%zu bytes)\n",
        dec_rval.consumed, (dec_rval.consumed +7/8));
}

//-----------------------------------------------------------------------------
unsigned int rrc_gNB_get_next_transaction_identifier(module_id_t gnb_mod_idP)
//-----------------------------------------------------------------------------
{
  static unsigned int transaction_id[NUMBER_OF_gNB_MAX] = {0};
  // used also in NGAP thread, so need thread safe operation
  unsigned int tmp = __atomic_add_fetch(&transaction_id[gnb_mod_idP], 1, __ATOMIC_SEQ_CST);
  tmp %= NR_RRC_TRANSACTION_IDENTIFIER_NUMBER;
  LOG_T(NR_RRC, "generated xid is %d\n", tmp);
  return tmp;
}

static NR_SRB_ToAddModList_t *createSRBlist(gNB_RRC_UE_t *ue, bool reestablish)
{
  if (!ue->Srb[1].Active) {
    LOG_E(NR_RRC, "Call SRB list while SRB1 doesn't exist\n");
    return NULL;
  }
  NR_SRB_ToAddModList_t *list = CALLOC(sizeof(*list), 1);
  for (int i = 0; i < maxSRBs; i++)
    if (ue->Srb[i].Active) {
      asn1cSequenceAdd(list->list, NR_SRB_ToAddMod_t, srb);
      srb->srb_Identity = i;
      if (reestablish && i == 2) {
        asn1cCallocOne(srb->reestablishPDCP, NR_SRB_ToAddMod__reestablishPDCP_true);
      }
    }
  return list;
}

static NR_DRB_ToAddModList_t *createDRBlist(gNB_RRC_UE_t *ue, bool reestablish)
{
  NR_DRB_ToAddMod_t *DRB_config = NULL;
  NR_DRB_ToAddModList_t *DRB_configList = CALLOC(sizeof(*DRB_configList), 1);

  for (int i = 0; i < MAX_DRBS_PER_UE; i++) {
    if (ue->established_drbs[i].status != DRB_INACTIVE) {
      DRB_config = generateDRB_ASN1(&ue->established_drbs[i]);
      if (reestablish) {
        ue->established_drbs[i].reestablishPDCP = NR_DRB_ToAddMod__reestablishPDCP_true;
        asn1cCallocOne(DRB_config->reestablishPDCP, NR_DRB_ToAddMod__reestablishPDCP_true);
      }
      asn1cSeqAdd(&DRB_configList->list, DRB_config);
    }
  }
  return DRB_configList;
}

static void freeSRBlist(NR_SRB_ToAddModList_t *l)
{
  if (l) {
    for (int i = 0; i < l->list.count; i++)
      free(l->list.array[i]);
    free(l);
  } else
    LOG_E(NR_RRC, "Call free SRB list on NULL pointer\n");
}

static void apply_macrlc_config(gNB_RRC_INST *rrc, rrc_gNB_ue_context_t *const ue_context_pP, const protocol_ctxt_t *const ctxt_pP)
{
  NR_CellGroupConfig_t *cgc = get_softmodem_params()->sa ? ue_context_pP->ue_context.masterCellGroup : NULL;
  gNB_RRC_UE_t *ue_p = &ue_context_pP->ue_context;
  nr_rrc_mac_update_cellgroup(ue_p->rnti, cgc);
  nr_rrc_addmod_srbs(ctxt_pP->rntiMaybeUEid, ue_p->Srb, maxSRBs, cgc->rlc_BearerToAddModList);
  NR_DRB_ToAddModList_t *DRBs = fill_DRB_configList(ue_p);
  nr_rrc_addmod_drbs(ctxt_pP->rntiMaybeUEid, DRBs, cgc->rlc_BearerToAddModList);
  freeDRBlist(DRBs);

  // Check if Relay UE needs RRCReconfiguration for DRB 2 reset
  if (ue_p->relay_drb2_needs_reconfig) {
    LOG_D(NR_RRC, "[Relay UE] DRB 2 reconfigured (new Remote UE session) - RLC entity recreated at gNB\n");
    LOG_D(NR_RRC, "[Relay UE] Sending RRCReconfiguration to reset DRB 2 RLC entity\n");

    // Create RRCReconfiguration with radioBearerConfig for DRB 2 reestablishment
    NR_RRCReconfiguration_t *rrc_reconfig = CALLOC(1, sizeof(NR_RRCReconfiguration_t));
    rrc_reconfig->rrc_TransactionIdentifier = 0;
    rrc_reconfig->criticalExtensions.present = NR_RRCReconfiguration__criticalExtensions_PR_rrcReconfiguration;

    NR_RRCReconfiguration_IEs_t *reconfig_ies = CALLOC(1, sizeof(NR_RRCReconfiguration_IEs_t));
    rrc_reconfig->criticalExtensions.choice.rrcReconfiguration = reconfig_ies;

    // Create radioBearerConfig
    NR_RadioBearerConfig_t *radio_config = CALLOC(1, sizeof(NR_RadioBearerConfig_t));
    reconfig_ies->radioBearerConfig = radio_config;

    // Create DRB list with DRB 2
    NR_DRB_ToAddModList_t *drb_list = CALLOC(1, sizeof(NR_DRB_ToAddModList_t));
    radio_config->drb_ToAddModList = drb_list;

    // Add DRB 2 with reestablishPDCP flag
    NR_DRB_ToAddMod_t *drb = generateDRB_ASN1(&ue_p->established_drbs[1]); // DRB 2 is at index 1
    asn1cCallocOne(drb->reestablishPDCP, NR_DRB_ToAddMod__reestablishPDCP_true);
    asn1cSeqAdd(&drb_list->list, drb);

    // Encode the message
    uint8_t buffer[1024];
    asn_enc_rval_t enc_rval = uper_encode_to_buffer(&asn_DEF_NR_RRCReconfiguration, NULL, rrc_reconfig, buffer, sizeof(buffer));
    AssertFatal(enc_rval.encoded > 0, "Failed to encode RRCReconfiguration for Relay DRB 2 reset\n");

    int size = (enc_rval.encoded + 7) >> 3; // Convert bits to bytes

    LOG_D(NR_RRC, "[Relay UE] Encoded RRCReconfiguration (%d bytes) with DRB 2 reestablishPDCP=true\n", size);

    // Send via SRB1/DCCH
    protocol_ctxt_t ctxt = *ctxt_pP;
    ctxt.rntiMaybeUEid = ue_p->rnti;
    nr_pdcp_data_req_srb(ctxt.rntiMaybeUEid, DCCH, rrc_gNB_mui++, size, buffer, deliver_pdu_srb_f1, rrc, UU);

    // Free the ASN.1 structure
    ASN_STRUCT_FREE(asn_DEF_NR_RRCReconfiguration, rrc_reconfig);

    ue_p->relay_drb2_needs_reconfig = false;
  }
}

void apply_macrlc_config_reest(gNB_RRC_INST *rrc, rrc_gNB_ue_context_t *const ue_context_pP, const protocol_ctxt_t *const ctxt_pP, ue_id_t ue_id)
{
  gNB_RRC_UE_t *ue_p = &ue_context_pP->ue_context;
  nr_rrc_mac_update_cellgroup(ue_id, ue_p->masterCellGroup);

  nr_rrc_addmod_srbs(ctxt_pP->rntiMaybeUEid, ue_p->Srb, maxSRBs, ue_p->masterCellGroup->rlc_BearerToAddModList);
}

//-----------------------------------------------------------------------------
static void rrc_gNB_generate_RRCSetup(instance_t instance,
                                      rnti_t rnti,
                                      rrc_gNB_ue_context_t *const ue_context_pP,
                                      const uint8_t *masterCellGroup,
                                      int masterCellGroup_len)
//-----------------------------------------------------------------------------
{
  LOG_I(NR_RRC, "rrc_gNB_generate_RRCSetup for RNTI %04x\n", rnti);

  gNB_RRC_UE_t *ue_p = &ue_context_pP->ue_context;
  gNB_RRC_INST *rrc = RC.nrrrc[instance];
  unsigned char buf[1024];
  uint8_t xid = rrc_gNB_get_next_transaction_identifier(instance);
  ue_p->xids[xid] = RRC_SETUP;
  NR_SRB_ToAddModList_t *SRBs = createSRBlist(ue_p, false);

  int size = do_RRCSetup(ue_context_pP, buf, xid, masterCellGroup, masterCellGroup_len, &rrc->configuration, SRBs);
  AssertFatal(size > 0, "do_RRCSetup failed\n");
  AssertFatal(size <= 1024, "memory corruption\n");

  LOG_DUMPMSG(NR_RRC, DEBUG_RRC,
              (char *)buf,
              size,
              "[MSG] RRC Setup\n");
  nr_pdcp_add_srbs(true, rnti, SRBs, 0, NULL, NULL, UU);

  // Set Remote UE info in PDCP UE entity for SRAP encapsulation on downlink
  if (ue_p->is_remote_ue) {
    nr_pdcp_manager_lock(nr_pdcp_ue_manager);
    nr_pdcp_ue_t *pdcp_ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, rnti);
    if (pdcp_ue) {
      pdcp_ue->is_remote_ue = true;
      pdcp_ue->relay_ue_rnti = ue_p->relay_ue_rnti;
      pdcp_ue->remote_ue_id = ue_p->remote_ue_id;
      LOG_D(NR_RRC, "[gNB] Set Remote UE info in PDCP: relay_rnti=0x%04lx, remote_id=%d\n",
            pdcp_ue->relay_ue_rnti, pdcp_ue->remote_ue_id);
    }
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
  }

  freeSRBlist(SRBs);

  // L2 Relay: For Remote UE, send RRCSetup directly via SRAP
  uint8_t *rrc_container = buf;
  int rrc_container_length = size;

  if (ue_p->is_remote_ue) {
    LOG_D(NR_RRC, "[REMOTE_UE_MSG] TX RRCSetup to Remote UE ID=%d (RNTI 0x%04x) via Relay RNTI=0x%04x\n",
          ue_p->remote_ue_id, rnti, ue_p->relay_ue_rnti);

    /* RRCSetup is CCCH (SRB0) = pure RRC without PDCP header
       Send directly to SRAP which will add U2N header and deliver to Relay UE's SRB1 */
    protocol_ctxt_t ctxt = {
      .module_id = instance,
      .rntiMaybeUEid = ue_p->relay_ue_rnti,  // Use Relay UE RNTI for RLC delivery
      .remote_ue_id = ue_p->remote_ue_id,    // For SRAP header
      .enb_flag = 1,
      .instance = 0,
      .frame = 0,
      .subframe = 0,
      .eNB_index = 0,
      .brOption = 0
    };

    LOG_D(NR_RRC, "[gNB] Sending RRCSetup via SRAP: rb_id=1 (will be sent on Relay UE SRB1), RRC size=%d bytes\n",
          rrc_container_length);

    /* Send via SRAP directly
       Note: rb_id=1 for gNB→Relay delivery (SRB1), SRAP will add U2N header with bearer_id from rb_id
       The Uu transmission to Relay UE will use SRB1 */
    extern void srap_deliver_pdu_srb(protocol_ctxt_t *ctxt, int srb_id, char *buf, int size, int sdu_id, nr_intf_type_t intf_type);
    nr_srap_data_req_srb(&ctxt, 1, rrc_container_length, (char *)rrc_container,
                         srap_deliver_pdu_srb, 0, UU);
  } else {
    // Normal UE: send via CCCH
    f1ap_dl_rrc_message_t dl_rrc = {
      .old_gNB_DU_ue_id = 0xFFFFFF,
      .rrc_container = rrc_container,
      .rrc_container_length = rrc_container_length,
      .rnti = rnti,
      .srb_id = CCCH
    };
    rrc->mac_rrc.dl_rrc_message_transfer(instance, &dl_rrc);
  }
}

//-----------------------------------------------------------------------------
static int rrc_gNB_generate_RRCSetup_for_RRCReestablishmentRequest(module_id_t module_id, rnti_t rnti, const int CC_id,
                                                                   const uint8_t *masterCellGroup, int masterCellGroup_len)
//-----------------------------------------------------------------------------
{
  LOG_I(NR_RRC, "generate RRCSetup for RRCReestablishmentRequest \n");
  rrc_gNB_ue_context_t         *ue_context_pP   = NULL;
  gNB_RRC_INST *rrc_instance_p = RC.nrrrc[module_id];

  /* RRCSetup carries a mandatory masterCellGroup IE. do_RRCSetup() DevAsserts it is
     non-NULL, so a reestablishment fallback with no DU cellGroup would crash the gNB.
     Fail the fallback gracefully instead of aborting the whole gNB. */
  if (masterCellGroup == NULL || masterCellGroup_len <= 0) {
    LOG_E(NR_RRC, "cannot generate RRCSetup for RRCReestablishmentRequest (RNTI %04x): no masterCellGroup from DU\n", rnti);
    return -1;
  }

  ue_context_pP = rrc_gNB_create_ue_context(rnti, rrc_instance_p, 0);

  gNB_RRC_UE_t *ue_p = &ue_context_pP->ue_context;
  unsigned char buf[1024];
  uint8_t xid = rrc_gNB_get_next_transaction_identifier(module_id);
  ue_p->xids[xid] = RRC_SETUP_FOR_REESTABLISHMENT;
  NR_SRB_ToAddModList_t *SRBs = createSRBlist(ue_p, true);

  int size = do_RRCSetup(ue_context_pP, buf, xid, masterCellGroup, masterCellGroup_len, &rrc_instance_p->configuration, SRBs);
  AssertFatal(size > 0, "do_RRCSetup failed\n");
  AssertFatal(size <= 1024, "memory corruption\n");

  AssertFatal(size>0,"Error generating RRCSetup for RRCReestablishmentRequest\n");

  LOG_DUMPMSG(NR_RRC, DEBUG_RRC,
              (char *)buf,
              size,
              "[MSG] RRC Setup\n");

  LOG_D(NR_RRC, "RRC_gNB --- MAC_CONFIG_REQ  (SRB1) ---> MAC_gNB for rnti %04x\n", rnti);
  freeSRBlist(SRBs);

  // update SCC and MIB/SIB (two calls)
  nr_mac_config_scc(RC.nrmac[rrc_instance_p->module_id],
                    rrc_instance_p->configuration.pdsch_AntennaPorts,
                    rrc_instance_p->configuration.pusch_AntennaPorts,
                    rrc_instance_p->configuration.sib1_tda,
                    rrc_instance_p->configuration.minRXTXTIME,
                    rrc_instance_p->carrier.servingcellconfigcommon);
  nr_mac_config_mib(RC.nrmac[rrc_instance_p->module_id], rrc_instance_p->carrier.mib);
  nr_mac_config_sib1(RC.nrmac[rrc_instance_p->module_id], rrc_instance_p->carrier.siblock1);

  LOG_I(NR_RRC, " [RAPROC] rnti: %04x Logical Channel DL-CCCH, Generating RRCSetup (bytes %d)\n", rnti, size);
  // configure MAC
  protocol_ctxt_t ctxt = {0};
  PROTOCOL_CTXT_SET_BY_INSTANCE(&ctxt, 0, GNB_FLAG_YES, rnti, 0, 0);
  apply_macrlc_config(rrc_instance_p, ue_context_pP, &ctxt);

  f1ap_dl_rrc_message_t dl_rrc = {
    .old_gNB_DU_ue_id = 0xFFFFFF,
    .rrc_container = buf,
    .rrc_container_length = size,
    .rnti = ue_p->rnti,
    .srb_id = CCCH
  };
  rrc_instance_p->mac_rrc.dl_rrc_message_transfer(module_id, &dl_rrc);
  return xid;
}

static void rrc_gNB_generate_RRCReject(module_id_t module_id, rrc_gNB_ue_context_t *const ue_context_pP)
//-----------------------------------------------------------------------------
{
  LOG_I(NR_RRC, "rrc_gNB_generate_RRCReject \n");
  gNB_RRC_INST *rrc = RC.nrrrc[module_id];
  gNB_RRC_UE_t *ue_p = &ue_context_pP->ue_context;

  unsigned char buf[1024];
  int size = do_RRCReject(module_id, buf);
  AssertFatal(size > 0, "do_RRCReject failed\n");
  AssertFatal(size <= 1024, "memory corruption\n");

  LOG_DUMPMSG(NR_RRC, DEBUG_RRC,
              (char *)buf,
              size,
              "[MSG] RRCReject \n");
  LOG_I(NR_RRC, " [RAPROC] ue %04x Logical Channel DL-CCCH, Generating NR_RRCReject (bytes %d)\n", ue_p->rnti, size);

  f1ap_dl_rrc_message_t dl_rrc = {
    .gNB_CU_ue_id = 0,
    .gNB_DU_ue_id = 0,
    .old_gNB_DU_ue_id = 0xFFFFFF,
    .rrc_container = buf,
    .rrc_container_length = size,
    .rnti = ue_p->rnti,
    .srb_id = CCCH,
    .execute_duplication  = 1,
    .RAT_frequency_priority_information.en_dc = 0
  };
  rrc->mac_rrc.dl_rrc_message_transfer(module_id, &dl_rrc);
}

//-----------------------------------------------------------------------------
/*
* Process the rrc setup complete message from UE (SRB1 Active)
*/
static void rrc_gNB_process_RRCSetupComplete(const protocol_ctxt_t *const ctxt_pP, rrc_gNB_ue_context_t *ue_context_pP, NR_RRCSetupComplete_IEs_t *rrcSetupComplete)
//-----------------------------------------------------------------------------
{
  gNB_RRC_UE_t *ue_p = &ue_context_pP->ue_context;
  if (ue_p->is_remote_ue) {
    LOG_D(NR_RRC, "[REMOTE_UE_MSG] RX RRCSetupComplete (decoded at RRC) from Remote UE ID=%d via Relay RNTI=0x%04x\n",
          ue_p->remote_ue_id, ue_p->relay_ue_rnti);

    // Log NAS Registration Request extraction
    if (rrcSetupComplete->dedicatedNAS_Message.size > 0) {
      LOG_D(NR_RRC, "[REMOTE_UE_MSG] Extracting NAS Registration Request from Remote UE (RNTI 0x%04x): %zu bytes\n",
            ue_p->rnti, rrcSetupComplete->dedicatedNAS_Message.size);
      LOG_D(NR_RRC, "[REMOTE_UE_MSG] Forwarding Registration Request to AMF via NGAP for Remote UE (RNTI 0x%04x)\n",
            ue_p->rnti);
    }
  }
  LOG_A(NR_RRC, PROTOCOL_NR_RRC_CTXT_UE_FMT" [RAPROC] Logical Channel UL-DCCH, " "processing NR_RRCSetupComplete from UE (SRB1 Active)\n",
      PROTOCOL_NR_RRC_CTXT_UE_ARGS(ctxt_pP));
  ue_context_pP->ue_context.Srb[1].Active = 1;
  ue_context_pP->ue_context.Srb[2].Active = 0;
  ue_context_pP->ue_context.StatusRrc = NR_RRC_CONNECTED;

  if (get_softmodem_params()->sa) {
    rrc_gNB_send_NGAP_NAS_FIRST_REQ(ctxt_pP, ue_context_pP, rrcSetupComplete);
  } else {
    rrc_gNB_generate_SecurityModeCommand(ctxt_pP, ue_context_pP);
  }
}

//-----------------------------------------------------------------------------
static void rrc_gNB_generate_defaultRRCReconfiguration(const protocol_ctxt_t *const ctxt_pP, rrc_gNB_ue_context_t *ue_context_pP)
//-----------------------------------------------------------------------------
{
  gNB_RRC_INST *rrc = RC.nrrrc[ctxt_pP->module_id];
  gNB_RRC_UE_t *ue_p = &ue_context_pP->ue_context;
  AssertFatal(ue_p->nb_of_pdusessions == 0, "logic bug: PDU sessions present before RRC Connection established\n");
  uint8_t xid = rrc_gNB_get_next_transaction_identifier(ctxt_pP->module_id);
  ue_p->xids[xid] = RRC_DEFAULT_RECONF;

  struct NR_RRCReconfiguration_v1530_IEs__dedicatedNAS_MessageList *dedicatedNAS_MessageList = CALLOC(1, sizeof(*dedicatedNAS_MessageList));

  /* Add all NAS PDUs to the list */
  for (int i = 0; i < ue_p->nb_of_pdusessions; i++) {
    if (ue_p->pduSession[i].param.nas_pdu.buffer != NULL) {
      asn1cSequenceAdd(dedicatedNAS_MessageList->list, NR_DedicatedNAS_Message_t, msg);
      OCTET_STRING_fromBuf(msg, (char *)ue_p->pduSession[i].param.nas_pdu.buffer, ue_p->pduSession[i].param.nas_pdu.length);
    }

    ue_p->pduSession[i].status = PDU_SESSION_STATUS_DONE;
    LOG_D(NR_RRC, "setting the status for the default DRB (index %d) to (%d,%s)\n", i, ue_p->pduSession[i].status, "PDU_SESSION_STATUS_DONE");
  }

  if (ue_p->nas_pdu.length) {
    asn1cSequenceAdd(dedicatedNAS_MessageList->list, NR_DedicatedNAS_Message_t, msg);
    OCTET_STRING_fromBuf(msg, (char *)ue_p->nas_pdu.buffer, ue_p->nas_pdu.length);

    // Check if this is Registration Accept for Remote UE
    if (ue_p->is_remote_ue && ue_p->nas_pdu.length > SECURITY_PROTECTED_5GS_NAS_MESSAGE_HEADER_LENGTH) {
      uint8_t nas_msg_type = ue_p->nas_pdu.buffer[SECURITY_PROTECTED_5GS_NAS_MESSAGE_HEADER_LENGTH];
      if (nas_msg_type == REGISTRATION_ACCEPT) {
        LOG_D(NR_RRC, "[REMOTE_UE_MSG] TX Registration Accept via RRCReconfiguration to Remote UE ID=%d (RNTI 0x%04x) via Relay RNTI=0x%04x\n",
              ue_p->remote_ue_id, ue_p->rnti, ue_p->relay_ue_rnti);
      }
    }
  }

  /* If list is empty free the list and reset the address */
  if (dedicatedNAS_MessageList->list.count == 0) {
    free(dedicatedNAS_MessageList);
    dedicatedNAS_MessageList = NULL;
  }

  NR_MeasConfig_t *measconfig = get_defaultMeasConfig(&rrc->configuration);

  uint8_t buffer[RRC_BUF_SIZE] = {0};
  int size = do_RRCReconfiguration(ctxt_pP,
                                   buffer,
                                   RRC_BUF_SIZE,
                                   xid,
                                   NULL, //*SRB_configList2,
                                   NULL, //*DRB_configList,
                                   NULL,
                                   NULL,
                                   NULL,
                                   measconfig,
                                   dedicatedNAS_MessageList,
                                   NULL,
                                   ue_context_pP,
                                   &rrc->carrier,
                                   &rrc->configuration,
                                   NULL,
                                   ue_p->masterCellGroup);
  AssertFatal(size > 0, "cannot encode RRCReconfiguration in %s()\n", __func__);
  LOG_W(NR_RRC, "do_RRCReconfiguration(): size %d\n", size);

  if (LOG_DEBUGFLAG(DEBUG_ASN1)) {
    xer_fprint(stdout, &asn_DEF_NR_CellGroupConfig, ue_p->masterCellGroup);
  }

  // suspicious if it is always malloced before ?
  free(ue_p->nas_pdu.buffer);

  LOG_DUMPMSG(NR_RRC, DEBUG_RRC,(char *)buffer, size, "[MSG] RRC Reconfiguration\n");

  /* Free all NAS PDUs */
  for (int i = 0; i < ue_p->nb_of_pdusessions; i++) {
    if (ue_p->pduSession[i].param.nas_pdu.buffer != NULL) {
      free(ue_p->pduSession[i].param.nas_pdu.buffer);
      ue_p->pduSession[i].param.nas_pdu.buffer = NULL;
    }
  }

  LOG_I(NR_RRC, "[gNB %d] Frame %d, Logical Channel DL-DCCH, Generate NR_RRCReconfiguration (bytes %d, UE id %x)\n",
          ctxt_pP->module_id,
          ctxt_pP->frame,
          size,
          ue_context_pP->ue_context.rnti);
  AssertFatal(!NODE_IS_DU(rrc->node_type), "illegal node type DU!\n");

  nr_pdcp_data_req_srb(ctxt_pP->rntiMaybeUEid, DCCH, rrc_gNB_mui++, size, buffer, deliver_pdu_srb_f1, rrc, UU);

  if (NODE_IS_DU(rrc->node_type) || NODE_IS_MONOLITHIC(rrc->node_type)) {
    gNB_RRC_UE_t *ue_p = &ue_context_pP->ue_context;
    nr_rrc_mac_update_cellgroup(ue_p->rnti, ue_p->masterCellGroup);

    uint32_t delay_ms = ue_p->masterCellGroup && ue_p->masterCellGroup->spCellConfig && ue_p->masterCellGroup->spCellConfig->spCellConfigDedicated
                                && ue_p->masterCellGroup->spCellConfig->spCellConfigDedicated->downlinkBWP_ToAddModList
                            ? NR_RRC_RECONFIGURATION_DELAY_MS + NR_RRC_BWP_SWITCHING_DELAY_MS
                            : NR_RRC_RECONFIGURATION_DELAY_MS;

    nr_mac_enable_ue_rrc_processing_timer(ctxt_pP->module_id, ue_p->rnti, *rrc->carrier.servingcellconfigcommon->ssbSubcarrierSpacing, delay_ms);
  }
}

//-----------------------------------------------------------------------------
/** Apply per-UE SL MCS override (from HARQ adaptation) to built sl_ConfigDedicatedNR. */
static void apply_sl_mcs_override_from_ue_context(const gNB_RRC_UE_t *ue_p,
                                                  NR_SetupRelease_SL_ConfigDedicatedNR_r16_t *sl_conf)
//-----------------------------------------------------------------------------
{
  if (!ue_p || !sl_conf || !sl_conf->choice.setup)
    return;
  struct NR_SL_PHY_MAC_RLC_Config_r16 *sl_phy = sl_conf->choice.setup->sl_PHY_MAC_RLC_Config_r16;
  if (!sl_phy || !sl_phy->sl_FreqInfoToAddModList_r16)
    return;
  const long max_mcs = (long)ue_p->sl_max_mcs_pssch_r16;
  for (int i = 0; i < sl_phy->sl_FreqInfoToAddModList_r16->list.count; i++) {
    NR_SL_FreqConfig_r16_t *freq = sl_phy->sl_FreqInfoToAddModList_r16->list.array[i];
    if (!freq || !freq->sl_BWP_ToAddModList_r16)
      continue;
    for (int j = 0; j < freq->sl_BWP_ToAddModList_r16->list.count; j++) {
      NR_SL_BWP_Config_r16_t *bwp = freq->sl_BWP_ToAddModList_r16->list.array[j];
      if (!bwp || !bwp->sl_BWP_PoolConfig_r16 || !bwp->sl_BWP_PoolConfig_r16->sl_TxPoolScheduling_r16
          || !bwp->sl_BWP_PoolConfig_r16->sl_TxPoolScheduling_r16->sl_PoolToAddModList_r16)
        continue;
      struct NR_SL_TxPoolDedicated_r16__sl_PoolToAddModList_r16 *pool_list =
        bwp->sl_BWP_PoolConfig_r16->sl_TxPoolScheduling_r16->sl_PoolToAddModList_r16;
      for (int k = 0; k < pool_list->list.count; k++) {
        NR_SL_ResourcePoolConfig_r16_t *pool_cfg = pool_list->list.array[k];
        if (!pool_cfg || !pool_cfg->sl_ResourcePool_r16 || !pool_cfg->sl_ResourcePool_r16->sl_MinMaxMCS_List_r16)
          continue;
        struct NR_SL_MinMaxMCS_List_r16 *mcs_list = pool_cfg->sl_ResourcePool_r16->sl_MinMaxMCS_List_r16;
        for (int m = 0; m < mcs_list->list.count; m++) {
          NR_SL_MinMaxMCS_Config_r16_t *mcs_cfg = mcs_list->list.array[m];
          if (mcs_cfg) {
            long old_pool_mcs = mcs_cfg->sl_MaxMCS_PSSCH_r16;
            long p = old_pool_mcs;
            if (p > max_mcs)
              p = max_mcs;
            mcs_cfg->sl_MaxMCS_PSSCH_r16 = p;
          }
        }
      }
    }
  }
}

//-----------------------------------------------------------------------------
NR_RRCReconfiguration_v1610_IEs_t* prepare_rrc_reconfig_v1610(module_id_t module_id,
                                                              rnti_t sl_rnti,
                                                              NR_SL_TxResourceReqList_r16_t *sl_TxRscReqList_r16,
                                                              const NR_SL_UE_AssistanceInformationNR_r16_t *trafficPatternList,
                                                              rrc_gNB_ue_context_t *ue_context_p) {
//-----------------------------------------------------------------------------

    LOG_D(NR_RRC, "Preparing RRCReconfiguration-v1610-IEs with Sidelink IEs.\n");
    NR_RRCReconfiguration_v1610_IEs_t* v1610_ies = (NR_RRCReconfiguration_v1610_IEs_t*)malloc(sizeof(NR_RRCReconfiguration_v1610_IEs_t));
    memset(v1610_ies, 0, sizeof(NR_RRCReconfiguration_v1610_IEs_t));

    v1610_ies->sl_ConfigDedicatedNR_r16 = CALLOC(1, sizeof(NR_SetupRelease_SL_ConfigDedicatedNR_r16_t));
    nr_rrc_pre_configure_NR_SetupRelease_SL_ConfigDedicatedNR(module_id, v1610_ies->sl_ConfigDedicatedNR_r16, sl_rnti, sl_TxRscReqList_r16, trafficPatternList);
    if (ue_context_p)
      apply_sl_mcs_override_from_ue_context(&ue_context_p->ue_context, v1610_ies->sl_ConfigDedicatedNR_r16);
    return v1610_ies;
}

//-----------------------------------------------------------------------------
void rrc_gNB_generate_dedicatedRRCReconfiguration(const protocol_ctxt_t *const ctxt_pP, rrc_gNB_ue_context_t *ue_context_pP)
//-----------------------------------------------------------------------------
{
  gNB_RRC_INST *rrc = RC.nrrrc[ctxt_pP->module_id];

  uint8_t xid = rrc_gNB_get_next_transaction_identifier(ctxt_pP->module_id);
  int drb_id_to_setup_start = 1;
  gNB_RRC_UE_t *ue_p = &ue_context_pP->ue_context;
  NR_DRB_ToAddModList_t *DRB_configList = fill_DRB_configList(ue_p);
  int nb_drb_to_setup = DRB_configList ? DRB_configList->list.count : 0;
  ue_p->xids[xid] = RRC_PDUSESSION_ESTABLISH;
  struct NR_RRCReconfiguration_v1530_IEs__dedicatedNAS_MessageList *dedicatedNAS_MessageList = NULL;
  NR_DedicatedNAS_Message_t *dedicatedNAS_Message = NULL;
  dedicatedNAS_MessageList = CALLOC(1, sizeof(struct NR_RRCReconfiguration_v1530_IEs__dedicatedNAS_MessageList));

  /* For Relay UE with 2 DRBs, only process NAS PDU once (not per DRB)
     Track which PDU sessions have been processed to avoid duplicates */
  bool pdu_session_processed[MAX_DRBS_PER_UE] = {false};

  for (int i=0; i < nb_drb_to_setup; i++) {
    NR_DRB_ToAddMod_t *DRB_config = DRB_configList->list.array[i];
    if (drb_id_to_setup_start == 1)
      drb_id_to_setup_start = DRB_config->drb_Identity;
    int j = ue_p->nb_of_pdusessions - 1;
    AssertFatal(j >= 0, "");

    // Only process each PDU session once, even if multiple DRBs map to it
    if (!pdu_session_processed[j]) {
      pdu_session_processed[j] = true;

      if (ue_p->pduSession[j].param.nas_pdu.buffer != NULL) {
        dedicatedNAS_Message = CALLOC(1, sizeof(NR_DedicatedNAS_Message_t));
        memset(dedicatedNAS_Message, 0, sizeof(OCTET_STRING_t));
        OCTET_STRING_fromBuf(dedicatedNAS_Message,
                             (char *)ue_p->pduSession[j].param.nas_pdu.buffer,
                             ue_p->pduSession[j].param.nas_pdu.length);
        ue_p->pduSession[j].status = PDU_SESSION_STATUS_DONE;
        asn1cSeqAdd(&dedicatedNAS_MessageList->list, dedicatedNAS_Message);

        LOG_D(NR_RRC, "add NAS info with size %d (pdusession idx %d, drb %d)\n",
              ue_p->pduSession[j].param.nas_pdu.length, j, i+1);
      } else {
        // TODO
        LOG_E(NR_RRC, "no NAS info (pdusession idx %d)\n", j);
      }

      ue_p->pduSession[j].xid = xid;
    } else {
      LOG_D(NR_RRC, "[Relay UE] Skipping NAS PDU for DRB %d (already processed for PDU session %d)\n", i+1, j);
    }
  }
  freeDRBlist(DRB_configList);

  /* If list is empty free the list and reset the address */
  if (dedicatedNAS_MessageList->list.count == 0) {
    free(dedicatedNAS_MessageList);
    dedicatedNAS_MessageList = NULL;
  }

  /* Free all NAS PDUs */
  for (int i = 0; i < ue_p->nb_of_pdusessions; i++) {
    if (ue_p->pduSession[i].param.nas_pdu.buffer != NULL) {
      /* Free the NAS PDU buffer and invalidate it */
      free(ue_p->pduSession[i].param.nas_pdu.buffer);
      ue_p->pduSession[i].param.nas_pdu.buffer = NULL;
    }
  }

  NR_CellGroupConfig_t *cellGroupConfig = ue_p->masterCellGroup;

  uint8_t buffer[RRC_BUF_SIZE] = {0};
  NR_SRB_ToAddModList_t *SRBs = createSRBlist(ue_p, false);
  NR_DRB_ToAddModList_t *DRBs = createDRBlist(ue_p, false);

  int size = do_RRCReconfiguration(ctxt_pP,
                                   buffer,
                                   RRC_BUF_SIZE,
                                   xid,
                                   SRBs,
                                   DRBs,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL,
                                   dedicatedNAS_MessageList,
                                   NULL,
                                   ue_context_pP,
                                   &rrc->carrier,
                                   &rrc->configuration,
                                   NULL,
                                   cellGroupConfig);
  LOG_DUMPMSG(NR_RRC,DEBUG_RRC,(char *)buffer,size,"[MSG] RRC Reconfiguration\n");
  freeSRBlist(SRBs);
  freeDRBlist(DRBs);
  LOG_I(NR_RRC, "[gNB %d] Frame %d, Logical Channel DL-DCCH, Generate RRCReconfiguration (bytes %d, UE RNTI %x)\n", ctxt_pP->module_id, ctxt_pP->frame, size, ue_p->rnti);
  LOG_D(NR_RRC,
        "[FRAME %05d][RRC_gNB][MOD %u][][--- PDCP_DATA_REQ/%d Bytes (rrcReconfiguration to UE %x MUI %d) --->][PDCP][MOD %u][RB %u]\n",
        ctxt_pP->frame,
        ctxt_pP->module_id,
        size,
        ue_p->rnti,
        rrc_gNB_mui,
        ctxt_pP->module_id,
        DCCH);

  nr_pdcp_data_req_srb(ctxt_pP->rntiMaybeUEid, DCCH, rrc_gNB_mui++, size, buffer, deliver_pdu_srb_f1, rrc, UU);

  // Skip MAC CellGroup update for Remote UE - they don't have MAC context at gNB
  if ((NODE_IS_DU(rrc->node_type) || NODE_IS_MONOLITHIC(rrc->node_type)) && !ue_p->is_remote_ue) {
    nr_rrc_mac_update_cellgroup(ue_context_pP->ue_context.rnti, ue_context_pP->ue_context.masterCellGroup);

    uint32_t delay_ms = ue_p->masterCellGroup && ue_p->masterCellGroup->spCellConfig && ue_p->masterCellGroup->spCellConfig->spCellConfigDedicated
                                && ue_p->masterCellGroup->spCellConfig->spCellConfigDedicated->downlinkBWP_ToAddModList
                            ? NR_RRC_RECONFIGURATION_DELAY_MS + NR_RRC_BWP_SWITCHING_DELAY_MS
                            : NR_RRC_RECONFIGURATION_DELAY_MS;

    nr_mac_enable_ue_rrc_processing_timer(ctxt_pP->module_id, ue_p->rnti, *rrc->carrier.servingcellconfigcommon->ssbSubcarrierSpacing, delay_ms);
  }
}

//-----------------------------------------------------------------------------
void
rrc_gNB_modify_dedicatedRRCReconfiguration(
  const protocol_ctxt_t     *const ctxt_pP,
  rrc_gNB_ue_context_t      *ue_context_pP)
//-----------------------------------------------------------------------------
{
  gNB_RRC_UE_t *ue_p = &ue_context_pP->ue_context;
  NR_DRB_ToAddModList_t *DRB_configList = fill_DRB_configList(ue_p);
  int qos_flow_index = 0;
  uint8_t xid = rrc_gNB_get_next_transaction_identifier(ctxt_pP->module_id);
  ue_p->xids[xid] = RRC_PDUSESSION_MODIFY;

  struct NR_RRCReconfiguration_v1530_IEs__dedicatedNAS_MessageList *dedicatedNAS_MessageList =
      CALLOC(1, sizeof(*dedicatedNAS_MessageList));
  NR_DRB_ToAddMod_t *DRB_config = NULL;

  for (int i = 0; i < ue_p->nb_of_pdusessions; i++) {
    // bypass the new and already configured pdu sessions
    if (ue_p->pduSession[i].status >= PDU_SESSION_STATUS_DONE) {
      ue_p->pduSession[i].xid = xid;
      continue;
    }

    if (ue_p->pduSession[i].cause != NGAP_CAUSE_NOTHING) {
      // set xid of failure pdu session
      ue_p->pduSession[i].xid = xid;
      ue_p->pduSession[i].status = PDU_SESSION_STATUS_FAILED;
      continue;
    }

    // search exist DRB_config
    int j;
    for (j = 0; i < MAX_DRBS_PER_UE; j++) {
      if (ue_p->established_drbs[j].status != DRB_INACTIVE
          && ue_p->established_drbs[j].cnAssociation.sdap_config.pdusession_id == ue_p->pduSession[i].param.pdusession_id)
        break;
    }

    if (j == MAX_DRBS_PER_UE) {
      ue_p->pduSession[i].xid = xid;
      ue_p->pduSession[i].status = PDU_SESSION_STATUS_FAILED;
      ue_p->pduSession[i].cause = NGAP_CAUSE_RADIO_NETWORK;
      ue_p->pduSession[i].cause_value = NGAP_CauseRadioNetwork_unspecified;
      continue;
    }

    // Reference TS23501 Table 5.7.4-1: Standardized 5QI to QoS characteristics mapping
    for (qos_flow_index = 0; qos_flow_index < ue_p->pduSession[i].param.nb_qos; qos_flow_index++) {
      switch (ue_p->pduSession[i].param.qos[qos_flow_index].fiveQI) {
        case 1: //100ms
        case 2: //150ms
        case 3: //50ms
        case 4: //300ms
        case 5: //100ms
        case 6: //300ms
        case 7: //100ms
        case 8: //300ms
        case 9: //300ms Video (Buffered Streaming)TCP-based (e.g., www, e-mail, chat, ftp, p2p file sharing, progressive video, etc.)
          // TODO
          break;

        default:
          LOG_E(NR_RRC, "not supported 5qi %lu\n", ue_p->pduSession[i].param.qos[qos_flow_index].fiveQI);
          ue_p->pduSession[i].status = PDU_SESSION_STATUS_FAILED;
          ue_p->pduSession[i].xid = xid;
          ue_p->pduSession[i].cause = NGAP_CAUSE_RADIO_NETWORK;
          ue_p->pduSession[i].cause_value = NGAP_CauseRadioNetwork_not_supported_5QI_value;
          continue;
      }
      LOG_I(NR_RRC,
            "PDU SESSION ID %ld, DRB ID %ld (index %d), QOS flow %d, 5QI %ld \n",
            DRB_config->cnAssociation->choice.sdap_Config->pdu_Session,
            DRB_config->drb_Identity,
            i,
            qos_flow_index,
            ue_p->pduSession[i].param.qos[qos_flow_index].fiveQI);
    }

    asn1cSeqAdd(&DRB_configList->list, DRB_config);

    ue_p->pduSession[i].status = PDU_SESSION_STATUS_DONE;
    ue_p->pduSession[i].xid = xid;

    if (ue_p->pduSession[i].param.nas_pdu.buffer != NULL) {
      asn1cSequenceAdd(dedicatedNAS_MessageList->list,NR_DedicatedNAS_Message_t, dedicatedNAS_Message);
      OCTET_STRING_fromBuf(dedicatedNAS_Message, (char *)ue_p->pduSession[i].param.nas_pdu.buffer, ue_p->pduSession[i].param.nas_pdu.length);
      LOG_I(NR_RRC, "add NAS info with size %d (pdusession id %d)\n", ue_p->pduSession[i].param.nas_pdu.length, ue_p->pduSession[i].param.pdusession_id);
    } else {
      LOG_W(NR_RRC, "no NAS info (pdusession id %d)\n", ue_p->pduSession[i].param.pdusession_id);
    }
  }

  /* If list is empty free the list and reset the address */
  if (dedicatedNAS_MessageList->list.count == 0) {
    free(dedicatedNAS_MessageList);
    dedicatedNAS_MessageList = NULL;
  }

  uint8_t buffer[RRC_BUF_SIZE];
  int size = do_RRCReconfiguration(ctxt_pP,
                                   buffer,
                                   RRC_BUF_SIZE,
                                   xid,
                                   NULL,
                                   DRB_configList,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL,
                                   dedicatedNAS_MessageList,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL);
  LOG_DUMPMSG(NR_RRC, DEBUG_RRC, (char *)buffer, size, "[MSG] RRC Reconfiguration\n");

  /* Free all NAS PDUs */
  for (int i = 0; i < ue_p->nb_of_pdusessions; i++) {
    if (ue_p->pduSession[i].param.nas_pdu.buffer != NULL) {
      /* Free the NAS PDU buffer and invalidate it */
      free(ue_p->pduSession[i].param.nas_pdu.buffer);
      ue_p->pduSession[i].param.nas_pdu.buffer = NULL;
    }
  }

  LOG_I(NR_RRC, "[gNB %d] Frame %d, Logical Channel DL-DCCH, Generate RRCReconfiguration (bytes %d, UE RNTI %x)\n", ctxt_pP->module_id, ctxt_pP->frame, size, ue_p->rnti);
  LOG_D(NR_RRC,
        "[FRAME %05d][RRC_gNB][MOD %u][][--- PDCP_DATA_REQ/%d Bytes (rrcReconfiguration to UE %x MUI %d) --->][PDCP][MOD %u][RB %u]\n",
        ctxt_pP->frame,
        ctxt_pP->module_id,
        size,
        ue_p->rnti,
        rrc_gNB_mui,
        ctxt_pP->module_id,
        DCCH);

  gNB_RRC_INST *rrc = RC.nrrrc[ctxt_pP->module_id];
  nr_pdcp_data_req_srb(ctxt_pP->rntiMaybeUEid, DCCH, rrc_gNB_mui++, size, buffer, deliver_pdu_srb_f1, rrc, UU);

  if (NODE_IS_DU(rrc->node_type) || NODE_IS_MONOLITHIC(rrc->node_type)) {
    uint32_t delay_ms = ue_p->masterCellGroup && ue_p->masterCellGroup->spCellConfig && ue_p->masterCellGroup->spCellConfig->spCellConfigDedicated
                                && ue_p->masterCellGroup->spCellConfig->spCellConfigDedicated->downlinkBWP_ToAddModList
                            ? NR_RRC_RECONFIGURATION_DELAY_MS + NR_RRC_BWP_SWITCHING_DELAY_MS
                            : NR_RRC_RECONFIGURATION_DELAY_MS;

    nr_mac_enable_ue_rrc_processing_timer(ctxt_pP->module_id, ue_p->rnti, *rrc->carrier.servingcellconfigcommon->ssbSubcarrierSpacing, delay_ms);
  }
}

//-----------------------------------------------------------------------------
void
rrc_gNB_generate_dedicatedRRCReconfiguration_release(
    const protocol_ctxt_t   *const ctxt_pP,
    rrc_gNB_ue_context_t    *const ue_context_pP,
    uint8_t                  xid,
    uint32_t                 nas_length,
    uint8_t                 *nas_buffer)
//-----------------------------------------------------------------------------
{
  gNB_RRC_UE_t *ue_p = &ue_context_pP->ue_context;

  NR_DRB_ToReleaseList_t *DRB_Release_configList2 = CALLOC(sizeof(*DRB_Release_configList2), 1);

  for (int i = 0; i < NB_RB_MAX; i++) {
    if ((ue_p->pduSession[i].status == PDU_SESSION_STATUS_TORELEASE) && ue_p->pduSession[i].xid == xid) {
      asn1cSequenceAdd(DRB_Release_configList2->list, NR_DRB_Identity_t, DRB_release);
      *DRB_release = i + 1;
    }
  }

  /* If list is empty free the list and reset the address */
  struct NR_RRCReconfiguration_v1530_IEs__dedicatedNAS_MessageList *dedicatedNAS_MessageList = NULL;
  if (nas_length > 0) {
    dedicatedNAS_MessageList = CALLOC(1, sizeof(*dedicatedNAS_MessageList));
    asn1cSequenceAdd(dedicatedNAS_MessageList->list, NR_DedicatedNAS_Message_t, dedicatedNAS_Message);
    OCTET_STRING_fromBuf(dedicatedNAS_Message, (char *)nas_buffer, nas_length);
    LOG_I(NR_RRC,"add NAS info with size %d\n", nas_length);
  } else {
    LOG_W(NR_RRC,"dedlicated NAS list is empty\n");
  }

  uint8_t buffer[RRC_BUF_SIZE] = {0};
  int size = do_RRCReconfiguration(ctxt_pP,
                                   buffer,
                                   RRC_BUF_SIZE,
                                   xid,
                                   NULL,
                                   NULL,
                                   DRB_Release_configList2,
                                   NULL,
                                   NULL,
                                   NULL,
                                   dedicatedNAS_MessageList,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL);
  LOG_DUMPMSG(NR_RRC,DEBUG_RRC,(char *)buffer,size, "[MSG] RRC Reconfiguration\n");

  /* Free all NAS PDUs */
  if (nas_length > 0) {
    /* Free the NAS PDU buffer and invalidate it */
    free(nas_buffer);
  }

  LOG_I(NR_RRC, "[gNB %d] Frame %d, Logical Channel DL-DCCH, Generate NR_RRCReconfiguration (bytes %d, UE RNTI %x)\n", ctxt_pP->module_id, ctxt_pP->frame, size, ue_p->rnti);
  LOG_D(NR_RRC,
        "[FRAME %05d][RRC_gNB][MOD %u][][--- PDCP_DATA_REQ/%d Bytes (rrcReconfiguration to UE %x MUI %d) --->][PDCP][MOD %u][RB %u]\n",
        ctxt_pP->frame,
        ctxt_pP->module_id,
        size,
        ue_p->rnti,
        rrc_gNB_mui,
        ctxt_pP->module_id,
        DCCH);

  gNB_RRC_INST *rrc = RC.nrrrc[ctxt_pP->module_id];
  nr_pdcp_data_req_srb(ctxt_pP->rntiMaybeUEid, DCCH, rrc_gNB_mui++, size, buffer, deliver_pdu_srb_f1, rrc, UU);

  if (NODE_IS_DU(rrc->node_type) || NODE_IS_MONOLITHIC(rrc->node_type)) {
    uint32_t delay_ms = ue_p->masterCellGroup && ue_p->masterCellGroup->spCellConfig && ue_p->masterCellGroup->spCellConfig->spCellConfigDedicated
                                && ue_p->masterCellGroup->spCellConfig->spCellConfigDedicated->downlinkBWP_ToAddModList
                            ? NR_RRC_RECONFIGURATION_DELAY_MS + NR_RRC_BWP_SWITCHING_DELAY_MS
                            : NR_RRC_RECONFIGURATION_DELAY_MS;

    nr_mac_enable_ue_rrc_processing_timer(ctxt_pP->module_id, ue_p->rnti, *rrc->carrier.servingcellconfigcommon->ssbSubcarrierSpacing, delay_ms);
  }
}

//-----------------------------------------------------------------------------
/*
* Process the RRC Reconfiguration Complete from the UE
*/
static void rrc_gNB_process_RRCReconfigurationComplete(const protocol_ctxt_t *const ctxt_pP, rrc_gNB_ue_context_t *ue_context_pP, const uint8_t xid)
{
  int                                 drb_id;
  uint8_t kRRCenc[16] = {0};
  uint8_t kRRCint[16] = {0};
  uint8_t kUPenc[16] = {0};
  uint8_t kUPint[16] = {0};
  gNB_RRC_UE_t *ue_p = &ue_context_pP->ue_context;
  NR_DRB_ToAddModList_t *DRB_configList = createDRBlist(ue_p, false);

  /* Derive the keys from kgnb */
  if (DRB_configList != NULL) {
    nr_derive_key(UP_ENC_ALG, ue_p->ciphering_algorithm, ue_p->kgnb, kUPenc);
    nr_derive_key(UP_INT_ALG, ue_p->integrity_algorithm, ue_p->kgnb, kUPint);
  }

  nr_derive_key(RRC_ENC_ALG, ue_p->ciphering_algorithm, ue_p->kgnb, kRRCenc);
  nr_derive_key(RRC_INT_ALG, ue_p->integrity_algorithm, ue_p->kgnb, kRRCint);

  /* Refresh SRBs/DRBs */
  LOG_D(NR_RRC, "Configuring PDCP DRBs/SRBs for UE %04x\n", ue_p->rnti);
  ue_id_t reestablish_ue_id = 0;
  for (int i = 0; i < MAX_MOBILES_PER_GNB; i++) {
    nr_reestablish_rnti_map_t *nr_reestablish_rnti_map = &(RC.nrrrc[ctxt_pP->module_id])->nr_reestablish_rnti_map[i];
    if (nr_reestablish_rnti_map->ue_id == ctxt_pP->rntiMaybeUEid) {
      ue_context_pP->ue_context.ue_reconfiguration_after_reestablishment_counter++;
      reestablish_ue_id = nr_reestablish_rnti_map[i].c_rnti;
      LOG_D(NR_RRC,
            "Removing reestablish_rnti_map[%d] UEid %lx, RNTI %04x\n",
            i,
            nr_reestablish_rnti_map->ue_id,
            nr_reestablish_rnti_map->c_rnti);
      // clear current C-RNTI from map
      nr_reestablish_rnti_map->ue_id = 0;
      nr_reestablish_rnti_map->c_rnti = 0;
      break;
    }
  }
  NR_SRB_ToAddModList_t *SRBs = createSRBlist(ue_p, false);

  nr_pdcp_add_srbs(ctxt_pP->enb_flag,
                   ctxt_pP->rntiMaybeUEid,
                   SRBs,
                   (ue_p->integrity_algorithm << 4) | ue_p->ciphering_algorithm,
                   kRRCenc,
                   kRRCint, UU);
  freeSRBlist(SRBs);

  /* Only configure PDCP/RLC for NEW DRBs, not for parameter updates
   * Check DRB_active[] to determine which DRBs are actually new
   * This prevents unnecessary DRB recreation attempts on every RRCReconfigurationComplete
   * (e.g., during SL MCS adaptation, only SL parameters change - DRBs are unchanged)
   */
  NR_DRB_ToAddModList_t *newDRBs = NULL;
  int new_drb_count = 0;

  if (DRB_configList != NULL) {
    // First pass: count how many DRBs are actually new
    for (int i = 0; i < DRB_configList->list.count; i++) {
      if (DRB_configList->list.array[i]) {
        int drb_id = (int)DRB_configList->list.array[i]->drb_Identity;
        if (ue_p->DRB_active[drb_id - 1] == 0) {
          new_drb_count++;
        }
      }
    }

    // If there are new DRBs, create a filtered list containing only new ones
    if (new_drb_count > 0) {
      newDRBs = CALLOC(1, sizeof(NR_DRB_ToAddModList_t));
      for (int i = 0; i < DRB_configList->list.count; i++) {
        if (DRB_configList->list.array[i]) {
          int drb_id = (int)DRB_configList->list.array[i]->drb_Identity;
          if (ue_p->DRB_active[drb_id - 1] == 0) {
            asn1cSeqAdd(&newDRBs->list, DRB_configList->list.array[i]);
            LOG_D(NR_RRC, "[gNB] UE %04x: Adding NEW DRB %d to PDCP configuration\n",
                  ue_p->rnti, drb_id);
          } else {
            LOG_D(NR_RRC, "[gNB] UE %04x: Skipping PDCP config for existing DRB %d (parameter update only)\n",
                  ue_p->rnti, drb_id);
          }
        }
      }

      // Configure PDCP/RLC only for new DRBs
      nr_pdcp_add_drbs(ctxt_pP->enb_flag,
                       ctxt_pP->rntiMaybeUEid,
                       reestablish_ue_id,
                       newDRBs,
                       (ue_p->integrity_algorithm << 4) | ue_p->ciphering_algorithm,
                       kUPenc,
                       kUPint,
                       get_softmodem_params()->sa ? ue_p->masterCellGroup->rlc_BearerToAddModList : NULL,
                       ue_p->is_remote_ue);

      LOG_D(NR_RRC, "[gNB] UE %04x: Configured %d new DRB(s) in PDCP/RLC\n",
            ue_p->rnti, new_drb_count);

      // Free the filtered list (not the DRB objects themselves, they're referenced from original list)
      free(newDRBs);
    } else {
      LOG_D(NR_RRC, "[gNB] UE %04x: RRCReconfigurationComplete received - no new DRBs to configure (parameter update only)\n",
            ue_p->rnti);
    }
  }

  /* Refresh DRBs */
  if (!NODE_IS_CU(RC.nrrrc[ctxt_pP->module_id]->node_type)) {
    LOG_D(NR_RRC,"Configuring RLC DRBs/SRBs for UE %04x\n",ue_context_pP->ue_context.rnti);

    // Remote UE doesn't have direct MAC/RLC configuration - skip this step
    if (ue_p->is_remote_ue) {
      LOG_D(NR_RRC, "[REMOTE_UE] Skipping RLC DRB configuration (no direct MAC/RLC, uses SRAP forwarding)\n");
    } else {
      const struct NR_CellGroupConfig__rlc_BearerToAddModList *bearer_list =
          ue_context_pP->ue_context.masterCellGroup->rlc_BearerToAddModList;
      nr_rrc_addmod_drbs(ctxt_pP->rntiMaybeUEid, DRB_configList, bearer_list);

      // Check if Relay UE needs RRCReconfiguration for DRB 2 reset
      if (ue_p->relay_drb2_needs_reconfig) {
        LOG_D(NR_RRC, "[Relay UE] DRB 2 reconfigured (new Remote UE session) - sending RRCReconfig to reset RLC entity\n");

        // Create DRB list with DRB 2 marked for reestablishment
        NR_DRB_ToAddModList_t *drb_list = CALLOC(1, sizeof(NR_DRB_ToAddModList_t));

        // Mark DRB 2 for reestablishment in the ue_p structure
        ue_p->established_drbs[1].reestablishPDCP = NR_DRB_ToAddMod__reestablishPDCP_true;

        // Generate proper ASN.1 DRB structure with all required fields
        NR_DRB_ToAddMod_t *drb = generateDRB_ASN1(&ue_p->established_drbs[1]);
        asn1cSeqAdd(&drb_list->list, drb);

        // Use the standard RRC message generation function
        uint8_t buffer[RRC_BUF_SIZE];
        int size = do_RRCReconfiguration(ctxt_pP,
                                         buffer,
                                         RRC_BUF_SIZE,
                                         0, // Transaction_id
                                         NULL, // SRB_configList
                                         drb_list, // DRB_configList (with reestablishPDCP)
                                         NULL, // DRB_releaseList
                                         NULL, // security_config
                                         NULL, // sdap_config
                                         NULL, // meas_config
                                         NULL, // dedicatedNAS_MessageList
                                         NULL, // rrc_ext_v1610
                                         ue_context_pP,
                                         &RC.nrrrc[ctxt_pP->module_id]->carrier,
                                         &RC.nrrrc[ctxt_pP->module_id]->configuration,
                                         NULL, // mac_CellGroupConfig
                                         NULL); // cellGroupConfig (no MAC/PHY changes)

        AssertFatal(size > 0, "Failed to generate RRCReconfiguration for Relay DRB 2 reset\n");

        LOG_D(NR_RRC, "[Relay UE] Generated RRCReconfiguration (%d bytes) with DRB 2 reestablishPDCP=true\n", size);

        // Send via SRB1/DCCH
        nr_pdcp_data_req_srb(ctxt_pP->rntiMaybeUEid, DCCH, rrc_gNB_mui++, size, buffer, deliver_pdu_srb_f1, RC.nrrrc[ctxt_pP->module_id], UU);

        // Free the DRB list
        freeDRBlist(drb_list);

        // Reset the flag
        ue_p->established_drbs[1].reestablishPDCP = -1;
        ue_p->relay_drb2_needs_reconfig = false;
      }
    }
  }

  /* Loop through DRBs and establish if necessary */
  if (DRB_configList != NULL) {
    for (int i = 0; i < DRB_configList->list.count; i++) {
      if (DRB_configList->list.array[i]) {
        drb_id = (int)DRB_configList->list.array[i]->drb_Identity;
        LOG_A(NR_RRC,
              "[gNB %d] Frame  %d : Logical Channel UL-DCCH, Received NR_RRCReconfigurationComplete from UE rnti %lx, reconfiguring DRB %d\n",
              ctxt_pP->module_id,
              ctxt_pP->frame,
              ctxt_pP->rntiMaybeUEid,
              (int)DRB_configList->list.array[i]->drb_Identity);
        //(int)*DRB_configList->list.array[i]->pdcp_Config->moreThanOneRLC->primaryPath.logicalChannel);

        if (ue_p->DRB_active[drb_id - 1] == 0) {
          ue_p->DRB_active[drb_id - 1] = DRB_ACTIVE;
          LOG_D(NR_RRC, "[gNB %d] Frame %d: Establish RLC UM Bidirectional, DRB %d Active\n",
                  ctxt_pP->module_id, ctxt_pP->frame, (int)DRB_configList->list.array[i]->drb_Identity);

          LOG_D(NR_RRC,
                  PROTOCOL_NR_RRC_CTXT_UE_FMT" RRC_gNB --- MAC_CONFIG_REQ  (DRB) ---> MAC_gNB\n",
                  PROTOCOL_NR_RRC_CTXT_UE_ARGS(ctxt_pP));

          //if (DRB_configList->list.array[i]->pdcp_Config->moreThanOneRLC->primaryPath.logicalChannel) {
          //  nr_DRB2LCHAN[i] = (uint8_t) * DRB_configList->list.array[i]->pdcp_Config->moreThanOneRLC->primaryPath.logicalChannel;
          //}

            // rrc_mac_config_req_eNB
        } else { // remove LCHAN from MAC/PHY
          if (ue_p->DRB_active[drb_id] == 1) {
            /* TODO : It may be needed if gNB goes into full stack working. */
            // DRB has just been removed so remove RLC + PDCP for DRB
            /*      rrc_pdcp_config_req (ctxt_pP->module_id, frameP, 1, CONFIG_ACTION_REMOVE,
            (ue_mod_idP * NB_RB_MAX) + DRB2LCHAN[i],UNDEF_SECURITY_MODE);
            */
            /*rrc_rlc_config_req(ctxt_pP,
                                SRB_FLAG_NO,
                                MBMS_FLAG_NO,
                                CONFIG_ACTION_REMOVE,
                                nr_DRB2LCHAN[i]);*/
          }

          // ue_p->DRB_active[drb_id] = 0;
          LOG_D(NR_RRC, PROTOCOL_NR_RRC_CTXT_UE_FMT" RRC_eNB --- MAC_CONFIG_REQ  (DRB) ---> MAC_eNB\n",
                  PROTOCOL_NR_RRC_CTXT_UE_ARGS(ctxt_pP));

          // rrc_mac_config_req_eNB

        } // end else of if (ue_p->DRB_active[drb_id] == 0)
      } // end if (DRB_configList->list.array[i])
    } // end for (int i = 0; i < DRB_configList->list.count; i++)

  } // end if DRB_configList != NULL
  freeDRBlist(DRB_configList);
}

//-----------------------------------------------------------------------------
void rrc_gNB_generate_RRCReestablishment(const protocol_ctxt_t *ctxt_pP,
                                         rrc_gNB_ue_context_t *ue_context_pP,
                                         const uint8_t *masterCellGroup_from_DU,
                                         NR_ServingCellConfigCommon_t *scc,
                                         const int CC_id)
//-----------------------------------------------------------------------------
{
  // int UE_id = -1;
  // NR_LogicalChannelConfig_t  *SRB1_logicalChannelConfig = NULL;
  // NR_SRB_ToAddMod_t          *SRB1_config = NULL;
  // rrc_gNB_carrier_data_t     *carrier = NULL;
  module_id_t module_id = ctxt_pP->module_id;
  gNB_RRC_INST *rrc = RC.nrrrc[ctxt_pP->module_id];
  int enable_ciphering = 0;
  gNB_RRC_UE_t *ue_p = &ue_context_pP->ue_context;

  // Need to drop spCellConfig when there is a RRCReestablishment
  // Save spCellConfig in spCellConfigReestablishment to recover after Reestablishment is completed
  ue_p->spCellConfigReestablishment = ue_p->masterCellGroup->spCellConfig;
  ue_p->masterCellGroup->spCellConfig = NULL;

  uint8_t buffer[RRC_BUF_SIZE] = {0};
  uint8_t xid = rrc_gNB_get_next_transaction_identifier(module_id);
  ue_p->xids[xid] = RRC_REESTABLISH;
  NR_SRB_ToAddModList_t *SRBs = createSRBlist(ue_p, true);
  int size = do_RRCReestablishment(ctxt_pP,
                                   ue_context_pP,
                                   CC_id,
                                   buffer,
                                   RRC_BUF_SIZE,
                                   xid,
                                   SRBs,
                                   masterCellGroup_from_DU,
                                   scc,
                                   &rrc->carrier);

  LOG_I(NR_RRC, "[RAPROC] UE %04x Logical Channel DL-DCCH, Generating NR_RRCReestablishment (bytes %d)\n", ue_p->rnti, size);

  uint8_t kRRCenc[16] = {0};
  uint8_t kRRCint[16] = {0};
  uint8_t kUPenc[16] = {0};
  /* Derive the keys from kgnb */
  if (ue_p->Srb[1].Active)
    nr_derive_key(UP_ENC_ALG, ue_p->ciphering_algorithm, ue_p->kgnb, kUPenc);

  nr_derive_key(RRC_ENC_ALG, ue_p->ciphering_algorithm, ue_p->kgnb, kRRCenc);
  nr_derive_key(RRC_INT_ALG, ue_p->integrity_algorithm, ue_p->kgnb, kRRCint);

  /* Configure SRB1 for UE */
  nr_pdcp_add_srbs(ctxt_pP->enb_flag, ctxt_pP->rntiMaybeUEid, SRBs, 0, NULL, NULL, UU);
  LOG_D(NR_RRC, "UE %04x --- MAC_CONFIG_REQ  (SRB1) ---> MAC_gNB\n", ue_p->rnti);
  freeSRBlist(SRBs);
  LOG_I(NR_RRC, "Set PDCP security RNTI %04lx nca %ld nia %d in RRCReestablishment\n", ctxt_pP->rntiMaybeUEid, ue_p->ciphering_algorithm, ue_p->integrity_algorithm);
  uint8_t security_mode =
      enable_ciphering ? ue_p->ciphering_algorithm | (ue_p->integrity_algorithm << 4) : 0 | (ue_p->integrity_algorithm << 4);

  nr_pdcp_config_set_security(ctxt_pP->rntiMaybeUEid,
                              DCCH,
                              security_mode,
                              kRRCenc,
                              kRRCint,
                              kUPenc);

  if (!NODE_IS_CU(rrc->node_type)) {
    apply_macrlc_config_reest(rrc, ue_context_pP, ctxt_pP, ctxt_pP->rntiMaybeUEid);
  }

  nr_pdcp_data_req_srb(ctxt_pP->rntiMaybeUEid, DCCH, rrc_gNB_mui++, size, buffer, deliver_pdu_srb_f1, rrc, UU);
}

/// @brief Function used in RRCReestablishmentComplete procedure to update the NGU Tunnels.
/// @param reestablish_rnti is the old C-RNTI
void RRCReestablishmentComplete_update_ngu_tunnel(const protocol_ctxt_t *const ctxt_pP,
                                                  rrc_gNB_ue_context_t *ue_context_pP,
                                                  const rnti_t reestablish_rnti)
{
  gNB_RRC_UE_t *ue_p = &ue_context_pP->ue_context;
  int i = 0;
  int j = 0;
  int ret = 0;

  if (get_softmodem_params()->sa) {
    LOG_W(NR_RRC, "RRC Reestablishment - Rework identity mapping need to be done properly!\n");
    gtpv1u_gnb_create_tunnel_req_t create_tunnel_req = {0};
    /* Save e RAB information for later */

    for (j = 0, i = 0; i < NB_RB_MAX; i++) {
      if (ue_p->pduSession[i].status == PDU_SESSION_STATUS_ESTABLISHED || ue_p->pduSession[i].status == PDU_SESSION_STATUS_DONE) {
        create_tunnel_req.pdusession_id[j] = ue_p->pduSession[i].param.pdusession_id;
        create_tunnel_req.incoming_rb_id[j] = i + 1;
        create_tunnel_req.outgoing_teid[j] = ue_p->pduSession[i].param.gtp_teid;
        // to be developped, use the first QFI only
        create_tunnel_req.outgoing_qfi[j] = ue_p->pduSession[i].param.qos[0].qfi;
        memcpy(create_tunnel_req.dst_addr[j].buffer, ue_p->pduSession[i].param.upf_addr.buffer, sizeof(uint8_t) * 20);
        create_tunnel_req.dst_addr[j].length = ue_p->pduSession[i].param.upf_addr.length;
        j++;
      }
    }

    create_tunnel_req.ue_id = ctxt_pP->rntiMaybeUEid; // warning put zero above
    create_tunnel_req.num_tunnels = j;
    ret = gtpv1u_update_ngu_tunnel(ctxt_pP->instance, &create_tunnel_req, reestablish_rnti);

    if (ret != 0) {
      LOG_E(NR_RRC, "RRC Reestablishment - gtpv1u_update_ngu_tunnel failed,start to release UE %x\n", reestablish_rnti);
      AssertFatal(false, "not implemented\n");
      return;
    }
  }
}

/// @brief Function used in RRCReestablishmentComplete procedure to update the NAS PDUSessions and the xid.
/// @param old_xid Refers to the old transaction identifier passed to rrc_gNB_process_RRCReestablishmentComplete as xid.
/// @todo parameters yet to process inside the for loop.
/// @todo should test if pdu session are Ok before! inside the for loop.
void RRCReestablishmentComplete_nas_pdu_update(rrc_gNB_ue_context_t *ue_context_pP, const uint8_t old_xid)
{
  gNB_RRC_UE_t *ue_p = &ue_context_pP->ue_context;
  /* Add all NAS PDUs to the list */
  for (int i = 0; i < ue_p->nb_of_pdusessions; i++) {
    ue_p->pduSession[i].status = PDU_SESSION_STATUS_DONE;
    ue_p->pduSession[i].xid = old_xid;
    LOG_D(NR_RRC,
          "RRC Reestablishment - setting the status for the default DRB (index %d) to (%d,%s)\n",
          i,
          ue_p->pduSession[i].status,
          "PDU_SESSION_STATUS_DONE");
  }
}

/// @brief Function used in RRCReestablishmentComplete procedure to Free all the NAS PDU buffers.
void RRCReestablishmentComplete_nas_pdu_free(rrc_gNB_ue_context_t *ue_context_pP)
{
  gNB_RRC_UE_t *ue_p = &ue_context_pP->ue_context;
  /* Free all NAS PDUs */
  for (int i = 0; i < ue_p->nb_of_pdusessions; i++) {
    if (ue_p->pduSession[i].param.nas_pdu.buffer != NULL) {
      /* Free the NAS PDU buffer and invalidate it */
      free(ue_p->pduSession[i].param.nas_pdu.buffer);
      ue_p->pduSession[i].param.nas_pdu.buffer = NULL;
    }
  }
}

/// @brief Function tha processes RRCReestablishmentComplete message sent by the UE, after RRCReestasblishment request.
/// @param ctxt_pP Protocol context containing information regarding the UE and gNB
/// @param reestablish_rnti is the old C-RNTI
/// @param ue_context_pP  UE context container information regarding the UE
/// @param xid Transaction Identifier used in RRC messages
void rrc_gNB_process_RRCReestablishmentComplete(const protocol_ctxt_t *const ctxt_pP,
                                                const rnti_t reestablish_rnti,
                                                rrc_gNB_ue_context_t *ue_context_pP,
                                                const uint8_t xid)
{
  gNB_RRC_UE_t *ue_p = &ue_context_pP->ue_context;
  LOG_I(NR_RRC,
        "[RAPROC] UE %04x Logical Channel UL-DCCH, processing NR_RRCReestablishmentComplete from UE (SRB1 Active)\n",
        ue_p->rnti);

  int i = 0;

  uint8_t new_xid = rrc_gNB_get_next_transaction_identifier(ctxt_pP->module_id);
  ue_p->xids[new_xid] = RRC_REESTABLISH_COMPLETE;
  ue_p->StatusRrc = NR_RRC_CONNECTED;

  ue_p->Srb[1].Active = 1;
  if (get_softmodem_params()->sa) {
    uint8_t send_security_mode_command = false;
    nr_rrc_pdcp_config_security(ctxt_pP, ue_context_pP, send_security_mode_command);
    LOG_D(NR_RRC, "RRC Reestablishment - set security successfully \n");
  }
  RRCReestablishmentComplete_update_ngu_tunnel(ctxt_pP, ue_context_pP, reestablish_rnti);
  RRCReestablishmentComplete_nas_pdu_update(ue_context_pP, xid);

  /* Update RNTI in ue_context */
  LOG_I(NR_RRC, "RRC Reestablishment - Updating UEid from %04x to %lx\n", ue_p->rnti, ctxt_pP->rntiMaybeUEid);
  rrc_gNB_update_ue_context_rnti(ctxt_pP->rntiMaybeUEid, RC.nrrrc[ctxt_pP->module_id], ue_p->gNB_ue_ngap_id);

  gNB_RRC_INST *rrc = RC.nrrrc[ctxt_pP->module_id];
  NR_CellGroupConfig_t *cellGroupConfig = calloc(1, sizeof(NR_CellGroupConfig_t));

  // Revert spCellConfig stored in spCellConfigReestablishment before had been dropped during RRC Reestablishment
  ue_p->masterCellGroup->spCellConfig = ue_p->spCellConfigReestablishment;
  ue_p->spCellConfigReestablishment = NULL;
  cellGroupConfig->spCellConfig = ue_p->masterCellGroup->spCellConfig;
  cellGroupConfig->mac_CellGroupConfig = ue_p->masterCellGroup->mac_CellGroupConfig;
  cellGroupConfig->physicalCellGroupConfig = ue_p->masterCellGroup->physicalCellGroupConfig;
  cellGroupConfig->rlc_BearerToReleaseList = NULL;
  cellGroupConfig->rlc_BearerToAddModList = calloc(1, sizeof(*cellGroupConfig->rlc_BearerToAddModList));

  /*
   * Get SRB2, DRB configuration from the existing UE context,
   * also start from SRB2 (i=1) and not from SRB1 (i=0).
   */
  for (i = 1; i < ue_p->masterCellGroup->rlc_BearerToAddModList->list.count; ++i)
    asn1cSeqAdd(&cellGroupConfig->rlc_BearerToAddModList->list, ue_p->masterCellGroup->rlc_BearerToAddModList->list.array[i]);

  for (i = 0; i < cellGroupConfig->rlc_BearerToAddModList->list.count; i++) {
    asn1cCallocOne(cellGroupConfig->rlc_BearerToAddModList->list.array[i]->reestablishRLC,
                   NR_RLC_BearerConfig__reestablishRLC_true);
  }

  NR_SRB_ToAddModList_t *SRBs = createSRBlist(ue_p, true);
  NR_DRB_ToAddModList_t *DRBs = createDRBlist(ue_p, true);

  uint8_t buffer[RRC_BUF_SIZE] = {0};
  int size = do_RRCReconfiguration(ctxt_pP,
                                   buffer,
                                   RRC_BUF_SIZE,
                                   new_xid,
                                   SRBs,
                                   DRBs,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL, // MeasObj_list,
                                   NULL,
                                   NULL,
                                   ue_context_pP,
                                   &rrc->carrier,
                                   NULL,
                                   NULL,
                                   cellGroupConfig);
  freeSRBlist(SRBs);
  freeDRBlist(DRBs);
  LOG_DUMPMSG(NR_RRC, DEBUG_RRC, (char *)buffer, size, "[MSG] RRC Reconfiguration\n");

  RRCReestablishmentComplete_nas_pdu_free(ue_context_pP);

  if (size < 0) {
    LOG_E(NR_RRC, "RRC decode err!!! do_RRCReconfiguration\n");
    return;
  } else {
    LOG_I(NR_RRC,
          "[gNB %d] Frame %d, Logical Channel DL-DCCH, Generate NR_RRCReconfiguration (bytes %d, UE id %04x)\n",
          ctxt_pP->module_id,
          ctxt_pP->frame,
          size,
          ue_p->rnti);
    LOG_D(NR_RRC,
          "[FRAME %05d][RRC_gNB][MOD %u][][--- PDCP_DATA_REQ/%d Bytes (RRCReconfiguration to UE %04x MUI %d) --->][PDCP][MOD "
          "%u][RB %u]\n",
          ctxt_pP->frame,
          ctxt_pP->module_id,
          size,
          ue_p->rnti,
          rrc_gNB_mui,
          ctxt_pP->module_id,
          DCCH);

    nr_rrc_mac_update_cellgroup(ue_context_pP->ue_context.rnti, cellGroupConfig);
    nr_pdcp_data_req_srb(ctxt_pP->rntiMaybeUEid, DCCH, rrc_gNB_mui++, size, buffer, deliver_pdu_srb_f1, rrc, UU);
  }

  if (NODE_IS_DU(RC.nrrrc[ctxt_pP->module_id]->node_type) || NODE_IS_MONOLITHIC(RC.nrrrc[ctxt_pP->module_id]->node_type)) {
    uint32_t delay_ms = ue_p->masterCellGroup && ue_p->masterCellGroup->spCellConfig
                                && ue_p->masterCellGroup->spCellConfig->spCellConfigDedicated
                                && ue_p->masterCellGroup->spCellConfig->spCellConfigDedicated->downlinkBWP_ToAddModList
                            ? NR_RRC_RECONFIGURATION_DELAY_MS + NR_RRC_BWP_SWITCHING_DELAY_MS
                            : NR_RRC_RECONFIGURATION_DELAY_MS;

    nr_mac_enable_ue_rrc_processing_timer(ctxt_pP->module_id,
                                          ue_p->rnti,
                                          *RC.nrrrc[ctxt_pP->module_id]->carrier.servingcellconfigcommon->ssbSubcarrierSpacing,
                                          delay_ms);
  }
}
//-----------------------------------------------------------------------------

int nr_rrc_reconfiguration_req(rrc_gNB_ue_context_t         *const ue_context_pP,
                               protocol_ctxt_t              *const ctxt_pP,
                               const int                    dl_bwp_id,
                               const int                    ul_bwp_id) {

  uint8_t xid = rrc_gNB_get_next_transaction_identifier(ctxt_pP->module_id);
  gNB_RRC_UE_t *ue_p = &ue_context_pP->ue_context;

  NR_CellGroupConfig_t *masterCellGroup = ue_p->masterCellGroup;
  if (dl_bwp_id > 0) {
    *masterCellGroup->spCellConfig->spCellConfigDedicated->firstActiveDownlinkBWP_Id = dl_bwp_id;
    *masterCellGroup->spCellConfig->spCellConfigDedicated->defaultDownlinkBWP_Id = dl_bwp_id;
  }
  if (ul_bwp_id > 0) {
    *masterCellGroup->spCellConfig->spCellConfigDedicated->uplinkConfig->firstActiveUplinkBWP_Id = ul_bwp_id;
  }

  uint8_t buffer[RRC_BUF_SIZE];
  int size = do_RRCReconfiguration(ctxt_pP,
                                       buffer,
                                       RRC_BUF_SIZE,
                                       xid,
                                       NULL,
                                       NULL,
                                       NULL,
                                       NULL,
                                       NULL,
                                       NULL,
                                       NULL,
                                       NULL,
                                       ue_context_pP,
                                       NULL,
                                       NULL,
                                       NULL,
                                       masterCellGroup);

  nr_rrc_mac_update_cellgroup(ue_context_pP->ue_context.rnti, masterCellGroup);

  gNB_RRC_INST *rrc = RC.nrrrc[ctxt_pP->module_id];
  nr_pdcp_data_req_srb(ctxt_pP->rntiMaybeUEid, DCCH, rrc_gNB_mui++, size, buffer, deliver_pdu_srb_f1, rrc, UU);

  if (NODE_IS_DU(rrc->node_type) || NODE_IS_MONOLITHIC(rrc->node_type)) {
    uint32_t delay_ms = ue_p->masterCellGroup && ue_p->masterCellGroup->spCellConfig && ue_p->masterCellGroup->spCellConfig->spCellConfigDedicated
                                && ue_p->masterCellGroup->spCellConfig->spCellConfigDedicated->downlinkBWP_ToAddModList
                            ? NR_RRC_RECONFIGURATION_DELAY_MS + NR_RRC_BWP_SWITCHING_DELAY_MS
                            : NR_RRC_RECONFIGURATION_DELAY_MS;

    nr_mac_enable_ue_rrc_processing_timer(ctxt_pP->module_id, ue_p->rnti, *rrc->carrier.servingcellconfigcommon->ssbSubcarrierSpacing, delay_ms);
  }

  return 0;
}

//-----------------------------------------------------------------------------
int nr_rrc_reconfiguration_req_sidelink(rrc_gNB_ue_context_t                         *const ue_context_pP,
                                        const protocol_ctxt_t                        *const ctxt_pP,
                                        NR_SidelinkUEInformationNR_r16_IEs_t         *sl_UEInfo_r16,
                                        const NR_SL_UE_AssistanceInformationNR_r16_t *trafficPatternList) {

  bool has_mcs_override = ue_context_pP != NULL;
  if (sl_UEInfo_r16 == NULL && trafficPatternList == NULL && !has_mcs_override)
    return 0;

  uint8_t xid = rrc_gNB_get_next_transaction_identifier(ctxt_pP->module_id);
  gNB_RRC_UE_t *ue_p = &ue_context_pP->ue_context;
  ue_p->xids[xid] = RRC_REESTABLISH_COMPLETE;

  NR_RRCReconfiguration_v1610_IEs_t* rrc_ext_v1610 = NULL;
  rnti_t assigned_sl_rnti = ctxt_pP->rntiMaybeUEid;
  NR_SL_TxResourceReqList_r16_t *sl_TxRscReqList_r16 = (sl_UEInfo_r16 != NULL) ? sl_UEInfo_r16->sl_TxResourceReqList_r16 : NULL;
  rrc_ext_v1610 = prepare_rrc_reconfig_v1610(ctxt_pP->module_id, assigned_sl_rnti, sl_TxRscReqList_r16, trafficPatternList, ue_context_pP);


  uint8_t buffer[RRC_BUF_SIZE];
  int size = do_RRCReconfiguration(ctxt_pP,
                                       buffer,
                                       RRC_BUF_SIZE,
                                       xid,
                                       NULL,
                                       NULL,
                                       NULL,
                                       NULL,
                                       NULL,
                                       NULL,
                                       NULL,
                                       rrc_ext_v1610,
                                       ue_context_pP,
                                       NULL,
                                       NULL,
                                       NULL,
                                       NULL);
  if (rrc_ext_v1610->sl_ConfigDedicatedNR_r16)
    free_nr_sl_SetupRelease_SL_ConfigDedicatedNR_r16(rrc_ext_v1610->sl_ConfigDedicatedNR_r16);

  gNB_RRC_INST *rrc = RC.nrrrc[ctxt_pP->module_id];
  nr_pdcp_data_req_srb(ctxt_pP->rntiMaybeUEid, DCCH, rrc_gNB_mui++, size, buffer, deliver_pdu_srb_f1, rrc, UU);

  return 0;
}

/*------------------------------------------------------------------------------*/
int nr_rrc_gNB_decode_ccch(module_id_t module_id, rnti_t rnti, const uint8_t *buffer, int buffer_length,
                            const uint8_t *du_to_cu_rrc_container, int du_to_cu_rrc_container_len,
                            bool remote_ue_context, rnti_t relay_rnti, uint8_t remote_id)
{
  module_id_t                                       Idx;
  asn_dec_rval_t                                    dec_rval;
  NR_UL_CCCH_Message_t *ul_ccch_msg = NULL;
  gNB_RRC_INST *gnb_rrc_inst = RC.nrrrc[module_id];
  NR_RRCSetupRequest_IEs_t                         *rrcSetupRequest = NULL;
  NR_RRCReestablishmentRequest_IEs_t rrcReestablishmentRequest;

  LOG_I(NR_RRC, "Decoding CCCH: RNTI %04x, payload_size %d\n", rnti, buffer_length);

  /* Check for SRAP U2N header (L2 relay - Remote UE via Relay UE)
     Can be passed via parameters (from SRAP processor) or detected in buffer (legacy path) */
  bool is_from_remote_ue = remote_ue_context;
  rnti_t relay_ue_rnti = relay_rnti;
  uint8_t remote_ue_id = remote_id;
  const uint8_t *rrc_pdu = buffer;
  int rrc_pdu_length = buffer_length;

  if (remote_ue_context) {
    LOG_D(NR_RRC, "[gNB] CCCH from Remote UE ID %d via Relay 0x%04x, RRC payload %d bytes (already stripped by SRAP processor)\n",
          remote_ue_id, relay_ue_rnti, rrc_pdu_length);
  }

  // If Remote UE context not already provided, check buffer for legacy SRAP header detection
  if (!remote_ue_context && get_softmodem_params()->relay_type == U2N && buffer_length > SRAP_U2N_HDR_LEN) {
    /* Check if this message has a valid SRAP U2N header (2 bytes)
       SRAP Octet1 format: [D/C=1][Reserved=00][Bearer ID (5 bits)]
       Only process as SRAP if it looks like a valid header */
    uint8_t octet1 = buffer[0];
    uint8_t octet2 = buffer[1];

    // Check D/C bit (bit 7) = 1 for Data
    bool is_data_pdu = (octet1 & 0x80) != 0;
    // Check Reserved bits (bits 6-5) = 00
    bool reserved_bits_ok = (octet1 & 0x60) == 0;
    // Bearer ID should be valid (0-2 for SRB0-SRB2)
    uint8_t bearer_id = octet1 & 0x1F;
    bool bearer_id_valid = (bearer_id <= 2);

    // Remote UE ID in octet2 (0 = Relay UE itself, 1-255 = Remote UE)
    remote_ue_id = octet2;

    // Only treat as SRAP if header format is valid AND remote_ue_id != 0
    if (is_data_pdu && reserved_bits_ok && bearer_id_valid && remote_ue_id != 0) {
      // This message is from Remote UE via Relay UE with valid SRAP header
      is_from_remote_ue = true;
      relay_ue_rnti = rnti;  // Current RNTI is Relay UE's RNTI

      // Skip SRAP header to get RRC PDU
      rrc_pdu = buffer + SRAP_U2N_HDR_LEN;
      rrc_pdu_length = buffer_length - SRAP_U2N_HDR_LEN;

      LOG_D(NR_RRC, "[gNB] Message from Remote UE ID %d via Relay UE RNTI 0x%04x, bearer %d (SRAP validated)\n",
            remote_ue_id, relay_ue_rnti, bearer_id);
    } else {
      // Not a valid SRAP header, treat as normal RRC message
      LOG_D(NR_RRC, "[gNB] No valid SRAP header detected (octet1=0x%02x, octet2=0x%02x), processing as normal RRC\n",
            octet1, octet2);
    }
  }

  dec_rval = uper_decode(NULL, &asn_DEF_NR_UL_CCCH_Message, (void **) &ul_ccch_msg, rrc_pdu, rrc_pdu_length, 0, 0);

  if (dec_rval.code != RC_OK || dec_rval.consumed == 0) {
    LOG_E(NR_RRC, " FATAL Error in receiving CCCH\n");
    return -1;
  }

  if (ul_ccch_msg->message.present == NR_UL_CCCH_MessageType_PR_c1) {
    switch (ul_ccch_msg->message.choice.c1->present) {
      case NR_UL_CCCH_MessageType__c1_PR_NOTHING:
        /* TODO */
        LOG_I(NR_RRC, "Received PR_NOTHING on UL-CCCH-Message\n");
        break;

      case NR_UL_CCCH_MessageType__c1_PR_rrcSetupRequest:
        LOG_D(NR_RRC, "Received RRCSetupRequest on UL-CCCH-Message (UE rnti %04x)\n", rnti);

        /* CRITICAL FIX: Do NOT check for duplicate RNTI when message is from Remote UE
           In this case, 'rnti' belongs to the Relay UE, not the Remote UE
           Remote UE will get a new RNTI allocated below */
        rrc_gNB_ue_context_t *ue_context_p = NULL;
        if (!is_from_remote_ue) {
          ue_context_p = rrc_gNB_get_ue_context_by_rnti(gnb_rrc_inst, rnti);
          if (ue_context_p != NULL) {
            LOG_W(NR_RRC, "Got RRC setup request for a already registered RNTI %x, dropping the old one and give up this rrcSetupRequest\n", ue_context_p->ue_context.rnti);
            if (get_softmodem_params()->sl_mode == 1)
              nr_rrc_mac_clear_sl_harq_schedule(module_id, rnti);
            rrc_gNB_remove_ue_context(gnb_rrc_inst, ue_context_p);
            ue_context_p = NULL;  // Will not proceed with RRCSetup
          }
        } else {
          LOG_D(NR_RRC, "[gNB] Remote UE RRCSetupRequest via Relay RNTI 0x%04x - skipping duplicate RNTI check (Relay UE context preserved)\n", rnti);
        }

        if (ue_context_p == NULL) {
          rrcSetupRequest = &ul_ccch_msg->message.choice.c1->choice.rrcSetupRequest->rrcSetupRequest;
          if (NR_InitialUE_Identity_PR_randomValue == rrcSetupRequest->ue_Identity.present) {
            /* randomValue                         BIT STRING (SIZE (39)) */
            if (rrcSetupRequest->ue_Identity.choice.randomValue.size != 5) { // 39-bit random value
              LOG_E(NR_RRC, "wrong InitialUE-Identity randomValue size, expected 5, provided %lu", (long unsigned int)rrcSetupRequest->ue_Identity.choice.randomValue.size);
              return -1;
            }
            uint64_t random_value = 0;
            memcpy(((uint8_t *)&random_value) + 3, rrcSetupRequest->ue_Identity.choice.randomValue.buf, rrcSetupRequest->ue_Identity.choice.randomValue.size);

            /* if there is already a registered UE (with another RNTI) with this random_value,
             * the current one must be removed from MAC/PHY (zombie UE)
             */
            if ((ue_context_p = rrc_gNB_ue_context_random_exist(gnb_rrc_inst, random_value))) {
              LOG_W(NR_RRC, "new UE rnti (coming with random value) is already there, removing UE %x from MAC/PHY\n", rnti);
              AssertFatal(false, "not implemented\n");
            }

            // L2 Relay: For Remote UE, generate new RNTI instead of using Relay UE's RNTI
            rnti_t ue_rnti = rnti;
            if (is_from_remote_ue) {
              LOG_D(NR_RRC, "[REMOTE_UE_MSG] RX RRCSetupRequest (decoded at RRC) from Remote UE ID=%d via Relay RNTI=0x%04x\n",
                    remote_ue_id, relay_ue_rnti);
              // Generate new RNTI for Remote UE based on random_value
              ue_rnti = (rnti_t)(random_value & 0xFFFF);
              if (ue_rnti == 0 || ue_rnti == 0xFFFF || ue_rnti == 0xFFFE) {
                ue_rnti = (rnti_t)((random_value >> 16) & 0xFFFF);
              }
              LOG_D(NR_RRC, "[gNB] Allocating new RNTI 0x%04x for Remote UE ID %d (via Relay UE 0x%04x)\n",
                    ue_rnti, remote_ue_id, relay_ue_rnti);
            }

            ue_context_p = rrc_gNB_create_ue_context(ue_rnti, gnb_rrc_inst, random_value);
          } else if (NR_InitialUE_Identity_PR_ng_5G_S_TMSI_Part1 == rrcSetupRequest->ue_Identity.present) {
            /* TODO */
            /* <5G-S-TMSI> = <AMF Set ID><AMF Pointer><5G-TMSI> 48-bit */
            /* ng-5G-S-TMSI-Part1                  BIT STRING (SIZE (39)) */
            if (rrcSetupRequest->ue_Identity.choice.ng_5G_S_TMSI_Part1.size != 5) {
              LOG_E(NR_RRC, "wrong ng_5G_S_TMSI_Part1 size, expected 5, provided %lu \n", (long unsigned int)rrcSetupRequest->ue_Identity.choice.ng_5G_S_TMSI_Part1.size);
              return -1;
            }

            uint64_t s_tmsi_part1 = bitStr_to_uint64(&rrcSetupRequest->ue_Identity.choice.ng_5G_S_TMSI_Part1);

            // memcpy(((uint8_t *) & random_value) + 3,
            //         rrcSetupRequest->ue_Identity.choice.ng_5G_S_TMSI_Part1.buf,
            //         rrcSetupRequest->ue_Identity.choice.ng_5G_S_TMSI_Part1.size);

            if ((ue_context_p = rrc_gNB_ue_context_5g_s_tmsi_exist(gnb_rrc_inst, s_tmsi_part1))) {
              gNB_RRC_UE_t *UE = &ue_context_p->ue_context;
              LOG_I(NR_RRC, " 5G-S-TMSI-Part1 exists, old rnti %04x => %04x\n", UE->rnti, rnti);

              // TODO: MAC structures should not be accessed directly from the RRC! An implementation using the F1 interface should be developed.
              if (!NODE_IS_CU(RC.nrrrc[0]->node_type)) {
                nr_rrc_mac_remove_ue(ue_context_p->ue_context.rnti);
              } else {
                AssertFatal(false, "not implemented: need to switch RNTI in MAC via DL RRC Message Transfer\n");
              }

              /* replace rnti in the context */
              UE->rnti = rnti;
            } else {
              LOG_I(NR_RRC, "UE %04x 5G-S-TMSI-Part1 doesn't exist, setting ng_5G_S_TMSI_Part1 => %ld\n", rnti, s_tmsi_part1);

              ue_context_p = rrc_gNB_create_ue_context(rnti, gnb_rrc_inst, s_tmsi_part1);
              if (ue_context_p == NULL) {
                LOG_E(NR_RRC, "rrc_gNB_get_next_free_ue_context returned NULL\n");
                return -1;
              }
              gNB_RRC_UE_t *UE = &ue_context_p->ue_context;
              UE->Initialue_identity_5g_s_TMSI.presence = true;
              UE->ng_5G_S_TMSI_Part1 = s_tmsi_part1;
            }
          } else {
            /* TODO */
            uint64_t random_value = 0;
            memcpy(((uint8_t *)&random_value) + 3, rrcSetupRequest->ue_Identity.choice.randomValue.buf, rrcSetupRequest->ue_Identity.choice.randomValue.size);

            ue_context_p = rrc_gNB_create_ue_context(rnti, gnb_rrc_inst, random_value);
            LOG_E(NR_RRC, "RRCSetupRequest without random UE identity or S-TMSI not supported, let's reject the UE %04x\n", rnti);
            rrc_gNB_generate_RRCReject(module_id, ue_context_p);
            break;
          }
          gNB_RRC_UE_t *UE = &ue_context_p->ue_context;
          UE = &ue_context_p->ue_context;
          UE->establishment_cause = rrcSetupRequest->establishmentCause;
          UE->Srb[1].Active = 1;

          // L2 Relay: Store Remote UE information if applicable
          if (is_from_remote_ue) {
            UE->is_remote_ue = true;
            UE->relay_ue_rnti = relay_ue_rnti;
            UE->remote_ue_id = remote_ue_id;
            LOG_D(NR_RRC, "[gNB] Created Remote UE context: C-RNTI 0x%04x (newly allocated), "
                  "via Relay UE RNTI 0x%04x, Remote UE ID %d\n",
                  UE->rnti, relay_ue_rnti, remote_ue_id);
          } else {
            UE->is_remote_ue = false;
            UE->relay_ue_rnti = 0;
            UE->remote_ue_id = 0;
          }

          /* Use the RNTI from the created UE context (correct RNTI for Remote UE)
             For Remote UE: Create MINIMAL masterCellGroup (empty, PC5-only operation)
             Remote UE operates entirely over PC5/sidelink and should NOT receive Uu cellular configuration */
          const uint8_t *mcg = du_to_cu_rrc_container;
          int mcg_len = du_to_cu_rrc_container_len;
          static uint8_t minimal_mcg_buffer[1024];

          if (is_from_remote_ue) {
            /* Remote UE: Use Relay UE's masterCellGroup
               This is acceptable because Remote UE will ignore most Uu-specific config
               and only use the RLC bearer configuration for proper PDCP/RLC operation */
            rrc_gNB_ue_context_t *relay_ue_context = rrc_gNB_get_ue_context_by_rnti(gnb_rrc_inst, relay_ue_rnti);
            if (relay_ue_context && relay_ue_context->ue_context.masterCellGroup) {
              // Encode the Relay UE's masterCellGroup to buffer
              asn_enc_rval_t enc_rval = uper_encode_to_buffer(&asn_DEF_NR_CellGroupConfig,
                                                               NULL,
                                                               (void *)relay_ue_context->ue_context.masterCellGroup,
                                                               minimal_mcg_buffer,
                                                               sizeof(minimal_mcg_buffer));
              if (enc_rval.encoded > 0) {
                mcg = minimal_mcg_buffer;
                mcg_len = (enc_rval.encoded + 7) / 8;
                LOG_D(NR_RRC, "[gNB] Remote UE RNTI 0x%04x: Using Relay UE masterCellGroup (%d bytes) - Remote UE will use PC5 only\n",
                      ue_context_p->ue_context.rnti, mcg_len);
              } else {
                LOG_E(NR_RRC, "[gNB] Failed to encode Relay UE masterCellGroup for Remote UE\n");
                mcg = NULL;
                mcg_len = 0;
              }
            } else {
              LOG_W(NR_RRC, "[gNB] Relay UE context or masterCellGroup not found for Remote UE\n");
              mcg = NULL;
              mcg_len = 0;
            }
          }

          rrc_gNB_generate_RRCSetup(module_id,
                                    UE->rnti,
                                    ue_context_p,
                                    mcg,
                                    mcg_len);
        }
        break;

      case NR_UL_CCCH_MessageType__c1_PR_rrcResumeRequest:
        LOG_I(NR_RRC, "receive rrcResumeRequest message \n");
        break;

      case NR_UL_CCCH_MessageType__c1_PR_rrcReestablishmentRequest: {
        LOG_DUMPMSG(NR_RRC, DEBUG_RRC, (char *)(buffer), buffer_length, "[MSG] RRC Reestablishment Request\n");
        rrcReestablishmentRequest = ul_ccch_msg->message.choice.c1->choice.rrcReestablishmentRequest->rrcReestablishmentRequest;
        const NR_ReestablishmentCause_t cause = rrcReestablishmentRequest.reestablishmentCause;
        const long physCellId = rrcReestablishmentRequest.ue_Identity.physCellId;
        LOG_I(NR_RRC,
              "UE %04x NR_RRCReestablishmentRequest cause %s\n",
              rnti,
              ((cause == NR_ReestablishmentCause_otherFailure)      ? "Other Failure"
               : (cause == NR_ReestablishmentCause_handoverFailure) ? "Handover Failure"
                                                                    : "reconfigurationFailure"));
        uint8_t xid = -1;
        if (physCellId != gnb_rrc_inst->carrier.physCellId) {
          /* UE was moving from previous cell so quickly that RRCReestablishment for previous cell was received in this cell */
          LOG_E(NR_RRC,
                " NR_RRCReestablishmentRequest ue_Identity.physCellId(%ld) is not equal to current physCellId(%ld), fallback to RRC establishment\n",
                physCellId,
                gnb_rrc_inst->carrier.physCellId);
          xid = rrc_gNB_generate_RRCSetup_for_RRCReestablishmentRequest(module_id, rnti, 0, du_to_cu_rrc_container, du_to_cu_rrc_container_len);
          break;
        }

        LOG_I(NR_RRC, "physCellId: %ld\n", physCellId);

        for (int i = 0; i < rrcReestablishmentRequest.ue_Identity.shortMAC_I.size; i++) {
          LOG_D(NR_RRC, "rrcReestablishmentRequest.ue_Identity.shortMAC_I.buf[%d] = %x\n", i, rrcReestablishmentRequest.ue_Identity.shortMAC_I.buf[i]);
        }

        // 3GPP TS 38.321 version 15.13.0 Section 7.1 Table 7.1-1: RNTI values
        if (rrcReestablishmentRequest.ue_Identity.c_RNTI < 0x1 || rrcReestablishmentRequest.ue_Identity.c_RNTI > 0xffef) {
          /* c_RNTI range error should not happen */
          LOG_E(NR_RRC, "NR_RRCReestablishmentRequest c_RNTI range error, fallback to RRC establishment\n");
          xid = rrc_gNB_generate_RRCSetup_for_RRCReestablishmentRequest(module_id, rnti, 0, du_to_cu_rrc_container, du_to_cu_rrc_container_len);
          break;
        }

        rnti_t c_rnti = rrcReestablishmentRequest.ue_Identity.c_RNTI;
        LOG_I(NR_RRC, "c_RNTI: %04x\n", c_rnti);
        rrc_gNB_ue_context_t *ue_context_p = rrc_gNB_get_ue_context_by_rnti(gnb_rrc_inst, c_rnti);
        gNB_RRC_UE_t *UE = &ue_context_p->ue_context;
        if (ue_context_p == NULL) {
          LOG_E(NR_RRC, "NR_RRCReestablishmentRequest without UE context, fallback to RRC establishment\n");
          xid = rrc_gNB_generate_RRCSetup_for_RRCReestablishmentRequest(module_id, c_rnti, 0, du_to_cu_rrc_container, du_to_cu_rrc_container_len);
          break;
        }
        // c-plane not end
        if ((UE->StatusRrc != NR_RRC_RECONFIGURED) && (UE->reestablishment_cause == NR_ReestablishmentCause_spare1)) {
          LOG_E(NR_RRC, "NR_RRCReestablishmentRequest (UE %x c-plane is not end), RRC establishment failed\n", c_rnti);
          /* TODO RRC Release ? */
          break;
        }

        /* TODO: start timer in ITTI and drop UE if it does not come back */
        (void) xid; /* xid currently not used */

        // Insert C-RNTI to map
        for (int i = 0; i < MAX_MOBILES_PER_GNB; i++) {
          nr_reestablish_rnti_map_t *nr_reestablish_rnti_map = &gnb_rrc_inst->nr_reestablish_rnti_map[i];
          LOG_I(NR_RRC, "Insert nr_reestablish_rnti_map[%d] UEid: %lx, RNTI: %04x\n", i, nr_reestablish_rnti_map->ue_id, nr_reestablish_rnti_map->c_rnti);
          if (nr_reestablish_rnti_map->ue_id == 0) {
            nr_reestablish_rnti_map->ue_id = rnti;
            nr_reestablish_rnti_map->c_rnti = c_rnti;
            LOG_W(NR_RRC, "Insert nr_reestablish_rnti_map[%d] UEid: %lx, RNTI: %04x bug in UEid to fix \n", i, nr_reestablish_rnti_map->ue_id, nr_reestablish_rnti_map->c_rnti);
            break;
          }
        }

        UE->reestablishment_cause = cause;
        LOG_D(NR_RRC, "Accept RRCReestablishmentRequest from UE physCellId %ld cause %ld\n", physCellId, cause);

        UE->primaryCC_id = 0;
        // LG COMMENT Idx = (ue_mod_idP * NB_RB_MAX) + DCCH;
        Idx = DCCH;
        // SRB1
        UE->Srb[1].Active = 1;
        // SRB2: set  it to go through SRB1 with id 1 (DCCH)
        UE->Srb[2].Active = 1;
        protocol_ctxt_t ctxt = {.rntiMaybeUEid = rnti, .module_id = module_id, .instance = module_id, .enb_flag = 1, .eNB_index = module_id};
        rrc_gNB_generate_RRCReestablishment(&ctxt, ue_context_p, du_to_cu_rrc_container, gnb_rrc_inst->carrier.servingcellconfigcommon, 0);

        LOG_I(NR_RRC, "CALLING RLC CONFIG SRB1 (rbid %d)\n", Idx);
      } break;

      case NR_UL_CCCH_MessageType__c1_PR_rrcSystemInfoRequest:
        LOG_I(NR_RRC, "UE %04x receive rrcSystemInfoRequest message \n", rnti);
        /* TODO */
        break;

      default:
        LOG_E(NR_RRC, "UE %04x Unknown message\n", rnti);
        break;
    }
  }
  return 0;
}

/*! \fn uint64_t bitStr_to_uint64(BIT_STRING_t *)
 *\brief  This function extract at most a 64 bits value from a BIT_STRING_t object, the exact bits number depend on the BIT_STRING_t contents.
 *\param[in] pointer to the BIT_STRING_t object.
 *\return the extracted value.
 */
static inline uint64_t bitStr_to_uint64(BIT_STRING_t *asn) {
  uint64_t result = 0;
  int index;
  int shift;

  DevCheck ((asn->size > 0) && (asn->size <= 8), asn->size, 0, 0);

  shift = ((asn->size - 1) * 8) - asn->bits_unused;
  for (index = 0; index < (asn->size - 1); index++) {
    result |= (uint64_t)asn->buf[index] << shift;
    shift -= 8;
  }

  result |= asn->buf[index] >> asn->bits_unused;

  return result;
}

static void rrc_gNB_process_MeasurementReport(rrc_gNB_ue_context_t *ue_context, const NR_MeasurementReport_t *measurementReport)
{
  if (LOG_DEBUGFLAG(DEBUG_ASN1))
    xer_fprint(stdout, &asn_DEF_NR_MeasurementReport, (void *)measurementReport);

  DevAssert(measurementReport->criticalExtensions.present == NR_MeasurementReport__criticalExtensions_PR_measurementReport
            && measurementReport->criticalExtensions.choice.measurementReport != NULL);

  gNB_RRC_UE_t *ue_ctxt = &ue_context->ue_context;
  if (ue_ctxt->measResults != NULL) {
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_NR_MeasResults, ue_ctxt->measResults);
    ue_ctxt->measResults = NULL;
  }

  const NR_MeasId_t id = measurementReport->criticalExtensions.choice.measurementReport->measResults.measId;
  AssertFatal(id, "unexpected MeasResult for MeasurementId %ld received\n", id);
  asn1cCallocOne(ue_ctxt->measResults, measurementReport->criticalExtensions.choice.measurementReport->measResults);
}

static int handle_rrcReestablishmentComplete(const protocol_ctxt_t *const ctxt_pP,
                                             const NR_RRCReestablishmentComplete_t *reestablishment_complete)
{
  rnti_t reestablish_rnti = 0;
  gNB_RRC_INST *gnb_rrc_inst = RC.nrrrc[ctxt_pP->module_id];
  rrc_gNB_ue_context_t *ue_context_p = NULL;
  gNB_RRC_UE_t *UE = NULL;
  //  Select C-RNTI from map
  for (int i = 0; i < MAX_MOBILES_PER_GNB; i++) {
    nr_reestablish_rnti_map_t *nr_reestablish_rnti_map = &gnb_rrc_inst->nr_reestablish_rnti_map[i];
    LOG_I(NR_RRC,
          "nr_reestablish_rnti_map[%d] UEid %lx, RNTI %04x, ctxt_pP->rntiMaybeUEid: %lx\n",
          i,
          nr_reestablish_rnti_map->ue_id,
          nr_reestablish_rnti_map->c_rnti,
          ctxt_pP->rntiMaybeUEid);
    if (nr_reestablish_rnti_map->ue_id == ctxt_pP->rntiMaybeUEid) {
      LOG_I(NR_RRC,
            "Removing nr_reestablish_rnti_map[%d] UEid %lx, RNTI %04x\n",
            i,
            nr_reestablish_rnti_map->ue_id,
            nr_reestablish_rnti_map->c_rnti);
      reestablish_rnti = nr_reestablish_rnti_map->c_rnti;
      ue_context_p = rrc_gNB_get_ue_context_by_rnti(gnb_rrc_inst, reestablish_rnti);
      UE = &ue_context_p->ue_context;
      break;
    }
  }

  if (ue_context_p == NULL || UE == NULL) {
    LOG_E(RRC, "no UE found for reestablishment. ERROR: should send reply\n");
    return -1;
  }

  DevAssert(reestablishment_complete->criticalExtensions.present
            == NR_RRCReestablishmentComplete__criticalExtensions_PR_rrcReestablishmentComplete);
  rrc_gNB_process_RRCReestablishmentComplete(ctxt_pP,
                                             reestablish_rnti,
                                             ue_context_p,
                                             reestablishment_complete->rrc_TransactionIdentifier);

  nr_rrc_mac_remove_ue(reestablish_rnti);

  UE->ue_reestablishment_counter++;
  return 0;
}

static int handle_ueCapabilityInformation(const protocol_ctxt_t *const ctxt_pP,
                                          rrc_gNB_ue_context_t *ue_context_p,
                                          const NR_UECapabilityInformation_t *ue_cap_info)
{
  AssertFatal(ue_context_p != NULL, "Processing %s() for UE %lx, ue_context_p is NULL\n", __func__, ctxt_pP->rntiMaybeUEid);
  AssertFatal(ue_cap_info, "Processing %s() ue_cap_info cannot be NULL\n", __func__);
  gNB_RRC_UE_t *UE = &ue_context_p->ue_context;

  LOG_I(NR_RRC, "got UE capabilities for UE %lx\n", ctxt_pP->rntiMaybeUEid);
  int eutra_index = -1;

  if (ue_cap_info->criticalExtensions.present == NR_UECapabilityInformation__criticalExtensions_PR_ueCapabilityInformation) {
    const NR_UE_CapabilityRAT_ContainerList_t *ue_CapabilityRAT_ContainerList =
        ue_cap_info->criticalExtensions.choice.ueCapabilityInformation->ue_CapabilityRAT_ContainerList;
    for (int i = 0; i < ue_CapabilityRAT_ContainerList->list.count; i++) {
      const NR_UE_CapabilityRAT_Container_t *ue_cap_container = ue_CapabilityRAT_ContainerList->list.array[i];
      if (ue_cap_container->rat_Type == NR_RAT_Type_nr) {
        if (UE->UE_Capability_nr) {
          ASN_STRUCT_FREE(asn_DEF_NR_UE_NR_Capability, UE->UE_Capability_nr);
          UE->UE_Capability_nr = 0;
        }

        asn_dec_rval_t dec_rval = uper_decode(NULL,
                                              &asn_DEF_NR_UE_NR_Capability,
                                              (void **)&UE->UE_Capability_nr,
                                              ue_cap_container->ue_CapabilityRAT_Container.buf,
                                              ue_cap_container->ue_CapabilityRAT_Container.size,
                                              0,
                                              0);
        if (LOG_DEBUGFLAG(DEBUG_ASN1)) {
          xer_fprint(stdout, &asn_DEF_NR_UE_NR_Capability, UE->UE_Capability_nr);
        }

        if ((dec_rval.code != RC_OK) && (dec_rval.consumed == 0)) {
          LOG_E(NR_RRC,
                PROTOCOL_NR_RRC_CTXT_UE_FMT " Failed to decode nr UE capabilities (%zu bytes)\n",
                PROTOCOL_NR_RRC_CTXT_UE_ARGS(ctxt_pP),
                dec_rval.consumed);
          ASN_STRUCT_FREE(asn_DEF_NR_UE_NR_Capability, UE->UE_Capability_nr);
          UE->UE_Capability_nr = 0;
        }

        UE->UE_Capability_size = ue_cap_container->ue_CapabilityRAT_Container.size;
        if (eutra_index != -1) {
          LOG_E(NR_RRC, "fatal: more than 1 eutra capability\n");
          exit(1);
        }
        eutra_index = i;
      }

      if (ue_cap_container->rat_Type == NR_RAT_Type_eutra_nr) {
        if (UE->UE_Capability_MRDC) {
          ASN_STRUCT_FREE(asn_DEF_NR_UE_MRDC_Capability, UE->UE_Capability_MRDC);
          UE->UE_Capability_MRDC = 0;
        }
        asn_dec_rval_t dec_rval = uper_decode(NULL,
                                              &asn_DEF_NR_UE_MRDC_Capability,
                                              (void **)&UE->UE_Capability_MRDC,
                                              ue_cap_container->ue_CapabilityRAT_Container.buf,
                                              ue_cap_container->ue_CapabilityRAT_Container.size,
                                              0,
                                              0);

        if (LOG_DEBUGFLAG(DEBUG_ASN1)) {
          xer_fprint(stdout, &asn_DEF_NR_UE_MRDC_Capability, UE->UE_Capability_MRDC);
        }

        if ((dec_rval.code != RC_OK) && (dec_rval.consumed == 0)) {
          LOG_E(NR_RRC,
                PROTOCOL_NR_RRC_CTXT_FMT " Failed to decode nr UE capabilities (%zu bytes)\n",
                PROTOCOL_NR_RRC_CTXT_UE_ARGS(ctxt_pP),
                dec_rval.consumed);
          ASN_STRUCT_FREE(asn_DEF_NR_UE_MRDC_Capability, UE->UE_Capability_MRDC);
          UE->UE_Capability_MRDC = 0;
        }
        UE->UE_MRDC_Capability_size = ue_cap_container->ue_CapabilityRAT_Container.size;
      }

      if (ue_cap_container->rat_Type == NR_RAT_Type_eutra) {
        // TODO
      }
    }

    if (eutra_index == -1)
      return -1;
  }

  if (get_softmodem_params()->sa) {
    rrc_gNB_send_NGAP_UE_CAPABILITIES_IND(ctxt_pP, ue_context_p, ue_cap_info);
  }

  // we send the UE capabilities request before RRC connection is complete,
  // so we cannot have a PDU session yet
  AssertFatal(UE->nb_of_pdusessions == 0, "logic bug: received capabilities while PDU session established\n");
  // TODO: send UE context modification response with UE capabilities to
  // allow DU configure CellGroupConfig
  rrc_gNB_generate_defaultRRCReconfiguration(ctxt_pP, ue_context_p);

  return 0;
}

static int handle_ueAssistanceInformation(const protocol_ctxt_t *const ctxt_pP,
                                          rrc_gNB_ue_context_t *ue_context_p,
                                          const NR_UEAssistanceInformation_t *ue_assitance_info)
{
  AssertFatal(ue_context_p != NULL, "Processing %s() for UE %lx, ue_context_p is NULL\n", __func__, ctxt_pP->rntiMaybeUEid);
  int tp_cnt = 0;

  if (ue_assitance_info->criticalExtensions.present == NR_UEAssistanceInformation__criticalExtensions_PR_ueAssistanceInformation) {
    struct NR_UEAssistanceInformation_IEs *ueAssistanceInformation = ue_assitance_info->criticalExtensions.choice.ueAssistanceInformation;
    const NR_SL_UE_AssistanceInformationNR_r16_t *trafficPatternList =
        ueAssistanceInformation->nonCriticalExtension->nonCriticalExtension->sl_UE_AssistanceInformationNR_r16;

    for (int i = 0; i < trafficPatternList->list.count; i++) {
      const NR_SL_TrafficPatternInfo_r16_t *tp_container = trafficPatternList->list.array[i];
      long tp_period = tp_container->trafficPeriodicity_r16;
      long tp_timeOffset = tp_container->timingOffset_r16;
      long tp_QoSFlowId = tp_container->sl_QoS_FlowIdentity_r16;
      LOG_I(NR_RRC, "got NR_UEAssistanceInformation tp_period %ld tp_timeOffset %ld tp_QoSFlowId %ld\n", tp_period, tp_timeOffset, tp_QoSFlowId);
      tp_cnt++;
      // TODO :: Need to associate these values to resource allocation for sl mode 1.
    }
    nr_rrc_reconfiguration_req_sidelink(ue_context_p, ctxt_pP, NULL, trafficPatternList);
  }
  if (tp_cnt == 0)
    return -1;

  return 0;
}

static int handle_rrcSetupComplete(const protocol_ctxt_t *const ctxt_pP,
                                   rrc_gNB_ue_context_t *ue_context_p,
                                   const NR_RRCSetupComplete_t *setup_complete)
{
  if (!ue_context_p) {
    LOG_I(NR_RRC, "Processing NR_RRCSetupComplete UE %lx, ue_context_p is NULL\n", ctxt_pP->rntiMaybeUEid);
    return -1;
  }
  gNB_RRC_UE_t *UE = &ue_context_p->ue_context;

  NR_RRCSetupComplete_IEs_t *setup_complete_ies = setup_complete->criticalExtensions.choice.rrcSetupComplete;

  if (setup_complete_ies->ng_5G_S_TMSI_Value != NULL) {
    if (setup_complete_ies->ng_5G_S_TMSI_Value->present == NR_RRCSetupComplete_IEs__ng_5G_S_TMSI_Value_PR_ng_5G_S_TMSI_Part2) {
      if (setup_complete_ies->ng_5G_S_TMSI_Value->choice.ng_5G_S_TMSI_Part2.size != 2) {
        LOG_E(NR_RRC,
              "wrong ng_5G_S_TMSI_Part2 size, expected 2, provided %lu",
              (long unsigned int)
                  setup_complete->criticalExtensions.choice.rrcSetupComplete->ng_5G_S_TMSI_Value->choice.ng_5G_S_TMSI_Part2.size);
        return -1;
      }

      if (UE->ng_5G_S_TMSI_Part1 != 0) {
        UE->ng_5G_S_TMSI_Part2 = BIT_STRING_to_uint16(&setup_complete_ies->ng_5G_S_TMSI_Value->choice.ng_5G_S_TMSI_Part2);
      }

      /* TODO */
    } else if (setup_complete_ies->ng_5G_S_TMSI_Value->present == NR_RRCSetupComplete_IEs__ng_5G_S_TMSI_Value_PR_ng_5G_S_TMSI) {
      if (setup_complete_ies->ng_5G_S_TMSI_Value->choice.ng_5G_S_TMSI.size != 6) {
        LOG_E(NR_RRC,
              "wrong ng_5G_S_TMSI size, expected 6, provided %lu",
              (long unsigned int)setup_complete_ies->ng_5G_S_TMSI_Value->choice.ng_5G_S_TMSI.size);
        return -1;
      }

      uint64_t fiveg_s_TMSI = bitStr_to_uint64(&setup_complete_ies->ng_5G_S_TMSI_Value->choice.ng_5G_S_TMSI);
      LOG_I(NR_RRC,
            "Received rrcSetupComplete, 5g_s_TMSI: 0x%lX, amf_set_id: 0x%lX(%ld), amf_pointer: 0x%lX(%ld), 5g TMSI: 0x%X \n",
            fiveg_s_TMSI,
            fiveg_s_TMSI >> 38,
            fiveg_s_TMSI >> 38,
            (fiveg_s_TMSI >> 32) & 0x3F,
            (fiveg_s_TMSI >> 32) & 0x3F,
            (uint32_t)fiveg_s_TMSI);
      if (UE->Initialue_identity_5g_s_TMSI.presence) {
        UE->Initialue_identity_5g_s_TMSI.amf_set_id = fiveg_s_TMSI >> 38;
        UE->Initialue_identity_5g_s_TMSI.amf_pointer = (fiveg_s_TMSI >> 32) & 0x3F;
        UE->Initialue_identity_5g_s_TMSI.fiveg_tmsi = (uint32_t)fiveg_s_TMSI;
      }
    }
  }

  rrc_gNB_process_RRCSetupComplete(ctxt_pP, ue_context_p, setup_complete->criticalExtensions.choice.rrcSetupComplete);
  LOG_I(NR_RRC, PROTOCOL_NR_RRC_CTXT_UE_FMT " UE State = NR_RRC_CONNECTED \n", PROTOCOL_NR_RRC_CTXT_UE_ARGS(ctxt_pP));
  return 0;
}

static void handle_rrcReconfigurationComplete(const protocol_ctxt_t *const ctxt_pP,
                                              rrc_gNB_ue_context_t *ue_context_p,
                                              const NR_RRCReconfigurationComplete_t *reconfig_complete)
{
  LOG_I(NR_RRC, "Receive RRC Reconfiguration Complete message UE %lx\n", ctxt_pP->rntiMaybeUEid);
  AssertFatal(ue_context_p != NULL, "Processing %s() for UE %lx, ue_context_p is NULL\n", __func__, ctxt_pP->rntiMaybeUEid);
  gNB_RRC_UE_t *UE = &ue_context_p->ue_context;

  uint8_t xid = reconfig_complete->rrc_TransactionIdentifier;
  rrc_gNB_process_RRCReconfigurationComplete(ctxt_pP, ue_context_p, xid);

  bool successful_reconfig = true;
  if (get_softmodem_params()->sa) {
    switch (UE->xids[xid]) {
      case RRC_PDUSESSION_RELEASE: {
        gtpv1u_gnb_delete_tunnel_req_t req = {0};
        gtpv1u_delete_ngu_tunnel(ctxt_pP->instance, &req);
        // NGAP_PDUSESSION_RELEASE_RESPONSE
        rrc_gNB_send_NGAP_PDUSESSION_RELEASE_RESPONSE(ctxt_pP, ue_context_p, xid);
      } break;
      case RRC_PDUSESSION_ESTABLISH:
        if (UE->nb_of_pdusessions > 0)
          rrc_gNB_send_NGAP_PDUSESSION_SETUP_RESP(ctxt_pP, ue_context_p, xid);
        break;
      case RRC_PDUSESSION_MODIFY:
        rrc_gNB_send_NGAP_PDUSESSION_MODIFY_RESP(ctxt_pP, ue_context_p, xid);
        break;
      case RRC_DEFAULT_RECONF:
        rrc_gNB_send_NGAP_INITIAL_CONTEXT_SETUP_RESP(ctxt_pP, ue_context_p);
        break;
      case RRC_REESTABLISH_COMPLETE:
        LOG_D(NR_RRC, "Handling of RRC Reconfiguration Complete message UE %lx\n", ctxt_pP->rntiMaybeUEid);
        break;
      default:
        LOG_E(RRC, "Received unexpected xid: %d\n", xid);
        successful_reconfig = false;
        break;
    }
  }

  gNB_RRC_INST *rrc = RC.nrrrc[0];

  // Skip F1AP UE context modification for Remote UE (no direct DU connection)
  if (UE->is_remote_ue) {
    LOG_D(NR_RRC, "[REMOTE_UE] Skipping F1AP UE context modification after RRCReconfigurationComplete for RNTI 0x%04x\n",
          UE->rnti);
    return;
  }

  f1ap_ue_context_modif_req_t ue_context_modif_req = {
    .gNB_CU_ue_id = 0xffffffff, /* filled by F1 for the moment */
    .gNB_DU_ue_id = 0xffffffff, /* filled by F1 for the moment */
    .rnti = UE->rnti,
    .mcc = rrc->configuration.mcc[0],
    .mnc = rrc->configuration.mnc[0],
    .mnc_digit_length = rrc->configuration.mnc_digit_length[0],
    .nr_cellid = rrc->nr_cellid,
    .servCellId = 0, /* TODO: correct value? */
    .ReconfigComplOutcome = successful_reconfig ? RRCreconf_success : RRCreconf_failure,
  };
  rrc->mac_rrc.ue_context_modification_request(&ue_context_modif_req);
}

static int handle_sidelinkUEInformationNR(const protocol_ctxt_t *const ctxt_pP,
                                          rrc_gNB_ue_context_t *ue_context_p,
                                          NR_SidelinkUEInformationNR_r16_t *sidelinkUEInformationNR_r16)
{
  if (sidelinkUEInformationNR_r16 != NULL) {
    switch (sidelinkUEInformationNR_r16->criticalExtensions.present){
      case NR_SidelinkUEInformationNR_r16__criticalExtensions_PR_sidelinkUEInformationNR_r16: {
        NR_SidelinkUEInformationNR_r16_IEs_t *sl_UEInfo_r16 = sidelinkUEInformationNR_r16->criticalExtensions.choice.sidelinkUEInformationNR_r16;
        nr_rrc_reconfiguration_req_sidelink(ue_context_p, ctxt_pP, sl_UEInfo_r16, NULL);
      } break;
      case NR_SidelinkUEInformationNR_r16__criticalExtensions_PR_criticalExtensionsFuture:
        break;
      case NR_SidelinkUEInformationNR_r16__criticalExtensions_PR_NOTHING:
        break;
      default:
        break;
    }
  }

  return 0;
}

/*-----------------------------------------------------------------------------
   L2 Relay: Process SRAP message from Relay UE (contains Remote UE RRC message)
   Called by nr_pdcp_entity.c when SRAP header is detected on gNB downlink
  ----------------------------------------------------------------------------- */
int nr_rrc_gNB_process_srap_message(module_id_t module_id,
                                     rnti_t relay_rnti,
                                     const uint8_t *buffer,
                                     int buffer_length)
{
  LOG_D(NR_RRC, "[gNB] SRAP→RRC: Processing SRAP message from relay_rnti=0x%04x, size=%d\n",
        relay_rnti, buffer_length);

  if (buffer_length < 2) {
    LOG_E(NR_RRC, "[gNB] SRAP message too small (%d bytes), need at least 2-byte header\n", buffer_length);
    return -1;
  }

  // Extract SRAP header: [bearer_id (in octet1 bits 0-4)][remote_ue_id (octet2)]
  uint8_t bearer_id = buffer[0] & 0x1F;
  uint8_t remote_ue_id = buffer[1];
  const uint8_t *rrc_payload = buffer + 2;
  int rrc_payload_length = buffer_length - 2;

  LOG_D(NR_RRC, "[gNB] SRAP header: bearer_id=%d, remote_ue_id=%d, RRC payload %d bytes\n",
        bearer_id, remote_ue_id, rrc_payload_length);

  // Create protocol context for Relay UE (used for looking up Relay UE context)
  protocol_ctxt_t ctxt;
  memset(&ctxt, 0, sizeof(ctxt));
  ctxt.module_id = module_id;
  ctxt.enb_flag = 1;  // gNB
  ctxt.instance = 0;
  ctxt.rntiMaybeUEid = relay_rnti;
  ctxt.frame = 0;
  ctxt.subframe = 0;
  ctxt.eNB_index = 0;

  if (bearer_id == 0) {
    /* SRB0: CCCH message (RRCSetupRequest from Remote UE)
       Remote UE context not yet created, will be created during CCCH decode */
    LOG_D(NR_RRC, "[gNB] CCCH from Remote UE ID %d via Relay 0x%04x, forwarding to CCCH decoder\n",
          remote_ue_id, relay_rnti);

    // Use a temporary RNTI for initial processing - will be replaced during context creation
    return nr_rrc_gNB_decode_ccch(module_id, 0xFFFE, rrc_payload, rrc_payload_length,
                                   NULL, 0, true, relay_rnti, remote_ue_id);
  } else if (bearer_id == 1) {
    /* SRB1: DCCH message (RRCSetupComplete, etc. from Remote UE)
       Find the Remote UE context by remote_ue_id and relay_ue_rnti */
    gNB_RRC_INST *rrc_inst = RC.nrrrc[module_id];
    rrc_gNB_ue_context_t *remote_ue_ctx = NULL;

    RB_FOREACH(remote_ue_ctx, rrc_nr_ue_tree_s, &rrc_inst->rrc_ue_head) {
      if (remote_ue_ctx->ue_context.is_remote_ue &&
          remote_ue_ctx->ue_context.remote_ue_id == remote_ue_id &&
          remote_ue_ctx->ue_context.relay_ue_rnti == relay_rnti) {
        break;
      }
    }

    if (remote_ue_ctx == NULL) {
      LOG_E(NR_RRC, "[gNB] No Remote UE context found for ID %d via Relay 0x%04x, cannot process DCCH\n",
            remote_ue_id, relay_rnti);
      return -1;
    }

    LOG_D(NR_RRC, "[gNB] DCCH from Remote UE ID %d (RNTI 0x%04x) via Relay 0x%04x, forwarding to DCCH decoder\n",
          remote_ue_id, remote_ue_ctx->ue_context.rnti, relay_rnti);

    // Process as DCCH with Remote UE's RNTI
    protocol_ctxt_t remote_ctxt = ctxt;
    remote_ctxt.rntiMaybeUEid = remote_ue_ctx->ue_context.rnti;
    return rrc_gNB_decode_dcch(&remote_ctxt, bearer_id, rrc_payload, rrc_payload_length);
  } else if (bearer_id >= 4) {
    /* DRB: User plane data (bearer_id=4 for DRB1, bearer_id=5 for DRB2, etc.)
       Find the Remote UE context and deliver to PDCP */
    gNB_RRC_INST *rrc_inst = RC.nrrrc[module_id];
    rrc_gNB_ue_context_t *remote_ue_ctx = NULL;

    RB_FOREACH(remote_ue_ctx, rrc_nr_ue_tree_s, &rrc_inst->rrc_ue_head) {
      if (remote_ue_ctx->ue_context.is_remote_ue &&
          remote_ue_ctx->ue_context.remote_ue_id == remote_ue_id &&
          remote_ue_ctx->ue_context.relay_ue_rnti == relay_rnti) {
        break;
      }
    }

    if (remote_ue_ctx == NULL) {
      LOG_E(NR_RRC, "[gNB] No Remote UE context found for ID %d via Relay 0x%04x, cannot process DRB data\n",
            remote_ue_id, relay_rnti);
      return -1;
    }

    // Map SRAP bearer_id to DRB ID: bearer_id 4 → DRB1, bearer_id 5 → DRB2, etc.
    int drb_id = bearer_id - 3;

    LOG_D(NR_RRC, "[gNB] DRB data from Remote UE ID %d (RNTI 0x%04x) via Relay 0x%04x: bearer_id=%d → DRB%d, forwarding %d bytes to PDCP\n",
          remote_ue_id, remote_ue_ctx->ue_context.rnti, relay_rnti, bearer_id, drb_id, rrc_payload_length);

    // Forward to PDCP with Remote UE's RNTI
    protocol_ctxt_t remote_ctxt = ctxt;
    remote_ctxt.rntiMaybeUEid = remote_ue_ctx->ue_context.rnti;

    // Create mem_block for PDCP
    mem_block_t *sdu_buffer = get_free_mem_block(rrc_payload_length, __func__);
    if (sdu_buffer == NULL) {
      LOG_E(NR_RRC, "[gNB] Failed to allocate mem_block for Remote UE DRB data\n");
      return -1;
    }
    memcpy(sdu_buffer->data, rrc_payload, rrc_payload_length);

    // Deliver to PDCP (srb_flag=0 for DRB, MBMS_flag=0, rb_id=drb_id, intf_type=UU)
    bool result = pdcp_data_ind(&remote_ctxt, 0, 0, drb_id, rrc_payload_length, sdu_buffer, NULL, NULL, UU);

    if (!result) {
      LOG_E(NR_RRC, "[gNB] pdcp_data_ind failed for Remote UE DRB%d\n", drb_id);
      return -1;
    }

    return 0;
  } else {
    LOG_E(NR_RRC, "[gNB] Invalid bearer_id %d for Remote UE %d via Relay 0x%04x\n",
          bearer_id, remote_ue_id, relay_rnti);
    return -1;
  }
}

//-----------------------------------------------------------------------------
int rrc_gNB_decode_dcch(const protocol_ctxt_t *const ctxt_pP,
                        const rb_id_t Srb_id,
                        const uint8_t *const Rx_sdu,
                        const sdu_size_t sdu_sizeP)
//-----------------------------------------------------------------------------
{
  gNB_RRC_INST *gnb_rrc_inst = RC.nrrrc[ctxt_pP->module_id];

  if ((Srb_id != 1) && (Srb_id != 2)) {
    LOG_E(NR_RRC, "Received message on SRB%ld, should not have ...\n", Srb_id);
  } else {
    LOG_D(NR_RRC, "Received message on SRB%ld\n", Srb_id);
  }

  /* Check for SRAP U2N header (L2 relay - Remote UE via Relay UE)
     SRAP messages bypass PDCP so they arrive with [bearer_id][remote_ue_id][RRC] format */
  rnti_t relay_ue_rnti = ctxt_pP->rntiMaybeUEid;
  uint8_t remote_ue_id = 0;
  const uint8_t *rrc_pdu = Rx_sdu;
  int rrc_pdu_length = sdu_sizeP;

  if (get_softmodem_params()->relay_type == U2N && sdu_sizeP > 2) {
    // Check if this message has a valid SRAP U2N header (2 bytes)
    uint8_t octet1 = Rx_sdu[0];
    uint8_t octet2 = Rx_sdu[1];

    // Check Reserved bits (bits 6-5) = 00
    bool reserved_bits_ok = (octet1 & 0x60) == 0;
    // Bearer ID should be valid (0-2 for SRB0-SRB2)
    uint8_t bearer_id = octet1 & 0x1F;
    bool bearer_id_valid = (bearer_id <= 2);

    // Remote UE ID in octet2 (0 = Relay UE itself, 1-255 = Remote UE)
    remote_ue_id = octet2;

    // Only treat as SRAP if header format is valid AND remote_ue_id != 0
    if (reserved_bits_ok && bearer_id_valid && remote_ue_id != 0) {
      // This is a DCCH message from Remote UE forwarded via Relay UE
      relay_ue_rnti = ctxt_pP->rntiMaybeUEid;  // Current RNTI is Relay UE's RNTI

      // Skip SRAP header to get RRC PDU
      rrc_pdu = Rx_sdu + 2;
      rrc_pdu_length = sdu_sizeP - 2;

      LOG_D(NR_RRC, "[gNB] DCCH message from Remote UE ID %d via Relay UE RNTI 0x%04x, bearer %d (SRAP detected)\n",
            remote_ue_id, relay_ue_rnti, bearer_id);

      /* Route based on bearer_id:
         bearer_id=0 (SRB0) -> CCCH (RRCSetupRequest, RRCResumeRequest)
         bearer_id=1 (SRB1) -> DCCH (RRCSetupComplete, RRCReconfigurationComplete, etc.) */
      if (bearer_id == 0) {
        // SRB0: Initial signaling (RRCSetupRequest)
        LOG_D(NR_RRC, "[gNB] CCCH from Remote UE ID %d via Relay 0x%04x, forwarding to CCCH decoder\n",
              remote_ue_id, relay_ue_rnti);
        return nr_rrc_gNB_decode_ccch(ctxt_pP->module_id, 0xFFFE, rrc_pdu, rrc_pdu_length, NULL, 0,
                                       true, relay_ue_rnti, remote_ue_id);
      } else if (bearer_id == 1) {
        /* SRB1: Post-setup signaling (RRCSetupComplete, RRCReconfigurationComplete)
           Find the Remote UE context by remote_ue_id and relay_ue_rnti */
        rrc_gNB_ue_context_t *remote_ue_ctx = NULL;
        RB_FOREACH(remote_ue_ctx, rrc_nr_ue_tree_s, &RC.nrrrc[ctxt_pP->module_id]->rrc_ue_head) {
          if (remote_ue_ctx->ue_context.is_remote_ue &&
              remote_ue_ctx->ue_context.remote_ue_id == remote_ue_id &&
              remote_ue_ctx->ue_context.relay_ue_rnti == relay_ue_rnti) {
            break;
          }
        }

        if (remote_ue_ctx == NULL) {
          LOG_E(NR_RRC, "[gNB] No Remote UE context found for ID %d via Relay 0x%04x, cannot process DCCH\n",
                remote_ue_id, relay_ue_rnti);
          return -1;
        }

        LOG_D(NR_RRC, "[gNB] DCCH from Remote UE ID %d (RNTI 0x%04x) via Relay 0x%04x, RRC payload %d bytes\n",
              remote_ue_id, remote_ue_ctx->ue_context.rnti, relay_ue_rnti, rrc_pdu_length);

        // Process as DCCH with Remote UE's RNTI (recursively call this function)
        protocol_ctxt_t remote_ctxt = *ctxt_pP;
        remote_ctxt.rntiMaybeUEid = remote_ue_ctx->ue_context.rnti;

        // Recursive call with stripped message
        return rrc_gNB_decode_dcch(&remote_ctxt, SRB_FLAG_YES, rrc_pdu, rrc_pdu_length);
      }
    }
  }

  LOG_D(NR_RRC, "Decoding UL-DCCH Message\n");
  {
    for (int i = 0; i < sdu_sizeP; i++) {
      LOG_T(NR_RRC, "%x.", Rx_sdu[i]);
    }

    LOG_T(NR_RRC, "\n");
  }

  NR_UL_DCCH_Message_t *ul_dcch_msg = NULL;
  asn_dec_rval_t dec_rval = uper_decode(NULL, &asn_DEF_NR_UL_DCCH_Message, (void **)&ul_dcch_msg, Rx_sdu, sdu_sizeP, 0, 0);

  if ((dec_rval.code != RC_OK) && (dec_rval.consumed == 0)) {
    LOG_E(NR_RRC, "Failed to decode UL-DCCH (%zu bytes)\n", dec_rval.consumed);
    return -1;
  }

  if (LOG_DEBUGFLAG(DEBUG_ASN1)) {
    xer_fprint(stdout, &asn_DEF_NR_UL_DCCH_Message, (void *)ul_dcch_msg);
  }

  rrc_gNB_ue_context_t *ue_context_p = rrc_gNB_get_ue_context_by_rnti(gnb_rrc_inst, ctxt_pP->rntiMaybeUEid);

  if (ul_dcch_msg->message.present == NR_UL_DCCH_MessageType_PR_c1) {
    switch (ul_dcch_msg->message.choice.c1->present) {
      case NR_UL_DCCH_MessageType__c1_PR_NOTHING:
        LOG_I(NR_RRC, "Received PR_NOTHING on UL-DCCH-Message\n");
        break;

      case NR_UL_DCCH_MessageType__c1_PR_rrcReconfigurationComplete:
        // State-based validation to prevent spurious RRCReconfigurationComplete acceptance
        if (!ue_context_p) {
          LOG_W(NR_RRC, "Processing rrcReconfigurationComplete UE %lx, ue_context_p is NULL\n", ctxt_pP->rntiMaybeUEid);
          break;
        }

        /* State-based discrimination: verify minimum message size
           A valid RRCReconfigurationComplete (ASN.1 UPER encoded) contains:
           - Message type indicator (part of UL-DCCH choice, embedded in first byte)
           - RRC transaction ID (3 bits)
           - Optional late non-critical extension indicator (1 bit)
           Minimum valid size is 2 bytes (10 bits) when no optional fields are present.
           Example: 0x0800 = RRCReconfigurationComplete with txn_id=0, no extensions */
        if (sdu_sizeP < 2) {
          LOG_E(NR_RRC, "[gNB] UE %04x: REJECTING RRCReconfigurationComplete - message too small (%d bytes)\n",
                ue_context_p->ue_context.rnti, sdu_sizeP);
          LOG_E(NR_RRC, "[gNB] UE %04x: Minimum valid RRCReconfigurationComplete is 2 bytes (10 bits encoded).\n",
                ue_context_p->ue_context.rnti);
          break;
        }

        /* Additional check for Remote UE: verify we're in correct state
           Remote UE should be in CONNECTED state (after RRCSetup) before we accept RRCReconfigurationComplete */
        if (ue_context_p->ue_context.is_remote_ue) {
          if (ue_context_p->ue_context.StatusRrc != NR_RRC_CONNECTED &&
              ue_context_p->ue_context.StatusRrc != NR_RRC_RECONFIGURED) {
            LOG_E(NR_RRC, "[gNB] Remote UE %04x: REJECTING RRCReconfigurationComplete - UE not in valid state (current=%d)\n",
                  ue_context_p->ue_context.rnti, ue_context_p->ue_context.StatusRrc);
            LOG_E(NR_RRC, "[gNB] Remote UE %04x: UE must be in CONNECTED or RECONFIGURED state to send RRCReconfigurationComplete\n",
                  ue_context_p->ue_context.rnti);
            break;
          }
          LOG_D(NR_RRC, "[gNB] Remote UE %04x: Accepting RRCReconfigurationComplete (%d bytes, state=%d)\n",
                ue_context_p->ue_context.rnti, sdu_sizeP, ue_context_p->ue_context.StatusRrc);
        }

        handle_rrcReconfigurationComplete(ctxt_pP, ue_context_p, ul_dcch_msg->message.choice.c1->choice.rrcReconfigurationComplete);
        break;

      case NR_UL_DCCH_MessageType__c1_PR_rrcSetupComplete:
        if (handle_rrcSetupComplete(ctxt_pP, ue_context_p, ul_dcch_msg->message.choice.c1->choice.rrcSetupComplete) == -1)
          return -1;
        break;

      case NR_UL_DCCH_MessageType__c1_PR_measurementReport:
        DevAssert(ul_dcch_msg != NULL
                  && ul_dcch_msg->message.present == NR_UL_DCCH_MessageType_PR_c1
                  && ul_dcch_msg->message.choice.c1
                  && ul_dcch_msg->message.choice.c1->present == NR_UL_DCCH_MessageType__c1_PR_measurementReport);
        rrc_gNB_process_MeasurementReport(ue_context_p, ul_dcch_msg->message.choice.c1->choice.measurementReport);
        break;

      case NR_UL_DCCH_MessageType__c1_PR_ulInformationTransfer:
        LOG_D(NR_RRC, "Recived RRC GNB UL Information Transfer \n");
        if (!ue_context_p) {
          LOG_W(NR_RRC, "Processing ulInformationTransfer UE %lx, ue_context_p is NULL\n", ctxt_pP->rntiMaybeUEid);
          break;
        }

        LOG_D(NR_RRC, "[MSG] RRC UL Information Transfer \n");
        LOG_DUMPMSG(RRC, DEBUG_RRC, (char *)Rx_sdu, sdu_sizeP, "[MSG] RRC UL Information Transfer \n");

        if (get_softmodem_params()->sa) {
          rrc_gNB_send_NGAP_UPLINK_NAS(ctxt_pP, ue_context_p, ul_dcch_msg);
        }
        break;

      case NR_UL_DCCH_MessageType__c1_PR_securityModeComplete:
        // to avoid segmentation fault
        if (!ue_context_p) {
          LOG_I(NR_RRC, "Processing securityModeComplete UE %lx, ue_context_p is NULL\n", ctxt_pP->rntiMaybeUEid);
          break;
        }

        if (ue_context_p->ue_context.is_remote_ue) {
          LOG_D(NR_RRC, "[REMOTE_UE_MSG] RX securityModeComplete (decoded at RRC) from Remote UE ID=%d via Relay RNTI=0x%04x\n",
                ue_context_p->ue_context.remote_ue_id, ue_context_p->ue_context.relay_ue_rnti);
        }
        LOG_I(NR_RRC,
              PROTOCOL_NR_RRC_CTXT_UE_FMT " received securityModeComplete on UL-DCCH %d from UE\n",
              PROTOCOL_NR_RRC_CTXT_UE_ARGS(ctxt_pP),
              DCCH);
        LOG_D(NR_RRC,
              PROTOCOL_NR_RRC_CTXT_UE_FMT
              " RLC RB %02d --- RLC_DATA_IND %d bytes "
              "(securityModeComplete) ---> RRC_eNB\n",
              PROTOCOL_NR_RRC_CTXT_UE_ARGS(ctxt_pP),
              DCCH,
              sdu_sizeP);

        if (LOG_DEBUGFLAG(DEBUG_ASN1)) {
          xer_fprint(stdout, &asn_DEF_NR_UL_DCCH_Message, (void *)ul_dcch_msg);
        }

        /* configure ciphering */
        nr_rrc_pdcp_config_security(ctxt_pP, ue_context_p, 1);

        /* Send pending NAS-PDU (RegistrationAccept) via dlInformationTransfer for Remote UE only */
        /* Relay UE follows standard path: nas_pdu will be added to dedicatedNAS_MessageList in RRCReconfiguration */
        gNB_RRC_UE_t *UE = &ue_context_p->ue_context;
        if (UE->is_remote_ue && UE->nas_pdu.buffer && UE->nas_pdu.length > 0) {
          LOG_D(NR_RRC, "[gNB] Remote UE: Sending pending NAS-PDU (%u bytes) via dlInformationTransfer to UE 0x%04x after SecurityModeComplete\n",
                UE->nas_pdu.length, UE->rnti);

          // Check if this is Registration Accept
          if (UE->nas_pdu.length > SECURITY_PROTECTED_5GS_NAS_MESSAGE_HEADER_LENGTH) {
            uint8_t nas_msg_type = UE->nas_pdu.buffer[SECURITY_PROTECTED_5GS_NAS_MESSAGE_HEADER_LENGTH];
            if (nas_msg_type == REGISTRATION_ACCEPT) {
              LOG_D(NR_RRC, "[REMOTE_UE_MSG] TX Registration Accept via DLInformationTransfer to Remote UE ID=%d (RNTI 0x%04x) via Relay RNTI=0x%04x\n",
                    UE->remote_ue_id, UE->rnti, UE->relay_ue_rnti);
            }
          }

          uint8_t *nas_buffer = NULL;
          int rrc_length = do_NR_DLInformationTransfer(ctxt_pP->module_id, &nas_buffer,
                                                       rrc_gNB_get_next_transaction_identifier(ctxt_pP->module_id),
                                                       UE->nas_pdu.length, UE->nas_pdu.buffer);

          gNB_RRC_INST *rrc = RC.nrrrc[ctxt_pP->module_id];
          nr_pdcp_data_req_srb(ctxt_pP->rntiMaybeUEid, DCCH, rrc_gNB_get_next_transaction_identifier(ctxt_pP->module_id),
                              rrc_length, nas_buffer, deliver_pdu_srb_f1, rrc, UU);

          /* Clear the NAS PDU pointer for Remote UE (buffer was freed by do_NR_DLInformationTransfer) */
          UE->nas_pdu.buffer = NULL;
          UE->nas_pdu.length = 0;
        } else if (!UE->is_remote_ue && UE->nas_pdu.buffer && UE->nas_pdu.length > 0) {
          LOG_D(NR_RRC, "[gNB] Relay UE: Keeping nas_pdu (%u bytes) for inclusion in RRCReconfiguration dedicatedNAS_MessageList (standard 3GPP path)\n",
                UE->nas_pdu.length);
        }

        rrc_gNB_generate_UECapabilityEnquiry(ctxt_pP, ue_context_p);
        break;

      case NR_UL_DCCH_MessageType__c1_PR_securityModeFailure:
        LOG_DUMPMSG(NR_RRC, DEBUG_RRC, (char *)Rx_sdu, sdu_sizeP, "[MSG] NR RRC Security Mode Failure\n");
        LOG_W(NR_RRC,
              PROTOCOL_RRC_CTXT_UE_FMT
              " RLC RB %02d --- RLC_DATA_IND %d bytes "
              "(securityModeFailure) ---> RRC_gNB\n",
              PROTOCOL_RRC_CTXT_UE_ARGS(ctxt_pP),
              DCCH,
              sdu_sizeP);

        if (LOG_DEBUGFLAG(DEBUG_ASN1)) {
          xer_fprint(stdout, &asn_DEF_NR_UL_DCCH_Message, (void *)ul_dcch_msg);
        }

        rrc_gNB_generate_UECapabilityEnquiry(ctxt_pP, ue_context_p);
        break;

      case NR_UL_DCCH_MessageType__c1_PR_ueCapabilityInformation:
        if (handle_ueCapabilityInformation(ctxt_pP, ue_context_p, ul_dcch_msg->message.choice.c1->choice.ueCapabilityInformation)
            == -1)
          return -1;
        break;

      case NR_UL_DCCH_MessageType__c1_PR_rrcReestablishmentComplete:
        if (handle_rrcReestablishmentComplete(ctxt_pP, ul_dcch_msg->message.choice.c1->choice.rrcReestablishmentComplete)
            == -1)
          return -1;
        break;

      case NR_UL_DCCH_MessageType__c1_PR_ueAssistanceInformation:
        if (LOG_DEBUGFLAG(DEBUG_ASN1)) {
          xer_fprint(stdout, &asn_DEF_NR_UL_DCCH_Message, (void *)ul_dcch_msg);
        }
        if (handle_ueAssistanceInformation(ctxt_pP, ue_context_p, ul_dcch_msg->message.choice.c1->choice.ueAssistanceInformation) == -1)
          return -1;
        break;

      default:
        break;
    }
  } else if (ul_dcch_msg->message.present == NR_UL_DCCH_MessageType_PR_messageClassExtension) {
    LOG_I(NR_RRC, "[Received NR_UL_DCCH_MessageType_PR_messageClassExtension]\n");
    if (ul_dcch_msg->message.choice.messageClassExtension->present == NR_UL_DCCH_MessageType__messageClassExtension_PR_c2) {
      LOG_I(NR_RRC, "[Received NR_UL_DCCH_MessageType__messageClassExtension_PR_c2]\n");

      switch (ul_dcch_msg->message.choice.messageClassExtension->choice.c2->present) {
        case NR_UL_DCCH_MessageType__messageClassExtension__c2_PR_NOTHING:
          LOG_I(NR_RRC, "Received c2_PR_NOTHING on UL-DCCH-Message\n");
          break;

        case NR_UL_DCCH_MessageType__messageClassExtension__c2_PR_sidelinkUEInformationNR_r16:
          LOG_D(NR_RRC, "Received sidelinkUEInformationNR on UL-DCCH-Message from %4lx\n", ctxt_pP->rntiMaybeUEid);
          xer_fprint(stdout, &asn_DEF_NR_UL_DCCH_Message, (void *)ul_dcch_msg);
          if (handle_sidelinkUEInformationNR(ctxt_pP, ue_context_p, ul_dcch_msg->message.choice.messageClassExtension->choice.c2->choice.sidelinkUEInformationNR_r16) == -1)
            return -1;
          break;

        default:
          break;
      }
    }
  }
  return 0;
}

void rrc_gNB_process_f1_setup_req(f1ap_setup_req_t *f1_setup_req) {
  LOG_I(NR_RRC,"Received F1 Setup Request from gNB_DU %llu (%s)\n",(unsigned long long int)f1_setup_req->gNB_DU_id,f1_setup_req->gNB_DU_name);
  int cu_cell_ind = 0;
  MessageDef *msg_p =itti_alloc_new_message (TASK_RRC_GNB, 0, F1AP_SETUP_RESP);
  F1AP_SETUP_RESP (msg_p).num_cells_to_activate = 0;
  MessageDef *msg_p2=itti_alloc_new_message (TASK_RRC_GNB, 0, F1AP_GNB_CU_CONFIGURATION_UPDATE);

  for (int i = 0; i < f1_setup_req->num_cells_available; i++) {
    for (int j=0; j<RC.nb_nr_inst; j++) {
      gNB_RRC_INST *rrc = RC.nrrrc[j];

      if (rrc->configuration.mcc[0] == f1_setup_req->cell[i].mcc &&
          rrc->configuration.mnc[0] == f1_setup_req->cell[i].mnc &&
          rrc->nr_cellid == f1_setup_req->cell[i].nr_cellid) {
	//fixme: multi instance is not consistent here
	F1AP_SETUP_RESP (msg_p).gNB_CU_name  = rrc->node_name;
        // check that CU rrc instance corresponds to mcc/mnc/cgi (normally cgi should be enough, but just in case)
        LOG_W(NR_RRC, "instance %d sib1 length %d\n", i, f1_setup_req->sib1_length[i]);
        AssertFatal(rrc->carrier.mib == NULL, "CU MIB is already initialized: double F1 setup request?\n");
        asn_dec_rval_t dec_rval = uper_decode_complete(NULL,
                                  &asn_DEF_NR_BCCH_BCH_Message,
                                  (void **)&rrc->carrier.mib,
                                  f1_setup_req->mib[i],
                                  f1_setup_req->mib_length[i]);
        AssertFatal(dec_rval.code == RC_OK,
                    "[gNB_CU %"PRIu8"] Failed to decode NR_BCCH_BCH_MESSAGE (%zu bits)\n",
                    j,
                    dec_rval.consumed );

        dec_rval = uper_decode_complete(NULL,
                                        &asn_DEF_NR_SIB1, //&asn_DEF_NR_BCCH_DL_SCH_Message,
                                        (void **)&rrc->carrier.siblock1_DU,
                                        f1_setup_req->sib1[i],
                                        f1_setup_req->sib1_length[i]);
        AssertFatal(dec_rval.code == RC_OK,
                    "[gNB_DU %"PRIu8"] Failed to decode NR_BCCH_DLSCH_MESSAGE (%zu bits)\n",
                    j,
                    dec_rval.consumed );

        // Parse message and extract SystemInformationBlockType1 field
        rrc->carrier.sib1 = rrc->carrier.siblock1_DU;
        if ( LOG_DEBUGFLAG(DEBUG_ASN1)){
          LOG_I(NR_RRC, "Printing received SIB1 container inside F1 setup request message:\n");
          xer_fprint(stdout, &asn_DEF_NR_SIB1,(void *)rrc->carrier.sib1);
        }

        rrc->carrier.physCellId = f1_setup_req->cell[i].nr_pci;

	F1AP_GNB_CU_CONFIGURATION_UPDATE (msg_p2).gNB_CU_name                                = rrc->node_name;
	F1AP_GNB_CU_CONFIGURATION_UPDATE (msg_p2).cells_to_activate[cu_cell_ind].mcc                           = rrc->configuration.mcc[0];
	F1AP_GNB_CU_CONFIGURATION_UPDATE (msg_p2).cells_to_activate[cu_cell_ind].mnc                           = rrc->configuration.mnc[0];
	F1AP_GNB_CU_CONFIGURATION_UPDATE (msg_p2).cells_to_activate[cu_cell_ind].mnc_digit_length              = rrc->configuration.mnc_digit_length[0];
	F1AP_GNB_CU_CONFIGURATION_UPDATE (msg_p2).cells_to_activate[cu_cell_ind].nr_cellid                     = rrc->nr_cellid;
	F1AP_GNB_CU_CONFIGURATION_UPDATE (msg_p2).cells_to_activate[cu_cell_ind].nrpci                         = f1_setup_req->cell[i].nr_pci;
        int num_SI= 0;

        if (rrc->carrier.SIB23) {
          F1AP_GNB_CU_CONFIGURATION_UPDATE (msg_p2).cells_to_activate[cu_cell_ind].SI_container[2]        = rrc->carrier.SIB23;
          F1AP_GNB_CU_CONFIGURATION_UPDATE (msg_p2).cells_to_activate[cu_cell_ind].SI_container_length[2] = rrc->carrier.sizeof_SIB23;
          num_SI++;
        }

        F1AP_GNB_CU_CONFIGURATION_UPDATE (msg_p2).cells_to_activate[cu_cell_ind].num_SI = num_SI;
        cu_cell_ind++;
	F1AP_GNB_CU_CONFIGURATION_UPDATE (msg_p2).num_cells_to_activate = cu_cell_ind;
	// send
        break;
      } else {// setup_req mcc/mnc match rrc internal list element
        LOG_W(NR_RRC,"[Inst %d] No matching MCC/MNC: rrc->mcc/f1_setup_req->mcc %d/%d rrc->mnc/f1_setup_req->mnc %d/%d rrc->nr_cellid/f1_setup_req->nr_cellid %ld/%ld \n",
              j, rrc->configuration.mcc[0], f1_setup_req->cell[i].mcc,
                 rrc->configuration.mnc[0], f1_setup_req->cell[i].mnc,
                 rrc->nr_cellid, f1_setup_req->cell[i].nr_cellid);
      }
    }// for (int j=0;j<RC.nb_inst;j++)

    if (cu_cell_ind == 0) {
      AssertFatal(1 == 0, "No cell found\n");
    }  else {
      // send ITTI message to F1AP-CU task
      itti_send_msg_to_task (TASK_CU_F1, 0, msg_p);

      itti_send_msg_to_task (TASK_CU_F1, 0, msg_p2);

    }

    // handle other failure cases
  }//for (int i=0;i<f1_setup_req->num_cells_available;i++)
}

void rrc_gNB_process_initial_ul_rrc_message(const f1ap_initial_ul_rrc_message_t *ul_rrc)
{
  // first get RRC instance (note, no the ITTI instance)
  module_id_t i = 0;
  for (i=0; i < RC.nb_nr_inst; i++) {
    gNB_RRC_INST *rrc = RC.nrrrc[i];
    if (rrc->nr_cellid == ul_rrc->nr_cellid)
      break;
  }
  //AssertFatal(i != RC.nb_nr_inst, "Cell_id not found\n");
  // TODO REMOVE_DU_RRC in monolithic mode, the MAC does not have the
  // nr_cellid. Thus, the above check would fail. For the time being, just put
  // a warning, as we handle one DU only anyway
  if (i == RC.nb_nr_inst) {
    i = 0;
    LOG_W(RRC, "initial UL RRC message nr_cellid %ld does not match RRC's %ld\n", ul_rrc->nr_cellid, RC.nrrrc[0]->nr_cellid);
  }
  nr_rrc_gNB_decode_ccch(i, ul_rrc->crnti, ul_rrc->rrc_container, ul_rrc->rrc_container_length, ul_rrc->du2cu_rrc_container, ul_rrc->du2cu_rrc_container_length, false, 0, 0);

  if (ul_rrc->rrc_container)
    free(ul_rrc->rrc_container);
  if (ul_rrc->du2cu_rrc_container)
    free(ul_rrc->du2cu_rrc_container);
}

void rrc_gNB_process_release_request(const module_id_t gnb_mod_idP, x2ap_ENDC_sgnb_release_request_t *m)
{
  gNB_RRC_INST *rrc = RC.nrrrc[gnb_mod_idP];
  rrc_remove_nsa_user(rrc, m->rnti);
}

void rrc_gNB_process_dc_overall_timeout(const module_id_t gnb_mod_idP, x2ap_ENDC_dc_overall_timeout_t *m)
{
  gNB_RRC_INST *rrc = RC.nrrrc[gnb_mod_idP];
  rrc_remove_nsa_user(rrc, m->rnti);
}

static void rrc_CU_process_ue_context_setup_response(MessageDef *msg_p, instance_t instance)
{
  f1ap_ue_context_setup_t *resp = &F1AP_UE_CONTEXT_SETUP_RESP(msg_p);
  gNB_RRC_INST *rrc = RC.nrrrc[instance];
  rrc_gNB_ue_context_t *ue_context_p = rrc_gNB_get_ue_context_by_rnti(rrc, resp->rnti);
  gNB_RRC_UE_t *UE = &ue_context_p->ue_context;

  NR_CellGroupConfig_t *cellGroupConfig = NULL;
  asn_dec_rval_t dec_rval = uper_decode_complete(NULL,
                                                 &asn_DEF_NR_CellGroupConfig,
                                                 (void **)&cellGroupConfig,
                                                 (uint8_t *)resp->du_to_cu_rrc_information->cellGroupConfig,
                                                 resp->du_to_cu_rrc_information->cellGroupConfig_length);
  AssertFatal(dec_rval.code == RC_OK && dec_rval.consumed > 0, "Cell group config decode error\n");

  if (UE->masterCellGroup) {
    ASN_STRUCT_FREE(asn_DEF_NR_CellGroupConfig, UE->masterCellGroup);
    LOG_I(RRC, "UE %04x replacing existing CellGroupConfig with new one received from DU\n", UE->rnti);
  }
  UE->masterCellGroup = cellGroupConfig;
  if (LOG_DEBUGFLAG(DEBUG_ASN1))
    xer_fprint(stdout, &asn_DEF_NR_CellGroupConfig, UE->masterCellGroup);

  /* at this point, we don't have to do anything: the UE context setup request
   * includes the Security Command, whose response will trigger the following
   * messages (UE capability, to be specific) */
}

static void rrc_CU_process_ue_context_release_request(MessageDef *msg_p)
{
  const int instance = 0;
  f1ap_ue_context_release_req_t *req = &F1AP_UE_CONTEXT_RELEASE_REQ(msg_p);
  gNB_RRC_INST *rrc = RC.nrrrc[instance];
  rrc_gNB_ue_context_t *ue_context_p = rrc_gNB_get_ue_context_by_rnti(rrc, req->rnti);

  /* TODO: marshall types correctly */
  LOG_I(NR_RRC, "received UE Context Release Request for UE %04x, forwarding to AMF\n", req->rnti);
  rrc_gNB_send_NGAP_UE_CONTEXT_RELEASE_REQ(instance,
                                           ue_context_p,
                                           NGAP_CAUSE_RADIO_NETWORK,
                                           NGAP_CAUSE_RADIO_NETWORK_RADIO_CONNECTION_WITH_UE_LOST);
}

static void rrc_CU_process_ue_context_release_complete(MessageDef *msg_p)
{
  const int instance = 0;
  f1ap_ue_context_release_complete_t *complete = &F1AP_UE_CONTEXT_RELEASE_COMPLETE(msg_p);
  gNB_RRC_INST *rrc = RC.nrrrc[instance];
  rrc_gNB_ue_context_t *ue_context_p = rrc_gNB_get_ue_context_by_rnti(rrc, complete->rnti);
  gNB_RRC_UE_t *UE = &ue_context_p->ue_context;

  nr_pdcp_remove_UE(UE->rnti);
  newGtpuDeleteAllTunnels(instance, UE->rnti);
  rrc_gNB_send_NGAP_UE_CONTEXT_RELEASE_COMPLETE(instance, UE->gNB_ue_ngap_id);
  LOG_I(NR_RRC, "removed UE %04x \n", UE->rnti);
  rrc_gNB_remove_ue_context(rrc, ue_context_p);
}

static void rrc_CU_process_ue_context_modification_response(MessageDef *msg_p, instance_t instance)
{
  f1ap_ue_context_modif_resp_t *resp = &F1AP_UE_CONTEXT_MODIFICATION_RESP(msg_p);
  protocol_ctxt_t ctxt = {.rntiMaybeUEid = resp->rnti, .module_id = instance, .instance = instance, .enb_flag = 1, .eNB_index = instance};
  gNB_RRC_INST *rrc = RC.nrrrc[ctxt.module_id];
  rrc_gNB_ue_context_t *ue_context_p = rrc_gNB_get_ue_context_by_rnti(rrc, resp->rnti);
  gNB_RRC_UE_t *UE = &ue_context_p->ue_context;

  if (resp->drbs_to_be_setup_length > 0) {
    e1ap_bearer_setup_req_t req = {0};
    req.numPDUSessionsMod = UE->nb_of_pdusessions;
    req.gNB_cu_cp_ue_id = UE->gNB_ue_ngap_id;
    req.rnti = UE->rnti;
    for (int i = 0; i < req.numPDUSessionsMod; i++) {
      req.pduSessionMod[i].numDRB2Modify = resp->drbs_to_be_setup_length;
      for (int j = 0; j < resp->drbs_to_be_setup_length; j++) {
        f1ap_drb_to_be_setup_t *drb_f1 = resp->drbs_to_be_setup + j;
        DRB_nGRAN_to_setup_t *drb_e1 = req.pduSessionMod[i].DRBnGRanModList + j;

        drb_e1->id = drb_f1->drb_id;
        drb_e1->numDlUpParam = drb_f1->up_dl_tnl_length;
        drb_e1->DlUpParamList[0].tlAddress = drb_f1->up_dl_tnl[0].tl_address;
        drb_e1->DlUpParamList[0].teId = drb_f1->up_dl_tnl[0].teid;
      }
    }

    // send the F1 response message up to update F1-U tunnel info
    // it seems the rrc transaction id (xid) is not needed here
    rrc->cucp_cuup.bearer_context_mod(&req, instance);
  }

  if (resp->du_to_cu_rrc_information != NULL && resp->du_to_cu_rrc_information->cellGroupConfig != NULL) {
    LOG_W(RRC, "UE context modification response contains new CellGroupConfig for UE %04x, triggering reconfiguration\n", UE->rnti);
    NR_CellGroupConfig_t *cellGroupConfig = NULL;
    asn_dec_rval_t dec_rval = uper_decode_complete(NULL,
                                                   &asn_DEF_NR_CellGroupConfig,
                                                   (void **)&cellGroupConfig,
                                                   (uint8_t *)resp->du_to_cu_rrc_information->cellGroupConfig,
                                                   resp->du_to_cu_rrc_information->cellGroupConfig_length);
    AssertFatal(dec_rval.code == RC_OK && dec_rval.consumed > 0, "Cell group config decode error\n");

    if (UE->masterCellGroup) {
      ASN_STRUCT_FREE(asn_DEF_NR_CellGroupConfig, UE->masterCellGroup);
      LOG_I(RRC, "UE %04x replacing existing CellGroupConfig with new one received from DU\n", UE->rnti);
    }
    UE->masterCellGroup = cellGroupConfig;

    rrc_gNB_generate_dedicatedRRCReconfiguration(&ctxt, ue_context_p);
  }
}

unsigned int mask_flip(unsigned int x) {
  return((((x>>8) + (x<<8))&0xffff)>>6);
}

static unsigned int get_dl_bw_mask(const gNB_RRC_INST *rrc, const NR_UE_NR_Capability_t *cap)
{
  int common_band = *rrc->carrier.servingcellconfigcommon->downlinkConfigCommon->frequencyInfoDL->frequencyBandList.list.array[0];
  int common_scs  = rrc->carrier.servingcellconfigcommon->downlinkConfigCommon->frequencyInfoDL->scs_SpecificCarrierList.list.array[0]->subcarrierSpacing;
  for (int i=0;i<cap->rf_Parameters.supportedBandListNR.list.count;i++) {
     NR_BandNR_t *bandNRinfo = cap->rf_Parameters.supportedBandListNR.list.array[i];
     if (bandNRinfo->bandNR == common_band) {
       if (common_band < 257) { // FR1
          switch (common_scs) {
            case NR_SubcarrierSpacing_kHz15 :
               if (bandNRinfo->channelBWs_DL &&
                   bandNRinfo->channelBWs_DL->choice.fr1 &&
                   bandNRinfo->channelBWs_DL->choice.fr1->scs_15kHz)
                     return(mask_flip((unsigned int)*(uint16_t*)bandNRinfo->channelBWs_DL->choice.fr1->scs_15kHz->buf));
 	      break;
            case NR_SubcarrierSpacing_kHz30 :
               if (bandNRinfo->channelBWs_DL &&
                   bandNRinfo->channelBWs_DL->choice.fr1 &&
                   bandNRinfo->channelBWs_DL->choice.fr1->scs_30kHz)
                     return(mask_flip((unsigned int)*(uint16_t*)bandNRinfo->channelBWs_DL->choice.fr1->scs_30kHz->buf));
              break;
            case NR_SubcarrierSpacing_kHz60 :
               if (bandNRinfo->channelBWs_DL &&
                   bandNRinfo->channelBWs_DL->choice.fr1 &&
                   bandNRinfo->channelBWs_DL->choice.fr1->scs_60kHz)
                     return(mask_flip((unsigned int)*(uint16_t*)bandNRinfo->channelBWs_DL->choice.fr1->scs_60kHz->buf));
              break;
          }
       }
       else {
          switch (common_scs) {
            case NR_SubcarrierSpacing_kHz60 :
               if (bandNRinfo->channelBWs_DL &&
                   bandNRinfo->channelBWs_DL->choice.fr2 &&
                   bandNRinfo->channelBWs_DL->choice.fr2->scs_60kHz)
                     return(mask_flip((unsigned int)*(uint16_t*)bandNRinfo->channelBWs_DL->choice.fr2->scs_60kHz->buf));
              break;
            case NR_SubcarrierSpacing_kHz120 :
               if (bandNRinfo->channelBWs_DL &&
                   bandNRinfo->channelBWs_DL->choice.fr2 &&
                   bandNRinfo->channelBWs_DL->choice.fr2->scs_120kHz)
                     return(mask_flip((unsigned int)*(uint16_t*)bandNRinfo->channelBWs_DL->choice.fr2->scs_120kHz->buf));
              break;
       }
     }
   }
  }
  return(0);
}

static unsigned int get_ul_bw_mask(const gNB_RRC_INST *rrc, const NR_UE_NR_Capability_t *cap)
{
  int common_band = *rrc->carrier.servingcellconfigcommon->uplinkConfigCommon->frequencyInfoUL->frequencyBandList->list.array[0];
  int common_scs  = rrc->carrier.servingcellconfigcommon->uplinkConfigCommon->frequencyInfoUL->scs_SpecificCarrierList.list.array[0]->subcarrierSpacing;
  for (int i=0;i<cap->rf_Parameters.supportedBandListNR.list.count;i++) {
     NR_BandNR_t *bandNRinfo = cap->rf_Parameters.supportedBandListNR.list.array[i];
     if (bandNRinfo->bandNR == common_band) {
       if (common_band < 257) { // FR1
          switch (common_scs) {
            case NR_SubcarrierSpacing_kHz15 :
               if (bandNRinfo->channelBWs_UL &&
                   bandNRinfo->channelBWs_UL->choice.fr1 &&
                   bandNRinfo->channelBWs_UL->choice.fr1->scs_15kHz)
                     return(mask_flip((unsigned int)*(uint16_t*)bandNRinfo->channelBWs_UL->choice.fr1->scs_15kHz->buf));
 	      break;
            case NR_SubcarrierSpacing_kHz30 :
               if (bandNRinfo->channelBWs_UL &&
                   bandNRinfo->channelBWs_UL->choice.fr1 &&
                   bandNRinfo->channelBWs_UL->choice.fr1->scs_30kHz)
                     return(mask_flip((unsigned int)*(uint16_t*)bandNRinfo->channelBWs_UL->choice.fr1->scs_30kHz->buf));
              break;
            case NR_SubcarrierSpacing_kHz60 :
               if (bandNRinfo->channelBWs_UL &&
                   bandNRinfo->channelBWs_UL->choice.fr1 &&
                   bandNRinfo->channelBWs_UL->choice.fr1->scs_60kHz)
                     return(mask_flip((unsigned int)*(uint16_t*)bandNRinfo->channelBWs_UL->choice.fr1->scs_60kHz->buf));
              break;
          }
       }
       else {
          switch (common_scs) {
            case NR_SubcarrierSpacing_kHz60 :
               if (bandNRinfo->channelBWs_UL &&
                   bandNRinfo->channelBWs_UL->choice.fr2 &&
                   bandNRinfo->channelBWs_UL->choice.fr2->scs_60kHz)
                     return(mask_flip((unsigned int)*(uint16_t*)bandNRinfo->channelBWs_UL->choice.fr2->scs_60kHz->buf));
              break;
            case NR_SubcarrierSpacing_kHz120 :
               if (bandNRinfo->channelBWs_UL &&
                   bandNRinfo->channelBWs_UL->choice.fr2 &&
                   bandNRinfo->channelBWs_UL->choice.fr2->scs_120kHz)
                     return(mask_flip((unsigned int)*(uint16_t*)bandNRinfo->channelBWs_UL->choice.fr2->scs_120kHz->buf));
              break;
       }
     }
   }
  }
  return(0);
}

static int get_ul_mimo_layersCB(const gNB_RRC_INST *rrc, const NR_UE_NR_Capability_t *cap)
{
  int common_scs  = rrc->carrier.servingcellconfigcommon->uplinkConfigCommon->frequencyInfoUL->scs_SpecificCarrierList.list.array[0]->subcarrierSpacing;

  // check featureSet
  NR_FeatureSets_t *fs=cap->featureSets;
  if (fs) {
    // go through UL feature sets and look for one with current SCS
    for (int i=0;i<fs->featureSetsUplinkPerCC->list.count;i++) {
       if (fs->featureSetsUplinkPerCC->list.array[i]->supportedSubcarrierSpacingUL == common_scs &&
           fs->featureSetsUplinkPerCC->list.array[i]->mimo_CB_PUSCH &&
           fs->featureSetsUplinkPerCC->list.array[i]->mimo_CB_PUSCH->maxNumberMIMO_LayersCB_PUSCH)
           return(1<<*fs->featureSetsUplinkPerCC->list.array[i]->mimo_CB_PUSCH->maxNumberMIMO_LayersCB_PUSCH);
    }
  }
  return(1);
}

static int get_ul_mimo_layers(const gNB_RRC_INST *rrc, const NR_UE_NR_Capability_t *cap)
{
  int common_scs  = rrc->carrier.servingcellconfigcommon->uplinkConfigCommon->frequencyInfoUL->scs_SpecificCarrierList.list.array[0]->subcarrierSpacing;

  // check featureSet
  NR_FeatureSets_t *fs=cap->featureSets;
  if (fs) {
    // go through UL feature sets and look for one with current SCS
    for (int i=0;i<fs->featureSetsUplinkPerCC->list.count;i++) {
       if (fs->featureSetsUplinkPerCC->list.array[i]->supportedSubcarrierSpacingUL == common_scs &&
           fs->featureSetsUplinkPerCC->list.array[i]->maxNumberMIMO_LayersNonCB_PUSCH)
           return(1<<*fs->featureSetsUplinkPerCC->list.array[i]->maxNumberMIMO_LayersNonCB_PUSCH);
    }
  }
  return(1);
}

static int get_dl_mimo_layers(const gNB_RRC_INST *rrc, const NR_UE_NR_Capability_t *cap)
{
  int common_scs  = rrc->carrier.servingcellconfigcommon->downlinkConfigCommon->frequencyInfoDL->scs_SpecificCarrierList.list.array[0]->subcarrierSpacing;

  // check featureSet
  NR_FeatureSets_t *fs=cap->featureSets;
  if (fs) {
    // go through UL feature sets and look for one with current SCS
    for (int i=0;i<fs->featureSetsDownlinkPerCC->list.count;i++) {
       if (fs->featureSetsUplinkPerCC->list.array[i]->supportedSubcarrierSpacingUL == common_scs &&
           fs->featureSetsDownlinkPerCC->list.array[i]->maxNumberMIMO_LayersPDSCH)
           return(2<<*fs->featureSetsDownlinkPerCC->list.array[i]->maxNumberMIMO_LayersPDSCH);
    }
  }
  return(1);
}

int rrc_gNB_process_e1_setup_req(e1ap_setup_req_t *req, instance_t instance) {

  AssertFatal(req->supported_plmns <= PLMN_LIST_MAX_SIZE, "Supported PLMNs is more than PLMN_LIST_MAX_SIZE\n");
  gNB_RRC_INST *rrc = RC.nrrrc[0]; //TODO: remove hardcoding of RC index here
  MessageDef *msg_p = itti_alloc_new_message(TASK_RRC_GNB, instance, E1AP_SETUP_RESP);

  e1ap_setup_resp_t *resp = &E1AP_SETUP_RESP(msg_p);
  resp->transac_id = req->transac_id;

  for (int i=0; i < req->supported_plmns; i++) {
    if (rrc->configuration.mcc[i] == req->plmns[i].mcc &&
        rrc->configuration.mnc[i] == req->plmns[i].mnc) {
      LOG_E(NR_RRC, "PLMNs received from CUUP (mcc:%d, mnc:%d) did not match with PLMNs in RRC (mcc:%d, mnc:%d)\n",
            req->plmns[i].mcc, req->plmns[i].mnc, rrc->configuration.mcc[i], rrc->configuration.mnc[i]);
      return -1;
    }
  }

  itti_send_msg_to_task(TASK_CUCP_E1, instance, msg_p);

  return 0;
}

void prepare_and_send_ue_context_modification_f1(rrc_gNB_ue_context_t *ue_context_p, e1ap_bearer_setup_resp_t *e1ap_resp)
{
  /* Generate a UE context modification request message towards the DU to
   * instruct the DU for SRB2 and DRB configuration and get the updates on
   * master cell group config from the DU*/

  gNB_RRC_INST *rrc = RC.nrrrc[0];
  gNB_RRC_UE_t *UE = &ue_context_p->ue_context;

  // Skip F1AP/MAC for Remote UE - they use SRAP, not direct DU connection
  if (UE->is_remote_ue) {
    LOG_D(NR_RRC, "[REMOTE_UE] Skipping F1AP UE context modification (no direct DU connection)\n");
    LOG_D(NR_RRC, "[REMOTE_UE] Generating RRCReconfiguration with DRB config and NAS PDU (IP address)\n");

    /* Remote UE uses Relay UE's masterCellGroup (already copied during RRCSetup)
       Generate RRCReconfiguration with DRB config and NAS PDU containing IP address */
    protocol_ctxt_t ctxt = {0};
    PROTOCOL_CTXT_SET_BY_MODULE_ID(&ctxt, 0, GNB_FLAG_YES, UE->rnti, 0, 0, 0);
    rrc_gNB_generate_dedicatedRRCReconfiguration(&ctxt, ue_context_p);
    return;
  }

  /* Instruction towards the DU for DRB configuration and tunnel creation */
  int nb_drb = e1ap_resp->pduSession[0].numDRBSetup;
  f1ap_drb_to_be_setup_t drbs[nb_drb];
  for (int i = 0; i < nb_drb; i++) {
    drbs[i].drb_id = e1ap_resp->pduSession[0].DRBnGRanList[i].id;
    drbs[i].rlc_mode = rrc->um_on_default_drb ? RLC_MODE_UM : RLC_MODE_AM;
    drbs[i].up_ul_tnl[0].tl_address = e1ap_resp->pduSession[0].DRBnGRanList[i].UpParamList[0].tlAddress;
    drbs[i].up_ul_tnl[0].port = rrc->eth_params_s.my_portd;
    drbs[i].up_ul_tnl[0].teid = e1ap_resp->pduSession[0].DRBnGRanList[i].UpParamList[0].teId;
    drbs[i].up_ul_tnl_length = 1;
  }

  /* Instruction towards the DU for SRB2 configuration */
  int nb_srb = 0;
  f1ap_srb_to_be_setup_t srbs[1];
  if (UE->Srb[2].Active == 0) {
    UE->Srb[2].Active = 1;
    nb_srb = 1;
    srbs[0].srb_id = 2;
    srbs[0].lcid = 2;
  }

  f1ap_ue_context_modif_req_t ue_context_modif_req = {
    .gNB_CU_ue_id = 0xffffffff, /* filled by F1 for the moment */
    .gNB_DU_ue_id = 0xffffffff, /* filled by F1 for the moment */
    .rnti = UE->rnti,
    .mcc = rrc->configuration.mcc[0],
    .mnc = rrc->configuration.mnc[0],
    .mnc_digit_length = rrc->configuration.mnc_digit_length[0],
    .nr_cellid = rrc->nr_cellid,
    .servCellId = 0, /* TODO: correct value? */
    .srbs_to_be_setup_length = nb_srb,
    .srbs_to_be_setup = srbs,
    .drbs_to_be_setup_length = nb_drb,
    .drbs_to_be_setup = drbs,
  };
  rrc->mac_rrc.ue_context_modification_request(&ue_context_modif_req);
}

void rrc_gNB_process_e1_bearer_context_setup_resp(e1ap_bearer_setup_resp_t *resp, instance_t instance) {
  // Find the UE context from UE ID and send ITTI message to F1AP to send UE context modification message to DU

  rrc_gNB_ue_context_t *ue_context_p = rrc_gNB_get_ue_context(RC.nrrrc[instance], resp->gNB_cu_cp_ue_id);
  gNB_RRC_UE_t *UE = &ue_context_p->ue_context;
  protocol_ctxt_t ctxt = {0};
  PROTOCOL_CTXT_SET_BY_MODULE_ID(&ctxt, 0, GNB_FLAG_YES, UE->rnti, 0, 0, 0);

  gtpv1u_gnb_create_tunnel_resp_t create_tunnel_resp={0};
  create_tunnel_resp.num_tunnels = resp->numPDUSessions;
  for (int i=0; i < resp->numPDUSessions; i++) {
    create_tunnel_resp.pdusession_id[i]  = resp->pduSession[i].id;
    create_tunnel_resp.gnb_NGu_teid[i] = resp->pduSession[i].teId;
    memcpy(create_tunnel_resp.gnb_addr.buffer,
           &resp->pduSession[i].tlAddress,
           sizeof(in_addr_t));
    create_tunnel_resp.gnb_addr.length = sizeof(in_addr_t); // IPv4 byte length
  }

  nr_rrc_gNB_process_GTPV1U_CREATE_TUNNEL_RESP(&ctxt, &create_tunnel_resp, 0);

  /* Remote UE: Skip F1AP UE context modification - Remote UE doesn't have direct DU connection
     Instead, directly generate RRCReconfiguration and send via SRAP through Relay UE */
  if (UE->is_remote_ue) {
    LOG_D(NR_RRC, "[Remote UE] Skipping F1AP ue_context_modification_request (Remote UE uses SRAP, not F1AP)\n");
    LOG_D(NR_RRC, "[Remote UE] Generating RRCReconfiguration directly for SRAP forwarding\n");

    /* Remote UE uses Relay UE's masterCellGroup (already copied during RRCSetup)
       No need to wait for F1AP response - directly generate RRCReconfiguration */
    rrc_gNB_generate_dedicatedRRCReconfiguration(&ctxt, ue_context_p);
    return;
  }

  // TODO: SV: combine e1ap_bearer_setup_req_t and e1ap_bearer_setup_resp_t and minimize assignments
  prepare_and_send_ue_context_modification_f1(ue_context_p, resp);
}

static void print_rrc_meas(FILE *f, const NR_MeasResults_t *measresults)
{
  DevAssert(measresults->measResultServingMOList.list.count >= 1);
  if (measresults->measResultServingMOList.list.count > 1)
    LOG_W(RRC, "Received %d MeasResultServMO, but handling only 1!\n", measresults->measResultServingMOList.list.count);

  NR_MeasResultServMO_t *measresultservmo = measresults->measResultServingMOList.list.array[0];
  NR_MeasResultNR_t *measresultnr = &measresultservmo->measResultServingCell;
  NR_MeasQuantityResults_t *mqr = measresultnr->measResult.cellResults.resultsSSB_Cell;

  fprintf(f, "    servingCellId %ld MeasResultNR for phyCellId %ld:\n      resultSSB:", measresultservmo->servCellId, *measresultnr->physCellId);
  if (mqr != NULL) {
    const long rrsrp = *mqr->rsrp - 156;
    const float rrsrq = (float) (*mqr->rsrq - 87) / 2.0f;
    const float rsinr = (float) (*mqr->sinr - 46) / 2.0f;
    fprintf(f, "RSRP %ld dBm RSRQ %.1f dB SINR %.1f dB\n", rrsrp, rrsrq, rsinr);
  } else {
    fprintf(f, "NOT PROVIDED\n");
  }
}

static void write_rrc_stats(const gNB_RRC_INST *rrc)
{
  const char *filename = "nrRRC_stats.log";
  FILE *f = fopen(filename, "w");
  if (f == NULL) {
    LOG_E(NR_RRC, "cannot open %s for writing\n", filename);
    return;
  }

  rrc_gNB_ue_context_t *ue_context_p = NULL;
  /* cast is necessary to eliminate warning "discards ‘const’ qualifier" */
  RB_FOREACH(ue_context_p, rrc_nr_ue_tree_s, &((gNB_RRC_INST *)rrc)->rrc_ue_head)
  {
    const gNB_RRC_UE_t *ue_ctxt = &ue_context_p->ue_context;
    const rnti_t rnti = ue_ctxt->rnti;

    fprintf(f, "NR RRC UE rnti %04x:", rnti);

    if (ue_ctxt->Initialue_identity_5g_s_TMSI.presence)
      fprintf(f, " S-TMSI %x", ue_ctxt->Initialue_identity_5g_s_TMSI.fiveg_tmsi);

    fprintf(f, "\n");

    if (ue_ctxt->UE_Capability_nr) {
      fprintf(f,
              "    UE cap: BW DL %x. BW UL %x, DL MIMO Layers %d UL MIMO Layers (CB) %d UL MIMO Layers (nonCB) %d\n",
              get_dl_bw_mask(rrc, ue_ctxt->UE_Capability_nr),
              get_ul_bw_mask(rrc, ue_ctxt->UE_Capability_nr),
              get_dl_mimo_layers(rrc, ue_ctxt->UE_Capability_nr),
              get_ul_mimo_layersCB(rrc, ue_ctxt->UE_Capability_nr),
              get_ul_mimo_layers(rrc, ue_ctxt->UE_Capability_nr));
    }

    if (ue_ctxt->measResults)
      print_rrc_meas(f, ue_ctxt->measResults);
  }

  fclose(f);
}

/* Thresholds for SL HARQ-based MCS adaptation */
#define SL_HARQ_MCS_WINDOW      16
#define SL_HARQ_NACK_RATIO_HIGH 0.2f
#define SL_HARQ_NACK_RATIO_LOW  0.05f
#define SL_MCS_PSSCH_MAX        28

void nr_gNB_process_sl_harq_report_ind(const protocol_ctxt_t *const ctxt_pP, MessageDef *msg_p, instance_t instance)
{
  gNB_RRC_INST *gnb_rrc_inst = RC.nrrrc[instance];
  const rnti_t rnti = NR_RRC_SL_HARQ_REPORT_IND(msg_p).rnti;
  rrc_gNB_ue_context_t *ue_context_p = rrc_gNB_get_ue_context_by_rnti(gnb_rrc_inst, rnti);
  if (!ue_context_p) {
    LOG_W(NR_RRC, "%s: RNTI 0x%04X not found, dropping SL HARQ report\n", __func__, (unsigned)rnti);
    return;
  }

  frame_t frame = NR_RRC_SL_HARQ_REPORT_IND(msg_p).frame;
  slot_t slot = NR_RRC_SL_HARQ_REPORT_IND(msg_p).slot;
  uint32_t harq_payload = NR_RRC_SL_HARQ_REPORT_IND(msg_p).harq_payload;
  /* Low 16 bits = ACK(1)/NACK(0) per HARQ process; high 16 bits = active process bitmap */
  uint16_t ack_nack = (uint16_t)(harq_payload & 0xFFFF);
  uint16_t harq_process_bmap = (uint16_t)(harq_payload >> 16);


  LOG_D(RRC, "%4u.%2u %s RNTI 0x%04X payload 0x%04X 0x%04X : <== sl_harq\n",
        frame, slot, __func__, (unsigned)rnti,
        (unsigned)harq_process_bmap, (unsigned)ack_nack);

  uint32_t ack = 0, nack = 0;
  for (int i = 0; i < 16; i++) {
    if (!(harq_process_bmap & (1u << i)))
      continue;
    if (ack_nack & (1u << i))
      ack++;
    else
      nack++;
  }
  gNB_RRC_UE_t *ue_p = &ue_context_p->ue_context;
  ue_p->sl_harq_ack_count  += ack;
  ue_p->sl_harq_nack_count += nack;

  /* MCS adaptation: run once per window of SL_HARQ_MCS_WINDOW results */
  const uint32_t total = ue_p->sl_harq_ack_count + ue_p->sl_harq_nack_count;
  if (total >= SL_HARQ_MCS_WINDOW) {
    const float nack_ratio = (float)ue_p->sl_harq_nack_count / (float)total;
    ue_p->sl_harq_ack_count  = 0;
    ue_p->sl_harq_nack_count = 0;

#ifdef ENABLE_BLER_INSTRUMENTATION
    /* Skip SL MCS adaptation if Uu fixed MCS mode is active (for BLER testing)
     * harq_round_max=1 indicates fixed MCS mode for Uu interface
     * Continuous RRCReconfigurations keep LCID 1 active, preventing fixed MCS scheduler logic */
    const NR_bler_options_t *bo = &RC.nrmac[instance]->dl_bler;
    if (bo->harq_round_max == 1) {
      LOG_I(NR_RRC, "SL MCS adaptation skipped: Uu fixed MCS testing active (harq_round_max=1)\n");
      return;
    }
#endif

    uint8_t old_max_mcs = ue_p->sl_max_mcs_pssch_r16;
    uint8_t new_max_mcs = old_max_mcs;

    if (nack_ratio > SL_HARQ_NACK_RATIO_HIGH) {
      if (new_max_mcs > 0)
        new_max_mcs--;
    } else if (nack_ratio < SL_HARQ_NACK_RATIO_LOW) {
      if (new_max_mcs < SL_MCS_PSSCH_MAX)
        new_max_mcs++;
    }

    if (new_max_mcs != old_max_mcs) {
      ue_p->sl_max_mcs_pssch_r16 = new_max_mcs;
      LOG_D(NR_RRC,
            "SL HARQ-based MCS adaptation: RNTI %04x sl_max_mcs_pssch_r16 %d -> %d "
            "(ack=%u nack=%u nack_ratio=%.2f)\n",
            (unsigned)rnti, (int)old_max_mcs, (int)new_max_mcs, ack, nack, (double)nack_ratio);
      LOG_W(NR_RRC, "gNB: Sending RRCReconfiguration to RNTI %04x with updated SL TX params (max_mcs=%d)\n",
            (unsigned)rnti, (int)new_max_mcs);
      nr_rrc_reconfiguration_req_sidelink(ue_context_p, ctxt_pP, NULL, NULL);
    }
  }
}

///---------------------------------------------------------------------------------------------------------------///
///---------------------------------------------------------------------------------------------------------------///
void *rrc_gnb_task(void *args_p) {
  MessageDef *msg_p;
  instance_t                         instance;
  int                                result;
  protocol_ctxt_t ctxt = {.module_id = 0, .enb_flag = 1, .instance = 0, .rntiMaybeUEid = 0, .frame = -1, .subframe = -1, .eNB_index = 0, .brOption = false};

  long stats_timer_id = 1;
  if (!IS_SOFTMODEM_NOSTATS_BIT) {
    /* timer to write stats to file */
    timer_setup(1, 0, TASK_RRC_GNB, 0, TIMER_PERIODIC, NULL, &stats_timer_id);
  }
  
  itti_mark_task_ready(TASK_RRC_GNB);
  LOG_I(NR_RRC,"Entering main loop of NR_RRC message task\n");

  while (1) {
    // Wait for a message
    itti_receive_msg(TASK_RRC_GNB, &msg_p);
    const char *msg_name_p = ITTI_MSG_NAME(msg_p);
    instance = ITTI_MSG_DESTINATION_INSTANCE(msg_p);
    LOG_D(NR_RRC, "Received Msg %s\n", msg_name_p);
    switch (ITTI_MSG_ID(msg_p)) {
      case TERMINATE_MESSAGE:
        LOG_W(NR_RRC, " *** Exiting NR_RRC thread\n");
        itti_exit_task();
        break;

      case MESSAGE_TEST:
        LOG_I(NR_RRC, "[gNB %ld] Received %s\n", instance, msg_name_p);
        break;

      case TIMER_HAS_EXPIRED:
        /* only this one handled for now */
        DevAssert(TIMER_HAS_EXPIRED(msg_p).timer_id == stats_timer_id);
        write_rrc_stats(RC.nrrrc[0]);
        break;

      case F1AP_INITIAL_UL_RRC_MESSAGE:
        AssertFatal(NODE_IS_CU(RC.nrrrc[instance]->node_type) || NODE_IS_MONOLITHIC(RC.nrrrc[instance]->node_type),
                    "should not receive F1AP_INITIAL_UL_RRC_MESSAGE, need call by CU!\n");
        rrc_gNB_process_initial_ul_rrc_message(&F1AP_INITIAL_UL_RRC_MESSAGE(msg_p));
        break;

      /* Messages from PDCP */
      case F1AP_UL_RRC_MESSAGE:
        PROTOCOL_CTXT_SET_BY_INSTANCE(&ctxt,
                                      instance,
                                      GNB_FLAG_YES,
                                      F1AP_UL_RRC_MESSAGE(msg_p).rnti,
                                      0,
                                      0);
        LOG_D(NR_RRC,
              "Decoding DCCH %d: ue %04lx, inst %ld, ctxt %p, size %d\n",
              F1AP_UL_RRC_MESSAGE(msg_p).srb_id,
              ctxt.rntiMaybeUEid,
              instance,
              &ctxt,
              F1AP_UL_RRC_MESSAGE(msg_p).rrc_container_length);
        rrc_gNB_decode_dcch(&ctxt,
                            F1AP_UL_RRC_MESSAGE(msg_p).srb_id,
                            F1AP_UL_RRC_MESSAGE(msg_p).rrc_container,
                            F1AP_UL_RRC_MESSAGE(msg_p).rrc_container_length);
        free(F1AP_UL_RRC_MESSAGE(msg_p).rrc_container);
        break;

      case NGAP_DOWNLINK_NAS:
        rrc_gNB_process_NGAP_DOWNLINK_NAS(msg_p, instance, &rrc_gNB_mui);
        break;

      case NGAP_PDUSESSION_SETUP_REQ:
        rrc_gNB_process_NGAP_PDUSESSION_SETUP_REQ(msg_p, instance);
        break;

      case NGAP_PDUSESSION_MODIFY_REQ:
        rrc_gNB_process_NGAP_PDUSESSION_MODIFY_REQ(msg_p, instance);
        break;

      case NGAP_PDUSESSION_RELEASE_COMMAND:
        rrc_gNB_process_NGAP_PDUSESSION_RELEASE_COMMAND(msg_p, instance);
        break;

      /* Messages from gNB app */
      case NRRRC_CONFIGURATION_REQ:
        openair_rrc_gNB_configuration(instance, &NRRRC_CONFIGURATION_REQ(msg_p));
        break;

      /* Messages from F1AP task */
      case F1AP_SETUP_REQ:
        AssertFatal(NODE_IS_CU(RC.nrrrc[instance]->node_type), "should not receive F1AP_SETUP_REQUEST, need call by CU!\n");
        rrc_gNB_process_f1_setup_req(&F1AP_SETUP_REQ(msg_p));
        break;

      case F1AP_UE_CONTEXT_SETUP_RESP:
        rrc_CU_process_ue_context_setup_response(msg_p, instance);
        break;

      case F1AP_UE_CONTEXT_MODIFICATION_RESP:
        rrc_CU_process_ue_context_modification_response(msg_p, instance);
        break;

      case F1AP_UE_CONTEXT_RELEASE_REQ:
        rrc_CU_process_ue_context_release_request(msg_p);
        break;

      case F1AP_UE_CONTEXT_RELEASE_COMPLETE:
        rrc_CU_process_ue_context_release_complete(msg_p);
        break;

      /* Messages from X2AP */
      case X2AP_ENDC_SGNB_ADDITION_REQ:
        LOG_I(NR_RRC, "Received ENDC sgNB addition request from X2AP \n");
        rrc_gNB_process_AdditionRequestInformation(instance, &X2AP_ENDC_SGNB_ADDITION_REQ(msg_p));
        break;

      case X2AP_ENDC_SGNB_RECONF_COMPLETE:
        LOG_A(NR_RRC, "Handling of reconfiguration complete message at RRC gNB is pending \n");
        break;

      case NGAP_INITIAL_CONTEXT_SETUP_REQ:
        rrc_gNB_process_NGAP_INITIAL_CONTEXT_SETUP_REQ(msg_p, instance);
        break;

      case X2AP_ENDC_SGNB_RELEASE_REQUEST:
        LOG_I(NR_RRC, "Received ENDC sgNB release request from X2AP \n");
        rrc_gNB_process_release_request(instance, &X2AP_ENDC_SGNB_RELEASE_REQUEST(msg_p));
        break;

      case X2AP_ENDC_DC_OVERALL_TIMEOUT:
        rrc_gNB_process_dc_overall_timeout(instance, &X2AP_ENDC_DC_OVERALL_TIMEOUT(msg_p));
        break;

      case NGAP_UE_CONTEXT_RELEASE_REQ:
        rrc_gNB_process_NGAP_UE_CONTEXT_RELEASE_REQ(msg_p, instance);
        break;

      case NGAP_UE_CONTEXT_RELEASE_COMMAND:
        rrc_gNB_process_NGAP_UE_CONTEXT_RELEASE_COMMAND(msg_p, instance);
        break;

      case E1AP_SETUP_REQ:
        rrc_gNB_process_e1_setup_req(&E1AP_SETUP_REQ(msg_p), instance);
        break;

      case E1AP_BEARER_CONTEXT_SETUP_RESP:
        rrc_gNB_process_e1_bearer_context_setup_resp(&E1AP_BEARER_CONTEXT_SETUP_RESP(msg_p), instance);

      case NGAP_PAGING_IND:
        rrc_gNB_process_PAGING_IND(msg_p, instance);
        break;

      case NR_RRC_SL_HARQ_REPORT_IND:
        LOG_D(NR_RRC,
              "%4u.%2u RNTI 0x%04X NR_RRC_SL_HARQ_REPORT_IND received at RRC task\n",
              NR_RRC_SL_HARQ_REPORT_IND(msg_p).frame,
              NR_RRC_SL_HARQ_REPORT_IND(msg_p).slot,
              (unsigned)NR_RRC_SL_HARQ_REPORT_IND(msg_p).rnti);
        PROTOCOL_CTXT_SET_BY_INSTANCE(&ctxt,
                                      instance,
                                      GNB_FLAG_YES,
                                      NR_RRC_SL_HARQ_REPORT_IND(msg_p).rnti,
                                      0,
                                      0);
        nr_gNB_process_sl_harq_report_ind(&ctxt, msg_p, instance);
        break;
      default:
        LOG_E(NR_RRC, "[gNB %ld] Received unexpected message %s\n", instance, msg_name_p);
        break;
    }

    result = itti_free(ITTI_MSG_ORIGIN_ID(msg_p), msg_p);
    AssertFatal(result == EXIT_SUCCESS, "Failed to free memory (%d)!\n", result);
    msg_p = NULL;
  }
}

static void rrc_deliver_ue_ctxt_setup_req(void *deliver_pdu_data, ue_id_t ue_id, int srb_id, char *buf, int size, int sdu_id, nr_intf_type_t intf_type)
{
  DevAssert(deliver_pdu_data != NULL);
  gNB_RRC_INST *rrc = deliver_pdu_data;
  f1ap_ue_context_setup_t ue_context_setup_req = {
    .gNB_CU_ue_id = 0xffffffff, /* filled by F1 for the moment */
    .gNB_DU_ue_id = 0xffffffff, /* filled by F1 for the moment */
    .rnti = ue_id,
    .mcc = rrc->configuration.mcc[0],
    .mnc = rrc->configuration.mnc[0],
    .mnc_digit_length = rrc->configuration.mnc_digit_length[0],
    .nr_cellid = rrc->nr_cellid,
    .servCellId = 0, /* TODO: correct value? */
    .srbs_to_be_setup = 0, /* no new SRBs */
    .drbs_to_be_setup = 0, /* no new DRBs */
    .rrc_container = (uint8_t*) buf, /* security mode command */
    .rrc_container_length = size,
  };
  rrc->mac_rrc.ue_context_setup_request(&ue_context_setup_req);
}

//-----------------------------------------------------------------------------
void
rrc_gNB_generate_SecurityModeCommand(
  const protocol_ctxt_t *const ctxt_pP,
  rrc_gNB_ue_context_t  *const ue_context_pP
)
//-----------------------------------------------------------------------------
{
  uint8_t                             buffer[100];
  uint8_t                             size;
  gNB_RRC_UE_t *ue_p = &ue_context_pP->ue_context;

  T(T_ENB_RRC_SECURITY_MODE_COMMAND, T_INT(ctxt_pP->module_id), T_INT(ctxt_pP->frame), T_INT(ctxt_pP->subframe), T_INT(ctxt_pP->rntiMaybeUEid));
  NR_IntegrityProtAlgorithm_t integrity_algorithm = (NR_IntegrityProtAlgorithm_t)ue_p->integrity_algorithm;
  size = do_NR_SecurityModeCommand(ctxt_pP, buffer, rrc_gNB_get_next_transaction_identifier(ctxt_pP->module_id), ue_p->ciphering_algorithm, integrity_algorithm);
  LOG_DUMPMSG(NR_RRC,DEBUG_RRC,(char *)buffer,size,"[MSG] RRC Security Mode Command\n");

  if (ue_p->is_remote_ue) {
    LOG_D(NR_RRC, "[REMOTE_UE_MSG] TX SecurityModeCommand for Remote UE ID=%d via Relay RNTI=0x%04x (bytes %d)\n",
          ue_p->remote_ue_id, ue_p->relay_ue_rnti, size);
  }
  LOG_I(NR_RRC, "UE %04x Logical Channel DL-DCCH, Generate SecurityModeCommand (bytes %d)\n", ue_p->rnti, size);

  gNB_RRC_INST *rrc = RC.nrrrc[ctxt_pP->module_id];
  AssertFatal(!NODE_IS_DU(rrc->node_type), "illegal node type DU!\n");

  /* the callback will fill the UE context setup request and forward it */
  nr_pdcp_data_req_srb(ctxt_pP->rntiMaybeUEid, DCCH, rrc_gNB_mui++, size, buffer, rrc_deliver_ue_ctxt_setup_req, rrc, UU);
}

void
rrc_gNB_generate_UECapabilityEnquiry(
  const protocol_ctxt_t *const ctxt_pP,
  rrc_gNB_ue_context_t          *const ue_context_pP
)
//-----------------------------------------------------------------------------
{
  uint8_t                             buffer[100];
  uint8_t                             size;

  T(T_ENB_RRC_UE_CAPABILITY_ENQUIRY, T_INT(ctxt_pP->module_id), T_INT(ctxt_pP->frame), T_INT(ctxt_pP->subframe), T_INT(ctxt_pP->rntiMaybeUEid));
  size = do_NR_SA_UECapabilityEnquiry(
           ctxt_pP,
           buffer,
           rrc_gNB_get_next_transaction_identifier(ctxt_pP->module_id));
  LOG_I(NR_RRC,
        PROTOCOL_NR_RRC_CTXT_UE_FMT" Logical Channel DL-DCCH, Generate NR UECapabilityEnquiry (bytes %d)\n",
        PROTOCOL_NR_RRC_CTXT_UE_ARGS(ctxt_pP),
        size);

  gNB_RRC_INST *rrc = RC.nrrrc[ctxt_pP->module_id];
  AssertFatal(!NODE_IS_DU(rrc->node_type), "illegal node type DU!\n");

  nr_pdcp_data_req_srb(ctxt_pP->rntiMaybeUEid, DCCH, rrc_gNB_mui++, size, buffer, deliver_pdu_srb_f1, rrc, UU);
}

static void rrc_deliver_ue_ctxt_release_cmd(void *deliver_pdu_data, ue_id_t ue_id, int srb_id, char *buf, int size, int sdu_id, nr_intf_type_t intf_type)
{
  DevAssert(deliver_pdu_data != NULL);
  gNB_RRC_INST *rrc = deliver_pdu_data;
  uint8_t *rrc_container = malloc(size);
  AssertFatal(rrc_container != NULL, "out of memory\n");
  memcpy(rrc_container, buf, size);
  f1ap_ue_context_release_cmd_t ue_context_release_cmd = {
    .rnti = ue_id, /* TODO: proper IDs! */
    .cause = F1AP_CAUSE_RADIO_NETWORK,
    .cause_value = 10, // 10 = F1AP_CauseRadioNetwork_normal_release
    .srb_id = srb_id,
    .rrc_container = rrc_container,
    .rrc_container_length = size,
  };
  rrc->mac_rrc.ue_context_release_command(&ue_context_release_cmd);
}

//-----------------------------------------------------------------------------
/*
* Generate the RRC Connection Release to UE.
* If received, UE should switch to RRC_IDLE mode.
*/
void
rrc_gNB_generate_RRCRelease(
  const protocol_ctxt_t *const ctxt_pP,
  rrc_gNB_ue_context_t  *const ue_context_pP
)
//-----------------------------------------------------------------------------
{
  uint8_t buffer[RRC_BUF_SIZE] = {0};
  int size = do_NR_RRCRelease(buffer, RRC_BUF_SIZE, rrc_gNB_get_next_transaction_identifier(ctxt_pP->module_id));

  LOG_I(NR_RRC,
        PROTOCOL_NR_RRC_CTXT_UE_FMT" Logical Channel DL-DCCH, Generate RRCRelease (bytes %d)\n",
        PROTOCOL_NR_RRC_CTXT_UE_ARGS(ctxt_pP),
        size);

  gNB_RRC_INST *rrc = RC.nrrrc[ctxt_pP->module_id];
  nr_pdcp_data_req_srb(ctxt_pP->rntiMaybeUEid, DCCH, rrc_gNB_mui++, size, buffer, rrc_deliver_ue_ctxt_release_cmd, rrc, UU);

  /* UE will be freed after UE context release complete */
}

int rrc_gNB_generate_pcch_msg(uint32_t tmsi, uint8_t paging_drx, instance_t instance, uint8_t CC_id){
  const unsigned int Ttab[4] = {32,64,128,256};
  uint8_t Tc;
  uint8_t Tue;
  uint32_t pfoffset;
  uint32_t N;  /* N: min(T,nB). total count of PF in one DRX cycle */
  uint32_t Ns = 0;  /* Ns: max(1,nB/T) */
  uint8_t i_s;  /* i_s = floor(UE_ID/N) mod Ns */
  uint32_t T;  /* DRX cycle */
  uint32_t length;
  uint8_t buffer[RRC_BUF_SIZE];
  struct NR_SIB1 *sib1 = RC.nrrrc[instance]->carrier.siblock1->message.choice.c1->choice.systemInformationBlockType1;

  /* get default DRX cycle from configuration */
  Tc = sib1->servingCellConfigCommon->downlinkConfigCommon.pcch_Config.defaultPagingCycle;

  Tue = paging_drx;
  /* set T = min(Tc,Tue) */
  T = Tc < Tue ? Ttab[Tc] : Ttab[Tue];
  /* set N = PCCH-Config->nAndPagingFrameOffset */
  switch (sib1->servingCellConfigCommon->downlinkConfigCommon.pcch_Config.nAndPagingFrameOffset.present) {
    case NR_PCCH_Config__nAndPagingFrameOffset_PR_oneT:
      N = T;
      pfoffset = 0;
      break;
    case NR_PCCH_Config__nAndPagingFrameOffset_PR_halfT:
      N = T/2;
      pfoffset = 1;
      break;
    case NR_PCCH_Config__nAndPagingFrameOffset_PR_quarterT:
      N = T/4;
      pfoffset = 3;
      break;
    case NR_PCCH_Config__nAndPagingFrameOffset_PR_oneEighthT:
      N = T/8;
      pfoffset = 7;
      break;
    case NR_PCCH_Config__nAndPagingFrameOffset_PR_oneSixteenthT:
      N = T/16;
      pfoffset = 15;
      break;
    default:
      LOG_E(RRC, "[gNB %ld] In rrc_gNB_generate_pcch_msg:  pfoffset error (pfoffset %d)\n",
            instance, sib1->servingCellConfigCommon->downlinkConfigCommon.pcch_Config.nAndPagingFrameOffset.present);
      return (-1);

  }

  switch (sib1->servingCellConfigCommon->downlinkConfigCommon.pcch_Config.ns) {
    case NR_PCCH_Config__ns_four:
      if(*sib1->servingCellConfigCommon->downlinkConfigCommon.initialDownlinkBWP.pdcch_ConfigCommon->choice.setup->pagingSearchSpace == 0){
        LOG_E(RRC, "[gNB %ld] In rrc_gNB_generate_pcch_msg:  ns error only 1 or 2 is allowed when pagingSearchSpace is 0\n",
              instance);
        return (-1);
      } else {
        Ns = 4;
      }
      break;
    case NR_PCCH_Config__ns_two:
      Ns = 2;
      break;
    case NR_PCCH_Config__ns_one:
      Ns = 1;
      break;
    default:
      LOG_E(RRC, "[gNB %ld] In rrc_gNB_generate_pcch_msg: ns error (ns %ld)\n",
            instance, sib1->servingCellConfigCommon->downlinkConfigCommon.pcch_Config.ns);
      return (-1);
  }

  /* insert data to UE_PF_PO or update data in UE_PF_PO */
  pthread_mutex_lock(&ue_pf_po_mutex);
  uint8_t i = 0;

  for (i = 0; i < MAX_MOBILES_PER_ENB; i++) {
    if ((UE_PF_PO[CC_id][i].enable_flag == true && UE_PF_PO[CC_id][i].ue_index_value == (uint16_t)(tmsi%1024))
        || (UE_PF_PO[CC_id][i].enable_flag != true)) {
      /* set T = min(Tc,Tue) */
      UE_PF_PO[CC_id][i].T = T;
      /* set UE_ID */
      UE_PF_PO[CC_id][i].ue_index_value = (uint16_t)(tmsi%1024);
      /* calculate PF and PO */
      /* set PF_min and PF_offset: (SFN + PF_offset) mod T = (T div N)*(UE_ID mod N) */
      UE_PF_PO[CC_id][i].PF_min = (T / N) * (UE_PF_PO[CC_id][i].ue_index_value % N);
      UE_PF_PO[CC_id][i].PF_offset = pfoffset;
      /* set i_s */
      /* i_s = floor(UE_ID/N) mod Ns */
      i_s = (uint8_t)((UE_PF_PO[CC_id][i].ue_index_value / N) % Ns);
      UE_PF_PO[CC_id][i].i_s = i_s;

      // TODO,set PO

      if (UE_PF_PO[CC_id][i].enable_flag == true) {
        //paging exist UE log
        LOG_D(NR_RRC,"[gNB %ld] CC_id %d In rrc_gNB_generate_pcch_msg: Update exist UE %d, T %d, N %d, PF %d, i_s %d, PF_offset %d\n", instance, CC_id, UE_PF_PO[CC_id][i].ue_index_value,
              T, N, UE_PF_PO[CC_id][i].PF_min, UE_PF_PO[CC_id][i].i_s, UE_PF_PO[CC_id][i].PF_offset);
      } else {
        /* set enable_flag */
        UE_PF_PO[CC_id][i].enable_flag = true;
        //paging new UE log
        LOG_D(NR_RRC,"[gNB %ld] CC_id %d In rrc_gNB_generate_pcch_msg: Insert a new UE %d, T %d, N %d, PF %d, i_s %d, PF_offset %d\n", instance, CC_id, UE_PF_PO[CC_id][i].ue_index_value,
              T, N, UE_PF_PO[CC_id][i].PF_min, UE_PF_PO[CC_id][i].i_s, UE_PF_PO[CC_id][i].PF_offset);
      }
      break;
    }
  }

  pthread_mutex_unlock(&ue_pf_po_mutex);

  /* Create message for PDCP (DLInformationTransfer_t) */
  length = do_NR_Paging (instance,
                         buffer,
                         tmsi);

  if (length == -1) {
    LOG_I(NR_RRC, "do_Paging error\n");
    return -1;
  }
  // TODO, send message to pdcp

  return 0;
}
