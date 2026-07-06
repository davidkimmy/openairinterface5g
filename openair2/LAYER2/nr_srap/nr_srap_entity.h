/*
Author: Ejaz Ahmed
Email ID: ejaz.ahmed@applied.co
*/
#ifndef _NR_SRAP_ENTITY_H_
#define _NR_SRAP_ENTITY_H_

#include <intertask_interface.h>
#include "nr_srap_sdu.h"

typedef struct {
  int ue_id;           // Local Identity of Remote UE
  int bearer_id;       // Bearer ID for data flow
  int rlc_channel;     // Egress Relay RLC channel
} bearer_to_rlc_mapping_t;

typedef struct {
  uint16_t ue_id; // remote ue id
  bearer_to_rlc_mapping_t *uu_mapping;
  bearer_to_rlc_mapping_t *pc5_mapping;
} srap_mapping_t;

typedef struct {
  srap_mapping_t *array;
  int size;
} srap_mapping_list_t;

typedef struct {
  /* PDU stats */
  /* TX */
  uint32_t txpdu_pkts;     /* aggregated number of tx packets */
  uint32_t txpdu_bytes;    /* aggregated bytes of tx packets */
  /* RX */
  uint32_t rxpdu_pkts;     /* aggregated number of rx packets */
  uint32_t rxpdu_bytes;    /* aggregated bytes of rx packets */

  /* SDU stats */
  /* TX */
  uint32_t txsdu_pkts;     /* number of SDUs delivered */
  uint32_t txsdu_bytes;    /* number of bytes of SDUs delivered */

  /* RX */
  uint32_t rxsdu_pkts;     /* number of SDUs received */
  uint32_t rxsdu_bytes;    /* number of bytes of SDUs received */

} nr_srap_statistics_t;

typedef enum {
  NR_SRAP_UU,
  NR_SRAP_PC5
} nr_srap_entity_type_t;

typedef struct nr_srap_entity_s {
  nr_srap_entity_type_t type;
  srap_mapping_list_t bearer_to_rlc_map;
  uint8_t num_ue;
  int rnti;
  /* functions provided by the SRAP module */
  void (*recv_pdu)(protocol_ctxt_t *const  ctxt_pP,
                   struct nr_srap_entity_s *entity,
                   char *buffer, int size,
                   const srb_flag_t srb_flagP,
                   const MBMS_flag_t MBMS_flagP, const rb_id_t rb_id); // Rx recv pdu

  void (*process_sdu)(char *buffer,
                      int size,
                      uint8_t relay_type,
                      int rb_id,
                      char *pdu_buffer,
                      uint8_t header_size,
                      void *header); // Adds headers inside this function to received SDU from above layer and create PDU to send to the lower layers.

  void (*delete_entity)(struct nr_srap_entity_s *entity);

  void (*get_stats)(struct nr_srap_entity_s *entity, nr_srap_statistics_t *out);

  /* callbacks provided to the PDCP module */
  void (*deliver_sdu)(const protocol_ctxt_t *const  ctxt_pP, void *deliver_sdu_data,
                      struct nr_srap_entity_s *entity, char *buf, int size,
                      const srb_flag_t srb_flagP, const MBMS_flag_t MBMS_flagP,
                      const rb_id_t rb_id);

  void *deliver_sdu_data;

  void (*deliver_pdu)(protocol_ctxt_t *ctxt, int rb_id,
                      char *buf, int size, int sdu_id, nr_intf_type_t intf_type);

  void *deliver_pdu_data;

  /* configuration variables */
  int rb_id;

  /* state variables */
  uint32_t tx_next;
  uint32_t rx_next;
  uint32_t rx_deliv;
  uint32_t rx_reord;

  int is_gnb;

  /* rx management */
  nr_srap_sdu_t *rx_list;
  int           rx_size;
  int           rx_maxsize;
  nr_srap_statistics_t stats;
} nr_srap_entity_t;

nr_srap_entity_t *new_nr_srap_entity(nr_srap_entity_type_t type,
                                     void (*deliver_sdu)(const protocol_ctxt_t *const  ctxt_pP, void *deliver_sdu_data,
                                                         nr_srap_entity_t *entity, char *buf, int size,
                                                         const srb_flag_t srb_flagP, const MBMS_flag_t MBMS_flagP,
                                                         const rb_id_t rb_id),
                                     void *deliver_sdu_data,
                                     void (*deliver_pdu)(protocol_ctxt_t *ctxt, int rb_id,
                                                         char *buf, int size, int sdu_id,
                                                         nr_intf_type_t intf_type),
                                     void *deliver_pdu_data,
                                     int rnti);

void srap_forward_sdu_drb(protocol_ctxt_t *const ctxt_pP,
                          nr_srap_entity_t *entity,
                          const srb_flag_t srb_flagP,
                          const MBMS_flag_t MBMS_flagP,
                          unsigned char *buffer,
                          int size,
                          const rb_id_t rb_id,
                          uint8_t src_id,
                          uint8_t dst_id);

void nr_srap_entity_recv_pdu(protocol_ctxt_t *const  ctxt_pP,
                             nr_srap_entity_t *entity,
                             char *_buffer, int size,
                             const srb_flag_t srb_flagP,
                             const MBMS_flag_t MBMS_flagP,
                             const rb_id_t rb_id);
#endif /* _NR_SRAP_ENTITY_H_ */
