/*
Author: Ejaz Ahmed
Email ID: ejaz.ahmed@applied.co
*/

#ifndef _NR_SRAP_HEADER_H_
#define _NR_SRAP_HEADER_H_

#include <stdint.h>
#include "openair2/COMMON/platform_types.h"

#include "nr_srap_oai_api.h"


// 38.351 6.2.2 SRAP data pdu: 2 bytes hdr for U2N, 3 bytes hdr for U2U
//
// U2N octet1 layout: [ D/C (bit 7) | Reserved (bits 6-5) | Bearer ID (bits 4-0) ]
// U2N octet2       : Remote UE ID (1-255; 0 is not a valid Remote UE ID)
#define SRAP_HDR_DC_MASK        0x80  // D/C bit: 1 = data (SRAP payload), 0 = control
#define SRAP_HDR_RESERVED_MASK  0x60  // Reserved bits 6-5, must be 0 in a valid U2N header
#define SRAP_HDR_BEARER_ID_MASK 0x1F  // Bearer ID field (lower 5 bits)
#define SRAP_REMOTE_UE_ID_MIN   0x01  // Minimum valid Remote UE ID
#define SRAP_REMOTE_UE_ID_MAX   0xFF  // Maximum valid Remote UE ID
#define SRAP_U2N_HDR_LEN        2     // U2N SRAP data PDU header length (octet1 + octet2)

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
