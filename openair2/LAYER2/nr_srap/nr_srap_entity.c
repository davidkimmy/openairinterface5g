/*
Author: Ejaz Ahmed
Email ID: ejaz.ahmed@applied.co
*/

#include "nr_srap_header.h"
#include "nr_srap_entity.h"
#include "nr_srap_oai_api.h"
#include "executables/softmodem-common.h"
#include "nr_srap_manager.h"

extern nr_srap_manager_t *nr_srap_manager;

void srap_forward_sdu_drb(protocol_ctxt_t *const ctxt_pP,
                          nr_srap_entity_t *entity,
                          const srb_flag_t srb_flagP,
                          const MBMS_flag_t MBMS_flagP,
                          unsigned char *buffer,
                          int size,
                          const rb_id_t rb_id,
                          uint8_t src_id,
                          uint8_t dst_id)
{
  if (entity != NULL) {
    uint8_t *memblock = malloc16(size); // RLC data_req takes ownership + frees
    memcpy(memblock, buffer, size);
    if (entity->type == NR_SRAP_PC5) {
      enqueue_fwd_srap_pc5_data_req(ctxt_pP, srb_flagP, rb_id, 0, 0, size, memblock);
    } else if (entity->type == NR_SRAP_UU) {
      enqueue_fwd_srap_uu_data_req(ctxt_pP, srb_flagP, rb_id, 0, 0, size, memblock);
    }
  }
}

// Rx side function to receive SRAP pdu containing SRAP headers and PDCP PDU
void nr_srap_entity_recv_pdu(protocol_ctxt_t *const  ctxt_pP,
                             nr_srap_entity_t *entity,
                             char *_buffer, int size,
                             const srb_flag_t srb_flagP,
                             const MBMS_flag_t MBMS_flagP,
                             const rb_id_t rb_id) {
  unsigned char    *buffer = (unsigned char *)_buffer;
  nr_srap_sdu_t    *sdu;

  if (size < 1) {
    LOG_E(NR_SRAP, "bad PDU received (size = %d)\n", size);
    return;
  }
  AssertFatal(entity != NULL, "Entity is NULL!!!");
  entity->stats.rxpdu_pkts++;
  entity->stats.rxpdu_bytes += size;

  uint8_t relay_type = get_softmodem_params()->relay_type;
  uint8_t header_size;
  uint8_t src_id;
  uint8_t ue_id;
  /* DRB id carried in the SRAP header (bearer-id field). At an endpoint (gNB uplink / remote-UE
   * downlink) the packet must be delivered to PDCP on THIS DRB — derived from SRAP, not from the RLC
   * transport bearer it happened to arrive on (which may differ from the relayed bearer). */
  uint8_t hdr_bearer_id = 0;
  U2NHeader_t u2n_header;
  U2UHeader_t u2u_header;
  if (relay_type == U2N) {
    header_size = sizeof(u2n_header);
    decode_srap_header(&u2n_header, buffer);
    hdr_bearer_id = u2n_header.octet1 & 0x1F;
    LOG_D(NR_SRAP, "Rx - bearer id: %x, ue id: %x\n", u2n_header.octet1 & 0x1F, u2n_header.octet2);
  } else if (relay_type == U2U) {
    header_size = sizeof(u2u_header);
    decode_srap_header(&u2u_header, buffer);
    hdr_bearer_id = u2u_header.octet1 & 0x1F;
    LOG_D(NR_SRAP, "Rx - bearer id: %x, source ue id: %x, destination ue id: %x\n",
          u2u_header.octet1 & 0x1F, u2u_header.octet2, u2u_header.octet3);
  } else
    return;

  nr_srap_entity_t *forwarding_entity = NULL;
  sdu = nr_srap_new_sdu((char *) buffer  + header_size, size - header_size);

  nr_srap_manager_t  *m = get_nr_srap_manager();
  char *entity_types[] = {"NR_SRAP_UU", "NR_SRAP_PC5"};
  src_id = relay_type == U2N ? -1 : u2u_header.octet2;
  ue_id = relay_type == U2N ? u2n_header.octet2 : u2u_header.octet3;
  bool is_relay_ue = get_softmodem_params()->is_relay_ue;
  if (is_relay_ue) {
    if (entity->type == NR_SRAP_PC5) {
      forwarding_entity = nr_srap_get_entity(m, NR_SRAP_UU);
    } else if (entity->type == NR_SRAP_UU) {
      forwarding_entity = nr_srap_get_entity(m, NR_SRAP_PC5);
    }
    AssertFatal(forwarding_entity != NULL, "Forwarding entity is NULL!!!");
    /* Transport bearer for the forwarded PDU. For a control-plane SRB the transport differs from the SRAP
     * header bearer: forward on the relay's Uu SRB1 (UL, PC5->Uu), or on the remote's SL-SRB carried in the
     * SRAP header (DL, Uu->PC5). (DRB user data keeps the develop rb_id passthrough.) The SRAP header stays
     * in the buffer, so the endpoint still recovers the remote's real bearer. */
    rb_id_t fwd_rb_id = rb_id;
    if (srb_flagP)
      fwd_rb_id = (forwarding_entity->type == NR_SRAP_UU) ? 1 : hdr_bearer_id;
    LOG_D(NR_SRAP, "%s: Received SRAP SDU from %s; forwarding to %s (transport rb_id %ld, hdr bearer %d).\n",
          __FUNCTION__, entity_types[entity->type], entity_types[forwarding_entity->type], fwd_rb_id, hdr_bearer_id);
    ctxt_pP->rntiMaybeUEid = forwarding_entity->rnti;
    srap_forward_sdu_drb(ctxt_pP, forwarding_entity, srb_flagP, MBMS_flagP, buffer, size, fwd_rb_id, src_id, ue_id);
  }
  if (!is_relay_ue || (!is_relay_ue && (entity->type == NR_SRAP_UU))) {
    LOG_D(NR_SRAP, "Sending sdu: rb_id %lu ue_id = %d\n", rb_id, ue_id);
    if (entity->type == NR_SRAP_PC5) {
      LOG_D(NR_SRAP, "Sending PC5 SRAP indication to above layer from SRAP %s\n", __FUNCTION__);
    } else {
      LOG_D(NR_SRAP, "Sending Uu SRAP indication to above layer from SRAP %s\n", __FUNCTION__);
    }

    /* deliver on the DRB id read from the SRAP header (relay-UE packet: routed to the relay context's
     * bearer identified by SRAP), not the RLC transport rb_id. Also carry the Remote UE id (SRAP header
     * octet2) so the endpoint (gNB / remote UE) can route to the right Remote-UE context. */
    ctxt_pP->remote_ue_id = ue_id;
    entity->deliver_sdu(ctxt_pP, entity->deliver_sdu_data, entity, sdu->buffer, sdu->size, srb_flagP, MBMS_flagP, hdr_bearer_id);
    entity->stats.txsdu_pkts++;
    entity->stats.txsdu_bytes += sdu->size;
  }

  nr_srap_free_sdu(sdu);
}

void nr_srap_entity_delete(nr_srap_entity_t *entity)
{
  nr_srap_sdu_t *cur = entity->rx_list;
  while (cur != NULL) {
    nr_srap_sdu_t *next = cur->next;
    nr_srap_free_sdu(cur);
    cur = next;
  }
  free(entity);
}

static void nr_srap_entity_get_stats(nr_srap_entity_t *entity,
                                     nr_srap_statistics_t *out)
{
  *out = entity->stats;
}

static void nr_srap_entity_process_sdu(char *buffer,
                                       int size,
                                       uint8_t relay_type,
                                       int rb_id,
                                       char *pdu_buffer,
                                       uint8_t header_size,
                                       void *header) {
  char *pdu_buf = pdu_buffer;
  if (relay_type == U2N) {
    uint8_t dc_bit = 1; // control : 0, data : 1
    uint8_t remote_ue_id = get_softmodem_params()->remote_ue_id;
    create_header(dc_bit, relay_type, rb_id, -1, remote_ue_id, header); // In U2N relay case, we do not have a ue_src_id field in the header, the spec. 38351, 6.3.2 defines only U2N remote ue id.
  } else if (relay_type == U2U) {
    // TODO: Call the following functions to enable U2U relay support
    // create_header(relay_type, rb_id, src_ue_id, dest_ue_id, header);
  }
  encode_srap_header(header, (uint8_t*)pdu_buf);
  memcpy(pdu_buf + header_size, buffer, size);
  if (relay_type == U2N) {
    LOG_D(NR_SRAP, "Tx UE %s(): (drb %d) Adding SRAP headers to PDCP sdu: size %d bearer id %x, ue id: %x\n",
          __func__, rb_id, size, (((U2NHeader_t*)header)->octet1 & 0x1F), ((U2NHeader_t*)header)->octet2);
  } else if (relay_type == U2U) {
    LOG_D(NR_SRAP, "Tx UE %s(): (drb %d) Adding SRAP headers to PDCP sdu: size %d bearer id %x, src ue id: %x dest ue id: %x\n",
          __func__, rb_id, size, (((U2UHeader_t*)header)->octet1 & 0x1F), ((U2UHeader_t*)header)->octet2, ((U2UHeader_t*)header)->octet3);
  }
}

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
                                     int rnti)
{
  nr_srap_entity_t *ret;
  ret = calloc(1, sizeof(nr_srap_entity_t));
  if (ret == NULL) {
    LOG_E(NR_SRAP, "%s:%d:%s: out of memory\n", __FILE__, __LINE__, __FUNCTION__);
    exit(EXIT_FAILURE);
  }

  ret->type = type;

  ret->recv_pdu     = nr_srap_entity_recv_pdu;
  ret->process_sdu  = nr_srap_entity_process_sdu;
  ret->delete_entity = nr_srap_entity_delete;

  ret->get_stats = nr_srap_entity_get_stats;
  ret->deliver_sdu = deliver_sdu;
  ret->deliver_sdu_data = deliver_sdu_data;

  ret->deliver_pdu = deliver_pdu;
  ret->deliver_pdu_data = deliver_pdu_data;
  ret->rnti = rnti;
  return ret;
}
