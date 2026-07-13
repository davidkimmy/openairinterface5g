/*
Author: Ejaz Ahmed
Email ID: ejaz.ahmed@applied.co
*/

#define _GNU_SOURCE

#include "nr_srap_oai_api.h"
#include "openair2/LAYER2/nr_pdcp/nr_pdcp_oai_api.h"
#include <executables/nr-uesoftmodem.h>
#include "nr_srap_header.h"
#include "nr_srap_manager.h"
#include "common/ngran_types.h"
#include "openair2/RRC/NR/nr_rrc_common.h"  // For NR_SRB_INFO
#include "openair2/RRC/NR_UE/rrc_defs.h"    // For RRC_STATE_IDLE_NR, NR_UE_RRC_INST_t, NB_NR_UE_INST

extern nr_srap_manager_t *nr_srap_manager;

/* Weak reference to UE RRC instance (only available in UE builds)
   In gNB builds, this will be NULL (weak symbol not satisfied) */
extern __attribute__((weak)) NR_UE_RRC_INST_t *NR_UE_rrc_inst;

// Weak stub for CU-UP builds (CU-UP doesn't have RRC, so provide a no-op implementation)
__attribute__((weak)) int nr_rrc_gNB_process_srap_message(module_id_t module_id, rnti_t relay_rnti, const uint8_t *buffer, int buffer_length) {
  LOG_W(NR_SRAP, "nr_rrc_gNB_process_srap_message called but not available (CU-UP build?)\n");
  return -1;
}

// Weak stub for gNB builds (gNB doesn't have UE RRC decoder)
__attribute__((weak)) int8_t nr_rrc_ue_decode_ccch(const protocol_ctxt_t *const ctxt_pP,
                                                     const NR_SRB_INFO *const Srb_info,
                                                     const uint8_t gNB_index) {
  LOG_W(NR_SRAP, "nr_rrc_ue_decode_ccch called but not available (gNB build?)\n");
  return -1;
}

// Weak stub for gNB builds (gNB doesn't have UE RRC DCCH decoder)
__attribute__((weak)) int nr_rrc_ue_decode_dcch(const protocol_ctxt_t *const ctxt_pP,
                                                  const srb_id_t Srb_id,
                                                  const uint8_t *const Buffer,
                                                  size_t Buffer_size,
                                                  const uint8_t gNB_indexP,
                                                  const nr_intf_type_t src_intf) {
  LOG_W(NR_SRAP, "nr_rrc_ue_decode_dcch called but not available (gNB build?)\n");
  return -1;
}

static srap_data_req_queue tx_srap_uu_q;
static srap_data_req_queue tx_srap_pc5_q;
static srap_data_ind_queue rx_srap_uu_q;
static srap_data_ind_queue rx_srap_pc5_q;
static srap_data_req_queue fwd_srap_to_uu_q;
static srap_data_req_queue fwd_srap_to_pc5_q;

static void init_nr_srap_data_ind_queue(bool gNB_flag);

static void *tx_srap_rlc_pc5_data_req_thread(void *_)
{
  int i;

  LOG_D(NR_SRAP, "tx_srap_rlc_pc5_data_req_thread created on core %d\n", sched_getcpu());
  pthread_setname_np(pthread_self(), "Tx SRAP RLC PC5 queue");
  while (1) {
    if (pthread_mutex_lock(&tx_srap_pc5_q.m) != 0) abort();
    while (tx_srap_pc5_q.length == 0)
      if (pthread_cond_wait(&tx_srap_pc5_q.c, &tx_srap_pc5_q.m) != 0) abort();
    i = tx_srap_pc5_q.start;
    if (pthread_mutex_unlock(&tx_srap_pc5_q.m) != 0) abort();

    rlc_data_req(&tx_srap_pc5_q.q[i].ctxt_pP,
                 tx_srap_pc5_q.q[i].srb_flagP,
                 tx_srap_pc5_q.q[i].MBMS_flagP,
                 tx_srap_pc5_q.q[i].rb_idP,
                 tx_srap_pc5_q.q[i].muiP,
                 tx_srap_pc5_q.q[i].confirmP,
                 tx_srap_pc5_q.q[i].sdu_sizeP,
                 tx_srap_pc5_q.q[i].sdu_pP,
                 &tx_srap_pc5_q.q[i].sourceL2Id,
                 &tx_srap_pc5_q.q[i].destinationL2Id,
                 PC5);

    if (pthread_mutex_lock(&tx_srap_pc5_q.m) != 0) abort();

    tx_srap_pc5_q.length--;
    tx_srap_pc5_q.start = (tx_srap_pc5_q.start + 1) % SRAP_DATA_REQ_QUEUE_SIZE;

    if (pthread_cond_signal(&tx_srap_pc5_q.c) != 0) abort();
    if (pthread_mutex_unlock(&tx_srap_pc5_q.m) != 0) abort();
  }
}

static void *tx_srap_rlc_uu_data_req_thread(void *_)
{
  int i;

  LOG_D(NR_SRAP,"tx_srap_rlc_uu_data_req_thread created on core %d\n", sched_getcpu());
  pthread_setname_np(pthread_self(), "Tx SRAP RLC Uu queue");
  while (1) {
    if (pthread_mutex_lock(&tx_srap_uu_q.m) != 0) abort();
    while (tx_srap_uu_q.length == 0)
      if (pthread_cond_wait(&tx_srap_uu_q.c, &tx_srap_uu_q.m) != 0) abort();
    i = tx_srap_uu_q.start;
    if (pthread_mutex_unlock(&tx_srap_uu_q.m) != 0) abort();

    rlc_data_req(&tx_srap_uu_q.q[i].ctxt_pP,
                 tx_srap_uu_q.q[i].srb_flagP,
                 tx_srap_uu_q.q[i].MBMS_flagP,
                 tx_srap_uu_q.q[i].rb_idP,
                 tx_srap_uu_q.q[i].muiP,
                 tx_srap_uu_q.q[i].confirmP,
                 tx_srap_uu_q.q[i].sdu_sizeP,
                 tx_srap_uu_q.q[i].sdu_pP,
                 NULL,
                 NULL,
                 UU);

    if (pthread_mutex_lock(&tx_srap_uu_q.m) != 0) abort();

    tx_srap_uu_q.length--;
    tx_srap_uu_q.start = (tx_srap_uu_q.start + 1) % SRAP_DATA_REQ_QUEUE_SIZE;

    if (pthread_cond_signal(&tx_srap_uu_q.c) != 0) abort();
    if (pthread_mutex_unlock(&tx_srap_uu_q.m) != 0) abort();
  }
}

static void *fwd_srap_to_pc5_data_req_thread(void *_)
{
  int i;

  LOG_D(NR_SRAP, "fwd_srap_to_pc5_data_req_thread created on core %d\n", sched_getcpu());
  pthread_setname_np(pthread_self(), "Fwd SRAP PC5 queue");
  while (1) {
    if (pthread_mutex_lock(&fwd_srap_to_pc5_q.m) != 0) abort();
    while (fwd_srap_to_pc5_q.length == 0)
      if (pthread_cond_wait(&fwd_srap_to_pc5_q.c, &fwd_srap_to_pc5_q.m) != 0) abort();
    i = fwd_srap_to_pc5_q.start;
    if (pthread_mutex_unlock(&fwd_srap_to_pc5_q.m) != 0) abort();
    LOG_D(NR_SRAP, "Pass data from SRAP fwd_srap_to_pc5_q to RLC: rb_id=%ld, size=%d, src=0x%x, dst=0x%x\n",
          fwd_srap_to_pc5_q.q[i].rb_idP, fwd_srap_to_pc5_q.q[i].sdu_sizeP,
          fwd_srap_to_pc5_q.q[i].sourceL2Id, fwd_srap_to_pc5_q.q[i].destinationL2Id);

    rlc_data_req(&fwd_srap_to_pc5_q.q[i].ctxt_pP,
                 fwd_srap_to_pc5_q.q[i].srb_flagP,
                 fwd_srap_to_pc5_q.q[i].MBMS_flagP,
                 fwd_srap_to_pc5_q.q[i].rb_idP,
                 fwd_srap_to_pc5_q.q[i].muiP,
                 fwd_srap_to_pc5_q.q[i].confirmP,
                 fwd_srap_to_pc5_q.q[i].sdu_sizeP,
                 fwd_srap_to_pc5_q.q[i].sdu_pP,
                 &fwd_srap_to_pc5_q.q[i].sourceL2Id,
                 &fwd_srap_to_pc5_q.q[i].destinationL2Id,
                 PC5);

    if (pthread_mutex_lock(&fwd_srap_to_pc5_q.m) != 0) abort();

    fwd_srap_to_pc5_q.length--;
    fwd_srap_to_pc5_q.start = (fwd_srap_to_pc5_q.start + 1) % SRAP_DATA_REQ_QUEUE_SIZE;

    if (pthread_cond_signal(&fwd_srap_to_pc5_q.c) != 0) abort();
    if (pthread_mutex_unlock(&fwd_srap_to_pc5_q.m) != 0) abort();
  }
}

static void *fwd_srap_to_uu_data_req_thread(void *_)
{
  int i;

  LOG_D(NR_SRAP, "fwd_srap_to_uu_data_req_thread created on core %d\n", sched_getcpu());
  pthread_setname_np(pthread_self(), "Fwd SRAP UU queue");
  while (1) {
    if (pthread_mutex_lock(&fwd_srap_to_uu_q.m) != 0) abort();
    while (fwd_srap_to_uu_q.length == 0)
      if (pthread_cond_wait(&fwd_srap_to_uu_q.c, &fwd_srap_to_uu_q.m) != 0) abort();
    i = fwd_srap_to_uu_q.start;
    if (pthread_mutex_unlock(&fwd_srap_to_uu_q.m) != 0) abort();
    LOG_D(NR_SRAP, "Pass data from SRAP fwd_srap_to_uu_q to RLC %s %d\n", __FUNCTION__, __LINE__);

    rlc_data_req(&fwd_srap_to_uu_q.q[i].ctxt_pP,
                 fwd_srap_to_uu_q.q[i].srb_flagP,
                 fwd_srap_to_uu_q.q[i].MBMS_flagP,
                 fwd_srap_to_uu_q.q[i].rb_idP,
                 fwd_srap_to_uu_q.q[i].muiP,
                 fwd_srap_to_uu_q.q[i].confirmP,
                 fwd_srap_to_uu_q.q[i].sdu_sizeP,
                 fwd_srap_to_uu_q.q[i].sdu_pP,
                 NULL,
                 NULL,
                 UU);

    if (pthread_mutex_lock(&fwd_srap_to_uu_q.m) != 0) abort();

    fwd_srap_to_uu_q.length--;
    fwd_srap_to_uu_q.start = (fwd_srap_to_uu_q.start + 1) % SRAP_DATA_REQ_QUEUE_SIZE;

    if (pthread_cond_signal(&fwd_srap_to_uu_q.c) != 0) abort();
    if (pthread_mutex_unlock(&fwd_srap_to_uu_q.m) != 0) abort();
  }
}

static void init_nr_srap_rlc_data_req_queue(bool gNB_flag)
{
  pthread_t t1, t2, t3, t4;

  if (!gNB_flag) {
    pthread_mutex_init(&tx_srap_pc5_q.m, NULL);
    pthread_cond_init(&tx_srap_pc5_q.c, NULL);
    threadCreate(&t1, tx_srap_rlc_pc5_data_req_thread, NULL, "tx_srap_rlc_pc5_data_req_thread", -1, OAI_PRIORITY_RT_MAX - 1);
  }
  bool is_relay_ue = get_softmodem_params()->is_relay_ue;
  uint8_t relay_type = get_softmodem_params()->relay_type;
  bool is_u2n_relay_ue = is_relay_ue && (relay_type == U2N);
  bool is_u2u_relay_ue = is_relay_ue && (relay_type == U2U);

  if (gNB_flag || is_u2n_relay_ue) {
    pthread_mutex_init(&tx_srap_uu_q.m, NULL);
    pthread_cond_init(&tx_srap_uu_q.c, NULL);
    threadCreate(&t2, tx_srap_rlc_uu_data_req_thread, NULL, "tx_srap_rlc_uu_data_req_thread", -1, OAI_PRIORITY_RT_MAX - 1);
    if (is_u2n_relay_ue || is_u2u_relay_ue) {
      pthread_mutex_init(&fwd_srap_to_pc5_q.m, NULL);
      pthread_cond_init(&fwd_srap_to_pc5_q.c, NULL);
      threadCreate(&t3, fwd_srap_to_pc5_data_req_thread, NULL, "fwd_srap_to_pc5_data_req_thread", -1, OAI_PRIORITY_RT_MAX - 1);
      if (is_u2n_relay_ue) {
        pthread_mutex_init(&fwd_srap_to_uu_q.m, NULL);
        pthread_cond_init(&fwd_srap_to_uu_q.c, NULL);
        threadCreate(&t4, fwd_srap_to_uu_data_req_thread, NULL, "fwd_srap_to_uu_data_req_thread", -1, OAI_PRIORITY_RT_MAX - 1);
      }
    }
  }
}

void enqueue_fwd_srap_pc5_data_req(protocol_ctxt_t *const ctxt_pP,
                                   const srb_flag_t srb_flagP,
                                   const rb_id_t rb_idP,
                                   const mui_t muiP,
                                   confirm_t confirmP,
                                   sdu_size_t sdu_sizeP,
                                   mem_block_t *sdu_pP,
                                   const uint32_t sourceL2Id,
                                   const uint32_t destinationL2Id)
{
  int i;
  int logged = 0;
  if (pthread_mutex_lock(&fwd_srap_to_pc5_q.m) != 0) abort();
  while (fwd_srap_to_pc5_q.length == SRAP_DATA_REQ_QUEUE_SIZE) {
    if (!logged) {
      logged = 1;
      LOG_W(NR_SRAP, "%s: fwd_srap_to_pc5_q data queue is full\n", __FUNCTION__);
    }
    if (pthread_cond_wait(&fwd_srap_to_pc5_q.c, &fwd_srap_to_pc5_q.m) != 0) abort();
  }

  i = (fwd_srap_to_pc5_q.start + fwd_srap_to_pc5_q.length) % SRAP_DATA_REQ_QUEUE_SIZE;
  fwd_srap_to_pc5_q.length++;

  fwd_srap_to_pc5_q.q[i].ctxt_pP          = *ctxt_pP;
  fwd_srap_to_pc5_q.q[i].srb_flagP        = srb_flagP;
  fwd_srap_to_pc5_q.q[i].rb_idP           = rb_idP;
  fwd_srap_to_pc5_q.q[i].muiP             = muiP;
  fwd_srap_to_pc5_q.q[i].confirmP         = confirmP;
  fwd_srap_to_pc5_q.q[i].sdu_sizeP        = sdu_sizeP;
  fwd_srap_to_pc5_q.q[i].sdu_pP           = sdu_pP;
  fwd_srap_to_pc5_q.q[i].sourceL2Id       = sourceL2Id;
  fwd_srap_to_pc5_q.q[i].destinationL2Id  = destinationL2Id;

  if (pthread_cond_signal(&fwd_srap_to_pc5_q.c) != 0) abort();
  if (pthread_mutex_unlock(&fwd_srap_to_pc5_q.m) != 0) abort();
}

void enqueue_fwd_srap_uu_data_req(protocol_ctxt_t *const ctxt_pP,
                                  const srb_flag_t srb_flagP,
                                  const rb_id_t rb_idP,
                                  const mui_t muiP,
                                  confirm_t confirmP,
                                  sdu_size_t sdu_sizeP,
                                  mem_block_t *sdu_pP)
{
  int i;
  int logged = 0;
  if (pthread_mutex_lock(&fwd_srap_to_uu_q.m) != 0) abort();
  while (fwd_srap_to_uu_q.length == SRAP_DATA_REQ_QUEUE_SIZE) {
    if (!logged) {
      logged = 1;
      LOG_W(NR_SRAP, "%s: fwd_srap_to_uu_q data queue is full\n", __FUNCTION__);
    }
    if (pthread_cond_wait(&fwd_srap_to_uu_q.c, &fwd_srap_to_uu_q.m) != 0) abort();
  }

  i = (fwd_srap_to_uu_q.start + fwd_srap_to_uu_q.length) % SRAP_DATA_REQ_QUEUE_SIZE;
  fwd_srap_to_uu_q.length++;

  fwd_srap_to_uu_q.q[i].ctxt_pP    = *ctxt_pP;
  fwd_srap_to_uu_q.q[i].srb_flagP  = srb_flagP;
  fwd_srap_to_uu_q.q[i].rb_idP     = rb_idP;
  fwd_srap_to_uu_q.q[i].muiP       = muiP;
  fwd_srap_to_uu_q.q[i].confirmP   = confirmP;
  fwd_srap_to_uu_q.q[i].sdu_sizeP  = sdu_sizeP;
  fwd_srap_to_uu_q.q[i].sdu_pP     = sdu_pP;

  if (pthread_cond_signal(&fwd_srap_to_uu_q.c) != 0) abort();
  if (pthread_mutex_unlock(&fwd_srap_to_uu_q.m) != 0) abort();
}

static void enqueue_srap_pc5_data_req(const protocol_ctxt_t *const ctxt_pP,
                                      const srb_flag_t   srb_flagP,
                                      const MBMS_flag_t  MBMS_flagP,
                                      const rb_id_t      rb_idP,
                                      const mui_t        muiP,
                                      confirm_t    confirmP,
                                      sdu_size_t   sdu_sizeP,
                                      mem_block_t *sdu_pP)
{
  int i;
  int logged = 0;
  if (pthread_mutex_lock(&tx_srap_pc5_q.m) != 0) abort();
  while (tx_srap_pc5_q.length == SRAP_DATA_REQ_QUEUE_SIZE) {
    if (!logged) {
      logged = 1;
      LOG_W(NR_SRAP, "%s: tx_srap_pc5_q data queue is full\n", __FUNCTION__);
    }
    if (pthread_cond_wait(&tx_srap_pc5_q.c, &tx_srap_pc5_q.m) != 0) abort();
  }

  i = (tx_srap_pc5_q.start + tx_srap_pc5_q.length) % SRAP_DATA_REQ_QUEUE_SIZE;
  tx_srap_pc5_q.length++;

  tx_srap_pc5_q.q[i].ctxt_pP    = *ctxt_pP;
  tx_srap_pc5_q.q[i].srb_flagP  = srb_flagP;
  tx_srap_pc5_q.q[i].MBMS_flagP = MBMS_flagP;
  tx_srap_pc5_q.q[i].rb_idP     = rb_idP;
  tx_srap_pc5_q.q[i].muiP       = muiP;
  tx_srap_pc5_q.q[i].confirmP   = confirmP;
  tx_srap_pc5_q.q[i].sdu_sizeP  = sdu_sizeP;
  tx_srap_pc5_q.q[i].sdu_pP     = sdu_pP;
  /* For PC5 sidelink: set sourceL2Id from context (transmitter's L2 ID)
     and destinationL2Id = 0 (destination will be determined by lower layers) */
  tx_srap_pc5_q.q[i].sourceL2Id = ctxt_pP->rntiMaybeUEid;
  tx_srap_pc5_q.q[i].destinationL2Id = 0;  // For Remote UE: 0 = Relay UE

  if (pthread_cond_signal(&tx_srap_pc5_q.c) != 0) abort();
  if (pthread_mutex_unlock(&tx_srap_pc5_q.m) != 0) abort();
}

static void enqueue_srap_uu_data_req(const protocol_ctxt_t *const ctxt_pP,
                                     const srb_flag_t   srb_flagP,
                                     const MBMS_flag_t  MBMS_flagP,
                                     const rb_id_t      rb_idP,
                                     const mui_t        muiP,
                                     confirm_t    confirmP,
                                     sdu_size_t   sdu_sizeP,
                                     mem_block_t *sdu_pP)
{
  int i;
  int logged = 0;

  if (pthread_mutex_lock(&tx_srap_uu_q.m) != 0) abort();

  while (tx_srap_uu_q.length == SRAP_DATA_REQ_QUEUE_SIZE) {
    if (!logged) {
      logged = 1;
      LOG_W(NR_SRAP, "%s: tx_srap_uu_q data queue is full\n", __FUNCTION__);
    }
    if (pthread_cond_wait(&tx_srap_uu_q.c, &tx_srap_uu_q.m) != 0) abort();
  }

  i = (tx_srap_uu_q.start + tx_srap_uu_q.length) % SRAP_DATA_REQ_QUEUE_SIZE;
  tx_srap_uu_q.length++;

  tx_srap_uu_q.q[i].ctxt_pP    = *ctxt_pP;
  tx_srap_uu_q.q[i].srb_flagP  = srb_flagP;
  tx_srap_uu_q.q[i].MBMS_flagP = MBMS_flagP;
  tx_srap_uu_q.q[i].rb_idP     = rb_idP;
  tx_srap_uu_q.q[i].muiP       = muiP;
  tx_srap_uu_q.q[i].confirmP   = confirmP;
  tx_srap_uu_q.q[i].sdu_sizeP  = sdu_sizeP;
  tx_srap_uu_q.q[i].sdu_pP     = sdu_pP;

  if (pthread_cond_signal(&tx_srap_uu_q.c) != 0) abort();
  if (pthread_mutex_unlock(&tx_srap_uu_q.m) != 0) abort();
}

// TX callback: Enqueue SRAP message for transmission (used by TX path only)
void srap_deliver_pdu_drb(protocol_ctxt_t *ctxt, int rb_id,
                          char *buf, int size, int sdu_id,
                          nr_intf_type_t intf_type) {
  mem_block_t *memblock = get_free_mem_block(size, __FUNCTION__);
  memcpy(memblock->data, buf, size);

  /* Decouple SRAP header bearer_id from the Uu transport DRB.
     The caller passes the Remote UE's LOGICAL DRB id (rb_id), which process_sdu
     already encoded into the SRAP header bearer_id (logical DRB1 -> bearer 4).
     The actual Uu transport is the Relay UE's DEDICATED relayed-traffic DRB2
     (Relay DRB1 carries the Relay UE's own traffic). Only remap for gNB->Relay
     downlink of Remote UE traffic, identified by ctxt->remote_ue_id > 0. */
  int transport_rb_id = rb_id;
  if (intf_type == UU && ctxt->remote_ue_id > 0) {
    transport_rb_id = 2;  // Relay UE Uu DRB2 = relayed Remote UE traffic
  }

  /* For PC5: rb_id stays as DRB ID (1, 2, etc) for RLC entity lookup
     LCID mapping happens at MAC layer based on logical channel config */
  if (intf_type == PC5)
    enqueue_srap_pc5_data_req(ctxt, 0, MBMS_FLAG_NO, transport_rb_id, sdu_id, 0, size, memblock);
  else if (intf_type == UU)
    enqueue_srap_uu_data_req(ctxt, 0, MBMS_FLAG_NO, transport_rb_id, sdu_id, 0, size, memblock);
}

int srap_module_init(bool gNB_flag)
{
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  static int inited = 0;
  static int inited_ue = 0;

  if (pthread_mutex_lock(&lock)) abort();

  if (gNB_flag == true && inited) {
    LOG_E(NR_SRAP, "%s:%d:%s: fatal, inited already 1\n", __FILE__, __LINE__, __FUNCTION__);
    exit(EXIT_FAILURE);
  }

  if (gNB_flag == false && inited_ue) {
    LOG_E(NR_SRAP, "%s:%d:%s: fatal, inited_ue already 1\n", __FILE__, __LINE__, __FUNCTION__);
    exit(EXIT_FAILURE);
  }

  if (gNB_flag == true) inited = 1;
  if (gNB_flag == false) inited_ue = 1;

  LOG_D(NR_SRAP, "Created NR SRAP UE Mananger!!!");

  if (pthread_mutex_unlock(&lock)) abort();

  return 0;
}

void nr_srap_layer_init(bool gNB_flag)
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

  nr_srap_manager = new_nr_srap_manager(gNB_flag);

  init_nr_srap_data_ind_queue(gNB_flag);
  init_nr_srap_rlc_data_req_queue(gNB_flag);

}

static void do_srap_data_ind(protocol_ctxt_t *const  ctxt_pP,
                             const srb_flag_t srb_flagP,
                             const MBMS_flag_t MBMS_flagP,
                             const rb_id_t rb_id,
                             const sdu_size_t sdu_buffer_size,
                             mem_block_t *sdu_buffer,
                             nr_intf_type_t intf_type)
{
  if (ctxt_pP->module_id != 0 ||
      ctxt_pP->instance != 0  ||
      ctxt_pP->eNB_index != 0 ||
      ctxt_pP->brOption != 0) {
    LOG_E(NR_SRAP, "%s:%d:%s: fatal\n", __FILE__, __LINE__, __FUNCTION__);
    exit(EXIT_FAILURE);
  }

  nr_srap_manager_t  *m = get_nr_srap_manager();

  nr_srap_entity_t *srap_entity;
  if (intf_type == UU) {
    srap_entity = nr_srap_get_entity(m, NR_SRAP_UU);
  } else if (intf_type == PC5) {
    srap_entity = nr_srap_get_entity(m, NR_SRAP_PC5);
  }

  if ((srap_entity != NULL)) {
    srap_entity->recv_pdu(ctxt_pP, srap_entity, (char *)(sdu_buffer->data), sdu_buffer_size, srb_flagP, MBMS_flagP, rb_id);
  } else {
    LOG_E(NR_SRAP, "%s:%d:%s: no SRAP entity found (rb_id %ld, srb_flag %d)\n", __FILE__, __LINE__, __FUNCTION__, rb_id, srb_flagP);
  }

  free_mem_block(sdu_buffer, __FUNCTION__);
}

static void *srap_pc5_data_ind_thread(void *_)
{
  int i;

  pthread_setname_np(pthread_self(), "SRAP PC5 data ind");
  while (1) {
    if (pthread_mutex_lock(&rx_srap_pc5_q.m) != 0) {
      abort();
    }
    while (rx_srap_pc5_q.length == 0)
      if (pthread_cond_wait(&rx_srap_pc5_q.c, &rx_srap_pc5_q.m) != 0) {
        abort();
      }
    i = rx_srap_pc5_q.start;
    if (pthread_mutex_unlock(&rx_srap_pc5_q.m) != 0) {
      abort();
    }

    do_srap_data_ind(&rx_srap_pc5_q.q[i].ctxt_pP,
                     rx_srap_pc5_q.q[i].srb_flagP,
                     rx_srap_pc5_q.q[i].MBMS_flagP,
                     rx_srap_pc5_q.q[i].rb_id,
                     rx_srap_pc5_q.q[i].sdu_buffer_size,
                     rx_srap_pc5_q.q[i].sdu_buffer,
                     PC5);

    if (pthread_mutex_lock(&rx_srap_pc5_q.m) != 0) {
      abort();
    }

    rx_srap_pc5_q.length--;
    rx_srap_pc5_q.start = (rx_srap_pc5_q.start + 1) % SRAP_DATA_IND_QUEUE_SIZE;

    if (pthread_cond_signal(&rx_srap_pc5_q.c) != 0) {
      abort();
    }
    if (pthread_mutex_unlock(&rx_srap_pc5_q.m) != 0) {
      abort();
    }
  }
}

static void *srap_uu_data_ind_thread(void *_)
{
  int i;

  pthread_setname_np(pthread_self(), "SRAP Uu data ind");
  while (1) {
    if (pthread_mutex_lock(&rx_srap_uu_q.m) != 0) abort();
    while (rx_srap_uu_q.length == 0)
      if (pthread_cond_wait(&rx_srap_uu_q.c, &rx_srap_uu_q.m) != 0) abort();
    i = rx_srap_uu_q.start;
    if (pthread_mutex_unlock(&rx_srap_uu_q.m) != 0) abort();

    do_srap_data_ind(&rx_srap_uu_q.q[i].ctxt_pP,
                     rx_srap_uu_q.q[i].srb_flagP,
                     rx_srap_uu_q.q[i].MBMS_flagP,
                     rx_srap_uu_q.q[i].rb_id,
                     rx_srap_uu_q.q[i].sdu_buffer_size,
                     rx_srap_uu_q.q[i].sdu_buffer,
                     UU);
    if (pthread_mutex_lock(&rx_srap_uu_q.m) != 0) abort();

    rx_srap_uu_q.length--;
    rx_srap_uu_q.start = (rx_srap_uu_q.start + 1) % SRAP_DATA_IND_QUEUE_SIZE;

    if (pthread_cond_signal(&rx_srap_uu_q.c) != 0) abort();
    if (pthread_mutex_unlock(&rx_srap_uu_q.m) != 0) abort();
  }
}

static void init_nr_srap_data_ind_queue(bool gNB_flag)
{
  pthread_t t1, t2;

  if (!gNB_flag) {
    pthread_mutex_init(&rx_srap_pc5_q.m, NULL);
    pthread_cond_init(&rx_srap_pc5_q.c, NULL);
    if (pthread_create(&t1, NULL, srap_pc5_data_ind_thread, NULL) != 0) {
      LOG_E(NR_SRAP, "%s:%d:%s: fatal\n", __FILE__, __LINE__, __FUNCTION__);
      exit(EXIT_FAILURE);
    }
  }
  if (gNB_flag || (get_softmodem_params()->is_relay_ue && (get_softmodem_params()->relay_type == U2N))) {
    pthread_mutex_init(&rx_srap_uu_q.m, NULL);
    pthread_cond_init(&rx_srap_uu_q.c, NULL);
    if (pthread_create(&t2, NULL, srap_uu_data_ind_thread, NULL) != 0) {
      LOG_E(NR_SRAP, "%s:%d:%s: fatal\n", __FILE__, __LINE__, __FUNCTION__);
      exit(EXIT_FAILURE);
    }
  }
}

static void enqueue_srap_pc5_data_ind(protocol_ctxt_t *const  ctxt_pP,
                                      const srb_flag_t srb_flagP,
                                      const MBMS_flag_t MBMS_flagP,
                                      const rb_id_t rb_id,
                                      const sdu_size_t sdu_buffer_size,
                                      mem_block_t *const sdu_buffer)
{
  int i;
  int logged = 0;

  if (pthread_mutex_lock(&rx_srap_pc5_q.m) != 0) abort();
  while (rx_srap_pc5_q.length == SRAP_DATA_IND_QUEUE_SIZE) {
    if (!logged) {
      logged = 1;
      LOG_W(NR_SRAP, "%s: pdcp_data_ind queue is full\n", __FUNCTION__);
    }
    if (pthread_cond_wait(&rx_srap_pc5_q.c, &rx_srap_pc5_q.m) != 0) abort();
  }
  LOG_D(NR_SRAP, "Enqueuing data in rx_srap_pc5_q %s %d\n", __FUNCTION__, __LINE__);
  i = (rx_srap_pc5_q.start + rx_srap_pc5_q.length) % SRAP_DATA_IND_QUEUE_SIZE;
  rx_srap_pc5_q.length++;

  rx_srap_pc5_q.q[i].ctxt_pP         = *ctxt_pP;
  rx_srap_pc5_q.q[i].srb_flagP       = srb_flagP;
  rx_srap_pc5_q.q[i].MBMS_flagP      = MBMS_flagP;
  rx_srap_pc5_q.q[i].rb_id           = rb_id;
  rx_srap_pc5_q.q[i].sdu_buffer_size = sdu_buffer_size;
  rx_srap_pc5_q.q[i].sdu_buffer      = sdu_buffer;

  if (pthread_cond_signal(&rx_srap_pc5_q.c) != 0) abort();
  if (pthread_mutex_unlock(&rx_srap_pc5_q.m) != 0) abort();
}

static void enqueue_srap_uu_data_ind(protocol_ctxt_t *const  ctxt_pP,
                                     const srb_flag_t srb_flagP,
                                     const MBMS_flag_t MBMS_flagP,
                                     const rb_id_t rb_id,
                                     const sdu_size_t sdu_buffer_size,
                                     mem_block_t *const sdu_buffer)
{
  int i;
  int logged = 0;

  if (pthread_mutex_lock(&rx_srap_uu_q.m) != 0) abort();
  while (rx_srap_uu_q.length == SRAP_DATA_IND_QUEUE_SIZE) {
    if (!logged) {
      logged = 1;
      LOG_W(NR_SRAP, "%s: pdcp_data_ind queue is full\n", __FUNCTION__);
    }
    if (pthread_cond_wait(&rx_srap_uu_q.c, &rx_srap_uu_q.m) != 0) abort();
  }
  LOG_D(NR_SRAP, "Enqueuing data in rx_srap_uu_q %s %d\n", __FUNCTION__, __LINE__);
  i = (rx_srap_uu_q.start + rx_srap_uu_q.length) % SRAP_DATA_IND_QUEUE_SIZE;
  rx_srap_uu_q.length++;

  rx_srap_uu_q.q[i].ctxt_pP         = *ctxt_pP;
  rx_srap_uu_q.q[i].srb_flagP       = srb_flagP;
  rx_srap_uu_q.q[i].MBMS_flagP      = MBMS_flagP;
  rx_srap_uu_q.q[i].rb_id           = rb_id;
  rx_srap_uu_q.q[i].sdu_buffer_size = sdu_buffer_size;
  rx_srap_uu_q.q[i].sdu_buffer      = sdu_buffer;

  if (pthread_cond_signal(&rx_srap_uu_q.c) != 0) abort();
  if (pthread_mutex_unlock(&rx_srap_uu_q.m) != 0) abort();
}

bool srap_data_ind(protocol_ctxt_t *const ctxt_pP,
                   const srb_flag_t srb_flagP,
                   const MBMS_flag_t MBMS_flagP,
                   const rb_id_t rb_id,
                   const sdu_size_t sdu_buffer_size,
                   mem_block_t *const sdu_buffer,
                   const uint32_t *const srcID,
                   const uint32_t *const dstID,
                   nr_intf_type_t intf_type)
{
  if (intf_type == PC5) {
    enqueue_srap_pc5_data_ind(ctxt_pP,
                              srb_flagP,
                              MBMS_flagP,
                              rb_id,
                              sdu_buffer_size,
                              sdu_buffer);
  } else if (intf_type == UU) {
    enqueue_srap_uu_data_ind(ctxt_pP,
                             srb_flagP,
                             MBMS_flagP,
                             rb_id,
                             sdu_buffer_size,
                             sdu_buffer);
  }
  return true;
}

void srap_deliver_sdu_drb(const protocol_ctxt_t *const  ctxt_pP,
                          void *_ue, nr_srap_entity_t *entity,
                          char *buf, int size,
                          const srb_flag_t srb_flagP,
                          const MBMS_flag_t MBMS_flagP,
                          const rb_id_t rb_id) {

  mem_block_t *memblock = get_free_mem_block(size, __func__);

  nr_intf_type_t intf_type = (entity->type == NR_SRAP_UU) ? UU : PC5;

  if (memblock == NULL) {
    LOG_E(NR_SRAP, "%s:%d:%s: ERROR: malloc16 failed\n", __FILE__, __LINE__, __FUNCTION__);
    exit(EXIT_FAILURE);
  }

  memcpy(memblock->data, buf, size);

  /* PC5 SL-SRBs at Remote UE: No PDCP entities configured, deliver directly to RRC
     The gNB→Relay→Remote path for RRCSetup uses pure RRC (no PDCP header)
     RRCReconfiguration would use PDCP, but PC5 PDCP entities aren't created yet */
  if (srb_flagP && intf_type == PC5) {
    if (rb_id == 0) {
      // SRB0 (CCCH): RRCSetupRequest or initial access messages
      NR_SRB_INFO srb_info;
      memset(&srb_info, 0, sizeof(srb_info));
      memcpy(srb_info.Rx_buffer.Payload, buf, size);
      srb_info.Rx_buffer.payload_size = size;

      nr_rrc_ue_decode_ccch(ctxt_pP, &srb_info, 0);
      free_mem_block(memblock, __FUNCTION__);
      return;
    } else if (rb_id == 1 || rb_id == 2) {
      /* SRB1/SRB2 from gNB via Relay: Route based on RRC state and message type
         When UE is in IDLE state, bearer_id=1 messages are CCCH (RRCSetup, RRCReject)
         When UE is in CONNECTED state, bearer_id=1 messages are DCCH (SecurityModeCommand, RRCReconfiguration, etc.) */

      /* Check RRC state - if IDLE, route to CCCH decoder regardless of first byte
         NR_UE_rrc_inst is only available in UE builds (weak symbol, NULL in gNB builds) */
      bool is_idle = false;
      if (NR_UE_rrc_inst && ctxt_pP->module_id < NB_NR_UE_INST) {
        is_idle = (NR_UE_rrc_inst[ctxt_pP->module_id].nrRrcState == RRC_STATE_IDLE_NR);
      }

      /* If UE is IDLE, route to CCCH decoder (expecting RRCSetup)
         Otherwise use first-byte discrimination for DCCH messages */
      if (is_idle && rb_id == 1) {
        // CCCH message (RRCSetup, RRCReject) - deliver to CCCH decoder
        NR_SRB_INFO srb_info;
        memset(&srb_info, 0, sizeof(srb_info));
        memcpy(srb_info.Rx_buffer.Payload, buf, size);
        srb_info.Rx_buffer.payload_size = size;

        nr_rrc_ue_decode_ccch(ctxt_pP, &srb_info, 0);
      } else {
        /* UE is CONNECTED - use state-based discrimination instead of byte patterns
           When UE is in CONNECTED state receiving on SRB1, always use DCCH decoder
           Rationale: RRCReconfiguration with large payloads (e.g., 289 bytes with NAS PDU)
           can have varying first bytes (0x02, 0x04, 0x06, etc.) due to ASN.1 UPER encoding
           Byte-pattern discrimination is unreliable and causes misrouting to CCCH decoder
           Similar to RRCSetup discrimination fix at Relay UE (rrc_UE.c:3040) */

        /* State-based rule: If UE is CONNECTED, all messages on SRB1 are DCCH messages
           The only exception is during RRC setup, which is handled by the rrc_state == RRC_STATE_IDLE_NR check above */

        /* Message arrived from the gNB over the SRAP relay path (cellular Uu
           signalling). Tag it UU so the Remote UE sends RRCReconfigurationComplete
           regardless of the gNB's transaction id. */
        nr_rrc_ue_decode_dcch(ctxt_pP, rb_id, (uint8_t *)buf, size, 0, UU);
      }

      free_mem_block(memblock, __FUNCTION__);
      return;
    }
  }

  /* Convert bearer_id (from SRAP header / MAC LCID) to PDCP rb_id:
     For SRBs: bearer_id = PDCP rb_id (0→SRB0, 1→SRB1, 2→SRB2)
     For DRBs: bearer_id = LCID = PDCP rb_id + 3 (LCID 4 → DRB 1, LCID 5 → DRB 2) */
  rb_id_t rb_id_temp = srb_flagP ? rb_id : rb_id - 3;

  // Sending data indication to PDCP at the destination
  if (!pdcp_data_ind(ctxt_pP, srb_flagP, MBMS_flagP, rb_id_temp, size, memblock, NULL, NULL, intf_type)) {
    LOG_E(NR_SRAP, "%s:%d:%s: ERROR: pdcp_data_ind failed\n", __FILE__, __LINE__, __FUNCTION__);
    /* what to do in case of failure? for the moment: nothing */
  }
}

bool nr_srap_data_req_drb(protocol_ctxt_t *ctxt,
                          const rb_id_t rb_id,
                          const mui_t sdu_id,
                          const sdu_size_t sdu_buffer_size,
                          char *sdu_buffer,
                          nr_intf_type_t intf_type) {
    uint8_t relay_type = get_softmodem_params()->relay_type;
    nr_srap_manager_internal_t *m = nr_srap_manager;

    AssertFatal(m != NULL, "SRAP manager is not initialized!!!");

    U2NHeader_t u2n_header;
    U2UHeader_t u2u_header;
    int srap_pdu_size = sdu_buffer_size + ((relay_type == U2N) ? sizeof(u2n_header) : sizeof(u2u_header));
    char pdu_buf[srap_pdu_size];
    /* For relay, we need to work on two entities:
      (srap_entity[0] - pc5 and srap_entity[1] - uu) - Relay
      srap_entity[0] - gNB;
      srap_entity[0] - remote UE;
    */
    nr_srap_entity_t *srap_entity = NULL;
    if (intf_type == UU) {
      srap_entity = nr_srap_get_entity(m, NR_SRAP_UU);
    } else if (intf_type == PC5) {
      srap_entity = nr_srap_get_entity(m, NR_SRAP_PC5);
    }

    if ((srap_entity != NULL)) {
      /* Determine remote_ue_id: use context if available (gNB downlink to Remote UE),
         otherwise use softmodem param (Remote UE uplink, Relay UE forwarding) */
      uint8_t remote_ue_id = (ctxt->remote_ue_id > 0) ? ctxt->remote_ue_id : get_softmodem_params()->remote_ue_id;

      uint8_t dc_bit = 1;

      // Pass remote_ue_id explicitly to process_sdu
      srap_entity->process_sdu(sdu_buffer, sdu_buffer_size, relay_type, rb_id, pdu_buf,
                              (relay_type == U2N) ? sizeof(u2n_header) : sizeof(u2u_header),
                              (relay_type == U2N) ? (void*)&u2n_header : (void*)&u2u_header,
                              dc_bit, remote_ue_id);

      srap_deliver_pdu deliver_pdu_cb = srap_entity->deliver_pdu;
      deliver_pdu_cb(ctxt, rb_id, pdu_buf, srap_pdu_size, sdu_id, intf_type);

      return true;
   }
  LOG_E(NR_SRAP, "[SRAP RX] srap_data_ind: entity is NULL! intf_type=%d\n", intf_type);
  return false;
}


bool nr_srap_data_req_srb(protocol_ctxt_t *ctxt,
                          const rb_id_t rb_id,
                          const sdu_size_t sdu_buffer_size,
                          char *sdu_buffer,
                          srap_deliver_pdu deliver_pdu_cb,
                          int sdu_id,
                          nr_intf_type_t intf_type) {
    uint8_t relay_type = get_softmodem_params()->relay_type;
    nr_srap_manager_internal_t *m = nr_srap_manager;

    if (m == NULL) {
        LOG_E(NR_SRAP, "SRAP manager is not initialized!!!");
        return false;
    }

    /* Determine remote_ue_id based on node type:
       - Remote UE (intf_type==PC5): Always use softmodem param (context not explicitly initialized)
       - gNB (intf_type==UU): Use context if set (gNB explicitly sets it per Remote UE), otherwise softmodem param */
    uint8_t remote_ue_id;
    if (intf_type == PC5) {
      // Remote UE transmitting: use softmodem parameter
      remote_ue_id = get_softmodem_params()->remote_ue_id;
    } else {
      // gNB or Relay UE: use context if available, otherwise softmodem parameter
      remote_ue_id = (ctxt->remote_ue_id > 0) ? ctxt->remote_ue_id : get_softmodem_params()->remote_ue_id;
    }
    if (remote_ue_id == 0) remote_ue_id = 1; // Default fallback

    U2NHeader_t u2n_header;
    U2UHeader_t u2u_header;
    /* Always add SRAP header, including for SRB0 (episys/sl-mode1-relay behavior)
       This ensures Relay UE can extract remote_ue_id from header, not from RRC message bytes */
    int srap_pdu_size = sdu_buffer_size + ((relay_type == U2N) ? sizeof(u2n_header) : sizeof(u2u_header));
    char pdu_buf[srap_pdu_size];
    /* For relay, we need to work on two entities:
      (srap_entity[0] - pc5 and srap_entity[1] - uu) - Relay
      srap_entity[0] - gNB;
      srap_entity[0] - remote UE;
    */
    nr_srap_entity_t *srap_entity = NULL;
    if (intf_type == UU) {
      srap_entity = nr_srap_get_entity(m, NR_SRAP_UU);
    } else if (intf_type == PC5) {
      srap_entity = nr_srap_get_entity(m, NR_SRAP_PC5);
    }
    if ((srap_entity != NULL)) {
      // Always use SRAP header (episys/sl-mode1-relay behavior)
      uint8_t header_size = (relay_type == U2N) ? sizeof(u2n_header) : sizeof(u2u_header);
      void *srap_header = (relay_type == U2N) ? (void*)&u2n_header : (void*)&u2u_header;
      uint8_t dc_bit = 0;

      // Always use standard SRAP header creation (episys/sl-mode1-relay behavior)
      if (relay_type == U2N) {
        // This is SRB path - rb_id is already correct (SRB0 = rb_id 0, SRB1 = rb_id 1)
        create_header(dc_bit, relay_type, rb_id, -1, remote_ue_id, (U2NHeader_t*)srap_header);
        encode_srap_header((U2NHeader_t*)srap_header, (uint8_t*)pdu_buf);
        memcpy(pdu_buf + header_size, sdu_buffer, sdu_buffer_size);
      } else {
        // U2U relay type - use entity's process_sdu
        srap_entity->process_sdu(sdu_buffer,
                                sdu_buffer_size,
                                relay_type,
                                rb_id,
                                pdu_buf,
                                header_size,
                                srap_header,
                                dc_bit,
                                remote_ue_id);
      }

      deliver_pdu_cb(ctxt, rb_id, pdu_buf, srap_pdu_size, sdu_id, intf_type);
      return true;
   }
  return false;
}

void srap_deliver_pdu_srb(protocol_ctxt_t *ctxt, int srb_id, char *buf,
                          int size, int sdu_id, nr_intf_type_t intf_type)
{
  mem_block_t *memblock = get_free_mem_block(size, __FUNCTION__);
  memcpy(memblock->data, buf, size);

  if (intf_type == PC5)
    enqueue_srap_pc5_data_req(ctxt, 1, MBMS_FLAG_NO, srb_id, sdu_id, 0, size, memblock);
  else if (intf_type == UU)
    enqueue_srap_uu_data_req(ctxt, 1, MBMS_FLAG_NO, srb_id, sdu_id, 0, size, memblock);
}

/* Called by Uu RLC when it receives a SRAP U2N message
   At gNB: Strip header and deliver to RRC for Remote UE processing
   At Relay UE: Strip header and forward to PC5 for Remote UE */
void nr_srap_rlc_data_ind(int rb_id, char *buf, int size, nr_intf_type_t intf_type, rnti_t relay_rnti)
{
  if (intf_type != UU) {
    LOG_E(NR_SRAP, "nr_srap_rlc_data_ind: called with wrong interface type %d, expected UU\n", intf_type);
    return;
  }

  if (size < 2) {
    LOG_E(NR_SRAP, "Uu SRAP: message too small (%d bytes), need at least 2 for header\n", size);
    return;
  }

  bool is_gnb = get_softmodem_params()->is_relay_ue == 0 && get_softmodem_params()->relay_type == U2N;

  // Parse SRAP U2N header: [D/C+reserved+Bearer ID][Remote UE ID][RRC payload...]
  uint8_t bearer_id = (uint8_t)buf[0] & SRAP_HDR_BEARER_ID_MASK;  // Extract bearer_id from lower 5 bits
  uint8_t remote_ue_id = (uint8_t)buf[1];

  if (is_gnb) {
    /* gNB: Deliver to RRC via dedicated SRAP processing function
       RRC will detect SRAP header and process Remote UE message */

    // Forward declaration to avoid header dependency issues with CU-UP build
    extern int nr_rrc_gNB_process_srap_message(module_id_t module_id, rnti_t relay_rnti, const uint8_t *buffer, int buffer_length);

    // Call RRC wrapper function with full SRAP message [bearer_id][remote_ue_id][RRC]
    nr_rrc_gNB_process_srap_message(0, relay_rnti, (uint8_t*)buf, size);

  } else {
    /* Relay UE: Forward to PC5 for Remote UE
       Forward the FULL message including SRAP header - Remote UE SRAP will process it */

    /* Map Uu bearer to PC5 bearer:
       - bearer_id=0 (SRB0/CCCH) → PC5 SL-SRB0 (rb_id=0, LCID 56) - initial access only
       - bearer_id=1 (SRB1/DCCH) → PC5 SL-SRB1 (rb_id=1, LCID 57) - all signaling after RRCSetup
       - bearer_id=2 (SRB2) → PC5 SL-SRB2 (rb_id=2) - if configured
       SRBs (bearer 0/1/2) map 1:1 to PC5 SL-SRBs. DRBs carry the Remote UE's
       logical bearer as bearer_id = DRB + 3 (DRB1 -> bearer 4), so decode back
       to the PC5 DRB id (bearer 4 -> PC5 DRB1) for correct SL-RLC entity lookup. */
    int pc5_bearer_id = (bearer_id >= 4) ? (bearer_id - 3) : bearer_id;

    /* Send via proper SL-RLC0 bearer (supports segmentation and reliable delivery)
       Forward full SRAP message (including header) - Remote UE SRAP layer will strip it */
    mem_block_t *memblock = get_free_mem_block(size, __FUNCTION__);
    memcpy(memblock->data, buf, size);

    protocol_ctxt_t ctxt = {0};
    ctxt.module_id = 0;
    /* For PC5 sidelink bearers in SL Mode 1, use srcID (0) not Uu RNTI
       SL-RLC entities are indexed by PC5 L2 source ID, not Uu RNTI */
    ctxt.rntiMaybeUEid = 0;

    // PC5 sidelink requires src/dst L2 IDs for bearer lookup
    uint32_t src_l2_id = 0;  // Relay UE's srcid (from SL Mode 1 config)
    uint32_t dst_l2_id = (uint32_t)remote_ue_id;  // Remote UE's L2 ID

    /* srb_flag must reflect the traffic type, not be hardcoded: DRBs (bearer_id >= 4)
       are data and must land on the Remote UE's PC5 SL-DRB (→ PDCP → TUN). Forcing
       srb_flag=1 would deliver DRB data onto the PC5 SL-SRB and misroute it to the RRC
       decoder at the Remote UE, dropping the packet (e.g. ICMP echo reply). */
    srb_flag_t pc5_srb_flag = (bearer_id >= 4) ? 0 : 1;
    LOG_D(NR_SRAP, "[Relay UE] Uu→PC5: Sending %d bytes to PC5 SL-RLC%d (LCID %d) srb_flag=%d srcID=0x%x dstID=0x%x\n",
          size, pc5_bearer_id, 56 + pc5_bearer_id, pc5_srb_flag, src_l2_id, dst_l2_id);
    enqueue_fwd_srap_pc5_data_req(&ctxt, pc5_srb_flag, pc5_bearer_id, 0, 0, size, memblock, src_l2_id, dst_l2_id);
  }
}
