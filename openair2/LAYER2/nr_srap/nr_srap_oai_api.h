/*
Author: Ejaz Ahmed
Email ID: ejaz.ahmed@applied.co
*/

#ifndef _NR_SRAP_OAI_API_H_
#define _NR_SRAP_OAI_API_H_

#include "nr_srap_entity.h"

#pragma once

#define SRAP_DATA_REQ_QUEUE_SIZE 10000
#define SRAP_DATA_IND_QUEUE_SIZE 10000

typedef enum {
    NO_RELAY = 0,
    U2N = 1,
    U2U = 2
} relay_type_t;

typedef struct {
  protocol_ctxt_t ctxt_pP;
  srb_flag_t      srb_flagP;
  MBMS_flag_t     MBMS_flagP;
  rb_id_t         rb_idP;
  mui_t           muiP;
  confirm_t       confirmP;
  sdu_size_t      sdu_sizeP;
  mem_block_t     *sdu_pP;
} srap_data_req_queue_item;

typedef struct {
  srap_data_req_queue_item q[SRAP_DATA_REQ_QUEUE_SIZE];
  volatile int start;
  volatile int length;
  pthread_mutex_t m;
  pthread_cond_t c;
} srap_data_req_queue;

typedef struct {
  protocol_ctxt_t ctxt_pP;
  srb_flag_t      srb_flagP;
  MBMS_flag_t     MBMS_flagP;
  rb_id_t         rb_id;
  sdu_size_t      sdu_buffer_size;
  mem_block_t     *sdu_buffer;
} srap_data_ind_queue_item;

typedef struct {
  srap_data_ind_queue_item q[SRAP_DATA_IND_QUEUE_SIZE];
  volatile int start;
  volatile int length;
  pthread_mutex_t m;
  pthread_cond_t c;
} srap_data_ind_queue;

void nr_srap_layer_init(bool gNB_flag);

int srap_module_init(bool gNB_flag);

typedef void (*srap_deliver_pdu)(protocol_ctxt_t *ctxt, int rb_id,
                                 char *buf, int size, int sdu_id, nr_intf_type_t intf_type);

void srap_deliver_sdu_drb(const protocol_ctxt_t *const  ctxt_pP,
                          void *_ue, nr_srap_entity_t *entity,
                          char *buf, int size,
                          const srb_flag_t srb_flagP,
                          const MBMS_flag_t MBMS_flagP,
                          const rb_id_t rb_id);

void srap_deliver_sdu_srb(void *_ue, nr_srap_entity_t *entity,
                          char *buf, int size);

void srap_deliver_pdu_drb(protocol_ctxt_t *ctxt, int rb_id,
                          char *buf, int size, int sdu_id,
                          nr_intf_type_t intf_type);

void srap_deliver_pdu_srb(protocol_ctxt_t *ctxt, int srb_id, char *buf,
                          int size, int sdu_id, nr_intf_type_t intf_type);

void enqueue_srap_pc5_data_req(const protocol_ctxt_t *const ctxt_pP,
                               const srb_flag_t   srb_flagP,
                               const MBMS_flag_t  MBMS_flagP,
                               const rb_id_t      rb_idP,
                               const mui_t        muiP,
                               confirm_t    confirmP,
                               sdu_size_t   sdu_sizeP,
                               mem_block_t *sdu_pP);

void enqueue_srap_uu_data_req(const protocol_ctxt_t *const ctxt_pP,
                              const srb_flag_t   srb_flagP,
                              const MBMS_flag_t  MBMS_flagP,
                              const rb_id_t      rb_idP,
                              const mui_t        muiP,
                              confirm_t    confirmP,
                              sdu_size_t   sdu_sizeP,
                              mem_block_t *sdu_pP);

bool nr_srap_data_req_drb(protocol_ctxt_t *ctxt,
                          const rb_id_t rb_id,
                          const mui_t sdu_id,
                          const sdu_size_t sdu_buffer_size,
                          char *sdu_buffer,
                          nr_intf_type_t intf_type);

bool nr_srap_data_req_srb(protocol_ctxt_t *ctxt,
                          const rb_id_t rb_id,
                          const sdu_size_t sdu_buffer_size,
                          char *sdu_buffer,
                          srap_deliver_pdu deliver_pdu_cb,
                          int sdu_id,
                          nr_intf_type_t intf_type);

bool srap_data_ind(protocol_ctxt_t *const  ctxt_pP,
                   const srb_flag_t srb_flagP,
                   const MBMS_flag_t MBMS_flagP,
                   const rb_id_t rb_id,
                   const sdu_size_t sdu_buffer_size,
                   mem_block_t *const sdu_buffer,
                   const uint32_t *const srcID,
                   const uint32_t *const dstID,
                   nr_intf_type_t intf_type);

void enqueue_fwd_srap_pc5_data_req(protocol_ctxt_t *const ctxt_pP,
                                   const srb_flag_t srb_flagP,
                                   const rb_id_t rb_idP,
                                   const mui_t muiP,
                                   confirm_t confirmP,
                                   sdu_size_t sdu_sizeP,
                                   mem_block_t *sdu_pP);

void enqueue_fwd_srap_uu_data_req(protocol_ctxt_t *const ctxt_pP,
                                  const srb_flag_t srb_flagP,
                                  const rb_id_t rb_idP,
                                  const mui_t muiP,
                                  confirm_t confirmP,
                                  sdu_size_t sdu_sizeP,
                                  mem_block_t *sdu_pP);
#endif /* _NR_SRAP_OAI_API_H_ */
