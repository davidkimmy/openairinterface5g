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

/* from openair */
#include "rlc.h"
#include "LAYER2/nr_pdcp/nr_pdcp_oai_api.h"
#include "LAYER2/nr_srap/nr_srap_oai_api.h"
#include "LAYER2/nr_srap/nr_srap_manager.h"
#include "LAYER2/nr_srap/nr_srap_entity.h"
#include "LAYER2/nr_srap/nr_srap_header.h"  // For SRAP U2N header field masks

/* from nr rlc module */
#include "nr_rlc_asn1_utils.h"
#include "nr_rlc_ue_manager.h"
#include "nr_rlc_entity.h"
#include "nr_rlc_oai_api.h"
#include "NR_RLC-BearerConfig.h"
#include "NR_DRB-ToAddMod.h"
#include "NR_DRB-ToAddModList.h"
#include "NR_SRB-ToAddModList.h"
#include "NR_DRB-ToReleaseList.h"
#include "NR_CellGroupConfig.h"
#include "NR_RLC-Config.h"
#include "common/ran_context.h"
#include "NR_UL-CCCH-Message.h"

#include "openair2/F1AP/f1ap_du_rrc_message_transfer.h"
#include "common/utils/nr/nr_common.h"

extern RAN_CONTEXT_t RC;

#include <stdint.h>

#include <executables/softmodem-common.h>

static nr_rlc_ue_manager_t *nr_rlc_ue_manager;

/* TODO: handle time a bit more properly */
static uint64_t nr_rlc_current_time;
static int      nr_rlc_current_time_last_frame;
static int      nr_rlc_current_time_last_subframe;

int nr_rlc_packet_counter = 0;
uint64_t nr_rlc_last_check_time = 0;

#ifdef IP_TRAFFIC_MONITORING
static uint64_t get_current_time_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);  // Monotonic time since boot
  return (uint64_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

void nr_ue_SL_UEAsistance_trigger(int module_id, int frame, int ch_id, bool is_pc5, int tp_type)
{
  MessageDef *message_p;
  message_p = itti_alloc_new_message(TASK_RRC_NRUE, 0, RLC_TRAFFIC_PTN_CHG_IND);
  RLC_TRAFFIC_PTN_CHG_IND(message_p).frame = frame;
  RLC_TRAFFIC_PTN_CHG_IND(message_p).ch_id = ch_id;
  RLC_TRAFFIC_PTN_CHG_IND(message_p).is_pc5 = is_pc5;
  RLC_TRAFFIC_PTN_CHG_IND(message_p).tp_type = tp_type;
  LOG_D(RLC, "RRC SL_UEAsistance trigger: frame %d traffic pattern type %d \n", frame, tp_type);
  itti_send_msg_to_task(TASK_RRC_NRUE, GNB_MODULE_ID_TO_INSTANCE(module_id), message_p);
}

static void monitor_traffic_pattern(const module_id_t         module_idP,
                                    const frame_t             frameP,
                                    nr_rlc_entity_t           *rb,
                                    const logical_chan_id_t   channel_idP,
                                    bool                      is_pc5_link)
{
  static uint32_t traffic_pattern_type;
  uint64_t now = get_current_time_ms();
  nr_rlc_packet_counter++;
  bool status = false;
  if (now - nr_rlc_last_check_time >= 1000) {  // 1000 ms window
    int old_pattern_type;
    if (nr_rlc_packet_counter < 10) {
      if (traffic_pattern_type != 0) {
        old_pattern_type = traffic_pattern_type;
        traffic_pattern_type = 0;
        status = true;
      }
    }
    else if (nr_rlc_packet_counter < 50) {
      if (traffic_pattern_type != 1) {
        old_pattern_type = traffic_pattern_type;
        traffic_pattern_type = 1;
        status = true;
      }
    }
    else if (nr_rlc_packet_counter < 100) {
      if (traffic_pattern_type != 2) {
        old_pattern_type = traffic_pattern_type;
        traffic_pattern_type = 2;
        status = true;
      }
    }
    else {
      if (traffic_pattern_type != 3) {
        old_pattern_type = traffic_pattern_type;
        traffic_pattern_type = 3;
        status = true;
      }
    }
    if (status) {
      int md = rb->stats.mode;
      LOG_W(RLC, "Traffic pattern has been changed from %d to %d\n", old_pattern_type, traffic_pattern_type);
      LOG_D(RLC, "is_pc5_link %d channel_idP %u rxpdu_pkts %u mode %s Packet count in last 1000ms: %d\n",
            is_pc5_link, channel_idP, rb->stats.rxpdu_pkts,
            md == 0 ? "AM" : (md == 1 ? "UM" : "TM"), nr_rlc_packet_counter);
      nr_ue_SL_UEAsistance_trigger(module_idP, frameP, channel_idP, is_pc5_link, traffic_pattern_type);
    }
    // Reset for next window
    nr_rlc_packet_counter = 0;
    nr_rlc_last_check_time = now;
  }
}
#endif

void mac_rlc_data_ind (const module_id_t         module_idP,
                       const rnti_t              rntiP,
                       const eNB_index_t         eNB_index,
                       const frame_t             frameP,
                       const eNB_flag_t          enb_flagP,
                       const MBMS_flag_t         MBMS_flagP,
                       const logical_chan_id_t   channel_idP,
                       char                     *buffer_pP,
                       const tb_size_t           tb_sizeP,
                       num_tb_t                  num_tbP,
                       crc_t                    *crcs_pP,
                       uint16_t                  sourceL2Id,
                       uint8_t                   destinationL2Id)
{
  nr_rlc_ue_t *ue;
  nr_rlc_entity_t *rb;
  /* In Sidelink (Mode 1 and Mode 2), we have source and destination IDs defined.
    In SA Mode, these are undefined and therefore both are zeros.
  */
  bool is_pc5_link = sourceL2Id != 0 || destinationL2Id != 0;

  if (module_idP != 0 || eNB_index != 0 || /*enb_flagP != 1 ||*/ MBMS_flagP != 0) {
    LOG_E(RLC, "%s:%d:%s: fatal\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }

  if (enb_flagP)
    T(T_ENB_RLC_MAC_UL, T_INT(module_idP), T_INT(rntiP),
      T_INT(channel_idP), T_INT(tb_sizeP));

  nr_rlc_manager_lock(nr_rlc_ue_manager);
  ue = nr_rlc_manager_get_ue(nr_rlc_ue_manager, rntiP);

  if(ue == NULL) {
	  LOG_E(RLC, "[RLC_RX] RNTI 0x%04x: RLC instance for the given UE was not found\n", rntiP);
	  nr_rlc_manager_unlock(nr_rlc_ue_manager);
	  return;
  }

  switch (channel_idP) {
  case 0:
    if (is_pc5_link)
      rb = ue->sl_srb0;
    else
      rb = ue->srb0;
    break;
  case 1 ... 3:
    if (is_pc5_link)
      rb = ue->sl_srb[channel_idP - 1];
    else
      rb = ue->srb[channel_idP - 1];
    break;
  case 4 ... 32:
    if (is_pc5_link)
      rb = ue->sl_drb[channel_idP - 4];
    else
      rb = ue->drb[channel_idP - 4];
    break;
  default:       rb = NULL;                     break;
  }

  if (rb != NULL) {
    rb->set_time(rb, nr_rlc_current_time);
    rb->recv_pdu(rb, buffer_pP, tb_sizeP);
#ifdef IP_TRAFFIC_MONITORING
    if(channel_idP >= 3 && is_pc5_link == 1 && enb_flagP == 0) {
      monitor_traffic_pattern(module_idP, frameP, rb, channel_idP, is_pc5_link);
    }
#endif
  } else {
    LOG_E(RLC, "[RLC_RX] RNTI 0x%04x channel_id=%d is_pc5=%d enb_flag=%d: FATAL no RB found (sourceL2Id=0x%04x, destL2Id=0x%02x)\n",
          rntiP, channel_idP, is_pc5_link, enb_flagP, sourceL2Id, destinationL2Id);
    // exit(1);
  }

  nr_rlc_manager_unlock(nr_rlc_ue_manager);
}

tbs_size_t mac_rlc_data_req(
  const module_id_t       module_idP,
  const rnti_t            rntiP,
  const eNB_index_t       eNB_index,
  const frame_t           frameP,
  const eNB_flag_t        enb_flagP,
  const MBMS_flag_t       MBMS_flagP,
  const logical_chan_id_t channel_idP,
  const tb_size_t         tb_sizeP,
  char             *buffer_pP,
  const uint32_t sourceL2Id,
  const uint32_t destinationL2Id
   )
{
  int ret;
  nr_rlc_ue_t *ue;
  nr_rlc_entity_t *rb;
  int maxsize;
  /* In Sidelink (Mode 1 and Mode 2), we have source and destination IDs defined.
    In SA Mode, these are undefined and therefore both are zeros.
  */
  bool is_pc5_link = sourceL2Id != 0 || destinationL2Id != 0;

  nr_rlc_manager_lock(nr_rlc_ue_manager);
  // For PC5 sidelink, use sourceL2Id for UE lookup; otherwise use RNTI
  rnti_t lookup_key = is_pc5_link ? sourceL2Id : rntiP;
  ue = nr_rlc_manager_get_ue(nr_rlc_ue_manager, lookup_key);

  if(ue == NULL) {
    LOG_E(RLC, "[%s] RLC UE not found for lookup_key=0x%04x (is_pc5=%d, rntiP=0x%04x, srcL2Id=0x%04x, channel_idP=%d)\n",
          __FUNCTION__, lookup_key, is_pc5_link, rntiP, sourceL2Id, channel_idP);
    nr_rlc_manager_unlock(nr_rlc_ue_manager);
    return 0;
  }

  switch (channel_idP) {
  case 0:
    if (is_pc5_link)
      rb = ue->sl_srb0;
    else
      rb = ue->srb0;
    break;
  case 1 ... 3:
    if (is_pc5_link)
      rb = ue->sl_srb[channel_idP - 1];
    else
      rb = ue->srb[channel_idP - 1];
    break;
  case 4 ... 32:
    if (is_pc5_link)
      rb = ue->sl_drb[channel_idP - 4];
    else
      rb = ue->drb[channel_idP - 4];
    break;
  default:
  rb = NULL;
  LOG_E(RLC, "In %s:%d:%s: data request for unknown RB with LCID 0x%02x !\n", __FILE__, __LINE__, __FUNCTION__, channel_idP);
  break;
  }

  if (rb != NULL) {
    LOG_D(RLC, "MAC PDU to get created for channel_idP:%d \n", channel_idP);
    rb->set_time(rb, nr_rlc_current_time);
    maxsize = tb_sizeP;
    ret = rb->generate_pdu(rb, buffer_pP, maxsize);
  } else {
    LOG_D(RLC, "MAC PDU failed to get created for channel_idP:%d \n", channel_idP);
    ret = 0;
  }

  nr_rlc_manager_unlock(nr_rlc_ue_manager);

  if (enb_flagP)
    T(T_ENB_RLC_MAC_DL, T_INT(module_idP), T_INT(rntiP),
      T_INT(channel_idP), T_INT(ret));

  return ret;
}

/* Shared implementation for the RLC buffer-status query. The caller passes the UE
   key in rntiP (the RNTI for Uu/SA, the sidelink source L2 ID for PC5) and states
   the link type via is_pc5_link, so this function never has to guess the link type
   from the key value. */
static mac_rlc_status_resp_t nr_rlc_status_ind_impl(
  const rnti_t            rntiP,
  const frame_t           frameP,
  const logical_chan_id_t channel_idP,
  const bool              is_pc5_link
  )
{
  nr_rlc_ue_t *ue;
  mac_rlc_status_resp_t ret;
  nr_rlc_entity_t *rb;

  nr_rlc_manager_lock(nr_rlc_ue_manager);
  ue = nr_rlc_manager_get_ue(nr_rlc_ue_manager, rntiP);

  if(ue == NULL) {
    LOG_W(RLC, "[%s] RLC UE not found for rntiP=0x%04x (is_pc5=%d, channel_idP=%d)\n",
          __FUNCTION__, rntiP, is_pc5_link, channel_idP);
    ret.bytes_in_buffer = 0;
    ret.pdus_in_buffer = 0;
    ret.head_sdu_creation_time = 0;
    ret.head_sdu_remaining_size_to_send = 0;
    ret.head_sdu_is_segmented = 0;
    nr_rlc_manager_unlock(nr_rlc_ue_manager);
    return ret;
  }

  switch (channel_idP) {
  case 0:
    if (is_pc5_link)
      rb = ue->sl_srb0;
    else
      rb = ue->srb0;
    break;
  case 1 ... 3:
    if (is_pc5_link)
      rb = ue->sl_srb[channel_idP - 1];
    else
      rb = ue->srb[channel_idP - 1];
    break;
  case 4 ... MAX_DRBS_PER_UE:
    if (is_pc5_link)
      rb = ue->sl_drb[channel_idP - 4];
    else
      rb = ue->drb[channel_idP - 4];
    break;
  default:                         rb = NULL;                     break;
  }

  if (rb != NULL) {
    nr_rlc_entity_buffer_status_t buf_stat;
    rb->set_time(rb, nr_rlc_current_time);
    /* 38.321 deals with BSR values up to 81338368 bytes, after what it
     * reports '> 81338368' (table 6.1.3.1-2). Passing 100000000 is thus
     * more than enough.
     */
    // Fix me: temproary reduction meanwhile cpu cost of this computation is optimized
    buf_stat = rb->buffer_status(rb, 1000*1000);
    ret.bytes_in_buffer = buf_stat.status_size
                        + buf_stat.retx_size
                        + buf_stat.tx_size;
  } else {
    if (!(frameP%128) || channel_idP == 0) //to suppress this warning message
      LOG_W(RLC, "[%s] Radio Bearer (channel ID %d) is NULL for UE with rntiP %x\n", __FUNCTION__, channel_idP, rntiP);
    ret.bytes_in_buffer = 0;
  }

  nr_rlc_manager_unlock(nr_rlc_ue_manager);

  ret.pdus_in_buffer = 0;
  /* TODO: creation time may be important (unit: frame, as it seems) */
  ret.head_sdu_creation_time = 0;
  ret.head_sdu_remaining_size_to_send = 0;
  ret.head_sdu_is_segmented = 0;
  return ret;
}

mac_rlc_status_resp_t mac_rlc_status_ind(
  const module_id_t       module_idP,
  const rnti_t            rntiP,
  const eNB_index_t       eNB_index,
  const frame_t           frameP,
  const sub_frame_t       subframeP,
  const eNB_flag_t        enb_flagP,
  const MBMS_flag_t       MBMS_flagP,
  const logical_chan_id_t channel_idP,
  const uint32_t sourceL2Id,
  const uint32_t destinationL2Id
  )
{
  /* Uu/SA path: the UE is keyed by its RNTI. PC5 sidelink queries use
     mac_rlc_status_ind_sl instead. */
  return nr_rlc_status_ind_impl(rntiP, frameP, channel_idP, false);
}

/* Sidelink (PC5) buffer-status query used by the NR SL scheduler. The link type
   is PC5 by construction, so the RLC layer never infers it from the RNTI value.
   The UE is looked up by srcL2Id, the sidelink source L2 ID (0 for a Relay UE). */
mac_rlc_status_resp_t mac_rlc_status_ind_sl(
  const module_id_t       module_idP,
  const uint32_t          srcL2Id,
  const frame_t           frameP,
  const sub_frame_t       subframeP,
  const logical_chan_id_t channel_idP
  )
{
  return nr_rlc_status_ind_impl(srcL2Id, frameP, channel_idP, true);
}

rlc_buffer_occupancy_t mac_rlc_get_buffer_occupancy_ind(
  const module_id_t       module_idP,
  const rnti_t            rntiP,
  const eNB_index_t       eNB_index,
  const frame_t           frameP,
  const sub_frame_t       subframeP,
  const eNB_flag_t        enb_flagP,
  const logical_chan_id_t channel_idP)
{
  nr_rlc_ue_t *ue;
  rlc_buffer_occupancy_t ret;
  nr_rlc_entity_t *rb;

  if (enb_flagP) {
    LOG_E(RLC, "Tx mac_rlc_get_buffer_occupancy_ind function is not implemented for eNB LcId=%u\n", channel_idP);
    exit(1);
  }

  /* TODO: handle time a bit more properly */
  if (nr_rlc_current_time_last_frame != frameP ||
      nr_rlc_current_time_last_subframe != subframeP) {
    nr_rlc_current_time++;
    nr_rlc_current_time_last_frame = frameP;
    nr_rlc_current_time_last_subframe = subframeP;
  }

  nr_rlc_manager_lock(nr_rlc_ue_manager);
  ue = nr_rlc_manager_get_ue(nr_rlc_ue_manager, rntiP);

  switch (channel_idP) {
  case 1 ... 3:               rb = ue->srb[channel_idP - 1]; break;
  case 4 ... MAX_DRBS_PER_UE: rb = ue->drb[channel_idP - 4]; break;
  default:                    rb = NULL;                     break;
  }

  if (rb != NULL) {
    nr_rlc_entity_buffer_status_t buf_stat;
    rb->set_time(rb, nr_rlc_current_time);
    /* 38.321 deals with BSR values up to 81338368 bytes, after what it
     * reports '> 81338368' (table 6.1.3.1-2). Passing 100000000 is thus
     * more than enough.
     */
    // Fixme : Laurent reduced size for CPU saving
    // Fix me: temproary reduction meanwhile cpu cost of this computation is optimized
    buf_stat = rb->buffer_status(rb, 1000*1000);
    ret = buf_stat.status_size
        + buf_stat.retx_size
        + buf_stat.tx_size;
  } else {
    if (!(frameP%128)) //to suppress this warning message
      LOG_W(RLC, "[%s] Radio Bearer (channel ID %d) is NULL for UE with rntiP %x\n", __FUNCTION__, channel_idP, rntiP);
    ret = 0;
  }

  nr_rlc_manager_unlock(nr_rlc_ue_manager);

  return ret;
}


rlc_op_status_t rlc_data_req(const protocol_ctxt_t *const ctxt_pP,
                             const srb_flag_t srb_flagP,
                             const MBMS_flag_t MBMS_flagP,
                             const rb_id_t rb_idP,
                             const mui_t muiP,
                             confirm_t confirmP,
                             sdu_size_t sdu_sizeP,
                             mem_block_t *sdu_pP,
                             const uint32_t *const sourceL2Id,
                             const uint32_t *const destinationL2Id,
                             nr_intf_type_t intf_type)
{
  int rnti = ctxt_pP->rntiMaybeUEid;
  nr_rlc_ue_t *ue;
  nr_rlc_entity_t *rb;

  // For PC5 sidelink, use sourceL2Id instead of RNTI for UE lookup
  if (intf_type == PC5 && sourceL2Id != NULL) {
    rnti = (int)*sourceL2Id;
    LOG_D(RLC, "%s PC5: using sourceL2Id 0x%x as rnti for UE lookup\n", __FUNCTION__, rnti);
  }

  if (ctxt_pP->enb_flag)
    T(T_ENB_RLC_DL, T_INT(ctxt_pP->module_id), T_INT(ctxt_pP->rntiMaybeUEid), T_INT(rb_idP), T_INT(sdu_sizeP));

  nr_rlc_manager_lock(nr_rlc_ue_manager);
  ue = nr_rlc_manager_get_ue(nr_rlc_ue_manager, rnti);

  rb = NULL;

  if (srb_flagP) {
    if (rb_idP >= 0 && rb_idP <= 2) {
      if (intf_type == PC5) {
        if (rb_idP == 0)
          rb = ue->sl_srb0;
        else
          rb = ue->sl_srb[rb_idP - 1];
      } else if (intf_type == UU) {
        rb = ue->srb[rb_idP - 1];
      }
    } else {
      LOG_E(RLC, "%s:%d:%s: fatal: Unknown RB %ld\n", __FILE__, __LINE__, __FUNCTION__, rb_idP);
    }
  } else {
    if (rb_idP >= 1 && rb_idP <= MAX_DRBS_PER_UE) {
      if (intf_type == PC5) {
        rb = ue->sl_drb[rb_idP - 1];
      } else if (intf_type == UU) {
        rb = ue->drb[rb_idP - 1];
      }
    }
  }

  if(rb_idP == 1 && !srb_flagP)
    LOG_D(RLC, "Sending traffic with UE-specific drb rb_idP 1.\n");
  if(rb_idP == 2 && !srb_flagP)
    LOG_D(RLC, "Sending traffic with Relay-specific drb rb_idP 2.\n");

  if (rb != NULL) {
    rb->set_time(rb, nr_rlc_current_time);
    rb->recv_sdu(rb, (char *)sdu_pP->data, sdu_sizeP, muiP);
  } else {
    LOG_E(RLC, "%s:%d:%s: fatal: SDU sent to unknown RB %ld\n", __FILE__, __LINE__, __FUNCTION__, rb_idP);
  }

  nr_rlc_manager_unlock(nr_rlc_ue_manager);

  free_mem_block(sdu_pP, __func__);

  return RLC_OP_STATUS_OK;
}

int nr_rlc_get_available_tx_space(
  const rnti_t            rntiP,
  const logical_chan_id_t channel_idP)
{
  nr_rlc_ue_t *ue;
  nr_rlc_entity_t *rb;
  int ret;

  nr_rlc_manager_lock(nr_rlc_ue_manager);
  ue = nr_rlc_manager_get_ue(nr_rlc_ue_manager, rntiP);

  switch (channel_idP) {
  case 1 ... 3:               rb = ue->srb[channel_idP - 1]; break;
  case 4 ... MAX_DRBS_PER_UE: rb = ue->drb[channel_idP - 4]; break;
  default:                    rb = NULL;                     break;
  }

  if (rb != NULL) {
    ret = rb->available_tx_space(rb);
  } else {
    LOG_E(RLC, "[%s] Radio Bearer (channel ID %d) is NULL for UE with rntiP %x\n", __FUNCTION__, channel_idP, rntiP);
    ret = -1;
  }

  nr_rlc_manager_unlock(nr_rlc_ue_manager);

  return ret;
}

int rlc_module_init(int enb_flag)
{
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  static int inited = 0;
  static int inited_ue = 0;

  if (pthread_mutex_lock(&lock)) abort();

  if (enb_flag == 1 && inited) {
    LOG_E(RLC, "%s:%d:%s: fatal, inited already 1\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }

  if (enb_flag == 0 && inited_ue) {
    LOG_E(RLC, "%s:%d:%s: fatal, inited_ue already 1\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }

  if (enb_flag == 1) inited = 1;
  if (enb_flag == 0) inited_ue = 1;

  nr_rlc_ue_manager = new_nr_rlc_ue_manager(enb_flag);

  if (pthread_mutex_unlock(&lock)) abort();

  return 0;
}

void rlc_util_print_hex_octets(comp_name_t componentP, unsigned char *dataP, const signed long sizeP)
{
}

/* L2 Relay: Check for SRAP U2N messages on Uu SRB1 and DRB2 (per TS 38.401) and,
 * if matched, forward them straight to SRAP (bypassing PDCP).
 *   SRB1 carries Remote UE signaling (RRCSetupRequest, RRCSetupComplete, etc.)
 *   DRB2 at Relay UE is configured as transport bearer for Remote UE user plane
 *   At gNB: Remote UE messages forwarded by Relay UE on SRB1 (signaling) or DRB2 (user plane)
 *   At Relay UE: Downlink messages from gNB for Remote UE
 *   SRAP messages must bypass PDCP (no PDCP header/integrity) and go directly to SRAP
 * Returns true if the SDU was consumed (forwarded to SRAP), false otherwise. */
static bool deliver_sdu_srap_u2n(nr_rlc_ue_t *ue, nr_rlc_entity_t *entity, char *buf, int size, int is_srb, int rb_id, int is_enb)
{
  if (!(entity->intf_type == UU && ((is_srb && rb_id == 1) || (!is_srb && rb_id == 2)) && size >= 2 &&
        get_softmodem_params()->relay_type == U2N))
    return false;

  uint8_t byte0 = (uint8_t)buf[0];
  uint8_t byte1 = (uint8_t)buf[1];

  /* Basic SRAP pattern check
     For signaling (SRB1): Bearer ID 0-2 (SRB0-2)
     For user plane (DRB2): Bearer ID 4+ (Remote UE DRB1+ mapped to bearer_id 4+) */
  bool matches_srap_pattern = ((byte0 & SRAP_HDR_RESERVED_MASK) == 0) &&         // Reserved bits clear (bits 6-5 must be 00)
                              (is_srb ? ((byte0 & SRAP_HDR_BEARER_ID_MASK) <= 2) : ((byte0 & SRAP_HDR_BEARER_ID_MASK) >= 4)) && // Bearer ID check depends on SRB/DRB
                              (byte1 >= SRAP_REMOTE_UE_ID_MIN && byte1 <= SRAP_REMOTE_UE_ID_MAX); // Remote UE ID 1-255

  if (!matches_srap_pattern)
    return false;

  uint8_t bearer_id = byte0 & SRAP_HDR_BEARER_ID_MASK;
  bool is_srap_u2n = false;
  if (is_enb) {
    // gNB uplink: Distinguish SRAP from Relay UE's own PDCP traffic via D/C bit
    bool dc_bit_set = (byte0 & SRAP_HDR_DC_MASK) != 0;  // D/C=1 means data (SRAP payload)
    if (dc_bit_set) {
      // D/C=1 → SRAP (Remote UE forwarded traffic)
      is_srap_u2n = true;
      LOG_D(RLC, "[gNB] Uu RLC %s: D/C=1, size=%d → SRAP\n", is_srb ? "SRB1" : "DRB2", size);
    } else {
      // D/C=0 → Normal PDCP (Relay UE's own traffic)
      LOG_D(RLC, "[gNB] Uu RLC %s: D/C=0, size=%d → Normal PDCP\n", is_srb ? "SRB1" : "DRB2", size);
    }
  } else {
    // Relay UE downlink: Check bearer_id
    if (is_srb) {
      // SRB1: bearer=1 or bearer=2 → SRAP (Remote UE SRB1/SRB2)
      if (bearer_id >= 1 && bearer_id <= 2) {
        is_srap_u2n = true;
        LOG_D(RLC, "[Relay UE] Uu RLC SRB1: bearer=%d → SRAP (forward to Remote UE PC5 SRB%d)\n", bearer_id, bearer_id);
      } else {
        LOG_D(RLC, "[Relay UE] Uu RLC SRB1: bearer=%d → Normal PDCP (Relay UE own traffic)\n", bearer_id);
      }
    } else {
      // DRB2: bearer=4+ → SRAP (Remote UE DRB traffic)
      if (bearer_id >= 4) {
        is_srap_u2n = true;
        LOG_D(RLC, "[Relay UE] Uu RLC DRB2: bearer=%d → SRAP (forward to Remote UE PC5 DRB%d)\n", bearer_id, bearer_id - 3);
      } else {
        LOG_D(RLC, "[Relay UE] Uu RLC DRB2: bearer=%d → Normal PDCP (unexpected pattern)\n", bearer_id);
      }
    }
  }

  if (!is_srap_u2n)
    return false;

  LOG_D(RLC, "[%s] Uu RLC %s: SRAP U2N bearer=%d remote_ue=%d, forwarding %d bytes to SRAP%s, relay_rnti=0x%04x\n",
        is_enb ? "gNB" : "Relay UE", is_srb ? "SRB1" : "DRB2", bearer_id, byte1, size,
        is_enb ? " (bypassing PDCP)" : " for PC5 relay", ue->rnti);
  nr_srap_rlc_data_ind(rb_id, buf, size, UU, ue->rnti);
  return true;
}

/* L2 Relay: decide whether an SDU that reached PDCP delivery should instead be
 * routed through SRAP (N2U path), based on interface type and SRAP header content.
 *   PC5 traffic: All messages on PC5 go through SRAP for relay
 *     At Relay UE: PC5 RX → SRAP strips U2N header → Uu TX
 *     At Remote UE: PC5 RX → SRAP strips U2N header → PDCP → TUN
 *   Uu traffic: Relay UE DRB2 receives Remote UE traffic from gNB (with N2U header)
 *     At Relay UE: Uu RX → SRAP strips N2U header → PC5 TX
 * Returns true if the SDU should be delivered to SRAP instead of PDCP. */
static bool deliver_sdu_use_srap(nr_rlc_entity_t *entity, char *buf, int size, int is_srb, int rb_id)
{
  if (entity->intf_type == PC5) {
    if (get_softmodem_params()->is_relay_ue) {
      // Relay UE: all PC5 traffic goes to SRAP for forwarding to Uu
      return true;
    } else if (get_softmodem_params()->relay_type > 0) {
      /* Remote UE: Check SRAP N2U header (from Relay UE via PC5)
         N2U header format: buf[0]=bearer_id (LCID), buf[1]=remote_ue_id
         Note: bearer_id uses LCID numbering (SRB1=1, DRB1=4), not RLC rb_id (SRB1=1, DRB1=1)
         So we check remote_ue_id match only, not bearer_id vs rb_id (different numbering schemes) */
      return size >= 2 && buf[1] == get_softmodem_params()->remote_ue_id;
    }
  } else if (entity->intf_type == UU && !is_srb) {
    if (get_softmodem_params()->is_relay_ue && rb_id == 2) {
      /* Check for SRAP N2U header: buf[0] high bits indicate SRAP message
         N2U format: bearer_id + remote_ue_id encoded in first bytes */
      return size >= 2 && (buf[0] == 0x84 || buf[0] == 0x85 || (buf[0] & SRAP_HDR_DC_MASK));
    }
  }
  return false;
}

static void deliver_sdu(void *_ue, nr_rlc_entity_t *entity, char *buf, int size)
{
  nr_rlc_ue_t *ue = _ue;
  int is_srb;
  int rb_id;
  protocol_ctxt_t ctx;
  mem_block_t *memblock;
  int i;
  int is_enb;

  if (entity->intf_type == PC5) {
    if (entity == ue->sl_srb0) {
      is_srb = 1;
      rb_id = 0;
      goto rb_found;
    }
  }
  if (entity->intf_type == PC5) {
    for (i = 0; i < sizeofArray(ue->sl_srb); i++) {
      if (entity == ue->sl_srb[i]) {
        is_srb = 1;
        rb_id = i + 1;
        goto rb_found;
      }
    }
  }

  /* is it SRB? */
  for (i = 0; i < sizeofArray(ue->srb); i++) {
    if (entity == ue->srb[i]) {
      is_srb = 1;
      rb_id = i+1;
      goto rb_found;
    }
  }

  /* maybe DRB? */
  if (entity->intf_type == UU) {
    for (i = 0; i < sizeofArray(ue->drb); i++) {
      if (entity == ue->drb[i]) {
        is_srb = 0;
        rb_id = i + 1;
        goto rb_found;
      }
    }
  } else if (entity->intf_type == PC5) {
    for (i = 0; i < sizeofArray(ue->sl_drb); i++) {
      if (entity == ue->sl_drb[i]) {
        is_srb = 0;
        rb_id = i + 1;
        goto rb_found;
      }
    }
  }

  LOG_E(RLC, "%s:%d:%s: fatal, no RB found for ue %d\n",
        __FILE__, __LINE__, __FUNCTION__, ue->rnti);
  exit(1);

rb_found:
  LOG_D(RLC, "%s:%d:%s: delivering SDU (rnti %d is_srb %d rb_id %d) size %d\n",
        __FILE__, __LINE__, __FUNCTION__, ue->rnti, is_srb, rb_id, size);

  is_enb = nr_rlc_manager_get_enb_flag(nr_rlc_ue_manager);

  /* L2 Relay: forward SRAP U2N messages straight to SRAP, bypassing PDCP */
  if (deliver_sdu_srap_u2n(ue, entity, buf, size, is_srb, rb_id, is_enb))
    return;

  /* unused fields? */
  ctx.instance = 0;
  ctx.frame = 0;
  ctx.subframe = 0;
  ctx.eNB_index = 0;
  ctx.brOption = 0;

  /* used fields? */
  ctx.module_id = 0;
  ctx.rntiMaybeUEid = ue->rnti;
  ctx.enb_flag = is_enb;

  if (is_enb) {
    T(T_ENB_RLC_UL,
      T_INT(0 /*ctxt_pP->module_id*/),
      T_INT(ue->rnti), T_INT(rb_id), T_INT(size));

    const ngran_node_t type = RC.nrrrc[0 /*ctxt_pP->module_id*/]->node_type;
    AssertFatal(!NODE_IS_CU(type),
                "Can't be CU, bad node type %d\n", type);

    // if (NODE_IS_DU(type) && is_srb == 0) {
    //   LOG_D(RLC, "call proto_agent_send_pdcp_data_ind() \n");
    //   proto_agent_send_pdcp_data_ind(&ctx, is_srb, 0, rb_id, size, memblock);
    //   return;
    // }

    if (NODE_IS_DU(type)) {
      if(is_srb) {
	MessageDef *msg;
	msg = itti_alloc_new_message(TASK_RLC_ENB, 0, F1AP_UL_RRC_MESSAGE);
	uint8_t *message_buffer = itti_malloc (TASK_RLC_ENB, TASK_DU_F1, size);
	memcpy (message_buffer, buf, size);
	F1AP_UL_RRC_MESSAGE(msg).rnti = ue->rnti;
	F1AP_UL_RRC_MESSAGE(msg).srb_id = rb_id;
	F1AP_UL_RRC_MESSAGE(msg).rrc_container = message_buffer;
	F1AP_UL_RRC_MESSAGE(msg).rrc_container_length = size;
	itti_send_msg_to_task(TASK_DU_F1, ENB_MODULE_ID_TO_INSTANCE(0 /*ctxt_pP->module_id*/), msg);
	return;
      } else {
	MessageDef *msg = itti_alloc_new_message_sized(TASK_RLC_ENB, 0, GTPV1U_TUNNEL_DATA_REQ,
						       sizeof(gtpv1u_tunnel_data_req_t) + size);
	gtpv1u_tunnel_data_req_t *req=&GTPV1U_TUNNEL_DATA_REQ(msg);
	req->buffer=(uint8_t*)(req+1);
	memcpy(req->buffer,buf,size);
	req->length=size;
	req->offset=0;
	req->ue_id=ue->rnti;
	req->bearer_id=rb_id;
	LOG_D(RLC, "Received uplink user-plane traffic at RLC-DU to be sent to the CU, size %d \n", size);
	extern instance_t DUuniqInstance;
	itti_send_msg_to_task(TASK_GTPV1_U, DUuniqInstance, msg);
	return;
      }
    }
  }

  memblock = get_free_mem_block(size, __func__);
  if (memblock == NULL) {
    LOG_E(RLC, "%s:%d:%s: ERROR: get_free_mem_block failed\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }
  memcpy(memblock->data, buf, size);

  /* SRAP routing logic: Check interface type and data content
     Based on episys/sl-mode1-relay architecture with enhancements */
  if (deliver_sdu_use_srap(entity, buf, size, is_srb, rb_id)) {
    LOG_D(RLC, "Deliver from RLC to SRAP for rb_id %d\n", rb_id);
    if (!srap_data_ind(&ctx, is_srb, 0, rb_id, size, memblock, NULL, NULL, entity->intf_type)) {
      LOG_E(RLC, "%s:%d:%s: ERROR: srap_data_ind failed\n", __FILE__, __LINE__, __FUNCTION__);
    }
  } else {
    LOG_D(RLC, "Deliver directly from RLC to PDCP for rb_id %d\n", rb_id);
    if (!pdcp_data_ind(&ctx, is_srb, 0, rb_id, size, memblock, NULL, NULL, entity->intf_type)) {
      LOG_E(RLC, "%s:%d:%s: ERROR: pdcp_data_ind failed\n", __FILE__, __LINE__, __FUNCTION__);
      /* what to do in case of failure? for the moment: nothing */
    }
  }
}

static void successful_delivery(void *_ue, nr_rlc_entity_t *entity, int sdu_id)
{
  nr_rlc_ue_t *ue = _ue;
  int i;
  int is_srb;
  int rb_id;
#if 0
  MessageDef *msg;
#endif
  int is_enb;

  if (entity->intf_type == PC5) {
    /* check sl_srb0 first */
    if (entity == ue->sl_srb0) {
      is_srb = 1;
      rb_id = 0;
      goto rb_found;
    }
    /* then check sl_srb[1..2] */
    for (i = 0; i < 2; i++) {
      if (entity == ue->sl_srb[i]) {
        is_srb = 1;
        rb_id = i + 1;
        goto rb_found;
      }
    }
  }

  /* is it SRB? */
  for (i = 0; i < 2; i++) {
    if (entity == ue->srb[i]) {
      is_srb = 1;
      rb_id = i+1;
      goto rb_found;
    }
  }

  /* maybe DRB? */
  for (i = 0; i < MAX_DRBS_PER_UE; i++) {
    if (entity == ue->drb[i]) {
      is_srb = 0;
      rb_id = i+1;
      goto rb_found;
    }
  }

  /* maybe DRB? */
  for (i = 0; i < MAX_DRBS_PER_UE; i++) {
    if (entity == ue->sl_drb[i]) {
      is_srb = 0;
      rb_id = i+1;
      goto rb_found;
    }
  }

  LOG_E(RLC, "%s:%d:%s: fatal, no RB found for ue %d\n",
        __FILE__, __LINE__, __FUNCTION__, ue->rnti);
  exit(1);

rb_found:
  LOG_D(RLC, "sdu %d was successfully delivered on %s %d\n",
        sdu_id,
        is_srb ? "SRB" : "DRB",
        rb_id);

  /* TODO: do something for DRBs? */
  if (is_srb == 0)
    return;

  is_enb = nr_rlc_manager_get_enb_flag(nr_rlc_ue_manager);
  if (!is_enb)
    return;

#if 0
  msg = itti_alloc_new_message(TASK_RLC_ENB, RLC_SDU_INDICATION);
  RLC_SDU_INDICATION(msg).rnti          = ue->rnti;
  RLC_SDU_INDICATION(msg).is_successful = 1;
  RLC_SDU_INDICATION(msg).srb_id        = rb_id;
  RLC_SDU_INDICATION(msg).message_id    = sdu_id;
  /* TODO: accept more than 1 instance? here we send to instance id 0 */
  itti_send_msg_to_task(TASK_RRC_ENB, 0, msg);
#endif
}

static void max_retx_reached(void *_ue, nr_rlc_entity_t *entity)
{
  nr_rlc_ue_t *ue = _ue;
  int i;
  int is_srb;
  int rb_id;
#if 0
  MessageDef *msg;
#endif
  int is_enb;

  if (entity->intf_type == PC5) {
    /* check sl_srb0 first */
    if (entity == ue->sl_srb0) {
      is_srb = 1;
      rb_id = 0;
      goto rb_found;
    }
    /* then check sl_srb[1..2] */
    for (i = 0; i < 2; i++) {
      if (entity == ue->sl_srb[i]) {
        is_srb = 1;
        rb_id = i + 1;
        goto rb_found;
      }
    }
  }

  /* is it SRB? */
  for (i = 0; i < 2; i++) {
    if (entity == ue->srb[i]) {
      is_srb = 1;
      rb_id = i+1;
      goto rb_found;
    }
  }

  /* maybe DRB? */
  for (i = 0; i < MAX_DRBS_PER_UE; i++) {
    if (entity == ue->drb[i]) {
      is_srb = 0;
      rb_id = i+1;
      goto rb_found;
    }
  }

  /* maybe DRB? */
  for (i = 0; i < MAX_DRBS_PER_UE; i++) {
    if (entity == ue->sl_drb[i]) {
      is_srb = 0;
      rb_id = i+1;
      goto rb_found;
    }
  }

  LOG_E(RLC, "%s:%d:%s: fatal, no RB found for ue %d\n",
        __FILE__, __LINE__, __FUNCTION__, ue->rnti);
  exit(1);

rb_found:
  LOG_E(RLC, "max RETX reached on %s %d\n",
        is_srb ? "SRB" : "DRB",
        rb_id);

  /* TODO: do something for DRBs? */
  if (is_srb == 0)
    return;

  is_enb = nr_rlc_manager_get_enb_flag(nr_rlc_ue_manager);
  if (!is_enb)
    return;

#if 0
  msg = itti_alloc_new_message(TASK_RLC_ENB, RLC_SDU_INDICATION);
  RLC_SDU_INDICATION(msg).rnti          = ue->rnti;
  RLC_SDU_INDICATION(msg).is_successful = 0;
  RLC_SDU_INDICATION(msg).srb_id        = rb_id;
  RLC_SDU_INDICATION(msg).message_id    = -1;
  /* TODO: accept more than 1 instance? here we send to instance id 0 */
  itti_send_msg_to_task(TASK_RRC_ENB, 0, msg);
#endif
}

void nr_rlc_add_srb(int rnti, int srb_id, const NR_RLC_BearerConfig_t *rlc_BearerConfig)
{
  nr_rlc_entity_t            *nr_rlc_am;
  nr_rlc_ue_t                *ue;

  struct NR_RLC_Config *r = rlc_BearerConfig->rlc_Config;
  struct NR_LogicalChannelConfig *l = rlc_BearerConfig->mac_LogicalChannelConfig;
  int channel_id = rlc_BearerConfig->logicalChannelIdentity;
  int logical_channel_group;

  int t_status_prohibit;
  int t_poll_retransmit;
  int poll_pdu;
  int poll_byte;
  int max_retx_threshold;
  int t_reassembly;
  int sn_field_length;

  LOG_D(RLC,"Trying to add SRB %d\n",srb_id);
  if (srb_id != 1 && srb_id != 2) {
    LOG_E(RLC, "%s:%d:%s: fatal, bad srb id %d\n",
        __FILE__, __LINE__, __FUNCTION__, srb_id);
    exit(1);
  }

  if (channel_id != srb_id) {
    LOG_E(RLC, "%s:%d:%s: todo, remove this limitation\n",
          __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }

  logical_channel_group = *l->ul_SpecificParameters->logicalChannelGroup;

  /* TODO: accept other values? */
  if (logical_channel_group != 0) {
    LOG_E(RLC, "%s:%d:%s: fatal error\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }

  switch (r->present) {
  case NR_RLC_Config_PR_am: {
    struct NR_RLC_Config__am *am;
    am = r->choice.am;
    t_reassembly       = decode_t_reassembly(am->dl_AM_RLC.t_Reassembly);
    t_status_prohibit  = decode_t_status_prohibit(am->dl_AM_RLC.t_StatusProhibit);
    t_poll_retransmit  = decode_t_poll_retransmit(am->ul_AM_RLC.t_PollRetransmit);
    poll_pdu           = decode_poll_pdu(am->ul_AM_RLC.pollPDU);
    poll_byte          = decode_poll_byte(am->ul_AM_RLC.pollByte);
    max_retx_threshold = decode_max_retx_threshold(am->ul_AM_RLC.maxRetxThreshold);
    if (*am->dl_AM_RLC.sn_FieldLength != *am->ul_AM_RLC.sn_FieldLength) {
      LOG_E(RLC, "%s:%d:%s: fatal\n", __FILE__, __LINE__, __FUNCTION__);
      exit(1);
    }
    sn_field_length    = decode_sn_field_length_am(*am->dl_AM_RLC.sn_FieldLength);
    break;
  }
  default:
    LOG_E(RLC, "%s:%d:%s: fatal error\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }

  nr_rlc_manager_lock(nr_rlc_ue_manager);
  ue = nr_rlc_manager_get_ue(nr_rlc_ue_manager, rnti);
  if (ue->srb[srb_id - 1] != NULL) {
    LOG_W(RLC, "%s:%d:%s: SRB %d already exists for UE with RNTI %04x, do nothing\n", __FILE__, __LINE__, __FUNCTION__, srb_id, rnti);
  } else {
    /* hack: hardcode values for NR */
    t_poll_retransmit = 45;
    t_reassembly = 35;
    t_status_prohibit = 0;
    poll_pdu = -1;
    poll_byte = -1;
    max_retx_threshold = 8;
    sn_field_length = 12;
    nr_rlc_am = new_nr_rlc_entity_am(RLC_RX_MAXSIZE,
                                     RLC_TX_MAXSIZE,
                                     deliver_sdu, ue,
                                     successful_delivery, ue,
                                     max_retx_reached, ue,
                                     t_poll_retransmit,
                                     t_reassembly, t_status_prohibit,
                                     poll_pdu, poll_byte, max_retx_threshold,
                                     sn_field_length,
                                     UU);
    nr_rlc_ue_add_srb_rlc_entity(ue, srb_id, nr_rlc_am);

    LOG_I(RLC, "%s:%d:%s: added srb %d to UE with RNTI 0x%x\n", __FILE__, __LINE__, __FUNCTION__, srb_id, rnti);
  }
  nr_rlc_manager_unlock(nr_rlc_ue_manager);
}

void nr_rlc_add_srb_sl(int rnti, int srb_id, const NR_SL_RLC_BearerConfig_r16_t *rlc_BearerConfig) {
  nr_rlc_entity_t            *nr_rlc_am;
  nr_rlc_ue_t                *ue;

  struct NR_SL_RLC_Config_r16 *r = rlc_BearerConfig->sl_RLC_Config_r16;
  struct NR_SL_LogicalChannelConfig_r16 *l = rlc_BearerConfig->sl_MAC_LogicalChannelConfig_r16;
  int channel_id = srb_id;
  int logical_channel_group;

  int t_status_prohibit;
  int t_poll_retransmit;
  int poll_pdu;
  int poll_byte;
  int max_retx_threshold;
  int t_reassembly;
  int sn_field_length;

  LOG_D(RLC,"Trying to add SRB %d\n", srb_id);
  if (srb_id < 0 && srb_id > 3) {
    LOG_E(RLC, "%s:%d:%s: fatal, bad srb id %d\n",
        __FILE__, __LINE__, __FUNCTION__, srb_id);
    exit(1);
  }

  if (channel_id != srb_id) {
    LOG_E(RLC, "%s:%d:%s: todo, remove this limitation\n",
          __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }

  logical_channel_group = *l->sl_LogicalChannelGroup_r16;

  /* TODO: accept other values? */
  if (logical_channel_group != 0) {
    LOG_E(RLC, "%s:%d:%s: fatal error\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }

  switch (r->present) {
  case NR_RLC_Config_PR_am: {
    struct NR_SL_RLC_Config_r16__sl_AM_RLC_r16 *am;
    am = r->choice.sl_AM_RLC_r16;
    t_poll_retransmit  = decode_t_poll_retransmit(am->sl_T_PollRetransmit_r16);
    poll_pdu           = decode_poll_pdu(am->sl_PollPDU_r16);
    poll_byte          = decode_poll_byte(am->sl_PollByte_r16);
    max_retx_threshold = decode_max_retx_threshold(am->sl_MaxRetxThreshold_r16);
    sn_field_length    = decode_sn_field_length_am(*am->sl_SN_FieldLengthAM_r16);
    break;
  }
  default:
    LOG_E(RLC, "%s:%d:%s: fatal error\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }

  nr_rlc_manager_lock(nr_rlc_ue_manager);
  ue = nr_rlc_manager_get_ue(nr_rlc_ue_manager, rnti);

  nr_rlc_entity_t *temp_srb_entity = (srb_id == 0) ? ue->sl_srb0 : ue->sl_srb[srb_id - 1];
  if (temp_srb_entity != NULL) {
    /* Do NOT delete and recreate - this would reset tx_next/rx_next and break sequence numbers
       The existing RLC entity preserves the sequence number state, which is critical for RLC AM
       Configuration updates (like MCS changes) don't require RLC entity recreation */
    nr_rlc_manager_unlock(nr_rlc_ue_manager);
    return;
  }
  {
    /* hack: hardcode values for NR */
    t_poll_retransmit = 45;
    t_reassembly = 35;
    t_status_prohibit = 0;
    poll_pdu = -1;
    poll_byte = -1;
    max_retx_threshold = 8;
    sn_field_length = 12;
    nr_rlc_am = new_nr_rlc_entity_am(RLC_RX_MAXSIZE,
                                     RLC_TX_MAXSIZE,
                                     deliver_sdu, ue,
                                     successful_delivery, ue,
                                     max_retx_reached, ue,
                                     t_poll_retransmit,
                                     t_reassembly, t_status_prohibit,
                                     poll_pdu, poll_byte, max_retx_threshold,
                                     sn_field_length,
                                     PC5);
    nr_rlc_ue_add_srb_rlc_entity(ue, srb_id, nr_rlc_am);

    LOG_D(RLC, "%s:%d:%s: added sl_srb %d to UE with RNTI 0x%x\n", __FILE__, __LINE__, __FUNCTION__, srb_id, rnti);
  }
  nr_rlc_manager_unlock(nr_rlc_ue_manager);
}

static bool add_drb_am(int rnti, int drb_id, const NR_RLC_BearerConfig_t *rlc_BearerConfig)
{
  nr_rlc_entity_t            *nr_rlc_am;
  nr_rlc_ue_t                *ue;
  bool created = false;

  struct NR_RLC_Config *r = rlc_BearerConfig->rlc_Config;
  struct NR_LogicalChannelConfig *l = rlc_BearerConfig->mac_LogicalChannelConfig;
  int logical_channel_group;

  int t_status_prohibit;
  int t_poll_retransmit;
  int poll_pdu;
  int poll_byte;
  int max_retx_threshold;
  int t_reassembly;
  int sn_field_length;

  if (!(drb_id >= 1 && drb_id <= MAX_DRBS_PER_UE)) {
    LOG_E(RLC, "%s:%d:%s: fatal, bad srb id %d\n",
          __FILE__, __LINE__, __FUNCTION__, drb_id);
    exit(1);
  }

  logical_channel_group = *l->ul_SpecificParameters->logicalChannelGroup;

  /* TODO: accept other values? */
  if (logical_channel_group != 1) {
    LOG_E(RLC, "%s:%d:%s: fatal error\n", __FILE__, __LINE__, __FUNCTION__);
    //exit(1);
  }

  switch (r->present) {
  case NR_RLC_Config_PR_am: {
    struct NR_RLC_Config__am *am;
    am = r->choice.am;
    t_reassembly       = decode_t_reassembly(am->dl_AM_RLC.t_Reassembly);
    t_status_prohibit  = decode_t_status_prohibit(am->dl_AM_RLC.t_StatusProhibit);
    t_poll_retransmit  = decode_t_poll_retransmit(am->ul_AM_RLC.t_PollRetransmit);
    poll_pdu           = decode_poll_pdu(am->ul_AM_RLC.pollPDU);
    poll_byte          = decode_poll_byte(am->ul_AM_RLC.pollByte);
    max_retx_threshold = decode_max_retx_threshold(am->ul_AM_RLC.maxRetxThreshold);
    if (*am->dl_AM_RLC.sn_FieldLength != *am->ul_AM_RLC.sn_FieldLength) {
      LOG_E(RLC, "%s:%d:%s: fatal\n", __FILE__, __LINE__, __FUNCTION__);
      exit(1);
    }
    sn_field_length    = decode_sn_field_length_am(*am->dl_AM_RLC.sn_FieldLength);
    break;
  }
  default:
    LOG_E(RLC, "%s:%d:%s: fatal error\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }

  nr_rlc_manager_lock(nr_rlc_ue_manager);
  ue = nr_rlc_manager_get_ue(nr_rlc_ue_manager, rnti);
  if (ue->drb[drb_id-1] != NULL) {
    LOG_W(RLC, "%s:%d:%s: DRB %d already exists for UE with RNTI %04x, do nothing\n", __FILE__, __LINE__, __FUNCTION__, drb_id, rnti);
    created = false;
  } else {
    nr_rlc_am = new_nr_rlc_entity_am(RLC_RX_MAXSIZE,
                                     RLC_TX_MAXSIZE,
                                     deliver_sdu, ue,
                                     successful_delivery, ue,
                                     max_retx_reached, ue,
                                     t_poll_retransmit,
                                     t_reassembly, t_status_prohibit,
                                     poll_pdu, poll_byte, max_retx_threshold,
                                     sn_field_length,
                                     UU);
    nr_rlc_ue_add_drb_rlc_entity(ue, drb_id, nr_rlc_am);

    LOG_I(RLC, "%s:%d:%s: added drb %d to UE with RNTI 0x%x\n", __FILE__, __LINE__, __FUNCTION__, drb_id,rnti);
    created = true;
  }
  nr_rlc_manager_unlock(nr_rlc_ue_manager);
  return created;
}

static void add_drb_am_sl(int src_id, int drb_id, const NR_SL_RLC_BearerConfig_r16_t *rlc_BearerConfig)
{
  nr_rlc_entity_t            *nr_rlc_am;
  nr_rlc_ue_t                *ue;

  struct NR_SL_RLC_Config_r16 *r = rlc_BearerConfig->sl_RLC_Config_r16;
  struct NR_SL_LogicalChannelConfig_r16 *l = rlc_BearerConfig->sl_MAC_LogicalChannelConfig_r16;
  int logical_channel_group;

  int t_status_prohibit;
  int t_poll_retransmit;
  int poll_pdu;
  int poll_byte;
  int max_retx_threshold;
  int t_reassembly;
  int sn_field_length;

  if (!(drb_id >= 1 && drb_id <= MAX_DRBS_PER_UE)) {
    LOG_E(RLC, "%s:%d:%s: fatal, bad srb id %d\n",
          __FILE__, __LINE__, __FUNCTION__, drb_id);
    exit(1);
  }

  logical_channel_group = *l->sl_LogicalChannelGroup_r16;

  /* TODO: accept other values? */
  if (logical_channel_group != 1) {
    LOG_E(RLC, "%s:%d:%s: fatal error\n", __FILE__, __LINE__, __FUNCTION__);
    //exit(1);
  }

  struct NR_SL_RLC_Config_r16__sl_AM_RLC_r16 *am;
  am = r->choice.sl_AM_RLC_r16;
  t_reassembly       = 35;
  t_status_prohibit  = 35;
  t_poll_retransmit  = decode_t_poll_retransmit(am->sl_T_PollRetransmit_r16);
  poll_pdu           = decode_poll_pdu(am->sl_PollPDU_r16);
  poll_byte          = decode_poll_byte(am->sl_PollByte_r16);
  max_retx_threshold = decode_max_retx_threshold(am->sl_MaxRetxThreshold_r16);
  sn_field_length    = decode_sn_field_length_am(*am->sl_SN_FieldLengthAM_r16);

  nr_rlc_manager_lock(nr_rlc_ue_manager);
  ue = nr_rlc_manager_get_ue(nr_rlc_ue_manager, src_id);
  if (ue->drb[drb_id-1] != NULL) {
    LOG_W(RLC, "%s:%d:%s: DRB %d already exists for SL UE with src_id %04x, do nothing\n", __FILE__, __LINE__, __FUNCTION__, drb_id, src_id);
  } else {
    nr_rlc_am = new_nr_rlc_entity_am(RLC_RX_MAXSIZE,
                                     RLC_TX_MAXSIZE,
                                     deliver_sdu, ue,
                                     successful_delivery, ue,
                                     max_retx_reached, ue,
                                     t_poll_retransmit,
                                     t_reassembly, t_status_prohibit,
                                     poll_pdu, poll_byte, max_retx_threshold,
                                     sn_field_length,
                                     PC5);

    nr_rlc_ue_add_drb_rlc_entity(ue, drb_id, nr_rlc_am);

    LOG_I(RLC, "%s:%d:%s: added drb %d to UE with SRCID 0x%x\n", __FILE__, __LINE__, __FUNCTION__, drb_id, src_id);
  }
  nr_rlc_manager_unlock(nr_rlc_ue_manager);
}
static void add_drb_um(int rnti, int drb_id, const NR_RLC_BearerConfig_t *rlc_BearerConfig)
{
  nr_rlc_entity_t            *nr_rlc_um;
  nr_rlc_ue_t                *ue;

  struct NR_RLC_Config *r = rlc_BearerConfig->rlc_Config;
  struct NR_LogicalChannelConfig *l = rlc_BearerConfig->mac_LogicalChannelConfig;
  int channel_id = rlc_BearerConfig->logicalChannelIdentity;
  int logical_channel_group;

  int sn_field_length;
  int t_reassembly;

  if (!(drb_id >= 1 && drb_id <= MAX_DRBS_PER_UE)) {
    LOG_E(RLC, "%s:%d:%s: fatal, bad srb id %d\n",
          __FILE__, __LINE__, __FUNCTION__, drb_id);
    exit(1);
  }

  if (channel_id != drb_id + 3) {
    LOG_E(RLC, "%s:%d:%s: todo, remove this limitation\n",
          __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }

  logical_channel_group = *l->ul_SpecificParameters->logicalChannelGroup;

  /* TODO: accept other values? */
  if (logical_channel_group != 1) {
    LOG_E(RLC, "%s:%d:%s: fatal error\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }

  switch (r->present) {
  case NR_RLC_Config_PR_um_Bi_Directional: {
    struct NR_RLC_Config__um_Bi_Directional *um;
    um = r->choice.um_Bi_Directional;
    t_reassembly = decode_t_reassembly(um->dl_UM_RLC.t_Reassembly);
    if (*um->dl_UM_RLC.sn_FieldLength != *um->ul_UM_RLC.sn_FieldLength) {
      LOG_E(RLC, "%s:%d:%s: fatal\n", __FILE__, __LINE__, __FUNCTION__);
      exit(1);
    }
    sn_field_length = decode_sn_field_length_um(*um->dl_UM_RLC.sn_FieldLength);
    break;
  }
  default:
    LOG_E(RLC, "%s:%d:%s: fatal error\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }

  nr_rlc_manager_lock(nr_rlc_ue_manager);
  ue = nr_rlc_manager_get_ue(nr_rlc_ue_manager, rnti);
  if (ue->drb[drb_id-1] != NULL) {
    LOG_W(RLC, "DEBUG add_drb_um %s:%d:%s: warning DRB %d already exist for ue %d, do nothing\n", __FILE__, __LINE__, __FUNCTION__, drb_id, rnti);
  } else {
    nr_rlc_um = new_nr_rlc_entity_um(RLC_RX_MAXSIZE,
                                     RLC_TX_MAXSIZE,
                                     deliver_sdu, ue,
                                     t_reassembly,
                                     sn_field_length,
                                     UU);
    nr_rlc_ue_add_drb_rlc_entity(ue, drb_id, nr_rlc_um);

    LOG_D(RLC, "%s:%d:%s: added drb %d to UE with RNTI 0x%x\n", __FILE__, __LINE__, __FUNCTION__, drb_id, rnti);
  }
  nr_rlc_manager_unlock(nr_rlc_ue_manager);
}

static void add_drb_um_sl(int src_id, int drb_id, const NR_SL_RLC_BearerConfig_r16_t *rlc_BearerConfig)
{
  nr_rlc_entity_t            *nr_rlc_um;
  nr_rlc_ue_t                *ue;

  struct NR_SL_RLC_Config_r16 *r = rlc_BearerConfig->sl_RLC_Config_r16;
  struct NR_SL_LogicalChannelConfig_r16 *l = rlc_BearerConfig->sl_MAC_LogicalChannelConfig_r16;
  int logical_channel_group;

  int sn_field_length;
  int t_reassembly;

  if (!(drb_id >= 1 && drb_id <= MAX_DRBS_PER_UE)) {
    LOG_E(RLC, "%s:%d:%s: fatal, bad srb id %d\n",
          __FILE__, __LINE__, __FUNCTION__, drb_id);
    exit(1);
  }

  logical_channel_group = *l->sl_LogicalChannelGroup_r16;

  /* TODO: accept other values? */
  if (logical_channel_group != 1) {
    LOG_E(RLC, "%s:%d:%s: fatal error\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }

  struct NR_SL_RLC_Config_r16__sl_UM_RLC_r16 *um;
  um = r->choice.sl_UM_RLC_r16;
  t_reassembly = 35; // up to UE implementation, choose 35ms
  sn_field_length = decode_sn_field_length_um(*um->sl_SN_FieldLengthUM_r16);

  nr_rlc_manager_lock(nr_rlc_ue_manager);
  ue = nr_rlc_manager_get_ue(nr_rlc_ue_manager, src_id);
  if (ue->drb[drb_id-1] != NULL) {
    LOG_W(RLC, "DEBUG add_drb_um %s:%d:%s: warning DRB %d already exist for SL ue %d, do nothing\n", __FILE__, __LINE__, __FUNCTION__, drb_id, src_id);
  } else {
    nr_rlc_um = new_nr_rlc_entity_um(RLC_RX_MAXSIZE,
                                     RLC_TX_MAXSIZE,
                                     deliver_sdu, ue,
                                     t_reassembly,
                                     sn_field_length,
                                     PC5);
    nr_rlc_ue_add_drb_rlc_entity(ue, drb_id, nr_rlc_um);

    LOG_D(RLC, "%s:%d:%s: added drb %d to UE with SRCID 0x%x\n", __FILE__, __LINE__, __FUNCTION__, drb_id, src_id);
  }
  nr_rlc_manager_unlock(nr_rlc_ue_manager);
}

bool nr_rlc_add_drb(int rnti, int drb_id, const NR_RLC_BearerConfig_t *rlc_BearerConfig)
{
  bool created = false;
  switch (rlc_BearerConfig->rlc_Config->present) {
  case NR_RLC_Config_PR_am:
    created = add_drb_am(rnti, drb_id, rlc_BearerConfig);
    break;
  case NR_RLC_Config_PR_um_Bi_Directional:
    add_drb_um(rnti, drb_id, rlc_BearerConfig);
    created = true; // add_drb_um doesn't return bool yet
    break;
  default:
    LOG_E(RLC, "%s:%d:%s: fatal: unhandled DRB type\n",
          __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }
  if (created) {
    LOG_I(RLC, "%s:%s:%d: added DRB %d to UE with RNTI 0x%x\n", __FILE__, __FUNCTION__, __LINE__, drb_id, rnti);
  }
  return created;
}

void nr_rlc_add_drb_sl(int srcid, int drb_id, const NR_SL_RLC_BearerConfig_r16_t *rlc_BearerConfig)
{
  switch (rlc_BearerConfig->sl_RLC_Config_r16->present) {
  case NR_SL_RLC_Config_r16_PR_sl_AM_RLC_r16:
    add_drb_am_sl(srcid, drb_id, rlc_BearerConfig);
    break;
  case NR_SL_RLC_Config_r16_PR_sl_UM_RLC_r16:
    add_drb_um_sl(srcid, drb_id, rlc_BearerConfig);
    break;
  default:
    LOG_E(RLC, "%s:%d:%s: fatal: unhandled DRB type\n",
          __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }
  LOG_I(RLC, "%s:%s:%d: added SL_DRB %d to UE with SRCID 0x%x\n", __FILE__, __FUNCTION__, __LINE__, drb_id,srcid);
}
/* Dummy function due to dependency from LTE libraries */
rlc_op_status_t rrc_rlc_config_asn1_req (const protocol_ctxt_t   * const ctxt_pP,
    const LTE_SRB_ToAddModList_t   * const srb2add_listP,
    const LTE_DRB_ToAddModList_t   * const drb2add_listP,
    const LTE_DRB_ToReleaseList_t  * const drb2release_listP,
    const LTE_PMCH_InfoList_r9_t * const pmch_InfoList_r9_pP,
    const uint32_t sourceL2Id,
    const uint32_t destinationL2Id)
{
  return 0;
}

struct srb0_data {
  struct gNB_MAC_INST_s *mac;
  int rnti;
  void *rawUE;
  void (*send_initial_ul_rrc_message)(struct gNB_MAC_INST_s *mac,
                                      int                    rnti,
                                      const uint8_t         *sdu,
                                      sdu_size_t             sdu_len,
                                      void                  *rawUE);
};

void deliver_sdu_srb0(void *deliver_sdu_data, struct nr_rlc_entity_t *entity,
                      char *buf, int size)
{
  struct srb0_data *s0 = (struct srb0_data *)deliver_sdu_data;
  s0->send_initial_ul_rrc_message(s0->mac, s0->rnti, (unsigned char *)buf,
                                  size, s0->rawUE);
}

void nr_rlc_activate_srb0(int rnti, struct gNB_MAC_INST_s *mac, void *rawUE,
                          void (*send_initial_ul_rrc_message)(
                                     struct gNB_MAC_INST_s *mac,
                                     int                    rnti,
                                     const uint8_t         *sdu,
                                     sdu_size_t             sdu_len,
                                     void                  *rawUE))
{
  nr_rlc_entity_t            *nr_rlc_tm;
  nr_rlc_ue_t                *ue;
  struct srb0_data           *srb0_data;

  srb0_data = calloc(1, sizeof(struct srb0_data));
  AssertFatal(srb0_data != NULL, "out of memory\n");

  srb0_data->mac       = mac;
  srb0_data->rnti      = rnti;
  srb0_data->rawUE     = rawUE;
  srb0_data->send_initial_ul_rrc_message = send_initial_ul_rrc_message;

  nr_rlc_manager_lock(nr_rlc_ue_manager);
  ue = nr_rlc_manager_get_ue(nr_rlc_ue_manager, rnti);
  if (ue->srb0 != NULL) {
    LOG_W(RLC, "SRB0 already exists for UE with RNTI 0x%x, do nothing\n", rnti);
    free(srb0_data);
    nr_rlc_manager_unlock(nr_rlc_ue_manager);
    return;
  }

  nr_rlc_tm = new_nr_rlc_entity_tm(10000,
                                   deliver_sdu_srb0, srb0_data);
  nr_rlc_ue_add_srb_rlc_entity(ue, 0, nr_rlc_tm);

  LOG_I(RLC, "activated srb0 for UE with RNTI 0x%x\n", rnti);
  nr_rlc_manager_unlock(nr_rlc_ue_manager);
}

rlc_op_status_t rrc_rlc_config_req   (
  const protocol_ctxt_t* const ctxt_pP,
  const srb_flag_t      srb_flagP,
  const MBMS_flag_t     mbms_flagP,
  const config_action_t actionP,
  const rb_id_t         rb_idP)
{
  nr_rlc_ue_t *ue;
  int      i;

  if (mbms_flagP) {
    LOG_E(RLC, "%s:%d:%s: todo (MBMS NOT supported)\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }
  if (actionP != CONFIG_ACTION_REMOVE) {
    LOG_E(RLC, "%s:%d:%s: todo (only CONFIG_ACTION_REMOVE supported)\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }
  if (ctxt_pP->module_id) {
    LOG_E(RLC, "%s:%d:%s: todo (only module_id 0 supported)\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }
  if ((srb_flagP && !(rb_idP >= 1 && rb_idP <= 2)) ||
      (!srb_flagP && !(rb_idP >= 1 && rb_idP <= MAX_DRBS_PER_UE))) {
    LOG_E(RLC, "%s:%d:%s: bad rb_id (%ld) (is_srb %d)\n", __FILE__, __LINE__, __FUNCTION__, rb_idP, srb_flagP);
    exit(1);
  }
  nr_rlc_manager_lock(nr_rlc_ue_manager);
  LOG_D(RLC, "%s:%d:%s: remove rb %ld (is_srb %d) for UE RNTI %lx\n", __FILE__, __LINE__, __FUNCTION__, rb_idP, srb_flagP, ctxt_pP->rntiMaybeUEid);
  ue = nr_rlc_manager_get_ue(nr_rlc_ue_manager, ctxt_pP->rntiMaybeUEid);
  if (srb_flagP) {
    if (ue->srb[rb_idP-1] != NULL) {
      ue->srb[rb_idP-1]->delete(ue->srb[rb_idP-1]);
      ue->srb[rb_idP-1] = NULL;
    } else
      LOG_W(RLC, "removing non allocated SRB %ld, do nothing\n", rb_idP);
  } else {
    if (ue->drb[rb_idP-1] != NULL) {
      ue->drb[rb_idP-1]->delete(ue->drb[rb_idP-1]);
      ue->drb[rb_idP-1] = NULL;
    } else
      LOG_W(RLC, "removing non allocated DRB %ld, do nothing\n", rb_idP);
  }
  /* remove UE if it has no more RB configured */
  for (i = 0; i < 2; i++)
    if (ue->srb[i] != NULL)
      break;
  if (i == 2) {
    for (i = 0; i < MAX_DRBS_PER_UE; i++)
      if (ue->drb[i] != NULL)
        break;
    if (i == MAX_DRBS_PER_UE)
      nr_rlc_manager_remove_ue(nr_rlc_ue_manager, ctxt_pP->rntiMaybeUEid);
  }
  nr_rlc_manager_unlock(nr_rlc_ue_manager);
  return RLC_OP_STATUS_OK;
}

void nr_rlc_remove_ue(int rnti)
{
  LOG_W(RLC, "remove UE %x\n", rnti);
  nr_rlc_manager_lock(nr_rlc_ue_manager);
  nr_rlc_manager_remove_ue(nr_rlc_ue_manager, rnti);
  nr_rlc_manager_unlock(nr_rlc_ue_manager);
}

rlc_op_status_t rrc_rlc_remove_ue (const protocol_ctxt_t* const x)
{
  nr_rlc_remove_ue(x->rntiMaybeUEid);
  return RLC_OP_STATUS_OK;
}

void nr_rlc_tick(int frame, int subframe)
{
  if (frame != nr_rlc_current_time_last_frame ||
      subframe != nr_rlc_current_time_last_subframe) {
    nr_rlc_current_time_last_frame = frame;
    nr_rlc_current_time_last_subframe = subframe;
    nr_rlc_current_time++;
  }
}

/* This is a hack, to compile the gNB.
 * TODO: remove it. The solution is to cleanup CMakeLists.txt
 */
void rlc_tick(int a, int b)
{
  LOG_E(RLC, "%s:%d:%s: this code should not be reached\n",
        __FILE__, __LINE__, __FUNCTION__);
  exit(1);
}

void nr_rlc_activate_avg_time_to_tx(
  const rnti_t            rnti,
  const logical_chan_id_t channel_id,
  const bool              is_on)
{
  nr_rlc_ue_t *ue;
  nr_rlc_entity_t *rb;

  nr_rlc_manager_lock(nr_rlc_ue_manager);
  ue = nr_rlc_manager_get_ue(nr_rlc_ue_manager, rnti);

  switch (channel_id) {
  case 1 ... 3: rb = ue->srb[channel_id - 1]; break;
  case 4 ... 8: rb = ue->drb[channel_id - 4]; break;
  default:      rb = NULL;                    break;
  }

  if (rb != NULL) {
    rb->avg_time_is_on = is_on;
    time_average_reset(rb->txsdu_avg_time_to_tx);
  } else {
    LOG_E(RLC, "[%s] Radio Bearer (channel ID %d) is NULL for UE with rnti %x\n", __FUNCTION__, channel_id, rnti);
  }

  nr_rlc_manager_unlock(nr_rlc_ue_manager);
}

/* returns false in case of error, true if everything ok */
const bool nr_rlc_get_statistics(
  int rnti,
  int srb_flag,
  int rb_id,
  nr_rlc_statistics_t *out)
{
  nr_rlc_ue_t     *ue;
  nr_rlc_entity_t *rb;
  bool             ret;

  nr_rlc_manager_lock(nr_rlc_ue_manager);
  ue = nr_rlc_manager_get_ue(nr_rlc_ue_manager, rnti);

  rb = NULL;

  if (srb_flag) {
    if (rb_id >= 1 && rb_id <= 2)
      rb = ue->srb[rb_id - 1];
  } else {
    if (rb_id >= 1 && rb_id <= 5)
      rb = ue->drb[rb_id - 1];
  }

  if (rb != NULL) {
    rb->get_stats(rb, out);
    ret = true;
  } else {
    ret = false;
  }

  nr_rlc_manager_unlock(nr_rlc_ue_manager);

  return ret;
}

void nr_rlc_srb_recv_sdu(const int rnti, const logical_chan_id_t channel_id, unsigned char *buf, int size)
{
  nr_rlc_ue_t *ue;
  nr_rlc_entity_t *rb;

  T(T_ENB_RLC_DL, T_INT(0), T_INT(rnti), T_INT(0), T_INT(size));

  nr_rlc_manager_lock(nr_rlc_ue_manager);
  ue = nr_rlc_manager_get_ue(nr_rlc_ue_manager, rnti);

  if (channel_id == 0) {
    rb = ue->srb0;
  } else {
    rb = ue->srb[channel_id - 1];
  }

  AssertFatal(rb != NULL, "SDU sent to unknown RB RNTI %04x SRB %d\n", rnti, channel_id);

  rb->set_time(rb, nr_rlc_current_time);
  rb->recv_sdu(rb, (char *)buf, size, -1);

  nr_rlc_manager_unlock(nr_rlc_ue_manager);
}
