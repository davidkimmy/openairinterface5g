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
    rlc_data_req(&tx_srap_pc5_q.q[i].ctxt_pP,
                 tx_srap_pc5_q.q[i].srb_flagP,
                 tx_srap_pc5_q.q[i].MBMS_flagP,
                 tx_srap_pc5_q.q[i].rb_idP,
                 tx_srap_pc5_q.q[i].muiP,
                 tx_srap_pc5_q.q[i].confirmP,
                 tx_srap_pc5_q.q[i].sdu_sizeP,
                 tx_srap_pc5_q.q[i].sdu_pP,
                 NULL,
                 NULL,
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
    LOG_D(NR_SRAP, "Pass data from SRAP fwd_srap_to_pc5_q to RLC%s %d\n", __FUNCTION__, __LINE__);
    rlc_data_req(&fwd_srap_to_pc5_q.q[i].ctxt_pP,
                 fwd_srap_to_pc5_q.q[i].srb_flagP,
                 fwd_srap_to_pc5_q.q[i].MBMS_flagP,
                 fwd_srap_to_pc5_q.q[i].rb_idP,
                 fwd_srap_to_pc5_q.q[i].muiP,
                 fwd_srap_to_pc5_q.q[i].confirmP,
                 fwd_srap_to_pc5_q.q[i].sdu_sizeP,
                 fwd_srap_to_pc5_q.q[i].sdu_pP,
                 NULL,
                 NULL,
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
                                   mem_block_t *sdu_pP)
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

void enqueue_srap_pc5_data_req(const protocol_ctxt_t *const ctxt_pP,
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


void srap_deliver_sdu_srb(void *_ue, nr_srap_entity_t *entity,
                          char *buf, int size)
{
  // TODO:
}

void srap_deliver_pdu_drb(protocol_ctxt_t *ctxt, int rb_id,
                          char *buf, int size, int sdu_id,
                          nr_intf_type_t intf_type) {
  mem_block_t *memblock = get_free_mem_block(size, __FUNCTION__);
  memcpy(memblock->data, buf, size);
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

  // Sending data indication to PDCP at the destination
  if (!pdcp_data_ind(ctxt_pP, srb_flagP, MBMS_flagP, rb_id + 1, size, memblock, NULL, NULL, intf_type)) {
    // restoring rb_id in case of receiving sdu from srap layer.
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
      srap_entity->process_sdu(sdu_buffer, sdu_buffer_size, relay_type, rb_id, pdu_buf,
                              (relay_type == U2N) ? sizeof(u2n_header) : sizeof(u2u_header),
                              (relay_type == U2N) ? (void*)&u2n_header : (void*)&u2u_header);
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