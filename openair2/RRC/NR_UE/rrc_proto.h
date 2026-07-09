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

/* \file proto.h
 * \brief RRC functions prototypes for eNB and UE
 * \author R. Knopp, K.H. HSU
 * \date 2018
 * \version 0.1
 * \company Eurecom / NTUST
 * \email: knopp@eurecom.fr, kai-hsiang.hsu@eurecom.fr
 * \note
 * \warning
 */

#ifndef _RRC_PROTO_H_
#define _RRC_PROTO_H_


#include "rrc_defs.h"
#include "NR_RRCReconfiguration.h"
#include "NR_MeasConfig.h"
#include "NR_CellGroupConfig.h"
#include "NR_RadioBearerConfig.h"
#include "openair2/PHY_INTERFACE/queue_t.h"
#include "common/utils/ocp_itti/intertask_interface.h"
#include "executables/nr-uesoftmodem.h"

extern queue_t nr_rach_ind_queue;
extern queue_t nr_rx_ind_queue;
extern queue_t nr_crc_ind_queue;
extern queue_t nr_uci_ind_queue;
extern queue_t nr_sfn_slot_queue;
extern queue_t nr_chan_param_queue;
extern queue_t nr_dl_tti_req_queue;
extern queue_t nr_tx_req_queue;
extern queue_t nr_ul_dci_req_queue;
extern queue_t nr_ul_tti_req_queue;
//
//  main_rrc.c
//
/**\brief Layer 3 initialization*/
NR_UE_RRC_INST_t* nr_l3_init_ue(char*,char*);

//
//  UE_rrc.c
//

/**\brief Initial the top level RRC structure instance*/
NR_UE_RRC_INST_t* openair_rrc_top_init_ue_nr(char*,char*);



/**\brief Decode RRC Connection Reconfiguration, sent from E-UTRA RRC Connection Reconfiguration v1510 carring EN-DC config
   \param buffer  encoded NR-RRC-Connection-Reconfiguration/Secondary-Cell-Group-Config message.
   \param size    length of buffer*/
//TODO check to use which one
//int8_t nr_rrc_ue_decode_rrcReconfiguration(const uint8_t *buffer, const uint32_t size);
int8_t nr_rrc_ue_decode_secondary_cellgroup_config(const module_id_t module_id, const uint8_t *buffer, const uint32_t size);
   

/**\brief Process NR RRC connection reconfiguration via SRB3
   \param rrcReconfiguration  decoded rrc connection reconfiguration*/
int8_t nr_rrc_ue_process_rrcReconfiguration(const module_id_t module_id, NR_RRCReconfiguration_t *rrcReconfiguration);

/**\prief Process measurement config from NR RRC connection reconfiguration message
   \param meas_config   measurement configuration*/
int8_t nr_rrc_ue_process_meas_config(NR_MeasConfig_t *meas_config);

/**\prief Process radio bearer config from NR RRC connection reconfiguration message
   \param radio_bearer_config    radio bearer configuration*/
int8_t nr_rrc_ue_process_radio_bearer_config(NR_RadioBearerConfig_t *radio_bearer_config);

/**\brief decode NR BCCH-BCH (MIB) message
   \param module_idP    module id
   \param gNB_index     gNB index
   \param sduP          pointer to buffer of ASN message BCCH-BCH
   \param sdu_len       length of buffer*/
int8_t nr_rrc_ue_decode_NR_BCCH_BCH_Message(const module_id_t module_id, const uint8_t gNB_index, uint8_t *const bufferP, const uint8_t buffer_len);

int8_t nr_rrc_ue_decode_NR_DL_DCCH_Message(const module_id_t module_id, const uint8_t gNB_index, const uint8_t *buffer, const uint32_t size);

/**\brief interface between MAC and RRC thru SRB0 (RLC TM/no PDCP)
   \param module_id  module id
   \param CC_id      component carrier id
   \param gNB_index  gNB index
   \param channel    indicator for channel of the pdu
   \param pduP       pointer to pdu
   \param pdu_len    data length of pdu*/
int8_t nr_mac_rrc_data_ind_ue(const module_id_t module_id,
                              const int CC_id,
                              const uint8_t gNB_index,
                              const frame_t frame,
                              const int slot,
                              const rnti_t rnti,
                              const channel_t channel,
                              const uint8_t* pduP,
                              const sdu_size_t pdu_len);

void nr_mac_rrc_sync_ind(const module_id_t module_id,
                         const frame_t frame,
                         const bool in_sync);
void nr_mac_rrc_ra_ind(const module_id_t mod_id, int frame, bool success);

/**\brief
   \param module_id  module id
   \param CC_id      component carrier id
   \param gNB_index  gNB index
   \param frame_t    frameP
   \param rb_id_t    SRB id
   \param buffer_pP  pointer to buffer*/
int8_t nr_mac_rrc_data_req_ue(const module_id_t Mod_idP,
                              const int         CC_id,
                              const uint8_t     gNB_id,
                              const frame_t     frameP,
                              const rb_id_t     Srb_id,
                              uint8_t           *buffer_pP);

int8_t nr_rrc_RA_succeeded(const module_id_t mod_id, const uint8_t gNB_index);

/**\brief RRC UE task.
   \param void *args_p Pointer on arguments to start the task. */
void *rrc_nrue_task(void *args_p);

void nr_rrc_handle_timers(NR_UE_Timers_Constants_t *timers);

/**\brief RRC NSA UE task.
   \param void *args_p Pointer on arguments to start the task. */
void *recv_msgs_from_lte_ue(void *args_p);

void init_connections_with_lte_ue(void);

void nsa_sendmsg_to_lte_ue(const void *message, size_t msg_len, Rrc_Msg_Type_t msg_type);

void start_oai_nrue_threads(void);

/**\brief RRC UE generate RRCSetupRequest message.
   \param module_id  module id
   \param gNB_index  gNB index  */
void nr_rrc_ue_generate_RRCSetupRequest(module_id_t module_id, const uint8_t gNB_index);

void process_lte_nsa_msg(nsa_msg_t *msg, int msg_len);

int get_from_lte_ue_fd();

void nr_rrc_SI_timers(NR_UE_RRC_SI_INFO *SInfo);

void nr_ue_rrc_timer_trigger(int module_id, int frame, int slot, int gnb_id);

void configure_spcell(NR_UE_RRC_INST_t *rrc, NR_SpCellConfig_t *spcell_config);
void reset_rlf_timers_and_constants(NR_UE_Timers_Constants_t *tac);
void set_default_timers_and_constants(NR_UE_Timers_Constants_t *tac);
void nr_rrc_set_sib1_timers_and_constants(NR_UE_Timers_Constants_t *tac, NR_SIB1_t *sib1);
void nr_rrc_set_T304(NR_UE_Timers_Constants_t *tac, NR_ReconfigurationWithSync_t *reconfigurationWithSync);
void handle_rlf_sync(NR_UE_Timers_Constants_t *tac,
                     nr_sync_msg_t sync_msg);
void nr_rrc_handle_SetupRelease_RLF_TimersAndConstants(NR_UE_RRC_INST_t *rrc,
                                                       struct NR_SetupRelease_RLF_TimersAndConstants *rlf_TimersAndConstants);

int configure_NR_SL_Preconfig(uint8_t id,int sync_source);
void nr_UE_configure_Sidelink(uint8_t id, uint8_t is_sync_source, ueinfo_t *ueinfo);

void nr_UE_configure_Sidelink_Dedicated_Cfg(uint8_t id, uint8_t is_sync_source, int src_id, uint8_t mu);

int get_NAS_status();

void extract_nr_sl_ResourcePool(struct NR_SL_ResourcePool_r16 *sl_ResourcePool,
                                struct NR_SL_ResourcePool_r16 *recvd_sl_ResourcePool,
                                bool relay_to_remote_ue);

void extract_nr_sl_Rest_ResourcePool_Config(struct NR_SL_ResourcePool_r16 *sl_RxPool_r16,
                                            struct NR_SL_ResourcePool_r16 *recvd_sl_RxPool,
                                            bool relay_to_remote_ue);

void extract_nr_sl_rlc_config(NR_SL_RLC_Config_r16_t *sl_RLC_Config,
                              NR_SL_RLC_Config_r16_t *rcvd_sl_RLC_Config_r16);

void extract_nr_sl_mac_logical_channel_config(struct NR_SL_LogicalChannelConfig_r16 *sl_MAC_LogicalChannelConfig_r16,
                                              struct NR_SL_LogicalChannelConfig_r16 *rcvd_sl_MAC_LogicalChannelConfig_r16);

void extract_nr_sl_scs_specific_carrier(NR_SL_FreqConfig_r16_t *sl_FreqInfoToAddMod,
                                        NR_SL_FreqConfig_r16_t *recvd_sl_FreqInfoToAddMod);

void extract_nr_sl_SyncConfig(NR_SL_SyncConfig_r16_t *sl_syncconfig,
                              NR_SL_SyncConfig_r16_t *rcvd_sl_syncconfig);

void extract_nr_sl_rlc_bearer_config(NR_SL_RLC_BearerConfig_r16_t *sl_RLC_BearerConfig,
                                     NR_SL_RLC_BearerConfig_r16_t *rcvd_sl_RLC_BearerConfig);

void extract_nr_sl_scheduled_config(NR_SetupRelease_SL_ScheduledConfig_r16_t *sl_ScheduledConfig,
                                    NR_SetupRelease_SL_ScheduledConfig_r16_t *recv_sl_ScheduledConfig,
                                    NR_RNTI_Value_t sl_rnti);

void extract_nr_sl_FreqInfoToAddMod(NR_SL_FreqConfig_r16_t *sl_FreqInfoToAddMod,
                                    NR_SL_FreqConfig_r16_t *recvd_sl_FreqInfoToAddMod);

void extract_nr_sl_bwp_generic(NR_SL_BWP_Config_r16_t *sl_BWP_ToAddMod,
                               NR_SL_BWP_Config_r16_t *recvd_sl_BWP_ToAddMod);

void nr_rrc_ue_process_sl_ConfigDedicatedNR(const protocol_ctxt_t *const ctxt_pP,
                                            const uint8_t gNB_index,
                                            NR_SetupRelease_SL_ConfigDedicatedNR_r16_t *sl_conf);

void extract_nr_sl_PSCCH_Config(NR_SL_PSCCH_Config_r16_t *recvd_sl_PSCCH_Config,
                                NR_SL_PSCCH_Config_r16_t *targeted_sl_PSCCH_Config);

void extract_nr_sl_PSSCH_Config(NR_SL_PSSCH_Config_r16_t *recvd_sl_PSSCH_Config,
                                NR_SL_PSSCH_Config_r16_t *targeted_sl_PSSCH_Config);

void extract_nr_sl_PSFCH_Config(NR_SL_PSFCH_Config_r16_t *recvd_sl_PSFCH_Config,
                                NR_SL_PSFCH_Config_r16_t *targeted_sl_PSFCH_Config);

NR_SL_RLC_BearerConfig_r16_t *get_SRB_RLC_BearerConfig_sl(long priority,
                                                          e_NR_SL_LogicalChannelConfig_r16__sl_BucketSizeDuration_r16 bucketSizeDuration,
                                                          uint8_t srb_id);

int32_t nr_rrc_ue_establish_srb1(module_id_t ue_mod_idP,
                                 frame_t frameP,
                                 uint8_t remote_ue_index,
                                 NR_SRB_ToAddMod_t *SRB_config);

int32_t nr_rrc_ue_establish_sl_srb1(module_id_t ue_mod_idP,
                                    frame_t frameP,
                                    uint8_t remote_ue_index);

void nr_rrc_ue_process_RadioBearerConfig_sl(const protocol_ctxt_t *const ctxt_pP,
                                            const uint8_t ue_index,
                                            NR_RadioBearerConfig_t *const radioBearerConfig,
                                            NR_SL_RLC_BearerConfig_r16_t *nr_rlc_BearerConfig);
/** @}*/
#endif

