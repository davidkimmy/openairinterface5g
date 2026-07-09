/*
Author: Ejaz Ahmed
Email ID: ejaz.ahmed@applied.co
*/

#include "nr_srap_header.h"
#include "nr_srap_entity.h"
#include "nr_srap_oai_api.h"
#include "executables/softmodem-common.h"
#include "nr_srap_manager.h"
#include "openair2/LAYER2/nr_pdcp/nr_pdcp_oai_api.h"
#include "openair2/LAYER2/nr_rlc/nr_rlc_oai_api.h"

extern nr_srap_manager_t *nr_srap_manager;

// Relay context management functions
relay_ue_context_t *nr_srap_create_relay_context(void) {
  relay_ue_context_t *ctx = calloc(1, sizeof(relay_ue_context_t));
  if (ctx == NULL) {
    LOG_E(NR_SRAP, "Failed to allocate relay context\n");
    return NULL;
  }
  ctx->num_remote_ues = 0;
  ctx->next_drb_index = 1;  // Start from index 1 (DRB1)
  for (int i = 0; i < MAX_REMOTE_UES; i++) {
    ctx->remote_ues[i].is_active = false;
    ctx->remote_ues[i].remote_ue_id = 0;
    ctx->remote_ues[i].sl_drb_index = 0;
  }
  return ctx;
}

void nr_srap_free_relay_context(relay_ue_context_t *ctx) {
  if (ctx != NULL) {
    free(ctx);
  }
}

int nr_srap_get_drb_index_for_remote_ue(relay_ue_context_t *ctx, uint8_t remote_ue_id) {
  if (ctx == NULL) return -1;

  for (int i = 0; i < MAX_REMOTE_UES; i++) {
    if (ctx->remote_ues[i].is_active && ctx->remote_ues[i].remote_ue_id == remote_ue_id) {
      return ctx->remote_ues[i].sl_drb_index;
    }
  }
  return -1;  // Not found
}

void nr_srap_register_remote_ue(relay_ue_context_t *ctx, uint8_t remote_ue_id, uint8_t relay_srcid) {
  if (ctx == NULL) {
    LOG_E(NR_SRAP, "[Relay Context] NULL context in register_remote_ue\n");
    return;
  }

  // Check if already registered
  if (nr_srap_get_drb_index_for_remote_ue(ctx, remote_ue_id) >= 0)
    return;

  // Find free slot
  int free_slot = -1;
  for (int i = 0; i < MAX_REMOTE_UES; i++) {
    if (!ctx->remote_ues[i].is_active) {
      free_slot = i;
      break;
    }
  }

  if (free_slot < 0) {
    LOG_E(NR_SRAP, "[Relay Context] No free slots for remote_ue_id=%d (max %d)\n",
          remote_ue_id, MAX_REMOTE_UES);
    return;
  }

  // Allocate DRB index (need 2 consecutive DRBs per Remote UE: DRB1 and DRB2)
  uint8_t drb_base = ctx->next_drb_index;
  if (drb_base + 1 >= MAX_DRBS_PER_UE) {
    LOG_E(NR_SRAP, "[Relay Context] Out of DRB indices for remote_ue_id=%d\n", remote_ue_id);
    return;
  }

  /* No new RLC entities are created here: the Relay UE already has SL-DRB
     entities from preconfiguration (rrc_sl_preconfig.c) using its own
     sourceL2Id. This mapping only records which DRB to use per Remote UE. */
  ctx->remote_ues[free_slot].remote_ue_id = remote_ue_id;
  ctx->remote_ues[free_slot].sl_drb_index = drb_base;
  ctx->remote_ues[free_slot].is_active = true;
  ctx->num_remote_ues++;
  ctx->next_drb_index += 2;  // Reserve 2 DRBs per Remote UE

  LOG_D(NR_SRAP, "[Relay Context] Registered Remote UE ID %d → sl_drb[%d,%d], srcid=0x%x, total_remote_ues=%d\n",
        remote_ue_id, drb_base, drb_base+1, relay_srcid, ctx->num_remote_ues);
}

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
    mem_block_t *memblock = get_free_mem_block(size, __FUNCTION__);
    memcpy(memblock->data, buffer, size);
    if (entity->type == NR_SRAP_PC5) {
      /* PC5 forwarding (Uu→PC5 downlink): Use Relay UE's sourceL2Id as transmitter
         dst_id contains remote_ue_id from U2N header, use it as destinationL2Id */
      uint32_t relay_src_l2id = entity->srcid;

      enqueue_fwd_srap_pc5_data_req(ctxt_pP, srb_flagP, rb_id, 0, 0, size, memblock,
                                    relay_src_l2id, (uint32_t)dst_id);
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
  nr_srap_sdu_t    *sdu = NULL;

  if (size < 1) {
    LOG_E(NR_SRAP, "bad PDU received (size = %d)\n", size);
    return;
  }
  AssertFatal(entity != NULL, "Entity is NULL!!!");
  entity->stats.rxpdu_pkts++;
  entity->stats.rxpdu_bytes += size;

  uint8_t relay_type = get_softmodem_params()->relay_type;
  uint8_t header_size;
  uint8_t src_id = 127;
  uint8_t dest_id = 127;
  U2NHeader_t u2n_header;
  U2UHeader_t u2u_header;
  nr_srap_entity_t *forwarding_entity = NULL;
  char *entity_types[] = {"NR_SRAP_UU", "NR_SRAP_PC5"};
  nr_srap_manager_t  *m = get_nr_srap_manager();
  bool is_relay_ue = get_softmodem_params()->is_relay_ue;

  // Local copy of srb_flag that can be modified based on SRAP bearer_id
  srb_flag_t srb_flag = srb_flagP;

  /* Determine if SRAP header is present (episys/sl-mode1-relay behavior):
     - ALL messages on both Uu and PC5 interfaces have SRAP header
     - SRB0 messages now include SRAP header (fixes remote_ue_id extraction) */
  bool has_srap_header = true;

  if (has_srap_header) {
    if (relay_type == U2N) {
      header_size = sizeof(u2n_header);
      decode_srap_header(&u2n_header, buffer);
      LOG_D(NR_SRAP, "Rx - bearer id: %x, ue id: %x\n", u2n_header.octet1 & 0x1F, u2n_header.octet2);
    } else if (relay_type == U2U) {
      header_size = sizeof(u2u_header);
      decode_srap_header(&u2u_header, buffer);
      LOG_D(NR_SRAP, "Rx - bearer id: %x, source ue id: %x, destination ue id: %x\n",
            u2u_header.octet1 & 0x1F, u2u_header.octet2, u2u_header.octet3);
    } else
      return;

    sdu = nr_srap_new_sdu((char *) buffer  + header_size, size - header_size);

    src_id = relay_type == U2N ? -1 : u2u_header.octet2;
    dest_id = relay_type == U2N ? u2n_header.octet2 : u2u_header.octet3;
  } else {
    // PC5 uplink SRB0 (Remote UE → Relay UE): no SRAP header
    sdu = nr_srap_new_sdu((char *) buffer, size);
  }

  // Forward PC5→Uu for Relay UE (including SRB0 for Remote UE RRCSetupRequest)
  if (is_relay_ue && entity->type == NR_SRAP_PC5) {
    forwarding_entity = nr_srap_get_entity(m, NR_SRAP_UU);
    AssertFatal(forwarding_entity != NULL, "Forwarding entity is NULL!!!");

    // Initialize relay context if not already done
    if (entity->relay_ctx == NULL) {
      entity->relay_ctx = nr_srap_create_relay_context();
    }

    // Extract actual bearer_id and remote_ue_id from SRAP header
    rb_id_t actual_bearer_id = rb_id;  // Default to MAC's rb_id
    uint8_t remote_ue_id = 0;

    /* SRAP header is now ALWAYS present (episys/sl-mode1-relay behavior)
       Extract remote_ue_id and bearer_id from decoded SRAP header */
    if (relay_type == U2N) {
      remote_ue_id = dest_id;
      actual_bearer_id = u2n_header.octet1 & SRAP_HDR_BEARER_ID_MASK;  // Bearer ID from lower 5 bits
    }

    // Register Remote UE if this is first signaling message from this remote_ue_id
    if (remote_ue_id > 0 && srb_flagP) {
      /* This is a signaling message from a Remote UE
         Use PC5 entity's srcid (Relay UE's own sourceL2Id for PC5 forwarding) */
      uint8_t relay_srcid = entity->srcid;
      nr_srap_register_remote_ue(entity->relay_ctx, remote_ue_id, relay_srcid);
    }

    ctxt_pP->rntiMaybeUEid = forwarding_entity->rnti;

    if (srb_flagP && actual_bearer_id == 0) {
      /* SRB0: Remote UE's RRCSetupRequest - forward with SRAP header on Relay UE's SRB1
         (Relay UE's Uu SRB0 is torn down after RRCSetupComplete, only SRB1 exists) */
      U2NHeader_t u2n_header;
      int srap_pdu_size = size + sizeof(u2n_header);
      char pdu_buf[srap_pdu_size];

      /* Create SRAP header with Remote UE ID
         D/C bit = 1 for Data (carrying RRC PDU as data payload) */
      uint8_t dc_bit = 1;
      uint8_t remote_ue_id = get_softmodem_params()->remote_ue_id;
      if (remote_ue_id == 0) remote_ue_id = 1; // Default to 1 if not set

      create_header(dc_bit, relay_type, rb_id, -1, remote_ue_id, &u2n_header);
      encode_srap_header(&u2n_header, (uint8_t*)pdu_buf);
      memcpy(pdu_buf + sizeof(u2n_header), buffer, size);

      // Forward directly to Uu RLC SRB1, bypassing PDCP (forwarded messages don't need integrity/ciphering)
      mem_block_t *memblock = get_free_mem_block(srap_pdu_size, __FUNCTION__);
      memcpy(memblock->data, pdu_buf, srap_pdu_size);

      protocol_ctxt_t uu_ctxt = *ctxt_pP;
      uu_ctxt.enb_flag = 0;  // Relay UE is a UE, not gNB

      rlc_data_req(&uu_ctxt, 1, MBMS_FLAG_NO, 1, 0, 0, srap_pdu_size, memblock, NULL, NULL, UU);
    } else if (srb_flagP && actual_bearer_id == 1 && remote_ue_id > 0) {
      /* SRB1: Remote UE's RRCSetupComplete (or other DCCH messages)
         Message has Remote UE's N2U SRAP header (2 bytes), we need to:
         1. Strip the old N2U header from Remote UE
         2. Add new U2N header for gNB */

      if (size < 2) {
        LOG_E(NR_SRAP, "[Relay UE] PC5→Uu SRB1: Message too small (%d bytes), need at least 2 for N2U header\n", size);
        return;
      }

      // Strip Remote UE's N2U header (2 bytes)
      const uint8_t *rrc_payload = buffer + 2;
      int rrc_payload_size = size - 2;

      // Create new U2N header for gNB
      U2NHeader_t u2n_header;
      int srap_pdu_size = rrc_payload_size + sizeof(u2n_header);
      char pdu_buf[srap_pdu_size];

      uint8_t dc_bit = 1;
      create_header(dc_bit, relay_type, actual_bearer_id, -1, remote_ue_id, &u2n_header);
      encode_srap_header(&u2n_header, (uint8_t*)pdu_buf);
      memcpy(pdu_buf + sizeof(u2n_header), rrc_payload, rrc_payload_size);

      // Forward directly to Uu RLC SRB1, bypassing PDCP (forwarded messages don't need integrity/ciphering)
      mem_block_t *memblock = get_free_mem_block(srap_pdu_size, __FUNCTION__);
      memcpy(memblock->data, pdu_buf, srap_pdu_size);

      protocol_ctxt_t uu_ctxt = *ctxt_pP;
      uu_ctxt.enb_flag = 0;  // Relay UE is a UE, not gNB

      rlc_data_req(&uu_ctxt, 1, MBMS_FLAG_NO, 1, 0, 0, srap_pdu_size, memblock, NULL, NULL, UU);
    } else if (srb_flagP && rb_id != 0) {
      // SRB1+: Other signaling - forward with SRAP header
      U2NHeader_t u2n_header;
      int srap_pdu_size = size + sizeof(u2n_header);
      char pdu_buf[srap_pdu_size];

      uint8_t dc_bit = 0;
      uint8_t remote_ue_id = get_softmodem_params()->remote_ue_id;
      entity->process_sdu((char*)buffer, size, relay_type, rb_id, pdu_buf,
                          sizeof(u2n_header), (void*)&u2n_header, dc_bit, remote_ue_id);

      srap_forward_sdu_drb(ctxt_pP, forwarding_entity, srb_flagP, MBMS_flagP, (unsigned char*)pdu_buf, srap_pdu_size, rb_id, src_id, dest_id);
    } else {
      /* DRBs: Forward Remote UE traffic
         Need to add N2U header (Network-to-UE) before forwarding to Uu
         Process: PC5 U2N (stripped above) → add Uu N2U → forward to gNB */

      /* Only apply rb_id=2 mapping for actual DRB traffic (srb_flagP==0)
         SRB traffic that falls through here (due to LCID mismatch) should keep original rb_id */
      rb_id_t forward_rb_id = rb_id;
      if (!srb_flagP) {
        // This is actual DRB traffic - use rb_id=2 for Remote UE (allows Relay UE to use DRB 1)
        forward_rb_id = 2;

        // Add N2U SRAP header for Uu forwarding
        U2NHeader_t u2n_header;
        int srap_pdu_size = sdu->size + sizeof(u2n_header);
        char pdu_buf[srap_pdu_size];

        uint8_t dc_bit = 0;  // D/C bit = 0 for Data (DRB traffic)
        uint8_t remote_ue_id = get_softmodem_params()->remote_ue_id;
        entity->process_sdu((char*)sdu->buffer, sdu->size, relay_type, actual_bearer_id, pdu_buf,
                            sizeof(u2n_header), (void*)&u2n_header, dc_bit, remote_ue_id);

        srap_forward_sdu_drb(ctxt_pP, forwarding_entity, srb_flagP, MBMS_flagP, (unsigned char*)pdu_buf, srap_pdu_size, forward_rb_id, src_id, dest_id);
      } else {
        /* This is SRB traffic that didn't match the special cases above
           Keep original rb_id (likely SRB1 misdelivered on LCID 4) */
        LOG_W(NR_SRAP, "[Relay UE] PC5→Uu: SRB traffic (srb_flag=%d, rb_id=%ld) in else block, forwarding with original rb_id\n",
              srb_flagP, (long)rb_id);

        // For SRB, also need to add N2U header
        U2NHeader_t u2n_header;
        int srap_pdu_size = sdu->size + sizeof(u2n_header);
        char pdu_buf[srap_pdu_size];

        uint8_t dc_bit = 1;  // D/C bit = 1 for signaling
        uint8_t remote_ue_id = get_softmodem_params()->remote_ue_id;
        entity->process_sdu((char*)sdu->buffer, sdu->size, relay_type, actual_bearer_id, pdu_buf,
                            sizeof(u2n_header), (void*)&u2n_header, dc_bit, remote_ue_id);

        srap_forward_sdu_drb(ctxt_pP, forwarding_entity, srb_flagP, MBMS_flagP, (unsigned char*)pdu_buf, srap_pdu_size, forward_rb_id, src_id, dest_id);
      }
    }
  } else if (is_relay_ue && entity->type == NR_SRAP_UU && rb_id != 0) {
    // Forward Uu→PC5 for Relay UE (excluding SRB0, which is handled separately)
    forwarding_entity = nr_srap_get_entity(m, NR_SRAP_PC5);
    AssertFatal(forwarding_entity != NULL, "Forwarding entity is NULL!!!");

    // Map Relay UE's Uu DRB2 (rb_id=2) to PC5 DRB1 (rb_id=1) for Remote UE traffic
    rb_id_t pc5_rb_id = (rb_id == 2) ? 1 : rb_id;

    LOG_D(NR_SRAP, "%s: Received SRAP SDU from %s; forwarding to %s with rb_id %ld (Uu rb_id=%ld).\n", __FUNCTION__, entity_types[entity->type], entity_types[forwarding_entity->type], pc5_rb_id, rb_id);
    ctxt_pP->rntiMaybeUEid = forwarding_entity->rnti;
    srap_forward_sdu_drb(ctxt_pP, forwarding_entity, srb_flagP, MBMS_flagP, buffer, size, pc5_rb_id, src_id, dest_id);
  }

  if (sdu && (!is_relay_ue || (!is_relay_ue && (entity->type == NR_SRAP_UU)))) {
    LOG_D(NR_SRAP, "Sending upstream: src_id = %d  dest_id = %d\n", src_id, dest_id);

    // gNB receiving uplink from Relay UE: route to Remote UE's PDCP entity
    protocol_ctxt_t delivery_ctxt = *ctxt_pP;

    // Extract actual bearer_id from SRAP U2N header (not MAC rb_id which is always Relay UE's SRB1)
    rb_id_t actual_rb_id = rb_id;  // Default to MAC rb_id
    if (!is_relay_ue && entity->type == NR_SRAP_UU && relay_type == U2N && header_size > 0) {
      actual_rb_id = u2n_header.octet1 & SRAP_HDR_BEARER_ID_MASK;  // Extract bearer_id from SRAP header
    } else if (!is_relay_ue && entity->type == NR_SRAP_PC5 && relay_type == U2N && header_size > 0) {
      // Remote UE receiving from PC5: extract bearer_id from SRAP header
      actual_rb_id = u2n_header.octet1 & SRAP_HDR_BEARER_ID_MASK;  // Extract bearer_id from SRAP header
    }

    if (!is_relay_ue && entity->type == NR_SRAP_UU && relay_type == U2N && dest_id > 0) {
      /* U2N uplink: dest_id = remote_ue_id, current RNTI = relay_ue_rnti
         Need to find Remote UE's RNTI for PDCP delivery */
      rnti_t relay_ue_rnti = ctxt_pP->rntiMaybeUEid;
      uint8_t remote_ue_id = dest_id;

      // Find Remote UE RNTI using PDCP helper function
      rnti_t remote_ue_rnti = nr_pdcp_get_remote_ue_rnti(relay_ue_rnti, remote_ue_id);

      if (remote_ue_rnti != 0) {
        // Update context to use Remote UE's RNTI for PDCP entity lookup
        delivery_ctxt.rntiMaybeUEid = remote_ue_rnti;
      } else {
        LOG_E(NR_SRAP, "[gNB] SRAP Uu RX: No Remote UE PDCP entity found for ID %d via Relay 0x%x\n",
              remote_ue_id, relay_ue_rnti);
        // Keep original RNTI - will likely fail in PDCP but better than crash
      }

      /* Fix srb_flag based on actual bearer_id from SRAP header
         For Remote UE: bearer_id=0 is SRB0 (RRCSetupRequest), bearer_id >= 1 are DRBs (data)
         Note: RRCSetupComplete (SRB1) uses bearer_id=1 but goes through RLC→RRC path, not this SRAP→PDCP path */
      if (actual_rb_id == 0) {
        srb_flag = 1;  // SRB0 (CCCH) - RRCSetupRequest
      } else {
        srb_flag = 0;  // DRB - data traffic (ping, etc.)
      }
    }

    if (entity->type == NR_SRAP_PC5) {
      LOG_D(NR_SRAP, "Sending PC5 SRAP indication to above layer from SRAP %s\n", __FUNCTION__);
    } else {
      LOG_D(NR_SRAP, "Sending Uu SRAP indication to above layer from SRAP %s\n", __FUNCTION__);
    }


    // Use actual_rb_id (from SRAP header) instead of MAC rb_id for correct PDCP routing
    entity->deliver_sdu(&delivery_ctxt, entity->deliver_sdu_data, entity, sdu->buffer, sdu->size, srb_flag, MBMS_flagP, actual_rb_id);
    entity->stats.txsdu_pkts++;
    entity->stats.txsdu_bytes += sdu->size;
  }
  // Note: Relay UE PC5 SRB0 is now handled by forwarding above, not local RRC processing

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
  nr_srap_free_relay_context(entity->relay_ctx);
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
                                       void *header,
                                       uint8_t dc_bit,
                                       uint8_t remote_ue_id) {
  char *pdu_buf = pdu_buffer;

  // For SRB0 (rb_id=0), header is NULL - just copy the data without SRAP header
  if (header == NULL) {
    memcpy(pdu_buf, buffer, size);
    return;
  }

  // For other RBs, add SRAP header
  if (relay_type == U2N) {
    uint8_t dc_bit = 1; // D/C bit: 1 for Data (RRC PDU), 0 for Control
    /* Use remote_ue_id parameter instead of global softmodem param
       Fallback to softmodem param if remote_ue_id is 0 (for backward compatibility) */
    if (remote_ue_id == 0) {
      remote_ue_id = get_softmodem_params()->remote_ue_id;
    }

    /* SRAP header uses Relay UE's Uu bearer ID (LCID), not Remote UE's local PC5 rb_id
       SRAP header uses bearer_id to represent Remote UE's logical bearer
       For DRBs: bearer_id = rb_id + 3 (Remote UE DRB1 = rb_id 1 → bearer_id 4)
       For SRBs: bearer_id = rb_id (SRB1 = rb_id 1 → bearer_id 1)

       Note: The Relay UE uses its own Uu DRB2 (LCID 5) as the transport bearer,
       but the SRAP header bearer_id represents the Remote UE's logical DRB ID,
       not the Relay UE's transport channel. */
    rb_id_t bearer_id = rb_id;
    if (rb_id >= 1 && dc_bit == 1) {
      /* For DRBs (dc_bit=1 for Data), convert Remote UE's rb_id to SRAP bearer_id
         Remote UE DRB1 (rb_id=1) → bearer_id=4
         SRBs keep original rb_id (SRB1 rb_id=1 → bearer_id=1) */
      if (rb_id <= 2) {
        bearer_id = rb_id + 3;  // DRB 1 → bearer_id 4, DRB 2 → bearer_id 5
      }
    }

    create_header(dc_bit, relay_type, bearer_id, -1, remote_ue_id, header); // In U2N relay case, we do not have a ue_src_id field in the header, the spec. 38351, 6.3.2 defines only U2N remote ue id.
  } else if (relay_type == U2U) {
    // TODO: Call the following functions to enable U2U relay support
    // create_header(relay_type, rb_id, src_ue_id, dest_ue_id, header);
  }
  encode_srap_header(header, (uint8_t*)pdu_buf);
  memcpy(pdu_buf + header_size, buffer, size);

  if (relay_type == U2U) {
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
  ret->srcid = (type == NR_SRAP_PC5) ? rnti : 0;  // For PC5 entity, rnti param is actually srcid
  ret->relay_ctx = NULL;
  return ret;
}
