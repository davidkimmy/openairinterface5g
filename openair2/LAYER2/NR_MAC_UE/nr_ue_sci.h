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

/* \file nr_ue_sci.h
 * \brief Definitions and Structures for sci/slsch procedures for Sidelink UE
 * \author R. Knopp
 * \date 2023
 * \version 0.1
 * \company Eurecom
 * \email: knopp@eurecom.fr
 * \note
 * \warning
 */

#ifndef __LAYER2_NR_UE_SCI_H__
#define __LAYER2_NR_UE_SCI_H__
#include "NR_MAC_COMMON/nr_mac.h"

/* F1 SL-SCH MAC subheader: a compact 2-byte big-endian SDU length prepended to the single RLC PDU carried in a
 * PSSCH TB. SRC/DST/LCID are already conveyed in SCI-2, so for the single-DRB F1 path only the SDU length is
 * needed — it lets the RX strip the transport-block padding and deliver the exact RLC PDU (vs the whole padded
 * TB). TX (sl_schedule_tx_actions) writes it; RX (NR_IF_Module SLSCH case) reads+strips it. */
/* SL-SCH MAC multiplexing (proper, TS 38.321 6.1.6): each PSSCH transport block starts with an
 * NR_SLSCH_MAC_SUBHEADER_FIXED (SRC/DST L2 IDs), followed by one or more MAC sub-PDUs, each carrying a
 * standard NR_MAC_SUBHEADER_SHORT/LONG (parsed by get_mac_len). This lets multiple bearers — and, for
 * RLC-AM, a STATUS PDU alongside a data PDU — share ONE TB, so ARQ never starves the RRC/user bytes.
 * LCID 0/1 = SL-SRB0/SRB1 (mode-1 U2N relay control plane, RRCSetupRequest/Setup + DCCH), 4 = SL-DRB1
 * (user data), 63 = padding. Scheduler priority: SRB0 > SRB1 > DRB. */
#define SL_SCH_SUBHEADER_LEN 3 /* legacy ad-hoc len (kept for compatibility); new code uses NR_MAC_SUBHEADER_* */
#define SL_SCH_LCID_SRB0 0
#define SL_SCH_LCID_SRB1 1
#define SL_SCH_LCID_DRB1 4
/* SL CSI report MAC CE (TS 38.321). Fixed subheader (LCID only) + 1-byte nr_sl_csi_report_t.
 * The node that measured CSI-RS packs it; the TX node reads it and adapts PSSCH MCS. */
#define SL_SCH_LCID_SL_CSI_REPORT 62
#define SL_SCH_LCID_PADDING 63

/* Default SL TX MCS when --mcs is not set. Also used to seed the CQI-adaptive sl_max_mcs. */
#define SL_F1_DEFAULT_MCS 9

typedef enum {
  NR_SL_SCI_FORMAT_1A = 0,
  NR_SL_SCI_FORMAT_2A = 1,
  NR_SL_SCI_FORMAT_2B = 2,
  NR_SL_SCI_FORMAT_2C = 3
} nr_sci_format_t;

typedef struct {
	// 1st stage fields
	uint8_t priority; // 3 bits
	dci_field_t frequency_resource_assignment; // depending on sl-MaxNumPerReserve and N_subChannel^SL 
	dci_field_t time_resource_assignment; // depending on sl_MaxNumPerReserve
	dci_field_t resource_reservation_period; // sl-ResourceReservePeriodList and sl-MultiReserveResource
	dci_field_t dmrs_pattern; // depending on N_pattern and sl-PSSCH-DMRS-TimePatternList
	uint8_t second_stage_sci_format; // 2 bits - Table 8.3.1.1-1
        uint8_t beta_offset_indicator; // 2 bits - depending sl-BetaOffsets2ndSCI and Table 8.3.1.1-2
	uint8_t number_of_dmrs_port; // 1 bit - Table 8.3.1.1-3
	uint8_t mcs; // 5 bits
	dci_field_t additional_mcs; // depending on sl-Additional-MCS-Table
	dci_field_t psfch_overhead; // depending on sl-PSFCH-Period
        dci_field_t reserved; // depending on N_reserved (sl-NumReservedBits) and sl-IndicationUE-B
        dci_field_t conflict_information_receiver; // depending on sl-IndicationUE-B
	// 2nd stage fields
	uint8_t harq_pid; // 4 bits
	uint8_t ndi; // 1 bit
	uint8_t rv_index; // 2 bits
	uint8_t source_id; // 8 bits
	uint16_t dest_id; // 16 bits
	uint8_t harq_feedback; //1 bit
	uint8_t cast_type; // 2 bits formac 2A
	uint8_t csi_req; // 1 bit format 2A, format 2C
	uint16_t zone_id; // 12 bits format 2B
	dci_field_t communication_range; // 4 bits depending on sl-ZoneConfigMCR-Index, format 2B
        uint8_t providing_req_ind; // 1 bit, format 2C
	dci_field_t resource_combinations; // depending on n_subChannel^SL (sl-NumSubchennel), N_rsv_period (sl-ResourceReservePeriodList) and sl-MultiReservedResource, format 2C
        uint8_t first_resource_location; // 8 bits, format 2C
	dci_field_t reference_slot_location; // depending on mu, format 2C
	uint8_t resource_set_type; // 1 bit, format 2C
	dci_field_t lowest_subchannel_indices; // depending on n_subChannel^SL, format 2C
} nr_sci_pdu_t;

// episys SL data-plane port (F1 minimal): sidelink resource-index helpers. Defined in nr_ue_sci_slsch.c.
// FRIV (frequency resource index) <-> subchannel allocation (38.212 8.1.5); TRIV (time resource index).
uint32_t compute_FRIV(uint8_t sl_max_num_per_reserve, uint8_t L_sub_chan, uint8_t n_start_subch1,
                      uint8_t n_start_subch2, uint8_t N_sl_subch);
uint32_t compute_TRIV(uint8_t N, uint8_t t1, uint8_t t2);
// Inverse of compute_TRIV: recover t1/t2 from a received TRIV. In nr_ue_sci_slsch.c.
int inverse_TRIV(uint8_t N, uint32_t triv, uint8_t *t1, uint8_t *t2);
void convNRFRIV(int FRIV, int N_subch, long sl_MaxNumPerReserve, uint16_t *Lsc, uint16_t *startsc, uint16_t *startsc2);
// SCI bit-length (fills per-field nbits) + MSB-first packers into the PSCCH/PSSCH SCI payloads. In nr_ue_sci_slsch.c.
struct NR_SL_ResourcePool_r16; // ASN.1 (avoid pulling the generated header into this MAC header)
uint32_t nr_sci_size(const struct NR_SL_ResourcePool_r16 *sl_res_pool, nr_sci_pdu_t *sci_pdu, const nr_sci_format_t format);
void nr_pack_sci1(nr_sci_pdu_t *sci, int sci_size, uint64_t *payload);
void nr_pack_sci2(nr_sci_pdu_t *sci2, int sci2_size, nr_sci_format_t format, uint64_t *payload);
int get_nREDMRS(const struct NR_SL_ResourcePool_r16 *sl_res_pool);
// get_nRECSI_RS (MAC divisor form) is file-local (static) in nr_ue_sci_slsch.c — see note there.
int get_NREsci2(const int sci2_alpha, const int sci2_payload_len, const int sci2_beta_offset, const int pssch_numsym,
                const int pscch_numsym, const int pscch_numrbs, const int l_subch, const int subchannel_size,
                const int mcs, const int mcs_tb_ind);
// Assemble the PSCCH+PSSCH TX PDU (SCI-1A/SCI-2 payloads + SLSCH TB size + PSSCH DMRS positions). In nr_ue_sci_slsch.c.
struct NR_SL_BWP_Generic_r16;
struct sl_nr_tx_config_pscch_pssch_pdu;
/* csi_freq_density/csi_nr_of_rbs let the TB-size math subtract the CSI-RS
   REs from the SLSCH when sci2_pdu->csi_req is set (must match the PHY G reduction). Pass 0 to disable. */
void fill_pssch_pscch_pdu(struct sl_nr_tx_config_pscch_pssch_pdu *pdu,
                          const struct NR_SL_BWP_Generic_r16 *sl_bwp_generic,
                          const struct NR_SL_ResourcePool_r16 *sl_res_pool,
                          nr_sci_pdu_t *sci_pdu,
                          nr_sci_pdu_t *sci2_pdu,
                          const nr_sci_format_t format1,
                          const nr_sci_format_t format2,
                          uint8_t csi_freq_density,
                          uint16_t csi_nr_of_rbs);
/* fill the FAPI CSI-RS resource PDU (MAC->PHY) from the parsed sl_mac
   CSI-RS config. Defined in nr_ue_scheduler_sl.c; shared so the multi-pass SCI-2 handler can arm CSI-RS
   RX alongside the SLSCH RX config. Same values on TX (generation) and RX (measurement). */
struct sl_nr_tti_csi_rs_pdu;
struct sl_nr_ue_mac_params;
void fill_sl_csi_rs_pdu(struct sl_nr_tti_csi_rs_pdu *csi, const struct sl_nr_ue_mac_params *sl_mac, uint8_t scs);
// RX blind-decode config builders (from a decoded/preconfigured SCI-1 PDU). In nr_ue_sci_slsch.c.
struct sl_nr_rx_config_pssch_pdu;
struct sl_nr_rx_config_pssch_sci_pdu;
void config_pssch_slsch_pdu_rx(struct sl_nr_rx_config_pssch_pdu *nr_sl_pssch_pdu,
                               nr_sci_pdu_t *sci_pdu,
                               const struct NR_SL_BWP_Generic_r16 *sl_bwp_generic,
                               const struct NR_SL_ResourcePool_r16 *sl_res_pool,
                               uint8_t csi_freq_density,
                               uint16_t csi_nr_of_rbs);
int config_pssch_sci_pdu_rx(struct sl_nr_rx_config_pssch_sci_pdu *nr_sl_pssch_sci_pdu,
                            nr_sci_format_t sci2_format,
                            nr_sci_pdu_t *sci_pdu,
                            uint32_t pscch_Nid,
                            int pscch_subchannel_index,
                            const struct NR_SL_BWP_Generic_r16 *sl_bwp_generic,
                            const struct NR_SL_ResourcePool_r16 *sl_res_pool);
// PSCCH (SCI-1A) receive config: the region + scrambling id the transmitter used, so the RX can decode
// SCI-1A and take the PSSCH Nid from its CRC instead of guessing. In nr_ue_sci_slsch.c.
struct sl_nr_rx_config_pscch_pdu;
void config_pscch_pdu_rx(struct sl_nr_rx_config_pscch_pdu *nr_sl_pscch_pdu,
                         nr_sci_pdu_t *sci_pdu,
                         const struct NR_SL_BWP_Generic_r16 *sl_bwp_generic,
                         const struct NR_SL_ResourcePool_r16 *sl_res_pool);
// Unpack a received SCI-1A payload into sci_pdu (inverse of nr_pack_sci1). In nr_ue_sci_slsch.c.
void extract_pscch_pdu(uint64_t *sci1_payload,
                       int len,
                       const struct NR_SL_BWP_Generic_r16 *sl_bwp_generic,
                       const struct NR_SL_ResourcePool_r16 *sl_res_pool,
                       nr_sci_pdu_t *sci_pdu);
// Minimal F1 SLSCH scheduler: populate SCI-1 + SCI-2 field values (fixed resource/MCS). In nr_ue_sci_slsch.c.
void nr_schedule_slsch(const struct NR_SL_ResourcePool_r16 *sl_tx_res_pool,
                       nr_sci_pdu_t *sci_pdu,
                       nr_sci_pdu_t *sci2_pdu,
                       uint8_t harq_pid,
                       uint8_t ndi,
                       uint8_t rv,
                       uint16_t src_id,
                       uint16_t dest_id,
                       uint8_t mcs,
                       uint8_t psfch_overhead);

// episys SL PSFCH port (4c-A): SCI-2 (format 2A) RX decode -> mac->sci_pdu_rx.
// (nr_ue_process_sci2_indication_pdu proto is in mac_proto.h, which has NR_UE_MAC_INST_t.)
struct NR_SL_BWP_ConfigCommon_r16; // file-scope fwd decl so the tag isn't prototype-scoped
void extract_pssch_sci_pdu(uint64_t *sci2_payload,
                           int len,
                           const struct NR_SL_BWP_ConfigCommon_r16 *sl_bwp,
                           const struct NR_SL_ResourcePool_r16 *sl_res_pool,
                           nr_sci_pdu_t *sci_pdu);
#endif
