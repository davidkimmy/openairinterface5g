/*
Author: Ejaz Ahmed
Email ID: ejaz.ahmed@applied.co
*/

#ifndef _NR_SRAP_HEADER_H_
#define _NR_SRAP_HEADER_H_

#include <stdint.h>
#include "common/platform_types.h"

#include "nr_srap_oai_api.h"


// SRAP header octet-1 field masks (TS 38.351 6.2.2): bit7 = D/C (1=data/relayed SRAP), bits 5-6 reserved
// (0), bits 0-4 = bearer id. octet2 = remote UE id (1..255). Used to detect relayed SRAP PDUs at the
// relay/gNB and distinguish them from the relay's own (PDCP-ciphered) Uu traffic.
#define SRAP_HDR_DC_MASK        0x80
#define SRAP_HDR_RESERVED_MASK  0x60
#define SRAP_HDR_BEARER_ID_MASK 0x1F
#define SRAP_REMOTE_UE_ID_MIN   1
#define SRAP_REMOTE_UE_ID_MAX   255
// 38.351 6.2.2 SRAP data pdu: 2 bytes hdr for U2N, 3 bytes hdr for U2U
// U2N Header (2 octets)
typedef struct U2NHeader {
    uint8_t octet1;
    uint8_t octet2;
} U2NHeader_t;

// U2U Header (3 octets)
typedef struct U2UHeader {
    uint8_t octet1;
    uint8_t octet2;
    uint8_t octet3;
} U2UHeader_t;

// Function to encode SRAP Header
void encode_srap_header(void* header, uint8_t* buffer);

// Function to decode SRAP Header
void decode_srap_header(void* header, uint8_t* buffer);

// Function to create SRAP headers
void create_header(uint8_t dc_bit, relay_type_t relay_type, uint8_t bearer_id, int8_t src_ue_id, int8_t dest_ue_id, void* header);

#endif /* _NR_SRAP_HEADER_H_ */
