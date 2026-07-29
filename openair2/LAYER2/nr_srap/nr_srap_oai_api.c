/*
Author: Ejaz Ahmed
Email ID: ejaz.ahmed@applied.co
*/

#define _GNU_SOURCE

#include "nr_srap_oai_api.h"
#include "openair2/LAYER2/nr_pdcp/nr_pdcp_oai_api.h"
#include "openair2/LAYER2/nr_rlc/nr_rlc_oai_api.h"
#include <executables/nr-uesoftmodem.h>
#include "nr_srap_header.h"
#include "nr_srap_manager.h"
#include "common/ngran_types.h"

extern nr_srap_manager_t *nr_srap_manager;

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
    LOG_D(NR_SRAP, "Pass data from SRAP tx_srap_pc5_q to RLC%s %d\n", __FUNCTION__, __LINE__);
    // develop SL RLC TX: address ue->sl_drb[]/sl_srb[] by local src_id (carried in ctxt rntiMaybeUEid).
    // SL mode-1 U2N relay control plane: SRB SDUs go to the SL-SRB RLC, user data to the SL-DRB RLC.
    if (tx_srap_pc5_q.q[i].srb_flagP)
      nr_rlc_data_req_sl_srb(tx_srap_pc5_q.q[i].ctxt_pP.rntiMaybeUEid,
                             tx_srap_pc5_q.q[i].rb_idP,
                             tx_srap_pc5_q.q[i].muiP,
                             tx_srap_pc5_q.q[i].sdu_sizeP,
                             tx_srap_pc5_q.q[i].sdu_pP);
    else
      nr_rlc_data_req_sl(tx_srap_pc5_q.q[i].ctxt_pP.rntiMaybeUEid,
                         tx_srap_pc5_q.q[i].rb_idP,
                         tx_srap_pc5_q.q[i].muiP,
                         tx_srap_pc5_q.q[i].sdu_sizeP,
                         tx_srap_pc5_q.q[i].sdu_pP);

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

    // develop Uu RLC TX (6-arg nr_rlc_data_req; no MBMS/confirm)
    nr_rlc_data_req(&tx_srap_uu_q.q[i].ctxt_pP,
                    tx_srap_uu_q.q[i].srb_flagP,
                    tx_srap_uu_q.q[i].rb_idP,
                    tx_srap_uu_q.q[i].muiP,
                    tx_srap_uu_q.q[i].sdu_sizeP,
                    tx_srap_uu_q.q[i].sdu_pP);

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
    LOG_D(NR_SRAP, "Pass data from SRAP fwd_srap_to_pc5_q to RLC%s %d\n", __FUNCTION__, __LINE__);
    // develop SL RLC TX (relay forward -> PC5): SRB SDUs -> SL-SRB RLC, user data -> SL-DRB RLC.
    if (fwd_srap_to_pc5_q.q[i].srb_flagP)
      nr_rlc_data_req_sl_srb(fwd_srap_to_pc5_q.q[i].ctxt_pP.rntiMaybeUEid,
                             fwd_srap_to_pc5_q.q[i].rb_idP,
                             fwd_srap_to_pc5_q.q[i].muiP,
                             fwd_srap_to_pc5_q.q[i].sdu_sizeP,
                             fwd_srap_to_pc5_q.q[i].sdu_pP);
    else
      nr_rlc_data_req_sl(fwd_srap_to_pc5_q.q[i].ctxt_pP.rntiMaybeUEid,
                         fwd_srap_to_pc5_q.q[i].rb_idP,
                         fwd_srap_to_pc5_q.q[i].muiP,
                         fwd_srap_to_pc5_q.q[i].sdu_sizeP,
                         fwd_srap_to_pc5_q.q[i].sdu_pP);

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
    // develop Uu RLC TX (relay forward -> Uu; 6-arg nr_rlc_data_req)
    nr_rlc_data_req(&fwd_srap_to_uu_q.q[i].ctxt_pP,
                    fwd_srap_to_uu_q.q[i].srb_flagP,
                    fwd_srap_to_uu_q.q[i].rb_idP,
                    fwd_srap_to_uu_q.q[i].muiP,
                    fwd_srap_to_uu_q.q[i].sdu_sizeP,
                    fwd_srap_to_uu_q.q[i].sdu_pP);

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
                                   uint8_t *sdu_pP)
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

  fwd_srap_to_pc5_q.q[i].ctxt_pP    = *ctxt_pP;
  fwd_srap_to_pc5_q.q[i].srb_flagP  = srb_flagP;
  fwd_srap_to_pc5_q.q[i].rb_idP     = rb_idP;
  fwd_srap_to_pc5_q.q[i].muiP       = muiP;
  fwd_srap_to_pc5_q.q[i].confirmP   = confirmP;
  fwd_srap_to_pc5_q.q[i].sdu_sizeP  = sdu_sizeP;
  fwd_srap_to_pc5_q.q[i].sdu_pP     = sdu_pP;

  if (pthread_cond_signal(&fwd_srap_to_pc5_q.c) != 0) abort();
  if (pthread_mutex_unlock(&fwd_srap_to_pc5_q.m) != 0) abort();
}

void enqueue_fwd_srap_uu_data_req(protocol_ctxt_t *const ctxt_pP,
                                  const srb_flag_t srb_flagP,
                                  const rb_id_t rb_idP,
                                  const mui_t muiP,
                                  confirm_t confirmP,
                                  sdu_size_t sdu_sizeP,
                                  uint8_t *sdu_pP)
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

void enqueue_srap_pc5_data_req(const protocol_ctxt_t *const ctxt_pP,
                               const srb_flag_t   srb_flagP,
                               const MBMS_flag_t  MBMS_flagP,
                               const rb_id_t      rb_idP,
                               const mui_t        muiP,
                               confirm_t    confirmP,
                               sdu_size_t   sdu_sizeP,
                               uint8_t *sdu_pP)
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

  if (pthread_cond_signal(&tx_srap_pc5_q.c) != 0) abort();
  if (pthread_mutex_unlock(&tx_srap_pc5_q.m) != 0) abort();
}

void enqueue_srap_uu_data_req(const protocol_ctxt_t *const ctxt_pP,
                              const srb_flag_t   srb_flagP,
                              const MBMS_flag_t  MBMS_flagP,
                              const rb_id_t      rb_idP,
                              const mui_t        muiP,
                              confirm_t    confirmP,
                              sdu_size_t   sdu_sizeP,
                              uint8_t *sdu_pP)
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


void srap_deliver_sdu_srb(void *_ue, nr_srap_entity_t *entity,
                          char *buf, int size)
{
  // TODO:
}

void srap_deliver_pdu_drb(protocol_ctxt_t *ctxt, int rb_id,
                          char *buf, int size, int sdu_id,
                          nr_intf_type_t intf_type) {
  uint8_t *memblock = malloc16(size); // RLC data_req takes ownership + frees
  memcpy(memblock, buf, size);
  if (intf_type == PC5)
    enqueue_srap_pc5_data_req(ctxt, 0, MBMS_FLAG_NO, rb_id, sdu_id, 0, size, memblock);
  else if (intf_type == UU)
    enqueue_srap_uu_data_req(ctxt, 0, MBMS_FLAG_NO, rb_id, sdu_id, 0, size, memblock);
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

/* Create the PC5 SRAP entity (index 0) for a relay or remote UE. Called when the SL DRB is added.
 * Keyed by the SL local src_id. Mirrors reference add_srap_entity. */
void add_srap_entity(int src_id)
{
  static bool srap_pc5_created;
  bool srap_enabled = get_softmodem_params()->relay_type > 0;
  if (srap_enabled && !srap_pc5_created) {
    nr_srap_manager_internal_t *m = nr_srap_manager;
    if (m && m->srap_entity[0]) {
      m->srap_entity[0] = new_nr_srap_entity(NR_SRAP_PC5, srap_deliver_sdu_drb, NULL, srap_deliver_pdu_drb, NULL, src_id);
      srap_pc5_created = true;
    }
  }
}

/* Create the Uu SRAP entity when an SRB is added. gNB keeps it at index 0; the relay UE (which also
 * has a PC5 entity at index 0) keeps it at index 1. Mirrors reference add_srb SRAP block. */
void add_srap_uu_entity(int ue_id, bool is_gnb)
{
  static bool srap_uu_created;
  bool srap_enabled = get_softmodem_params()->relay_type > 0;
  bool is_relay_ue = get_softmodem_params()->is_relay_ue;
  if (srap_enabled && !srap_uu_created) {
    nr_srap_manager_internal_t *m = nr_srap_manager;
    if (m && is_gnb && m->srap_entity[0]) { // gNB: Uu entity on index 0
      m->srap_entity[0] = new_nr_srap_entity(NR_SRAP_UU, srap_deliver_sdu_drb, NULL, srap_deliver_pdu_drb, NULL, ue_id);
      srap_uu_created = true;
    } else if (m && is_relay_ue && m->srap_entity[1]) { // relay UE: Uu entity on index 1 (PC5 at 0)
      m->srap_entity[1] = new_nr_srap_entity(NR_SRAP_UU, srap_deliver_sdu_drb, NULL, srap_deliver_pdu_drb, NULL, ue_id);
      srap_uu_created = true;
    }
  }
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
                             uint8_t *sdu_buffer,
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
    srap_entity->recv_pdu(ctxt_pP, srap_entity, (char *)sdu_buffer, sdu_buffer_size, srb_flagP, MBMS_flagP, rb_id);
  } else {
    LOG_E(NR_SRAP, "%s:%d:%s: no SRAP entity found (rb_id %ld, srb_flag %d)\n", __FILE__, __LINE__, __FUNCTION__, rb_id, srb_flagP);
  }

  free(sdu_buffer); // RX SRAP PDU buffer consumed by recv_pdu above
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
                                      uint8_t *const sdu_buffer)
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
                                     uint8_t *const sdu_buffer)
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
                   uint8_t *const sdu_buffer,
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

/* SL mode-1 U2N relay control-plane hooks into RRC. Weak defaults so this shared-L2 file links in BOTH the
 * gNB binary (which provides the strong nr_rrc_gNB_process_srap_message) and the UE binary (strong
 * nr_rrc_ue_srap_dl_deliver). bearer_id: 0 = SL-SRB0/CCCH (RRCSetupRequest/RRCSetup), 1 = SL-SRB1/DCCH. */
__attribute__((weak)) void nr_rrc_gNB_process_srap_message(int module_id, uint32_t relay_rnti, int bearer_id, uint8_t remote_ue_id, uint8_t *buf, int size)
{ (void)module_id; (void)relay_rnti; (void)bearer_id; (void)remote_ue_id; (void)buf; (void)size; }
__attribute__((weak)) void nr_rrc_ue_srap_dl_deliver(int ue_id, int bearer_id, uint8_t *buf, int size)
{ (void)ue_id; (void)bearer_id; (void)buf; (void)size; }
/* Resolve a relayed Remote UE's gNB PDCP/RRC ue-id (its own DU-less context) from the relay identifier +
 * SRAP remote_ue_id, so relayed USER-PLANE is delivered to the REMOTE's context (its own PDU session/N3
 * tunnel) rather than the relay's. Strong impl in rrc_gNB.c (find_remote_ue_context). -1 if not found. */
__attribute__((weak)) int nr_rrc_gNB_get_remote_ue_id(uint32_t relay_rnti, uint8_t remote_ue_id)
{ (void)relay_rnti; (void)remote_ue_id; return -1; }

void srap_deliver_sdu_drb(const protocol_ctxt_t *const  ctxt_pP,
                          void *_ue, nr_srap_entity_t *entity,
                          char *buf, int size,
                          const srb_flag_t srb_flagP,
                          const MBMS_flag_t MBMS_flagP,
                          const rb_id_t rb_id) {

  // SL mode-1 U2N relay control plane: an SRB SDU relayed over SRAP goes to RRC, NOT to PDCP/DRB. At the gNB
  // (UL) process the relayed Remote-UE signalling (create context / decode); at the remote UE (DL) hand the
  // gNB's RRC (RRCSetup/SecurityMode/Reconfig/NAS) to the UE RRC. Routed by bearer id (0=CCCH, 1=DCCH).
  if (srb_flagP) {
    if (ctxt_pP->enb_flag)
      nr_rrc_gNB_process_srap_message(0, (uint32_t)ctxt_pP->rntiMaybeUEid, (int)rb_id, ctxt_pP->remote_ue_id, (uint8_t *)buf, size);
    else
      nr_rrc_ue_srap_dl_deliver((int)ctxt_pP->rntiMaybeUEid, (int)rb_id, (uint8_t *)buf, size);
    return;
  }

  // develop nr_pdcp_data_ind takes a raw malloc16 buffer (takes ownership), not a mem_block_t.
  uint8_t *memblock = malloc16(size);
  if (memblock == NULL) {
    LOG_E(NR_SRAP, "%s:%d:%s: ERROR: malloc16 failed\n", __FILE__, __LINE__, __FUNCTION__);
    exit(EXIT_FAILURE);
  }

  memcpy(memblock, buf, size);

  /* Destination DRB for the relayed traffic. `rb_id` here is the SRAP-header bearer id.
   * At the gNB (UL destination): deliver to the RELAY-SPECIFIC DRB (rb_id + 1), a separate PDCP entity
   *   with its own SN context so the remote UE's independent-SN PDCP PDUs do NOT collide with the relay
   *   UE's own bearer (which would make the shared PDCP RX discard them as out-of-window). The gNB
   *   provisions this DRB (id+1) for relay_type>0 (see nr_pdcp_add_drb / RRC DRB setup).
   * At the remote UE (DL destination): deliver to its own SL DRB (rb_id), which develop provisions at
   *   DRB 1 (no relay-specific offset on the UE side). */
  rb_id_t dst_rb_id = ctxt_pP->enb_flag ? rb_id + 1 : rb_id;
  protocol_ctxt_t ctxt = *ctxt_pP;
  /* gNB UL: the relayed data arrived on the RELAY's Uu RLC, so ctxt->rntiMaybeUEid is the relay's ue-id and
   * the packet would egress the RELAY's PDU session/N3 tunnel. The Remote UE has its OWN core context + IP
   * (real registration), so re-key the delivery to the Remote UE's gNB context so its user plane egresses the
   * REMOTE's tunnel (the UPF matches the inner source IP to the remote's session). */
  if (ctxt_pP->enb_flag) {
    int remote_ue = nr_rrc_gNB_get_remote_ue_id((uint32_t)ctxt_pP->rntiMaybeUEid, ctxt_pP->remote_ue_id);
    if (remote_ue >= 0) {
      LOG_D(NR_SRAP, "gNB relayed UL: re-keying DRB%ld delivery from relay ue %ld to remote ue %d\n",
            dst_rb_id, ctxt_pP->rntiMaybeUEid, remote_ue);
      ctxt.rntiMaybeUEid = remote_ue;
    }
  }
  if (!nr_pdcp_data_ind(&ctxt, srb_flagP, dst_rb_id, size, memblock)) {
    LOG_E(NR_SRAP, "%s:%d:%s: ERROR: nr_pdcp_data_ind failed (rb_id %ld)\n", __FILE__, __LINE__, __FUNCTION__, dst_rb_id);
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
      srap_entity->process_sdu(sdu_buffer, sdu_buffer_size, relay_type, rb_id, pdu_buf,
                              (relay_type == U2N) ? sizeof(u2n_header) : sizeof(u2u_header),
                              (relay_type == U2N) ? (void*)&u2n_header : (void*)&u2u_header);
      srap_deliver_pdu deliver_pdu_cb = srap_entity->deliver_pdu;
      deliver_pdu_cb(ctxt, rb_id, pdu_buf, srap_pdu_size, sdu_id, intf_type);
      return true;
   }
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
      /* rb_id is the Remote UE's SRAP header bearer (0=CCCH/SL-SRB0, 1=DCCH/SL-SRB1). The RLC transport
       * differs from the header bearer for control plane: over Uu (gNB->relay) send on the relay's SRB1;
       * over PC5 (remote->relay) send on the matching remote SL-SRB. Mirror of recv_pdu's fwd_rb_id rule. */
      rb_id_t transport_rb_id = (intf_type == UU) ? 1 : rb_id;
      /* On PC5 the RLC transport is keyed by the LOCAL SL src id (nr_rlc_ue_t is looked up by src_id and its
       * sl_srb[]/sl_drb[] were created with the conf's sl_UEINFO srcid), which is a DIFFERENT namespace from
       * the SRAP header's Remote UE id (--remote-ue-id, filled in by process_sdu). Callers only know the
       * latter, so pin the transport key to the src id this SRAP entity was created with - exactly what the
       * relay's forward path does (nr_srap_entity_recv_pdu). Without this, a remote UE whose conf srcid !=
       * --remote-ue-id silently loses every SL-SRB SDU ("SDU sent to unknown sl_srb") and never registers. */
      protocol_ctxt_t tx_ctxt = *ctxt;
      if (intf_type == PC5)
        tx_ctxt.rntiMaybeUEid = srap_entity->rnti;
      srap_entity->process_sdu(sdu_buffer, sdu_buffer_size, relay_type, rb_id, pdu_buf,
                              (relay_type == U2N) ? sizeof(u2n_header) : sizeof(u2u_header),
                              (relay_type == U2N) ? (void*)&u2n_header : (void*)&u2u_header);
      deliver_pdu_cb(&tx_ctxt, transport_rb_id, pdu_buf, srap_pdu_size, sdu_id, intf_type);
      return true;
   }
  return false;
}

void srap_deliver_pdu_srb(protocol_ctxt_t *ctxt, int srb_id, char *buf,
                          int size, int sdu_id, nr_intf_type_t intf_type)
{
  uint8_t *memblock = malloc16(size); // RLC data_req takes ownership + frees
  memcpy(memblock, buf, size);

  if (intf_type == PC5)
    enqueue_srap_pc5_data_req(ctxt, 1, MBMS_FLAG_NO, srb_id, sdu_id, 0, size, memblock);
  else if (intf_type == UU)
    enqueue_srap_uu_data_req(ctxt, 1, MBMS_FLAG_NO, srb_id, sdu_id, 0, size, memblock);
}