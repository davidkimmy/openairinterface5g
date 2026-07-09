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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "nr_pdcp_asn1_utils.h"
#include "nr_pdcp_ue_manager.h"
#include "nr_pdcp_timer_thread.h"
#include "NR_RadioBearerConfig.h"
#include "NR_RLC-BearerConfig.h"
#include "NR_RLC-Config.h"
#include "NR_CellGroupConfig.h"
#include "openair2/RRC/NR/nr_rrc_proto.h"
#include <stdint.h>

/* from OAI */
#include "oai_asn1.h"
#include "nr_pdcp_oai_api.h"
#include "LAYER2/nr_rlc/nr_rlc_oai_api.h"
#include <openair3/ocp-gtpu/gtp_itf.h>
#include "openair2/SDAP/nr_sdap/nr_sdap.h"
#include "nr_pdcp_e1_api.h"
#include "gnb_config.h"
#include "executables/softmodem-common.h"

// For SL Mode 2 sourceL2Id support - include MAC defs after gnb_config to avoid conflicts
#include "LAYER2/NR_MAC_UE/mac_defs.h"
// Weak stub: get_mac_inst is only available in UE builds (overridden by UE MAC implementation)
__attribute__((weak)) NR_UE_MAC_INST_t *get_mac_inst(uint8_t module_id) {
  return NULL;  // gNB builds don't have UE MAC
}

#include "LAYER2/nr_srap/nr_srap_entity.h"
#include "LAYER2/nr_srap/nr_srap_header.h"
#include "LAYER2/nr_srap/nr_srap_manager.h"
#include "LAYER2/nr_srap/nr_srap_oai_api.h"

extern nr_srap_manager_t *nr_srap_manager;

// Forward declare gNB RRC types and function
struct rrc_gNB_ue_context_s;

/* Weak helper: check if a UE is a Remote UE (gNB only)
   Returns true if this is a Remote UE, false otherwise
   In UE builds, this weak symbol always returns false */
__attribute__((weak)) bool rrc_get_remote_ue_relay_info(rnti_t ue_rnti, rnti_t *relay_ue_rnti, uint8_t *remote_ue_id) {
  return false;  // UE builds: no Remote UE concept
}
#define TODO do { \
    printf("%s:%d:%s: todo\n", __FILE__, __LINE__, __FUNCTION__); \
    exit(1); \
  } while (0)

nr_pdcp_ue_manager_t *nr_pdcp_ue_manager;

/* TODO: handle time a bit more properly */
static uint64_t nr_pdcp_current_time;
static int      nr_pdcp_current_time_last_frame;
static int      nr_pdcp_current_time_last_subframe;

/* necessary globals for OAI, not used internally */
hash_table_t  *pdcp_coll_p;
static uint64_t pdcp_optmask;

uint8_t first_dcch = 0;
uint8_t proto_agent_flag = 0;

static ngran_node_t node_type;

/****************************************************************************/
/* rlc_data_req queue - begin                                               */
/****************************************************************************/


#include <pthread.h>

/* NR PDCP and RLC both use "big locks". In some cases a thread may do
 * lock(rlc) followed by lock(pdcp) (typically when running 'rx_sdu').
 * Another thread may first do lock(pdcp) and then lock(rlc) (typically
 * the GTP module calls 'nr_pdcp_data_req' that, in a previous implementation
 * was indirectly calling 'rlc_data_req' which does lock(rlc)).
 * To avoid the resulting deadlock it is enough to ensure that a call
 * to lock(pdcp) will never be followed by a call to lock(rlc). So,
 * here we chose to have a separate thread that deals with rlc_data_req,
 * out of the PDCP lock. Other solutions may be possible.
 * So instead of calling 'rlc_data_req' directly we have a queue and a
 * separate thread emptying it.
 */

typedef struct {
  protocol_ctxt_t ctxt_pP;
  srb_flag_t      srb_flagP;
  MBMS_flag_t     MBMS_flagP;
  rb_id_t         rb_idP;
  mui_t           muiP;
  confirm_t       confirmP;
  sdu_size_t      sdu_sizeP;
  mem_block_t     *sdu_pP;
} rlc_data_req_queue_item;

#define RLC_DATA_REQ_QUEUE_SIZE 10000

typedef struct {
  rlc_data_req_queue_item q[RLC_DATA_REQ_QUEUE_SIZE];
  volatile int start;
  volatile int length;
  pthread_mutex_t m;
  pthread_cond_t c;
} rlc_data_req_queue;

static rlc_data_req_queue q;
static rlc_data_req_queue pc5_q;

static void *rlc_data_req_thread(void *_)
{
  int i;

  LOG_I(RLC, "rlc_data_req_thread created on core %d\n", sched_getcpu());
  pthread_setname_np(pthread_self(), "RLC queue");
  while (1) {
    if (pthread_mutex_lock(&q.m) != 0) abort();
    while (q.length == 0)
      if (pthread_cond_wait(&q.c, &q.m) != 0) abort();
    i = q.start;
    if (pthread_mutex_unlock(&q.m) != 0) abort();

    rlc_data_req(&q.q[i].ctxt_pP,
                 q.q[i].srb_flagP,
                 q.q[i].MBMS_flagP,
                 q.q[i].rb_idP,
                 q.q[i].muiP,
                 q.q[i].confirmP,
                 q.q[i].sdu_sizeP,
                 q.q[i].sdu_pP,
                 NULL,
                 NULL,
                 UU);

    if (pthread_mutex_lock(&q.m) != 0) abort();

    q.length--;
    q.start = (q.start + 1) % RLC_DATA_REQ_QUEUE_SIZE;

    if (pthread_cond_signal(&q.c) != 0) abort();
    if (pthread_mutex_unlock(&q.m) != 0) abort();
  }
}

static void *rlc_data_req_thread_pc5(void *_)
{
  int i;

  LOG_I(RLC, "rlc_data_req_thread_pc5 created on core %d\n", sched_getcpu());
  pthread_setname_np(pthread_self(), "RLC PC5 queue");
  while (1) {
    if (pthread_mutex_lock(&pc5_q.m) != 0) abort();
    while (pc5_q.length == 0)
      if (pthread_cond_wait(&pc5_q.c, &pc5_q.m) != 0) abort();
    i = pc5_q.start;
    if (pthread_mutex_unlock(&pc5_q.m) != 0) abort();

    rlc_data_req(&pc5_q.q[i].ctxt_pP,
                 pc5_q.q[i].srb_flagP,
                 pc5_q.q[i].MBMS_flagP,
                 pc5_q.q[i].rb_idP,
                 pc5_q.q[i].muiP,
                 pc5_q.q[i].confirmP,
                 pc5_q.q[i].sdu_sizeP,
                 pc5_q.q[i].sdu_pP,
                 NULL,
                 NULL,
                 PC5);

    if (pthread_mutex_lock(&pc5_q.m) != 0) abort();

    pc5_q.length--;
    pc5_q.start = (pc5_q.start + 1) % RLC_DATA_REQ_QUEUE_SIZE;

    if (pthread_cond_signal(&pc5_q.c) != 0) abort();
    if (pthread_mutex_unlock(&pc5_q.m) != 0) abort();
  }
}

static void init_nr_rlc_data_req_queue(void)
{
  pthread_t t1, t2;

  pthread_mutex_init(&q.m, NULL);
  pthread_cond_init(&q.c, NULL);
  threadCreate(&t1, rlc_data_req_thread, NULL, "rlc_data_req_thread", -1, OAI_PRIORITY_RT_MAX - 1);

  if ((get_softmodem_params()->sl_mode >= 1) && (node_type == -1)) {
    pthread_mutex_init(&pc5_q.m, NULL);
    pthread_cond_init(&pc5_q.c, NULL);
    threadCreate(&t2, rlc_data_req_thread_pc5, NULL, "rlc_data_req_thread_pc5", -1, OAI_PRIORITY_RT_MAX - 1);
  }
}

static void enqueue_rlc_data_req(const protocol_ctxt_t *const ctxt_pP,
                                 const srb_flag_t   srb_flagP,
                                 const MBMS_flag_t  MBMS_flagP,
                                 const rb_id_t      rb_idP,
                                 const mui_t        muiP,
                                 confirm_t    confirmP,
                                 sdu_size_t   sdu_sizeP,
                                 mem_block_t *sdu_pP,
                                 nr_intf_type_t intf_type)
{
  int i;
  int logged = 0;
  if (intf_type == PC5) {
    if (pthread_mutex_lock(&pc5_q.m) != 0) abort();
    while (pc5_q.length == RLC_DATA_REQ_QUEUE_SIZE) {
      if (!logged) {
        logged = 1;
        LOG_W(PDCP, "%s: rlc_data_req PC5 queue is full\n", __FUNCTION__);
      }
      if (pthread_cond_wait(&pc5_q.c, &pc5_q.m) != 0) abort();
    }

    i = (pc5_q.start + pc5_q.length) % RLC_DATA_REQ_QUEUE_SIZE;
    pc5_q.length++;

    pc5_q.q[i].ctxt_pP    = *ctxt_pP;
    pc5_q.q[i].srb_flagP  = srb_flagP;
    pc5_q.q[i].MBMS_flagP = MBMS_flagP;
    pc5_q.q[i].rb_idP     = rb_idP;
    pc5_q.q[i].muiP       = muiP;
    pc5_q.q[i].confirmP   = confirmP;
    pc5_q.q[i].sdu_sizeP  = sdu_sizeP;
    pc5_q.q[i].sdu_pP     = sdu_pP;

    if (pthread_cond_signal(&pc5_q.c) != 0) abort();
    if (pthread_mutex_unlock(&pc5_q.m) != 0) abort();
  } else if (intf_type == UU) {
    if (pthread_mutex_lock(&q.m) != 0) abort();
    while (q.length == RLC_DATA_REQ_QUEUE_SIZE) {
      if (!logged) {
        logged = 1;
        LOG_W(PDCP, "%s: rlc_data_req queue is full\n", __FUNCTION__);
      }
      if (pthread_cond_wait(&q.c, &q.m) != 0) abort();
    }

    i = (q.start + q.length) % RLC_DATA_REQ_QUEUE_SIZE;
    q.length++;

    q.q[i].ctxt_pP    = *ctxt_pP;
    q.q[i].srb_flagP  = srb_flagP;
    q.q[i].MBMS_flagP = MBMS_flagP;
    q.q[i].rb_idP     = rb_idP;
    q.q[i].muiP       = muiP;
    q.q[i].confirmP   = confirmP;
    q.q[i].sdu_sizeP  = sdu_sizeP;
    q.q[i].sdu_pP     = sdu_pP;

    if (pthread_cond_signal(&q.c) != 0) abort();
    if (pthread_mutex_unlock(&q.m) != 0) abort();
  }
}

void du_rlc_data_req(const protocol_ctxt_t *const ctxt_pP,
                     const srb_flag_t   srb_flagP,
                     const MBMS_flag_t  MBMS_flagP,
                     const rb_id_t      rb_idP,
                     const mui_t        muiP,
                     confirm_t    confirmP,
                     sdu_size_t   sdu_sizeP,
                     mem_block_t *sdu_pP)
{
  enqueue_rlc_data_req(ctxt_pP,
                       srb_flagP,
                       MBMS_flagP,
                       rb_idP, muiP,
                       confirmP,
                       sdu_sizeP,
                       sdu_pP,
                       UU);
}

/****************************************************************************/
/* rlc_data_req queue - end                                                 */
/****************************************************************************/

/****************************************************************************/
/* pdcp_data_ind thread - begin                                             */
/****************************************************************************/

typedef struct {
  protocol_ctxt_t ctxt_pP;
  srb_flag_t      srb_flagP;
  MBMS_flag_t     MBMS_flagP;
  rb_id_t         rb_id;
  sdu_size_t      sdu_buffer_size;
  mem_block_t     *sdu_buffer;
} pdcp_data_ind_queue_item;

#define PDCP_DATA_IND_QUEUE_SIZE 10000

typedef struct {
  pdcp_data_ind_queue_item q[PDCP_DATA_IND_QUEUE_SIZE];
  volatile int start;
  volatile int length;
  pthread_mutex_t m;
  pthread_cond_t c;
} pdcp_data_ind_queue;

static pdcp_data_ind_queue rx_pq_pc5;
static pdcp_data_ind_queue pq;

static void do_pdcp_data_ind(
  const protocol_ctxt_t *const  ctxt_pP,
  const srb_flag_t srb_flagP,
  const MBMS_flag_t MBMS_flagP,
  const rb_id_t rb_id,
  const sdu_size_t sdu_buffer_size,
  mem_block_t *const sdu_buffer,
  nr_intf_type_t intf_type)
{
  nr_pdcp_ue_t *ue;
  nr_pdcp_entity_t *rb;
  ue_id_t rntiMaybeUEid = ctxt_pP->rntiMaybeUEid;

  if (ctxt_pP->module_id != 0 ||
      //ctxt_pP->enb_flag != 1 ||
      ctxt_pP->instance != 0 ||
      ctxt_pP->eNB_index != 0 ||
      ctxt_pP->brOption != 0) {
    LOG_E(PDCP, "%s:%d:%s: fatal\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }

  if (ctxt_pP->enb_flag)
    T(T_ENB_PDCP_UL, T_INT(ctxt_pP->module_id), T_INT(rntiMaybeUEid), T_INT(rb_id), T_INT(sdu_buffer_size));

  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, rntiMaybeUEid);

  if (srb_flagP == 1) {
    if (rb_id < 1 || rb_id > 2)
      rb = NULL;
    else {
      if (intf_type == PC5)
        rb = ue->sl_srb[rb_id - 1];
      else
        rb = ue->srb[rb_id - 1];
    }
  } else {
    if (rb_id < 1 || rb_id > MAX_DRBS_PER_UE)
      rb = NULL;
    else
      rb = ue->drb[rb_id - 1];
  }

  if (rb != NULL) {
      rb->recv_pdu(rb, (char *)sdu_buffer->data, sdu_buffer_size);
  } else {
    /* This can happen during initial setup when data arrives before DRB is configured
       It's a race condition but harmless - the packet is discarded and will be retransmitted */
    LOG_W(PDCP, "[PDCP_EARLY_DATA] Data arrived before DRB configured: ue_id=0x%04lx, rb_id=%ld, srb_flag=%d, size=%d (discarding, will be retransmitted)\n",
          ctxt_pP->rntiMaybeUEid, rb_id, srb_flagP, sdu_buffer_size);
  }

  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);

  free_mem_block(sdu_buffer, __FUNCTION__);
}

static void *pdcp_data_ind_thread(void *_)
{
  int i;

  pthread_setname_np(pthread_self(), "PDCP data ind");
  while (1) {
    if (pthread_mutex_lock(&pq.m) != 0) abort();
    while (pq.length == 0)
      if (pthread_cond_wait(&pq.c, &pq.m) != 0) abort();
    i = pq.start;
    if (pthread_mutex_unlock(&pq.m) != 0) abort();

    do_pdcp_data_ind(&pq.q[i].ctxt_pP,
                     pq.q[i].srb_flagP,
                     pq.q[i].MBMS_flagP,
                     pq.q[i].rb_id,
                     pq.q[i].sdu_buffer_size,
                     pq.q[i].sdu_buffer,
                     UU);

    if (pthread_mutex_lock(&pq.m) != 0) abort();

    pq.length--;
    pq.start = (pq.start + 1) % PDCP_DATA_IND_QUEUE_SIZE;

    if (pthread_cond_signal(&pq.c) != 0) abort();
    if (pthread_mutex_unlock(&pq.m) != 0) abort();
  }
}

static void *pdcp_pc5_data_ind_thread(void *_)
{
  int i;

  pthread_setname_np(pthread_self(), "PDCP data ind");
  while (1) {
    if (pthread_mutex_lock(&rx_pq_pc5.m) != 0) abort();
    while (rx_pq_pc5.length == 0)
      if (pthread_cond_wait(&rx_pq_pc5.c, &rx_pq_pc5.m) != 0) abort();
    i = rx_pq_pc5.start;
    if (pthread_mutex_unlock(&rx_pq_pc5.m) != 0) abort();

    do_pdcp_data_ind(&rx_pq_pc5.q[i].ctxt_pP,
                     rx_pq_pc5.q[i].srb_flagP,
                     rx_pq_pc5.q[i].MBMS_flagP,
                     rx_pq_pc5.q[i].rb_id,
                     rx_pq_pc5.q[i].sdu_buffer_size,
                     rx_pq_pc5.q[i].sdu_buffer,
                     PC5);

    if (pthread_mutex_lock(&rx_pq_pc5.m) != 0) abort();

    rx_pq_pc5.length--;
    rx_pq_pc5.start = (rx_pq_pc5.start + 1) % PDCP_DATA_IND_QUEUE_SIZE;

    if (pthread_cond_signal(&rx_pq_pc5.c) != 0) abort();
    if (pthread_mutex_unlock(&rx_pq_pc5.m) != 0) abort();
  }
}

static void init_nr_pdcp_data_ind_queue(bool gNB_flag)
{
  pthread_t t1, t2;
  if (!gNB_flag) {
    pthread_mutex_init(&rx_pq_pc5.m, NULL);
    pthread_cond_init(&rx_pq_pc5.c, NULL);
    if (pthread_create(&t1, NULL, pdcp_pc5_data_ind_thread, NULL) != 0) {
      LOG_E(PDCP, "%s:%d:%s: fatal\n", __FILE__, __LINE__, __FUNCTION__);
      exit(1);
    }
  }

  pthread_mutex_init(&pq.m, NULL);
  pthread_cond_init(&pq.c, NULL);
  if (pthread_create(&t2, NULL, pdcp_data_ind_thread, NULL) != 0) {
    LOG_E(PDCP, "%s:%d:%s: fatal\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }
}

static void enqueue_pdcp_pc5_data_ind(
  const protocol_ctxt_t *const ctxt_pP,
  const srb_flag_t srb_flagP,
  const MBMS_flag_t MBMS_flagP,
  const rb_id_t rb_id,
  const sdu_size_t sdu_buffer_size,
  mem_block_t *const sdu_buffer)
{
  int i;
  int logged = 0;

  if (pthread_mutex_lock(&rx_pq_pc5.m) != 0) abort();
  while (rx_pq_pc5.length == PDCP_DATA_IND_QUEUE_SIZE) {
    if (!logged) {
      logged = 1;
      LOG_W(PDCP, "%s: pdcp_data_ind queue is full\n", __FUNCTION__);
    }
    if (pthread_cond_wait(&rx_pq_pc5.c, &rx_pq_pc5.m) != 0) abort();
  }

  i = (rx_pq_pc5.start + rx_pq_pc5.length) % PDCP_DATA_IND_QUEUE_SIZE;
  rx_pq_pc5.length++;

  rx_pq_pc5.q[i].ctxt_pP         = *ctxt_pP;
  rx_pq_pc5.q[i].srb_flagP       = srb_flagP;
  rx_pq_pc5.q[i].MBMS_flagP      = MBMS_flagP;
  rx_pq_pc5.q[i].rb_id           = rb_id;
  rx_pq_pc5.q[i].sdu_buffer_size = sdu_buffer_size;
  rx_pq_pc5.q[i].sdu_buffer      = sdu_buffer;

  if (pthread_cond_signal(&rx_pq_pc5.c) != 0) abort();
  if (pthread_mutex_unlock(&rx_pq_pc5.m) != 0) abort();
}

static void enqueue_pdcp_data_ind(
  const protocol_ctxt_t *const  ctxt_pP,
  const srb_flag_t srb_flagP,
  const MBMS_flag_t MBMS_flagP,
  const rb_id_t rb_id,
  const sdu_size_t sdu_buffer_size,
  mem_block_t *const sdu_buffer)
{
  int i;
  int logged = 0;

  if (pthread_mutex_lock(&pq.m) != 0) abort();
  while (pq.length == PDCP_DATA_IND_QUEUE_SIZE) {
    if (!logged) {
      logged = 1;
      LOG_W(PDCP, "%s: pdcp_data_ind queue is full\n", __FUNCTION__);
    }
    if (pthread_cond_wait(&pq.c, &pq.m) != 0) abort();
  }

  i = (pq.start + pq.length) % PDCP_DATA_IND_QUEUE_SIZE;
  pq.length++;

  pq.q[i].ctxt_pP         = *ctxt_pP;
  pq.q[i].srb_flagP       = srb_flagP;
  pq.q[i].MBMS_flagP      = MBMS_flagP;
  pq.q[i].rb_id           = rb_id;
  pq.q[i].sdu_buffer_size = sdu_buffer_size;
  pq.q[i].sdu_buffer      = sdu_buffer;

  if (pthread_cond_signal(&pq.c) != 0) abort();
  if (pthread_mutex_unlock(&pq.m) != 0) abort();
}

bool pdcp_data_ind(const protocol_ctxt_t *const  ctxt_pP,
                   const srb_flag_t srb_flagP,
                   const MBMS_flag_t MBMS_flagP,
                   const rb_id_t rb_id,
                   const sdu_size_t sdu_buffer_size,
                   mem_block_t *const sdu_buffer,
                   const uint32_t *const srcID,
                   const uint32_t *const dstID,
                   nr_intf_type_t intf_type)
{
  if (intf_type == PC5)
    enqueue_pdcp_pc5_data_ind(ctxt_pP,
                              srb_flagP,
                              MBMS_flagP,
                              rb_id,
                              sdu_buffer_size,
                              sdu_buffer);
  else
    enqueue_pdcp_data_ind(ctxt_pP,
                          srb_flagP,
                          MBMS_flagP,
                          rb_id,
                          sdu_buffer_size,
                          sdu_buffer);
  return true;
}

/****************************************************************************/
/* pdcp_data_ind thread - end                                               */
/****************************************************************************/

/****************************************************************************/
/* hacks to be cleaned up at some point - begin                             */
/****************************************************************************/

#include "LAYER2/MAC/mac_extern.h"

static void reblock_tun_socket(int id)
{
  extern int nas_sock_fd[];
  int f;
  /* Calculate index to match netlink_init.c storage:
   * When id > 0: stored at index (id - 1) based on loop variable i = begx = id - 1
   * When id == 0 or id == -1: use index 0 for backwards compatibility */
  int index = (id <= 0) ? 0 : (id - 1);
  f = fcntl(nas_sock_fd[index], F_GETFL, 0);
  f &= ~(O_NONBLOCK);
  if (fcntl(nas_sock_fd[index], F_SETFL, f) == -1) {
    LOG_E(PDCP, "reblock_tun_socket failed for id=%d, index=%d\n", id, index);
    exit(1);
  }
}

static void *enb_tun_read_thread(void *_)
{
  extern int nas_sock_fd[];
  char rx_buf[NL_MAX_PAYLOAD];
  int len;
  protocol_ctxt_t ctxt;
  ue_id_t rntiMaybeUEid;

  int rb_id = 1;
  pthread_setname_np( pthread_self(),"enb_tun_read");
  while (1) {
    len = read(nas_sock_fd[0], &rx_buf, NL_MAX_PAYLOAD);
    if (len == -1) {
      LOG_E(PDCP, "%s:%d:%s: fatal\n", __FILE__, __LINE__, __FUNCTION__);
      exit(1);
    }

    LOG_D(PDCP, "%s(): nas_sock_fd read returns len %d\n", __func__, len);

    nr_pdcp_manager_lock(nr_pdcp_ue_manager);
    const bool has_ue = nr_pdcp_get_first_ue_id(nr_pdcp_ue_manager, &rntiMaybeUEid);
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);

    if (!has_ue) continue;

    ctxt.module_id = 0;
    ctxt.enb_flag = 1;
    ctxt.instance = 0;
    ctxt.frame = 0;
    ctxt.subframe = 0;
    ctxt.eNB_index = 0;
    ctxt.brOption = 0;
    ctxt.rntiMaybeUEid = rntiMaybeUEid;

    uint8_t qfi = 1; // SDAP entity was created for qfi 1 in gNB
    bool rqi = 0;
    int pdusession_id = 10;

    sdap_data_req(&ctxt, rntiMaybeUEid, SRB_FLAG_NO, rb_id, RLC_MUI_UNDEFINED, RLC_SDU_CONFIRM_NO, len, (unsigned char *)rx_buf, PDCP_TRANSMISSION_MODE_DATA, NULL, NULL, qfi, rqi, pdusession_id);
  }

  return NULL;
}

static void *ue_tun_read_thread(void *arg)
{
  extern int nas_sock_fd[];
  char rx_buf[NL_MAX_PAYLOAD];
  int len;
  protocol_ctxt_t ctxt;
  ue_id_t rntiMaybeUEid;
  int has_ue;

  bool relay_enabled = get_softmodem_params()->relay_type > 0 ? true : false;
  bool is_relay_ue = get_softmodem_params()->is_relay_ue;
  /* Calculate index to match netlink_init.c storage:
   * When id > 0: stored at index (id - 1) based on loop variable i = begx = id - 1
   * When id == 0: use index 0 for backwards compatibility */
  int id = (int)(intptr_t)arg;
  int index = (id <= 0) ? 0 : (id - 1);
  int rb_id = 1;
  rb_id = ((get_softmodem_params()->sl_mode == 2) && relay_enabled && !is_relay_ue) ? 2 : rb_id;
  pthread_setname_np( pthread_self(),"ue_tun_read");
  LOG_I(PDCP,"ue_tun_read_thread created on core %d for id=%d, using nas_sock_fd[%d]\n",sched_getcpu(), id, index);
  while (1) {
    len = read(nas_sock_fd[index], &rx_buf, NL_MAX_PAYLOAD);
    if (len == -1) {
      LOG_E(PDCP, "%s:%d:%s: fatal reading from nas_sock_fd[%d]\n", __FILE__, __LINE__, __FUNCTION__, index);
      exit(1);
    }

    LOG_D(PDCP, "%s(): nas_sock_fd read returns len %d\n", __func__, len);

    // For SL Mode 2, use sourceL2Id (src_id) instead of RNTI
    if (get_softmodem_params()->sl_mode == 2) {
      NR_UE_MAC_INST_t *mac = get_mac_inst(0);
      if (mac == NULL) {
        continue;
      }
      // Note: src_id can be 0, which is a valid sourceL2Id
      rntiMaybeUEid = mac->src_id;
      has_ue = true;
    } else {
      nr_pdcp_manager_lock(nr_pdcp_ue_manager);
      has_ue = nr_pdcp_get_first_ue_id(nr_pdcp_ue_manager, &rntiMaybeUEid);
      nr_pdcp_manager_unlock(nr_pdcp_ue_manager);

      if (!has_ue) {
        continue;
      }
    }

    ctxt.module_id = 0;
    ctxt.enb_flag = 0;
    ctxt.instance = 0;
    ctxt.frame = 0;
    ctxt.subframe = 0;
    ctxt.eNB_index = 0;
    ctxt.brOption = 0;
    ctxt.rntiMaybeUEid = rntiMaybeUEid;

    bool dc = SDAP_HDR_UL_DATA_PDU;
    extern uint8_t nas_qfi;
    extern uint8_t nas_pduid;
    sdap_data_req(&ctxt, rntiMaybeUEid, SRB_FLAG_NO, rb_id, RLC_MUI_UNDEFINED, RLC_SDU_CONFIRM_NO, len, (unsigned char *)rx_buf, PDCP_TRANSMISSION_MODE_DATA, NULL, NULL, nas_qfi, dc, nas_pduid);
  }

  return NULL;
}

static void start_pdcp_tun_enb(void)
{
  pthread_t t;

  reblock_tun_socket(-1);

  if (pthread_create(&t, NULL, enb_tun_read_thread, NULL) != 0) {
    LOG_E(PDCP, "%s:%d:%s: fatal\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }
}

static void start_pdcp_tun_ue(int id)
{
  pthread_t t;
  reblock_tun_socket(id);
/*
  if (pthread_create(&t, NULL, ue_tun_read_thread, (void*)(intptr_t)id) != 0) {
    LOG_E(PDCP, "%s:%d:%s: fatal\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }*/
  threadCreate(&t, ue_tun_read_thread, (void*)(intptr_t)id, "ue_tun_read_thread", -1, OAI_PRIORITY_RT_MAX - 1);
}

/****************************************************************************/
/* hacks to be cleaned up at some point - end                               */
/****************************************************************************/

int pdcp_fifo_flush_sdus(const protocol_ctxt_t *const ctxt_pP)
{
  return 0;
}

static void set_node_type() {
  node_type = get_node_type();
}

/* hack: dummy function needed due to LTE dependencies */
void pdcp_layer_init(void)
{
  abort();
}

void nr_pdcp_layer_init(bool gNB_flag)
{
  /* hack: be sure to initialize only once */
  static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
  static int initialized = 0;
  if (pthread_mutex_lock(&m) != 0) abort();
  if (initialized) {
    if (pthread_mutex_unlock(&m) != 0) abort();
    return;
  }
  initialized = 1;
  if (pthread_mutex_unlock(&m) != 0) abort();

  nr_pdcp_ue_manager = new_nr_pdcp_ue_manager(1);

  set_node_type();

  if ((RC.nrrrc == NULL) || (!NODE_IS_CU(node_type))) {
    init_nr_rlc_data_req_queue();
  }

  init_nr_pdcp_data_ind_queue(gNB_flag);
  nr_pdcp_init_timer_thread(nr_pdcp_ue_manager);
}

#include "nfapi/oai_integration/vendor_ext.h"
#include "executables/lte-softmodem.h"
#include "openair2/RRC/NAS/nas_config.h"

uint64_t nr_pdcp_module_init(uint64_t _pdcp_optmask, int id)
{
  /* hack: be sure to initialize only once */
  static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
  static int initialized = 0;
  if (pthread_mutex_lock(&m) != 0) abort();
  if (initialized) {
    abort();
  }
  initialized = 1;
  if (pthread_mutex_unlock(&m) != 0) abort();

#if 0
  pdcp_optmask = _pdcp_optmask;
  return pdcp_optmask;
#endif
  /* temporary enforce netlink when UE_NAS_USE_TUN is set,
     this is while switching from noS1 as build option
     to noS1 as config option                               */
  if ( _pdcp_optmask & UE_NAS_USE_TUN_BIT) {
    pdcp_optmask = pdcp_optmask | PDCP_USE_NETLINK_BIT ;
  }

  pdcp_optmask = pdcp_optmask | _pdcp_optmask ;
  LOG_I(PDCP, "pdcp init,%s %s\n",
        ((LINK_ENB_PDCP_TO_GTPV1U)?"usegtp":""),
        ((PDCP_USE_NETLINK)?"usenetlink":""));

  if (PDCP_USE_NETLINK) {
    nas_getparams();

    if(UE_NAS_USE_TUN) {
      char *ifsuffix_ue = get_softmodem_params()->nsa ? "nrue" : "ue";
      int num_if = (NFAPI_MODE == NFAPI_UE_STUB_PNF || IS_SOFTMODEM_SIML1 || NFAPI_MODE == NFAPI_MODE_STANDALONE_PNF)? MAX_MOBILES_PER_ENB : 1;
      netlink_init_tun(ifsuffix_ue, num_if, id);
      //Add --nr-ip-over-lte option check for next line
      if (IS_SOFTMODEM_NOS1){
        nas_config(get_tun_iface_id(), 1, !get_softmodem_params()->nsa ? 2 : 3, ifsuffix_ue);
        int pdusession_id = 10;
        set_qfi_pduid(7, pdusession_id);
      }
      LOG_I(PDCP, "UE pdcp will use tun interface\n");
      start_pdcp_tun_ue(id);
    } else if(ENB_NAS_USE_TUN) {
      char *ifsuffix_base_s = get_softmodem_params()->nsa ? "gnb" : "enb";
      netlink_init_tun(ifsuffix_base_s, 1, id);
      nas_config(1, 1, 1, ifsuffix_base_s);
      LOG_I(PDCP, "ENB pdcp will use tun interface\n");
      start_pdcp_tun_enb();
    } else {
      LOG_I(PDCP, "pdcp will use kernel modules\n");
      abort();
      netlink_init();
    }
  }
  return pdcp_optmask ;
}

static void deliver_sdu_drb(void *_ue, nr_pdcp_entity_t *entity,
                            char *buf, int size)
{
  nr_pdcp_ue_t *ue = _ue;
  int rb_id;
  int i;

  if (IS_SOFTMODEM_NOS1 || UE_NAS_USE_TUN) {
    LOG_D(PDCP, "IP packet received with size %d, to be sent to SDAP interface, UE ID/RNTI: %ld\n", size, ue->rntiMaybeUEid);
    sdap_data_ind(entity->rb_id, entity->is_gnb, entity->has_sdap_rx, entity->pdusession_id, ue->rntiMaybeUEid, buf, size);
  }
  else{
    for (i = 0; i < MAX_DRBS_PER_UE; i++) {
        if (entity == ue->drb[i]) {
          rb_id = i+1;
          goto rb_found;
        }
      }

      LOG_E(PDCP, "%s:%d:%s: fatal, no RB found for UE ID/RNTI %ld\n", __FILE__, __LINE__, __FUNCTION__, ue->rntiMaybeUEid);
      exit(1);

    rb_found:
    {
      LOG_D(PDCP, "%s() (drb %d) sending message to SDAP size %d with pdusession_id %d \n",
                  __func__, rb_id, size, ue->drb[rb_id - 1]->pdusession_id);
      sdap_data_ind(rb_id, ue->drb[rb_id - 1]->is_gnb, ue->drb[rb_id - 1]->has_sdap_rx, ue->drb[rb_id - 1]->pdusession_id, ue->rntiMaybeUEid, buf, size);
    }
  }
}

static void deliver_pdu_drb(void *deliver_pdu_data, ue_id_t ue_id, int rb_id,
                            char *buf, int size, int sdu_id, nr_intf_type_t intf_type)
{
  DevAssert(deliver_pdu_data == NULL);
  protocol_ctxt_t ctxt = { .enb_flag = 1, .rntiMaybeUEid = ue_id };

  bool srap_enabled = get_softmodem_params()->relay_type > 0 ? true : false;
  // We only send to SRAP from PDCP if we are CU (split occurs at PDCP/SRAP) or a standard gNB
  bool gNB_flag = (NODE_IS_MONOLITHIC(node_type) || NODE_IS_CU(node_type));
  bool is_relay_ue = get_softmodem_params()->is_relay_ue;
  bool remote_UE_flag = ((node_type == -1) && !is_relay_ue);

  /* CRITICAL: Check if this is gNB downlink for Remote UE
     Remote UE has no RLC configured - must forward via SRAP to Relay UE */
  if (gNB_flag && srap_enabled) {
    rnti_t relay_ue_rnti;
    uint8_t remote_ue_id;

    // Check if this UE is a Remote UE (uses weak symbol, always false in UE builds)
    if (rrc_get_remote_ue_relay_info(ue_id, &relay_ue_rnti, &remote_ue_id)) {
      /* Forward to SRAP with Relay UE's RNTI.
         Pass the Remote UE's LOGICAL DRB id (rb_id) so the SRAP header bearer_id
         encodes the Remote UE's own bearer (DRB1 -> bearer 4), matching the uplink.
         The Uu TRANSPORT is remapped to the Relay UE's dedicated DRB2 inside
         srap_deliver_pdu_drb (keyed on ctxt.remote_ue_id > 0). */
      ctxt.rntiMaybeUEid = relay_ue_rnti;
      ctxt.remote_ue_id = remote_ue_id;  // CRITICAL: Pass Remote UE ID for SRAP header creation

      // Use original nr_srap_data_req_drb from episys/sl-mode1-relay
      nr_srap_data_req_drb(&ctxt, rb_id, sdu_id, size, buf, UU);
      return;
    }
  }

  if (NODE_IS_CU(node_type)) {
    MessageDef  *message_p = itti_alloc_new_message_sized(TASK_PDCP_ENB, 0,
							  GTPV1U_TUNNEL_DATA_REQ,
							  sizeof(gtpv1u_tunnel_data_req_t)
							  + size
							  + GTPU_HEADER_OVERHEAD_MAX);
    AssertFatal(message_p != NULL, "OUT OF MEMORY");
    gtpv1u_tunnel_data_req_t *req=&GTPV1U_TUNNEL_DATA_REQ(message_p);
    uint8_t *gtpu_buffer_p = (uint8_t*)(req+1);
    memcpy(gtpu_buffer_p+GTPU_HEADER_OVERHEAD_MAX,
	   buf, size);
    req->buffer        = gtpu_buffer_p;
    req->length        = size;
    req->offset        = GTPU_HEADER_OVERHEAD_MAX;
    req->ue_id = ue_id;
    req->bearer_id = rb_id;
    LOG_D(PDCP, "%s() (drb %d) sending message to gtp size %d\n", __func__, rb_id, size);
    extern instance_t CUuniqInstance;
    itti_send_msg_to_task(TASK_GTPV1_U, CUuniqInstance, message_p);
  } else if (remote_UE_flag && srap_enabled) { // only remote UE should send PDCP traffic
    rb_id = 1; // set rb_id to 1 to reduce RLC layer delay.
    nr_srap_data_req_drb(&ctxt, rb_id, sdu_id, size, buf, PC5);
  } else if (gNB_flag && srap_enabled && rb_id > 1) {
    rb_id = 1; // set rb_id to 1 to reduce RLC layer delay.
    nr_srap_data_req_drb(&ctxt, rb_id, sdu_id, size, buf, UU);
  } else { // without srap
    mem_block_t *memblock = get_free_mem_block(size, __FUNCTION__);
    memcpy(memblock->data, buf, size);
    LOG_D(PDCP, "%s(): (drb %d) calling rlc_data_req size %d\n", __func__, rb_id, size);
    //for (i = 0; i < size; i++) printf(" %2.2x", (unsigned char)memblock->data[i]);
    //printf("\n");
    // TODO: intf_type will need to be updated for SL Mode 1
    nr_intf_type_t intf_type = ((get_softmodem_params()->sl_mode == 2) && (node_type == -1)) ? PC5 : UU;
    enqueue_rlc_data_req(&ctxt, 0, MBMS_FLAG_NO, rb_id, sdu_id, 0, size, memblock, intf_type);
  }
}

static void deliver_sdu_srb(void *_ue, nr_pdcp_entity_t *entity,
                            char *buf, int size)
{
  nr_pdcp_ue_t *ue = _ue;
  int srb_id;
  int i;

  uint16_t srb_size = (entity->type == NR_PDCP_SRB) ? sizeofArray(ue->srb) : sizeofArray(ue->sl_srb);

  for (i = 0; i < srb_size ; i++) {
    nr_pdcp_entity_t *srb_entity = (entity->type == NR_PDCP_SRB) ? ue->srb[i] : ue->sl_srb[i];
    if (entity == srb_entity) {
      srb_id = i+1;
      goto srb_found;
    }
  }

  LOG_E(PDCP, "%s:%d:%s: fatal, no SRB found for UE ID/RNTI %ld\n", __FILE__, __LINE__, __FUNCTION__, ue->rntiMaybeUEid);
  exit(1);

srb_found:
  if (entity->is_gnb) {
    MessageDef *message_p = itti_alloc_new_message(TASK_PDCP_GNB, 0, F1AP_UL_RRC_MESSAGE);
    AssertFatal(message_p != NULL, "OUT OF MEMORY\n");
    f1ap_ul_rrc_message_t *ul_rrc = &F1AP_UL_RRC_MESSAGE(message_p);
    ul_rrc->rnti = ue->rntiMaybeUEid;
    ul_rrc->srb_id = srb_id;
    ul_rrc->rrc_container = malloc(size);
    AssertFatal(ul_rrc->rrc_container != NULL, "OUT OF MEMORY\n");
    memcpy(ul_rrc->rrc_container, buf, size);
    ul_rrc->rrc_container_length = size;
    itti_send_msg_to_task(TASK_RRC_GNB, 0, message_p);
  } else {
    uint8_t *rrc_buffer_p = itti_malloc(TASK_PDCP_UE, TASK_RRC_NRUE, size);
    AssertFatal(rrc_buffer_p != NULL, "OUT OF MEMORY\n");
    memcpy(rrc_buffer_p, buf, size);
    MessageDef *message_p = itti_alloc_new_message(TASK_PDCP_UE, 0, NR_RRC_DCCH_DATA_IND);
    AssertFatal(message_p != NULL, "OUT OF MEMORY\n");
    NR_RRC_DCCH_DATA_IND(message_p).dcch_index = srb_id;
    NR_RRC_DCCH_DATA_IND(message_p).sdu_p = rrc_buffer_p;
    NR_RRC_DCCH_DATA_IND(message_p).sdu_size = size;
    NR_RRC_DCCH_DATA_IND(message_p).rnti = ue->rntiMaybeUEid;
    itti_send_msg_to_task(TASK_RRC_NRUE, 0, message_p);
  }
}

void deliver_pdu_srb_rlc(void *deliver_pdu_data, ue_id_t ue_id, int srb_id,
                         char *buf, int size, int sdu_id, nr_intf_type_t intf_type)
{
  protocol_ctxt_t ctxt = { .enb_flag = 1, .rntiMaybeUEid = ue_id };
  mem_block_t *memblock = get_free_mem_block(size, __FUNCTION__);
  memcpy(memblock->data, buf, size);
  enqueue_rlc_data_req(&ctxt, 1, MBMS_FLAG_NO, srb_id, sdu_id, 0, size, memblock, intf_type);
}

void deliver_pdu_srb_f1(void *deliver_pdu_data, ue_id_t ue_id, int srb_id,
                        char *buf, int size, int sdu_id, nr_intf_type_t intf_type)
{
  DevAssert(deliver_pdu_data != NULL);
  gNB_RRC_INST *rrc = deliver_pdu_data;
  f1ap_dl_rrc_message_t dl_rrc = {.old_gNB_DU_ue_id = 0xFFFFFF,
                                  .rrc_container = (uint8_t *)buf,
                                  .rrc_container_length = size,
                                  .rnti = ue_id,
                                  .srb_id = srb_id};
  rrc->mac_rrc.dl_rrc_message_transfer(0, &dl_rrc);
}

static void add_srb(int is_gnb, ue_id_t rntiMaybeUEid, struct NR_SRB_ToAddMod *s, int ciphering_algorithm, int integrity_algorithm, unsigned char *ciphering_key, unsigned char *integrity_key, nr_intf_type_t intf_type)
{
  nr_pdcp_entity_t *pdcp_srb;
  nr_pdcp_ue_t *ue;
  int t_Reordering=3000;

  int srb_id = s->srb_Identity;
  if (s->pdcp_Config == NULL ||
      s->pdcp_Config->t_Reordering == NULL) t_Reordering = 3000;
  else t_Reordering = decode_t_reordering(*s->pdcp_Config->t_Reordering);

  AssertFatal((intf_type == PC5) || (intf_type == UU), "Invalid interface type is provided!!!");

  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, rntiMaybeUEid);
  nr_pdcp_entity_t *pdcp_entity = (intf_type == PC5) ? ue->sl_srb[srb_id - 1] : ue->srb[srb_id - 1];
  if (pdcp_entity != NULL) {
    LOG_D(PDCP, "%s:%d:%s: warning SRB %d already exist for UE ID/RNTI %ld, do nothing\n", __FILE__, __LINE__, __FUNCTION__, srb_id, rntiMaybeUEid);
  } else {
    nr_pdcp_entity_type_t pdcp_entity_type = (intf_type == UU) ? NR_PDCP_SRB : NR_PDCP_SL_SRB;
    pdcp_srb = new_nr_pdcp_entity(pdcp_entity_type, is_gnb, srb_id,
                                  0, false, false, // sdap parameters
                                  deliver_sdu_srb, ue, NULL, ue,
                                  12, t_Reordering, -1,
                                  ciphering_algorithm,
                                  integrity_algorithm,
                                  ciphering_key,
                                  integrity_key);
    nr_pdcp_ue_add_srb_pdcp_entity(ue, srb_id, pdcp_srb, intf_type);
    static bool srap_uu_created;
    bool srap_enabled = get_softmodem_params()->relay_type > 0 ? true : false;
    /* Use the cached file-scoped node_type (set once in nr_pdcp_layer_init); calling
       get_node_type() here re-parses the config file and leaks managed config pointers
       on every add_srb (CONFIG_MAX_ALLOCATEDPTRS pool exhaustion crash). */
    LOG_D(PDCP, "add_srb SRB%d: srap_enabled=%d, srap_uu_created=%d, node_type=%d\n", srb_id, srap_enabled, srap_uu_created, node_type);
    if (srap_enabled && !srap_uu_created) {
      nr_srap_manager_internal_t *m = nr_srap_manager;
      // We check for node being the DU because SRAP exists there (not in the CU)
      if (m && !m->srap_entity[0] && (NODE_IS_DU(node_type) || NODE_IS_MONOLITHIC(node_type))) { // on gNB - UU entity is on index 0
        m->srap_entity[0] = new_nr_srap_entity(NR_SRAP_UU, srap_deliver_sdu_drb, ue, srap_deliver_pdu_drb, ue, rntiMaybeUEid);
        srap_uu_created = true;
      } else if (m && !m->srap_entity[1] && (get_softmodem_params()->is_relay_ue)) { // on relay UE, UU entity is on index 1
        m->srap_entity[1] = new_nr_srap_entity(NR_SRAP_UU, srap_deliver_sdu_drb, ue, srap_deliver_pdu_drb, ue, rntiMaybeUEid);
        srap_uu_created = true;
      } else if (!get_softmodem_params()->is_relay_ue && !NODE_IS_DU(node_type) && !NODE_IS_MONOLITHIC(node_type)) {
        /* Remote UE (node_type == -1, not a Relay UE): it reaches the network only
           over PC5 via the Relay UE and owns no Uu SRAP entity by design. This is
           the expected path, not a failure. */
        LOG_D(PDCP, "Remote UE: no Uu SRAP entity needed (relay traffic uses PC5)\n");
      } else {
        LOG_W(PDCP, "Failed to create SRAP UU entity: conditions not met (node_type=%d, is_relay_ue=%d, entity[0]=%p, entity[1]=%p)\n",
              node_type, get_softmodem_params()->is_relay_ue,
              m ? m->srap_entity[0] : NULL, m ? m->srap_entity[1] : NULL);
      }
    }
    LOG_D(PDCP, "%s:%d:%s: added srb %d to UE ID/RNTI %ld\n", __FILE__, __LINE__, __FUNCTION__, srb_id, rntiMaybeUEid);
  }
  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
}

void add_drb_am(int is_gnb, ue_id_t rntiMaybeUEid, ue_id_t reestablish_ue_id, struct NR_DRB_ToAddMod *s, int ciphering_algorithm, int integrity_algorithm, unsigned char *ciphering_key, unsigned char *integrity_key)
{
  nr_pdcp_entity_t *pdcp_drb;
  nr_pdcp_ue_t *ue;

  int drb_id = s->drb_Identity;

  int sn_size_ul = decode_sn_size_ul(*s->pdcp_Config->drb->pdcp_SN_SizeUL);
  int sn_size_dl = decode_sn_size_dl(*s->pdcp_Config->drb->pdcp_SN_SizeDL);
  int discard_timer = decode_discard_timer(*s->pdcp_Config->drb->discardTimer);

  int has_integrity;
  int has_ciphering;

  /* if pdcp_Config->t_Reordering is not present, it means infinity (-1) */
  int t_reordering = -1;
  if (s->pdcp_Config->t_Reordering != NULL) {
    t_reordering = decode_t_reordering(*s->pdcp_Config->t_Reordering);
  }

  if (s->pdcp_Config->drb != NULL
      && s->pdcp_Config->drb->integrityProtection != NULL)
    has_integrity = 1;
  else
    has_integrity = 0;

  if (s->pdcp_Config->ext1 != NULL
     && s->pdcp_Config->ext1->cipheringDisabled != NULL)
    has_ciphering = 0;
  else
    has_ciphering = 1;

  if ((!s->cnAssociation) || s->cnAssociation->present == NR_DRB_ToAddMod__cnAssociation_PR_NOTHING) {
    LOG_E(PDCP,"%s:%d:%s: fatal, cnAssociation is missing or present is NR_DRB_ToAddMod__cnAssociation_PR_NOTHING\n",__FILE__,__LINE__,__FUNCTION__);
    exit(-1);
  }

  int pdusession_id;
  bool has_sdap_rx = false;
  bool has_sdap_tx = false;
  bool is_sdap_DefaultDRB = false;
  NR_QFI_t *mappedQFIs2Add = NULL;
  uint8_t mappedQFIs2AddCount=0;
  if (s->cnAssociation->present == NR_DRB_ToAddMod__cnAssociation_PR_eps_BearerIdentity)
     pdusession_id = s->cnAssociation->choice.eps_BearerIdentity;
  else {
    if (!s->cnAssociation->choice.sdap_Config) {
      LOG_E(PDCP,"%s:%d:%s: fatal, sdap_Config is null",__FILE__,__LINE__,__FUNCTION__);
      exit(-1);
    }
    pdusession_id = s->cnAssociation->choice.sdap_Config->pdu_Session;
    if (is_gnb) {
      has_sdap_rx = s->cnAssociation->choice.sdap_Config->sdap_HeaderUL == NR_SDAP_Config__sdap_HeaderUL_present;
      has_sdap_tx = s->cnAssociation->choice.sdap_Config->sdap_HeaderDL == NR_SDAP_Config__sdap_HeaderDL_present;
    } else {
      has_sdap_tx = s->cnAssociation->choice.sdap_Config->sdap_HeaderUL == NR_SDAP_Config__sdap_HeaderUL_present;
      has_sdap_rx = s->cnAssociation->choice.sdap_Config->sdap_HeaderDL == NR_SDAP_Config__sdap_HeaderDL_present;
    }
    is_sdap_DefaultDRB = s->cnAssociation->choice.sdap_Config->defaultDRB == true ? 1 : 0;
    // Check if mappedQoS_FlowsToAdd pointer is NULL (relay forwarding DRB with no QoS flows)
    if (s->cnAssociation->choice.sdap_Config->mappedQoS_FlowsToAdd == NULL) {
      mappedQFIs2AddCount = 0;
      mappedQFIs2Add = NULL;
      LOG_D(PDCP, "No QoS flows mapped for DRB %d (mappedQoS_FlowsToAdd is NULL, relay forwarding DRB)\n", drb_id);
    } else {
      mappedQFIs2AddCount = s->cnAssociation->choice.sdap_Config->mappedQoS_FlowsToAdd->list.count;
      if (mappedQFIs2AddCount > 0) {
        mappedQFIs2Add = (NR_QFI_t*)s->cnAssociation->choice.sdap_Config->mappedQoS_FlowsToAdd->list.array[0];
        LOG_D(SDAP, "Captured mappedQoS_FlowsToAdd from RRC: %ld \n", *mappedQFIs2Add);
      } else {
        mappedQFIs2Add = NULL;
        LOG_D(PDCP, "No QoS flows mapped for DRB %d (count is 0, relay forwarding DRB)\n", drb_id);
      }
    }
  }
  /* TODO(?): accept different UL and DL SN sizes? */
  if (sn_size_ul != sn_size_dl) {
    LOG_E(PDCP, "%s:%d:%s: fatal, bad SN sizes, must be same. ul=%d, dl=%d\n",
          __FILE__, __LINE__, __FUNCTION__, sn_size_ul, sn_size_dl);
    exit(1);
  }


  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, rntiMaybeUEid);
  if (ue->drb[drb_id-1] != NULL) {
    LOG_W(PDCP, "%s:%d:%s: warning DRB %d already exist for UE ID/RNTI %ld, do nothing (preventing duplicate creation)\n", __FILE__, __LINE__, __FUNCTION__, drb_id, rntiMaybeUEid);
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
    return;
  }

  pdcp_drb = new_nr_pdcp_entity(NR_PDCP_DRB_AM, is_gnb, drb_id, pdusession_id,
                                has_sdap_rx, has_sdap_tx,
                                deliver_sdu_drb, ue, deliver_pdu_drb, ue,
                                sn_size_dl, t_reordering, discard_timer,
                                has_ciphering ? ciphering_algorithm : 0,
                                has_integrity ? integrity_algorithm : 0,
                                has_ciphering ? ciphering_key : NULL,
                                has_integrity ? integrity_key : NULL);
  nr_pdcp_ue_add_drb_pdcp_entity(ue, drb_id, pdcp_drb);

  if (reestablish_ue_id > 0) {
    nr_pdcp_ue_t *reestablish_ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, reestablish_ue_id);
    if (reestablish_ue != NULL) {
      pdcp_drb->tx_next = reestablish_ue->drb[drb_id - 1]->tx_next;
      LOG_D(PDCP, "Applying tx_next %d in DRB %d from old UEid %lx to new UEid %lx\n", reestablish_ue->drb[drb_id - 1]->tx_next, drb_id, reestablish_ue_id, rntiMaybeUEid);
    }
  }

  LOG_D(PDCP, "%s:%d:%s: added drb %d to UE ID/RNTI %ld\n", __FILE__, __LINE__, __FUNCTION__, drb_id, rntiMaybeUEid);
  new_nr_sdap_entity(is_gnb, has_sdap_rx, has_sdap_tx, rntiMaybeUEid, pdusession_id, is_sdap_DefaultDRB, drb_id, mappedQFIs2Add, mappedQFIs2AddCount);
  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
}

void add_srap_entity(int src_id) {
  static bool srap_pc5_created;
  bool srap_enabled = get_softmodem_params()->relay_type > 0 ? true : false;
  if (srap_enabled && !srap_pc5_created) {
    nr_srap_manager_internal_t *m = nr_srap_manager;
    if (m && !m->srap_entity[0]) {
      /* deliver_pdu callback is for TX (enqueueing PDU to RLC for transmission)
         SRB messages use parameter callback in nr_srap_data_req_srb, so this only affects DRB */
      m->srap_entity[0] = new_nr_srap_entity(NR_SRAP_PC5, srap_deliver_sdu_drb, NULL, srap_deliver_pdu_drb, NULL, src_id); // index 0 will always have NR_SRAP_PC5 entity.
      srap_pc5_created = true;
    } else {
      LOG_W(PDCP, "Failed to create PC5 SRAP entity: manager=%p, entity[0]=%p\n", m, m ? m->srap_entity[0] : NULL);
    }
  }
}

void add_drb_sl(ue_id_t srcid, NR_SL_RadioBearerConfig_r16_t *s, int ciphering_algorithm, int integrity_algorithm, unsigned char *ciphering_key, unsigned char *integrity_key)
{
  nr_pdcp_entity_t *pdcp_drb;

  AssertFatal(s->sl_PDCP_Config_r16 != NULL, "SL PDCP config is not there!\n");
  int slrb_id = s->slrb_Uu_ConfigIndex_r16;
  int sn_size = decode_sn_size_ul(*s->sl_PDCP_Config_r16->sl_PDCP_SN_Size_r16);
  int discard_timer = decode_discard_timer_sl(*s->sl_PDCP_Config_r16->sl_DiscardTimer_r16);

  // these 3 are configured differently in Sidelink 
  int has_integrity = 0;
  int has_ciphering = 0;
  // int has_rohc = 0;

  int t_reordering = 20;
  bool has_sdap = s->sl_SDAP_Config_r16 && s->sl_SDAP_Config_r16->sl_SDAP_Header_r16 == NR_SL_SDAP_Config_r16__sl_SDAP_Header_r16_present;
  bool is_sdap_DefaultRB = s->sl_SDAP_Config_r16 && s->sl_SDAP_Config_r16->sl_DefaultRB_r16 == true ? true : false;
  /* TODO(?): accept different UL and DL SN sizes? */

  uint8_t mappedQFIs2AddCount = s->sl_SDAP_Config_r16->sl_MappedQoS_Flows_r16->choice.sl_MappedQoS_FlowsListDedicated_r16->sl_MappedQoS_FlowsToAddList_r16->list.count;
  NR_QFI_t *mappedQFIs2Add = calloc(mappedQFIs2AddCount, sizeof(*mappedQFIs2Add));
  LOG_D(SDAP, "Captured mappedQoS_FlowsToAdd from RRC: count %d\n", mappedQFIs2AddCount);

  NR_SL_QoS_FlowIdentity_r16_t qfi = 0;
  for (int i = 0; i < mappedQFIs2AddCount; i++) {
    qfi = *s->sl_SDAP_Config_r16->sl_MappedQoS_Flows_r16->choice.sl_MappedQoS_FlowsListDedicated_r16->sl_MappedQoS_FlowsToAddList_r16->list.array[i];
    mappedQFIs2Add[i] = qfi;
  }

  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  nr_pdcp_ue_t *ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, srcid);
  if (ue->drb[slrb_id-1] != NULL) {
    LOG_W(PDCP, "%s:%d:%s: warning DRB %d already exist for UE ID/RNTI %ld, do nothing\n", __FILE__, __LINE__, __FUNCTION__, slrb_id, srcid);
  } else {
    int pdusession_id = 10;
    pdcp_drb = new_nr_pdcp_entity(NR_PDCP_DRB_AM, 0, slrb_id, pdusession_id,
                                  has_sdap, has_sdap,
                                  deliver_sdu_drb, ue, deliver_pdu_drb, ue,
                                  sn_size, t_reordering, discard_timer,
                                  has_ciphering ? ciphering_algorithm : 0,
                                  has_integrity ? integrity_algorithm : 0,
                                  has_ciphering ? ciphering_key : NULL,
                                  has_integrity ? integrity_key : NULL);
    nr_pdcp_ue_add_drb_pdcp_entity(ue, slrb_id, pdcp_drb);

    LOG_I(PDCP, "%s:%d:%s: added slrb %d to UE ID %ld\n", __FILE__, __LINE__, __FUNCTION__, slrb_id, srcid);
    add_srap_entity(srcid);
    new_nr_sdap_entity(0, has_sdap, has_sdap, srcid, pdusession_id, is_sdap_DefaultRB, slrb_id, mappedQFIs2Add, mappedQFIs2AddCount);
  }
  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
}
static void add_drb(int is_gnb,
                    ue_id_t rntiMaybeUEid,
                    ue_id_t reestablish_ue_id,
                    struct NR_DRB_ToAddMod *s,
                    NR_RLC_Config_t *rlc_Config,
                    int ciphering_algorithm,
                    int integrity_algorithm,
                    unsigned char *ciphering_key,
                    unsigned char *integrity_key)
{
  switch (rlc_Config->present) {
  case NR_RLC_Config_PR_am:
    add_drb_am(is_gnb, rntiMaybeUEid, reestablish_ue_id, s, ciphering_algorithm, integrity_algorithm, ciphering_key, integrity_key);
    break;
  case NR_RLC_Config_PR_um_Bi_Directional:
    // add_drb_um(rntiMaybeUEid, s);
    /* hack */
    add_drb_am(is_gnb, rntiMaybeUEid, reestablish_ue_id, s, ciphering_algorithm, integrity_algorithm, ciphering_key, integrity_key);
    break;
  default:
    LOG_E(PDCP, "%s:%d:%s: fatal: unhandled DRB type\n",
          __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }
  LOG_I(PDCP, "%s:%s:%d: added DRB for UE ID/RNTI %ld\n", __FILE__, __FUNCTION__, __LINE__, rntiMaybeUEid);
}

void nr_pdcp_add_srbs(eNB_flag_t enb_flag, ue_id_t rntiMaybeUEid, NR_SRB_ToAddModList_t *const srb2add_list, const uint8_t security_modeP, uint8_t *const kRRCenc, uint8_t *const kRRCint, nr_intf_type_t intf_type)
{
  if (srb2add_list != NULL) {
    for (int i = 0; i < srb2add_list->list.count; i++) {
      add_srb(enb_flag, rntiMaybeUEid, srb2add_list->list.array[i], security_modeP & 0x0f, (security_modeP >> 4) & 0x0f, kRRCenc, kRRCint, intf_type);
    }
  } else
    LOG_W(PDCP, "nr_pdcp_add_srbs() with void list\n");
}

void nr_pdcp_add_drbs(eNB_flag_t enb_flag,
                      ue_id_t rntiMaybeUEid,
                      ue_id_t reestablish_ue_id,
                      NR_DRB_ToAddModList_t *const drb2add_list,
                      const uint8_t security_modeP,
                      uint8_t *const kUPenc,
                      uint8_t *const kUPint,
                      struct NR_CellGroupConfig__rlc_BearerToAddModList *rlc_bearer2add_list,
                      bool is_remote_ue)
{
  bool is_relay_ue = (enb_flag && !is_remote_ue && get_softmodem_params()->relay_type == U2N);

  if (drb2add_list != NULL) {
    for (int i = 0; i < drb2add_list->list.count; i++) {
      int drb_id = drb2add_list->list.array[i]->drb_Identity;

      /* For Relay UE's DRB2: skip PDCP configuration at UE side only (!enb_flag)
         gNB side (enb_flag=1) MUST create PDCP for the transport bearer
         UE side (enb_flag=0) skips PDCP - routing handled by RLC deliver_sdu() */
      if (!enb_flag && is_relay_ue && drb_id == 2) {
        LOG_D(PDCP, "[Relay UE] Skipping PDCP configuration for DRB 2 (routing via SRAP in RLC)\n");
        continue;
      }

      add_drb(enb_flag, rntiMaybeUEid, reestablish_ue_id, drb2add_list->list.array[i], rlc_bearer2add_list->list.array[i]->rlc_Config, security_modeP & 0x0f, (security_modeP >> 4) & 0x0f, kUPenc, kUPint);
#if 1
      /* Restored from episys/sl-mode1-relay: Automatically create DRB 2 at gNB for Relay UE
         This creates the transport bearer PDCP entity for forwarding Remote UE traffic
         RRC (rrc_gNB.c:fill_DRB_configList) creates config, this creates the PDCP entity */
      bool relay_enabled = get_softmodem_params()->relay_type > 0 ? true : false;
      if (relay_enabled && enb_flag && !is_remote_ue) {
        /* CRITICAL: Clear QFI mappings for DRB2 before creation
           DRB2 is a relay transport bearer - it should NOT have QFI-to-DRB mappings
           Mappings are only for Relay UE's own traffic (DRB1), not forwarded Remote UE traffic */
        NR_SDAP_Config_t *saved_sdap_config = drb2add_list->list.array[i]->cnAssociation->choice.sdap_Config;
        void *saved_qfi_list = NULL;  // Use void* to avoid anonymous struct type

        if (saved_sdap_config && saved_sdap_config->mappedQoS_FlowsToAdd) {
          // Temporarily clear QFI list for DRB2 creation
          saved_qfi_list = saved_sdap_config->mappedQoS_FlowsToAdd;
          saved_sdap_config->mappedQoS_FlowsToAdd = NULL;
          LOG_D(PDCP, "[gNB] Cleared QFI mappings for Relay UE DRB2 (transport bearer - no QFI associations)\n");
        }

        drb2add_list->list.array[i]->drb_Identity = drb2add_list->list.array[i]->drb_Identity + 1;
        LOG_D(PDCP, "[gNB] Creating PDCP entity for Relay UE DRB %ld (transport bearer)\n",
                    drb2add_list->list.array[i]->drb_Identity);
        add_drb(enb_flag, rntiMaybeUEid, reestablish_ue_id, drb2add_list->list.array[i], rlc_bearer2add_list->list.array[i]->rlc_Config, security_modeP & 0x0f, (security_modeP >> 4) & 0x0f, kUPenc, kUPint);
        drb2add_list->list.array[i]->drb_Identity = drb2add_list->list.array[i]->drb_Identity - 1;

        // Restore QFI list for DRB1 (original configuration)
        if (saved_sdap_config && saved_qfi_list) {
          saved_sdap_config->mappedQoS_FlowsToAdd = saved_qfi_list;
        }
      }
#endif
    }
  } else
    LOG_W(PDCP, "nr_pdcp_add_drbs() with void list\n");
}

/* Dummy function due to dependency from LTE libraries */
bool rrc_pdcp_config_asn1_req(const protocol_ctxt_t *const  ctxt_pP,
                              LTE_SRB_ToAddModList_t  *const srb2add_list,
                              LTE_DRB_ToAddModList_t  *const drb2add_list,
                              LTE_DRB_ToReleaseList_t *const drb2release_list,
                              const uint8_t                   security_modeP,
                              uint8_t                  *const kRRCenc,
                              uint8_t                  *const kRRCint,
                              uint8_t                  *const kUPenc,
                              LTE_PMCH_InfoList_r9_t  *pmch_InfoList_r9,
                              rb_id_t                 *const defaultDRB)
{
  return 0;
}

uint64_t get_pdcp_optmask(void)
{
  return pdcp_optmask;
}

/* hack: dummy function needed due to LTE dependencies */
bool pdcp_remove_UE(const protocol_ctxt_t *const ctxt_pP)
{
  abort();
}

bool nr_pdcp_remove_UE(ue_id_t ue_id)
{
  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  nr_pdcp_manager_remove_ue(nr_pdcp_ue_manager, ue_id);
  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);

  return 1;
}

rnti_t nr_pdcp_get_remote_ue_rnti(rnti_t relay_ue_rnti, uint8_t remote_ue_id)
{
  rnti_t remote_ue_rnti = 0;

  nr_pdcp_manager_lock(nr_pdcp_ue_manager);

  /* Get UE list and count */
  nr_pdcp_ue_t **ue_list = nr_pdcp_manager_get_ue_list(nr_pdcp_ue_manager);
  int ue_count = nr_pdcp_manager_get_ue_count(nr_pdcp_ue_manager);

  /* Iterate through all UEs to find Remote UE matching relay+id */
  for (int i = 0; i < ue_count; i++) {
    nr_pdcp_ue_t *ue = ue_list[i];
    if (ue && ue->is_remote_ue &&
        ue->remote_ue_id == remote_ue_id &&
        ue->relay_ue_rnti == relay_ue_rnti) {
      remote_ue_rnti = ue->rntiMaybeUEid;
      break;
    }
  }

  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);

  return remote_ue_rnti;
}

/* hack: dummy function needed due to LTE dependencies */
void pdcp_config_set_security(const protocol_ctxt_t *const ctxt_pP,
                                 pdcp_t *const pdcp_pP,
                                 const rb_id_t rb_id,
                                 const uint16_t lc_idP,
                                 const uint8_t security_modeP,
                                 uint8_t *const kRRCenc_pP,
                                 uint8_t *const kRRCint_pP,
                                 uint8_t *const kUPenc_pP)
{
  abort();
}

void nr_pdcp_config_set_security(ue_id_t ue_id,
                                 const rb_id_t rb_id,
                                 const uint8_t security_modeP,
                                 uint8_t *const kRRCenc_pP,
                                 uint8_t *const kRRCint_pP,
                                 uint8_t *const kUPenc_pP)
{
  nr_pdcp_ue_t *ue;
  nr_pdcp_entity_t *rb;
  int integrity_algorithm;
  int ciphering_algorithm;

  nr_pdcp_manager_lock(nr_pdcp_ue_manager);

  ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);

  /* TODO: proper handling of DRBs, for the moment only SRBs are handled */

  if (rb_id >= 1 && rb_id <= 2) {
    rb = ue->srb[rb_id - 1];

    if (rb == NULL) {
      LOG_E(PDCP, "%s:%d:%s: no SRB found (ue_id %ld, rb_id %ld)\n", __FILE__, __LINE__, __FUNCTION__, ue_id, rb_id);
      nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
      return;
    }

    integrity_algorithm = (security_modeP>>4) & 0xf;
    ciphering_algorithm = security_modeP & 0x0f;
    rb->set_security(rb, integrity_algorithm, (char *)kRRCint_pP,
                     ciphering_algorithm, (char *)kRRCenc_pP);
    rb->security_mode_completed = false;
  } else {
    LOG_E(PDCP, "%s:%d:%s: TODO\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }

  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
}

bool nr_pdcp_data_req_srb(ue_id_t ue_id,
                          const rb_id_t rb_id,
                          const mui_t muiP,
                          const sdu_size_t sdu_buffer_size,
                          unsigned char *const sdu_buffer,
                          deliver_pdu deliver_pdu_cb,
                          void *data,
                          nr_intf_type_t intf_type)
{
  LOG_D(PDCP, "%s() called, size %d\n", __func__, sdu_buffer_size);
  nr_pdcp_ue_t *ue;
  nr_pdcp_entity_t *rb;

  bool srap_enabled = get_softmodem_params()->relay_type > 0 ? true : false;

  /* CRITICAL: Disable SRAP for PC5 peer-to-peer RRC signaling (Relay UE ↔ Remote UE sidelink config)
     SRAP should only be used for:
       - gNB downlink: gNB → Relay UE → Remote UE (cellular RRC via SRAP U2N headers)
       - Remote UE uplink: Remote UE → Relay UE → gNB (cellular RRC via SRAP N2U headers)
     Peer sidelink RRC (e.g., RRCReconfiguration xid=3 for SL config) must bypass SRAP */
  if (intf_type == PC5) {
    srap_enabled = false;
    LOG_D(PDCP, "[PDCP] PC5 interface: Disabling SRAP for peer-to-peer sidelink RRC signaling\n");
  }

  nr_pdcp_manager_lock(nr_pdcp_ue_manager);

  ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);

  if (rb_id < 1 || rb_id > 2)
    rb = NULL;
  else {
    if (intf_type == PC5)
      rb = ue->sl_srb[rb_id - 1];
    else
      rb = ue->srb[rb_id - 1];
  }

  if (rb == NULL) {
    LOG_E(PDCP, "%s:%d:%s: no SRB found (ue_id %ld, rb_id %ld)\n", __FILE__, __LINE__, __FUNCTION__, ue_id, rb_id);
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
    return 0;
  }

  /* For Remote UE traffic from gNB, extract remote_ue_id and route via SRAP
     For peer sidelink RRC (PC5), bypass SRAP (goes directly to RLC) */
  uint8_t remote_ue_id_for_srap = 0;
  bool route_via_srap = false;
  ue_id_t rlc_rnti = ue_id;  // RNTI to use for RLC delivery

  // Check if this is Remote UE cellular traffic (not peer sidelink)
  if (srap_enabled && ue->is_remote_ue) {
    remote_ue_id_for_srap = ue->remote_ue_id;
    rlc_rnti = ue->relay_ue_rnti;  // Deliver to Relay UE's RLC, not Remote UE's RLC
    route_via_srap = true;
  }

  /* Remote UE PC5 SL-SRBs don't have PDCP entities, so skip PDCP processing for Remote UE
     Deliver raw RRC message to SRAP (symmetric with uplink path) */
  char *pdu_buf_ptr;
  int pdu_size;
  int max_size = sdu_buffer_size + 3 + 4; // 3: max header, 4: max integrity
  char pdu_buf[max_size];

  if (route_via_srap) {
    // Remote UE: Skip PDCP processing, deliver raw RRC to SRAP
    pdu_buf_ptr = (char *)sdu_buffer;
    pdu_size = sdu_buffer_size;
  } else {
    // Normal UE: Process SDU with PDCP header
    pdu_size = rb->process_sdu(rb, (char *)sdu_buffer, sdu_buffer_size, muiP, pdu_buf, max_size);
    pdu_buf_ptr = pdu_buf;
    AssertFatal(rb->deliver_pdu == NULL, "SRB callback should be NULL, to be provided on every invocation\n");
  }

  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
  // For Remote UE: use Relay UE RNTI for RLC delivery, but pass remote_ue_id for SRAP header
  protocol_ctxt_t ctxt = { .enb_flag = 1, .rntiMaybeUEid = rlc_rnti, .remote_ue_id = remote_ue_id_for_srap };

  if (route_via_srap) {
    // Remote UE traffic: RRC → SRAP (no PDCP header) → RLC
    nr_srap_data_req_srb(&ctxt, rb_id, pdu_size, pdu_buf_ptr, srap_deliver_pdu_srb, muiP, intf_type);
  } else {
    // Normal UE traffic (including Relay UE itself): RRC → PDCP → RLC (bypass SRAP)
    deliver_pdu_cb(data, ue_id, rb_id, pdu_buf_ptr, pdu_size, muiP, intf_type);
  }

  return 1;
}

bool nr_pdcp_data_req_drb(protocol_ctxt_t *ctxt_pP,
                          const srb_flag_t srb_flagP,
                          const rb_id_t rb_id,
                          const mui_t muiP,
                          const confirm_t confirmP,
                          const sdu_size_t sdu_buffer_size,
                          unsigned char *const sdu_buffer,
                          const pdcp_transmission_mode_t mode,
                          const uint32_t *const sourceL2Id,
                          const uint32_t *const destinationL2Id)
{
  DevAssert(srb_flagP == SRB_FLAG_NO);

  ue_id_t ue_id = ctxt_pP->rntiMaybeUEid;

  nr_pdcp_ue_t *ue;
  nr_pdcp_entity_t *rb;

  if (ctxt_pP->module_id != 0 ||
      //ctxt_pP->enb_flag != 1 ||
      ctxt_pP->instance != 0 ||
      ctxt_pP->eNB_index != 0 /*||
      ctxt_pP->configured != 1 ||
      ctxt_pP->brOption != 0*/) {
    LOG_E(PDCP, "%s:%d:%s: fatal\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }

  nr_pdcp_manager_lock(nr_pdcp_ue_manager);

  ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);

  if (rb_id < 1 || rb_id > MAX_DRBS_PER_UE)
    rb = NULL;
  else
    rb = ue->drb[rb_id - 1];

  if (rb == NULL) {
    LOG_E(PDCP, "[gNB_PDCP_DL_ERROR] NO DRB FOUND! ue_id=0x%04lx, rb_id=%ld, is_remote_ue=%d - PACKET DROPPED!\n",
          ue_id, rb_id, ue->is_remote_ue);
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
    return 0;
  }

  bool is_gnb = (ctxt_pP->enb_flag == 1);
  bool srap_enabled = get_softmodem_params()->relay_type > 0;

  /* At gNB: Check if this is Remote UE traffic
     Remote UE has its own PDU session, packets come with Remote UE RNTI
     Must route via SRAP to add U2N header and forward to Relay UE's DRB2 */
  bool is_remote_ue_traffic = (is_gnb && srap_enabled && ue->is_remote_ue);

  int max_size = sdu_buffer_size + 3 + 4; // 3: max header, 4: max integrity
  char pdu_buf[max_size];
  int pdu_size = rb->process_sdu(rb, (char *)sdu_buffer, sdu_buffer_size, muiP, pdu_buf, max_size);
  deliver_pdu deliver_pdu_cb = rb->deliver_pdu;

  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
  bool is_pc5_link = sourceL2Id != 0 || destinationL2Id != 0;
  nr_intf_type_t intf_type = is_pc5_link ? PC5 : UU;

  // Route via SRAP for Remote UE traffic at gNB (using episys/sl-mode1-relay original function)
  if (is_remote_ue_traffic) {
    protocol_ctxt_t relay_ctx = *ctxt_pP;
    relay_ctx.rntiMaybeUEid = ue->relay_ue_rnti;
    relay_ctx.remote_ue_id = ue->remote_ue_id;  // Pass Remote UE ID to SRAP for header

    if (!nr_srap_data_req_drb(&relay_ctx, rb_id, muiP, pdu_size, pdu_buf, UU)) {
      LOG_E(PDCP, "[gNB] ERROR: nr_srap_data_req_drb failed for Remote UE traffic\n");
      return 0;
    }

    return 1;
  }

  // Normal path: deliver to RLC directly
  if (deliver_pdu_cb == NULL) {
    LOG_E(PDCP, "[%s] ERROR: deliver_pdu_cb is NULL for RNTI 0x%04lx rb_id %ld! Cannot deliver DRB packet!\n",
          is_gnb ? "gNB" : "UE", ue_id, rb_id);
    return 0;
  }

  deliver_pdu_cb(NULL, ue_id, rb_id, pdu_buf, pdu_size, muiP, intf_type);

  return 1;
}

bool cu_f1u_data_req(protocol_ctxt_t  *ctxt_pP,
                     const srb_flag_t srb_flagP,
                     const rb_id_t rb_id,
                     const mui_t muiP,
                     const confirm_t confirmP,
                     const sdu_size_t sdu_buffer_size,
                     unsigned char *const sdu_buffer,
                     const pdcp_transmission_mode_t mode,
                     const uint32_t *const sourceL2Id,
                     const uint32_t *const destinationL2Id) {
  //Force instance id to 0, OAI incoherent instance management
  ctxt_pP->instance=0;
  mem_block_t *memblock = get_free_mem_block(sdu_buffer_size, __func__);
  if (memblock == NULL) {
    LOG_E(RLC, "%s:%d:%s: ERROR: get_free_mem_block failed\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }
  memcpy(memblock->data,sdu_buffer, sdu_buffer_size);
  int ret = pdcp_data_ind(ctxt_pP, srb_flagP, false, rb_id, sdu_buffer_size, memblock, NULL, NULL, UU);
  if (!ret) {
    LOG_E(RLC, "%s:%d:%s: ERROR: pdcp_data_ind failed\n", __FILE__, __LINE__, __FUNCTION__);
    /* what to do in case of failure? for the moment: nothing */
  }
  return ret;
}

/* hack: dummy function needed due to LTE dependencies */
bool pdcp_data_req(protocol_ctxt_t  *ctxt_pP,
                   const srb_flag_t     srb_flagP,
                   const rb_id_t        rb_idP,
                   const mui_t          muiP,
                   const confirm_t      confirmP,
                   const sdu_size_t     sdu_buffer_sizeP,
                   unsigned char *const sdu_buffer_pP,
                   const pdcp_transmission_mode_t modeP,
                   const uint32_t *const sourceL2Id,
                   const uint32_t *const destinationL2Id)
{
  abort();
  return false;
}

void pdcp_set_pdcp_data_ind_func(pdcp_data_ind_func_t pdcp_data_ind)
{
  /* nothing to do */
}

void pdcp_set_rlc_data_req_func(send_rlc_data_req_func_t send_rlc_data_req)
{
  /* nothing to do */
}

//Dummy function needed due to LTE dependencies
void
pdcp_mbms_run ( const protocol_ctxt_t *const  ctxt_pP){
  /* nothing to do */
}

void nr_pdcp_tick(int frame, int subframe)
{
  if (frame != nr_pdcp_current_time_last_frame ||
      subframe != nr_pdcp_current_time_last_subframe) {
    nr_pdcp_current_time_last_frame = frame;
    nr_pdcp_current_time_last_subframe = subframe;
    nr_pdcp_current_time++;
    nr_pdcp_wakeup_timer_thread(nr_pdcp_current_time);
  }
}

/*
 * For the SDAP API
 */
nr_pdcp_ue_manager_t *nr_pdcp_sdap_get_ue_manager() {
  return nr_pdcp_ue_manager;
}

/* returns false in case of error, true if everything ok */
const bool nr_pdcp_get_statistics(ue_id_t ue_id, int srb_flag, int rb_id, nr_pdcp_statistics_t *out)
{
  nr_pdcp_ue_t     *ue;
  nr_pdcp_entity_t *rb;
  bool             ret;

  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);

  if (srb_flag == 1) {
    if (rb_id < 1 || rb_id > 2)
      rb = NULL;
    else
      rb = ue->srb[rb_id - 1];
  } else {
    if (rb_id < 1 || rb_id > 5)
      rb = NULL;
    else
      rb = ue->drb[rb_id - 1];
  }

  if (rb != NULL) {
    rb->get_stats(rb, out);
    ret = true;
  } else {
    ret = false;
  }

  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);

  return ret;
}
