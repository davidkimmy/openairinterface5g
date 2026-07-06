/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef _SIDELINK_NR_UE_INTERFACE_H_
#define _SIDELINK_NR_UE_INTERFACE_H_

#include "fapi_nr_ue_interface.h"

// episys SL data-plane port: PSSCH RX needs 2 config PDUs (SCI-2/demod cfg + SLSCH transport cfg); PSBCH uses 1.
#define SL_NR_RX_CONFIG_LIST_NUM 2
#define SL_NR_TX_CONFIG_LIST_NUM 1
#define SL_NR_RX_IND_MAX_PDU 1
#define SL_NR_MAX_PSCCH_SCI_LENGTH_IN_BYTES 8
#define SL_NR_MAX_PSSCH_SCI_LENGTH_IN_BYTES 8
#define SL_NR_MAX_SCI_LENGTH_IN_BYTES 8
// episys SL data-plane port: bound for the SLSCH TB carried in the TX config PDU (F1 minimal path).
#define SL_NR_MAX_SLSCH_PAYLOAD_BYTES 4096

typedef enum sl_sci_format_type_enum {
  SL_SCI_INVALID_FORMAT,
  SL_SCI_FORMAT_1A_ON_PSCCH,
  SL_SCI_FORMAT_2_ON_PSSCH,
} sl_sci_format_type_enum_t;

//Type of Contents of SL-RX indication from PHY to MAC
typedef enum sl_rx_pdu_type_enum {
  SL_NR_RX_PDU_TYPE_NONE,
  SL_NR_RX_PDU_TYPE_SSB,
  SL_NR_RX_PDU_TYPE_SLSCH,
  SL_NR_RX_PDU_TYPE_SLSCH_PSFCH  // episys SL data-plane port: PSSCH+PSFCH RX pdu type
} sl_rx_pdu_type_enum_t;

//Type of SL-RX CONFIG requests from MAC to PHY
typedef enum sl_nr_rx_config_type_enum {
  SL_NR_CONFIG_TYPE_RX_PSBCH = 1,
  SL_NR_CONFIG_TYPE_RX_PSCCH,
  SL_NR_CONFIG_TYPE_RX_PSSCH_SCI,
  SL_NR_CONFIG_TYPE_RX_PSSCH_SLSCH,
  SL_NR_CONFIG_TYPE_RX_PSSCH_SLSCH_PSFCH,  // episys SL data-plane port: PSSCH+SLSCH with PSFCH feedback
  SL_NR_CONFIG_TYPE_RX_PSFCH,
  SL_NR_CONFIG_TYPE_RX_MAXIMUM
} sl_nr_rx_config_type_enum_t;

//Type of SL-TX CONFIG requests from MAC to PHY
typedef enum sl_nr_tx_config_type_enum {
  SL_NR_CONFIG_TYPE_TX_PSBCH = SL_NR_CONFIG_TYPE_RX_MAXIMUM + 1,
  SL_NR_CONFIG_TYPE_TX_PSCCH_PSSCH,
  SL_NR_CONFIG_TYPE_TX_PSFCH,
  SL_NR_CONFIG_TYPE_TX_MAXIMUM
} sl_nr_tx_config_type_enum_t;

//PHY Indicates MAC about the SCI-indication
//During blind decoding, SCI-1A can be decoded in every subchannel.
typedef struct {
  sl_sci_format_type_enum_t sci_format_type; // SCI FORMAT 1A or SCI FORMAT 2
  // lowest subchannel index in the resource pool where the SCI-1A was detected
  // Not used for SCI-2
  uint16_t subch_index;
  //RSRP sent in case of sense pscch
  int16_t  pscch_rsrp;
  //Length of SCI decoded
  uint16_t sci_payloadlen;
  //Derived from PSCCH CRC Refer 38.211 section 8.3.1.1
  //Will be passed on if PSSCH decoding needs to be done.
  //Filled only if PSCCH decoded
  uint16_t Nid;
  // Payload of the received SCIs
  uint8_t sci_payloadBits[SL_NR_MAX_SCI_LENGTH_IN_BYTES];
} sl_nr_sci_indication_pdu_t;

// PHY indicates MAC about the received SCIs using this API
typedef struct {
  uint16_t sfn;
  uint8_t slot;
  uint16_t number_of_SCIs;
  //If response to sensing - then this will be set to TRUE
  uint8_t sensing_result;
  //in case pssch sensing is requested.
  int16_t pssch_rsrp;
  sl_nr_sci_indication_pdu_t sci_pdu;
} sl_nr_sci_indication_t;

// IF UE Rx PSBCH, PHY indicates MAC with received MIB and PSBCH RSRP
typedef struct sl_nr_ssb_pdu {
  uint8_t psbch_payload[4];
  int16_t rsrp_dbm;
  uint16_t rx_slss_id;
  bool decode_status;
} sl_nr_ssb_pdu_t;

//Use the same structure of pdsch pdu for slsch pdu
typedef fapi_nr_pdsch_pdu_t sl_nr_slsch_pdu_t;

typedef struct {
  sl_rx_pdu_type_enum_t pdu_type;
  union {
    sl_nr_ssb_pdu_t ssb_pdu;
    sl_nr_slsch_pdu_t rx_slsch_pdu;
  };
} sl_nr_rx_indication_body_t;

typedef struct {
  uint16_t sfn;
  uint16_t slot;
  uint16_t number_pdus;
  sl_nr_rx_indication_body_t rx_indication_body[SL_NR_RX_IND_MAX_PDU];
} sl_nr_rx_indication_t;

typedef struct sl_nr_rx_config_pscch_pdu {
  // Starting RE of the lowest subchannel in a resource where PSCCH
  // freq domain allocation starts
  uint16_t pscch_startrb;
  // Number of symbols used for PSCCH
  uint16_t pscch_numsym;
  // Number of  RBS used for PSCCH
  uint16_t pscch_numrbs;
  // Scrambling Id used for Generation of PSCCH DMRS Symbols
  uint32_t pscch_dmrs_scrambling_id;
  // num subchannels in a resource pool
  uint16_t num_subch;
  // Size of subchannels in RBs
  uint16_t subchannel_size;
  // PSCCH PSSCH RX: this is set to 1 - Blind decoding for SCI1A done on every subchannel
  // PSCCH SENSING: this is equal to number of subchannels forming a resource.
  uint16_t l_subch;
  //number of symbols for Sidelink transmission on PSSCH/PSCCH
  //(Total Sidelink symbols available - number of psfch symbols configured - 2)
  //Guard symbol + AGC symbol are also excluded
  //Indicates the number of symbols for PSCCH+PSSCH txn
  uint8_t pssch_numsym;
  //sci 1A length used to decode on PSCCH.
  uint8_t sci_1a_length;
  //This paramter is set if PSCCH RX is triggered on TX resource pool
  // as part of TX pool sensing procedure.
  uint8_t sense_pscch;
} sl_nr_rx_config_pscch_pdu_t;

typedef struct sl_nr_rx_config_pssch_sci_pdu {
  // Expected Length of SCI2 in bits
  uint8_t sci2_len;
  // Used to determine number of SCI2 modulated symbols
  uint8_t sci2_beta_offset;
  // Used to determine number of SCI2 modulated symbols
 //Values will be sl-scaling*100 (sl-scaling values 0.5, 0.65, 0.8, 1)
  uint8_t sci2_alpha_times_100;

  uint16_t targetCodeRate;
  uint8_t mod_order;
  uint8_t num_layers;

  //DMRS SYMBOL MASK. If bit set to 1 indicates it is a DMRS symbol. LSB is symbol 0
  // Table from SPEC 38.211, Table 8.4.1.1.2-1
  uint16_t dmrs_symbol_position;

  // Derived from PSCCH CRC Refer 38.211 section 8.3.1.1
  // to be used for PSSCH DMRS and PSSCH 38.211 Scrambling
  uint16_t Nid;

  // Starting RE of the lowest subchannel.
  //In Sym with PSCCH - Start of PSCCH
  //In Sym without PSCCH - Start of PSSCH
  // freq domain allocation starts
  uint16_t startrb;
  // Number of symbols used for PSCCH
  uint16_t pscch_numsym;
  // Number of  RBS used for PSCCH
  uint16_t pscch_numrbs;
  // num subchannels in a resource pool
  uint16_t num_subch;
  // Size of subchannels in RBs
  uint16_t subchannel_size;
  // In case of PSCCH PSSCH RX: this is always 1. Blind decoding done for every channel
  // In case of RESOURCE SENSING: this is equal to number of subchannels forming a resource.
  uint16_t l_subch;
  //number of symbols for Sidelink transmission on PSSCH/PSCCH
  //(Total Sidelink symbols available - number of psfch symbols configured - 2)
  //Guard symbol + AGC symbol are also excluded
  //Indicates the number of symbols for PSCCH+PSSCH txn
  uint8_t pssch_numsym;

  //This paramter is set if PSSCH sensing (PSSCH DMRS RSRP measurement)
  // is triggred as part of TX pool sensing procedure.
  uint8_t sense_pssch;

} sl_nr_rx_config_pssch_sci_pdu_t;

typedef struct sl_nr_rx_config_pssch_pdu {
  uint32_t tbslbrm;
  // Transport block size determined by MAC
  uint32_t tb_size;
  uint16_t target_coderate;
  uint8_t  harq_pid;
  uint8_t  mod_order;
  uint8_t  mcs;
  uint8_t  mcs_table;
  uint8_t  num_layers;
 //REdundancy version to be used for transmission
  uint8_t rv_index;
  uint8_t ndi;
} sl_nr_rx_config_pssch_pdu_t;

typedef struct {
  sl_nr_rx_config_type_enum_t pdu_type; // indicates the type of RX config request
  union {
    sl_nr_rx_config_pscch_pdu_t rx_pscch_config_pdu;
    sl_nr_rx_config_pssch_sci_pdu_t rx_sci2_config_pdu;
    sl_nr_rx_config_pssch_pdu_t rx_pssch_config_pdu;
  };
} sl_nr_rx_config_request_pdu_t;

// MAC commands PHY to perform an action on RX RESOURCE POOL or RX PSBCH using this RX CONFIG
// at this TTI as indicated in sfn, slot
typedef struct {
  uint16_t sfn;
  uint16_t slot;
  uint8_t number_pdus;
  sl_nr_rx_config_request_pdu_t sl_rx_config_list[SL_NR_RX_CONFIG_LIST_NUM];
} sl_nr_rx_config_request_t;

//MAC commands PHY to transmit Data on PSCCH, PSSCH.
typedef struct sl_nr_tx_config_pscch_pssch_pdu {

  //SCI 1A Payload Prepared by MAC, to be sent on PSCCH
  uint8_t pscch_sci_payload[SL_NR_MAX_PSCCH_SCI_LENGTH_IN_BYTES];
  //SCI 1A Payload Length in bits
  uint8_t pscch_sci_payload_len;
  //SCI 2 Payload Prepared by MAC and to be sent on PSSCH
  uint8_t sci2_payload[SL_NR_MAX_PSSCH_SCI_LENGTH_IN_BYTES];
  //SCI 2 Payload Length in bits
  uint8_t sci2_payload_len;

  // Starting RE of the lowest subchannel.
  //In Sym with PSCCH - Start of PSCCH
  //In Sym without PSCCH - Start of PSSCH
  // freq domain allocation starts
  uint16_t startrb;
  // Number of symbols used for PSCCH
  uint16_t pscch_numsym;
  // Number of  RBS used for PSCCH
  uint16_t pscch_numrbs;
  // Scrambling Id used for Generation of PSCCH DMRS Symbols
  uint32_t pscch_dmrs_scrambling_id;
  // num subchannels in a resource pool
  uint16_t num_subch;
  // Size of subchannels in RBs
  uint16_t subchannel_size;
  //PSCCH PSSCH TX: Size of subchannels in a PSSCH resource (l_subch)
  uint16_t l_subch;
  //number of symbols for Sidelink transmission on PSSCH/PSCCH
  //(Total Sidelink symbols available - number of psfch symbols configured - 2)
  //Guard symbol + AGC symbol are also excluded
  //Indicates the number of symbols for PSCCH+PSSCH txn
  uint8_t pssch_numsym;

  //.... Other Parameters for SCI-2 and PSSCH

  // Used to determine number of SCI2 modulated symbols
  uint8_t sci2_beta_offset;
  //Values will be sl-scaling*100 (sl-scaling values 0.5, 0.65, 0.8, 1)
  uint8_t sci2_alpha_times_100;

  uint32_t tbslbrm;
  uint32_t tb_size;
  uint16_t target_coderate;
  uint8_t  harq_pid;
  uint8_t  mod_order;
  uint8_t  mcs;
  uint8_t  mcs_table;
  uint8_t  num_layers;
  uint8_t rv_index;
  uint8_t ndi;

  //DMRS SYMBOL MASK. If bit set to 1 indicates it is a DMRS symbol. LSB is symbol 0
  // Table from SPEC 38.211, Table 8.4.1.1.2-1
  uint16_t dmrs_symbol_position;


  //....TBD.. any additional parameters

  //TX Power for PSSCH in symbol without PSCCH.
  // Power for PSCCH and power for PSSCH in symbol with PSCCH is calculated
  // from this value according to 38.213 section 16
  int16_t pssch_tx_power;

  // episys SL data-plane port: SLSCH transport block carried MAC->PHY by value (the SL tx path has no
  // separate tx_request PDU like Uu). PHY copies slsch_payload[0..slsch_payload_len) into
  // ue->ul_harq_processes[harq_pid].payload_AB before encoding. Bounded for the F1 minimal path.
  uint32_t slsch_payload_len;
  uint8_t slsch_payload[SL_NR_MAX_SLSCH_PAYLOAD_BYTES];

} sl_nr_tx_config_pscch_pssch_pdu_t;

// MAC indicates PHY to send PSBCH.
typedef struct sl_nr_tx_config_psbch_pdu {

  uint8_t psbch_payload[4]; // payload containing sl-MIB
  uint16_t tx_slss_id; // slss_id selected by RRC and to be used for MIB transmission on PSBCH
  int16_t psbch_tx_power; // TX Power used for sending PSBCH

} sl_nr_tx_config_psbch_pdu_t;

// MAC commands PHY to perform an action on TX RESOURCE POOL or TX PSBCH using this TX CONFIG
typedef struct {
  sl_nr_tx_config_type_enum_t pdu_type; // indicates the type of TX config request
  union {
    sl_nr_tx_config_psbch_pdu_t tx_psbch_config_pdu;
    sl_nr_tx_config_pscch_pssch_pdu_t tx_pscch_pssch_config_pdu;
  };
} sl_nr_tx_config_request_pdu_t;

// MAC commands PHY to perform an action on TX RESOURCE POOL or TX PSBCH using this TX CONFIG
// at this TTI as indicated in sfn, slot
typedef struct {
  uint16_t sfn;
  uint16_t slot;
  uint8_t number_pdus;
  sl_nr_tx_config_request_pdu_t tx_config_list[SL_NR_TX_CONFIG_LIST_NUM];
} sl_nr_tx_config_request_t;

typedef enum sl_sync_source_enum {

 SL_SYNC_SOURCE_NONE = 0,
 SL_SYNC_SOURCE_GNBENB, // NR-CELL as sync source
 SL_SYNC_SOURCE_GNSS, // GPS as sync source
 SL_SYNC_SOURCE_SYNC_REF_UE, // another SYNC REF UE as sync source
 SL_SYNC_SOURCE_LOCAL_TIMING, // if LOCAL UE timing is sync source , if above options are not available
 SL_SYNC_SOURCE_MAX

} sl_sync_source_enum_t ;


typedef struct sl_nr_ue_bwp_config {

  uint16_t sl_bwp_start; // BWP start RB
  uint16_t sl_bwp_size;  // BWP Size in number of RBs
  uint16_t sl_ssb_offset_point_a; // offset in REs from PointA to indicate SSB location
  uint16_t sl_dc_location; // DC location in SL-BWP
  uint8_t sl_num_symbols; // number of sidelink symbols in a non PSBCH sidelink slot
  uint8_t sl_start_symbol;// start sidelink symbol in a non PSBCH sidelink slot
  uint8_t  sl_scs; // sub carrier spacing of SL-BWP and SL-SSB
  uint8_t  sl_cyclic_prefix; // cyclic prefix of SL-BWP and SL-SSB 0- normal, 1-extended which is not supported

} sl_nr_bwp_config_t;

typedef struct sl_nr_ue_sync_source_config {

  // Sync source selected by the UE
  sl_sync_source_enum_t sync_source;

  // if GNSS is sync source - used in DFN calculation from GNSS timing
  //field sl-OffsetDFN, Section 5.8.12 in 38.331
  // value from 0 to 1000,
  uint16_t gnss_dfn_offset;

  //Only sent if sync_source is set to SYNC_SOURCE_SYNC_REF_UE
  //Valid values can be 0-671
  uint16_t rx_slss_id;

} sl_nr_sync_source_config_t;

typedef struct 
{
  // REference Rel16 38.101, 38.331
  //For NR-Sidelink operation on PC5 interface
  //Sidelink bandwidth configured - 10, 20, 30, and 40Mhz
  uint16_t sl_bandwidth;
  //Absolute frequency of SL point A in KHz
  //n38 (2570-2620 Mhz), n47 (5855-5925 Mhz) are defined.
  uint64_t sl_frequency;

  //Only 1 SCS-SpecificCarrier allowed for NR-SL communication
  uint16_t sl_grid_size;// bandwidth for each numerology
  uint16_t sl_num_tx_ant;//Number of Tx antennae - value 1
  uint16_t sl_num_rx_ant;//Number of Rx antennae - value 1

  //Indicates presence of 7.5KHz frequency shift wrt LTE anchor cell.
  //Value: 0 = false 1 = true
  uint8_t  sl_frequency_shift_7p5khz;
  //Indicates presence of +/-5Khz shift wrt FREF for V2X reference frequencies.
  //Possible values: {-1,0,1}
  int8_t  sl_value_N;
  uint32_t band;
} sl_nr_carrier_config_t;


typedef struct {
  // Mask indicating which of the below configs are changed
  // Bit0 - carrier_config, Bit1 - syncsource cfg
  // Bit2 - tdd config, Bit3 - bwp config
  uint32_t config_mask;

  sl_nr_carrier_config_t sl_carrier_config;
  sl_nr_sync_source_config_t sl_sync_source;
  //TDD config either from SL-PRECONFIG or read from SL-MIB
  //In case of TDD-config available from gNB, tdd_table from NR PHY config will be used
  fapi_nr_tdd_table_t tdd_table;
  //only 1 SL-BWP can be configured in REL16, REL17
  sl_nr_bwp_config_t sl_bwp_config;

} sl_nr_phy_config_request_t;

/* ---- episys SL data-plane port: PSFCH (HARQ feedback) config PDU ---- */
typedef struct sl_nr_tx_rx_config_psfch_pdu {
  //  These fields can be mapped directly to the same fields in nfapi_nr_ul_config_pucch_pdu
  uint8_t freq_hop_flag;
  uint8_t group_hop_flag;
  uint8_t sequence_hop_flag;
  uint16_t second_hop_prb;
  uint8_t nr_of_symbols;
  uint8_t start_symbol_index;
  uint8_t hopping_id;
  uint16_t prb;
  uint16_t sl_bwp_start;
  uint16_t initial_cyclic_shift;
  uint8_t mcs;
  uint8_t bit_len_harq;
} sl_nr_tx_rx_config_psfch_pdu_t;

/* ---- episys SL data-plane port: SL CSI periodicity + SL Configured-Grant (Type 1) ---- */
typedef enum NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR {
	NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_NOTHING,	/* No components present */
	NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots4,
	NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots5,
	NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots8,
	NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots10,
	NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots16,
	NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots20,
	NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots32,
	NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots40,
	NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots64,
	NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots80,
	NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots160,
	NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots320,
	NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR_slots640
} NR_UE_SL_CSI_ResourcePeriodicityAndOffset_PR;

/*--------------------------------------------------------------
 *  Structure: sl_config_grant_t
 *  Purpose:  Hold configuration, state, and feedback info for
 *            Sidelink Configured Grant Type 1 (CG Type 1)
 *--------------------------------------------------------------*/
typedef enum {
    CG_TYPE_1,
    CG_TYPE_2
} cg_type_t;

typedef struct {
  /* ======= Configuration from RRC ======= */
  uint16_t  sl_period_cg;              // sl-PeriodCG (in slots or ms)
  uint8_t  sl_psfch_to_pucch_cg_type1; // slot offset k for PUCCH feedback (sl-PSFCH-ToPUCCH-CG-Type1)
  uint8_t  sl_n1pucch_an;              // index of PUCCH resource (sl-N1PUCCH-AN)
  uint8_t  sl_pucch_set_id;            // PUCCH resource set ID
  uint8_t  harq_feedback_enabled;      // 0=disabled, 1=enabled (from SCI or RRC)
  uint8_t  mu_sl;                      // numerology for SL BWP
  uint8_t  mu_ul;                      // numerology for UL BWP
  uint8_t  k;                          // scaling factor (from TS 38.211)
  double   Tc;                         // basic time unit (from TS 38.211)
  uint16_t sl_num_harq_processes;
  uint16_t sl_harq_proc_id_offset;
  uint16_t cg_id;                      // unique ID or index for this CG config
  cg_type_t type;                      // Type 1 or Type 2
  long sl_priority;
  long sl_cg_maxtransnum;
  long sl_freqresource_cg_type1;
  long sl_timeresource_cg_type1;
  long sl_resource_pool_id;
  long sl_startsubchannelcg_type1;
  long sl_timeoffsetcg_type1;
  long sl_timereferencesfn_type1;
  /* ======= Runtime State ======= */
  bool     active;                     // CG Type 1 active for current period
  uint32_t start_slot;                 // starting slot of current CG period
  uint32_t end_slot;                   // ending slot of current CG period
  uint32_t last_psfch_slot;            // slot index of last PSFCH reception
  uint32_t pucch_slot;                 // computed slot for PUCCH feedback (last_psfch_slot + k)
  uint8_t  psfch_count;                // number of PSFCH receptions this CG period
  uint8_t  harq_bit;                   // computed HARQ feedback bit (0=NACK, 1=ACK)
  uint8_t  harq_priority;              // priority value for the HARQ bit
  bool     feedback_pending;           // flag: feedback waiting to be sent
  bool     pucch_configured;           // flag: PUCCH resource exists and valid
  bool     feedback_sent;              // flag: PUCCH feedback transmitted
  /* ======= Diagnostics / Logging ======= */
  uint8_t  num_psfch_ack;              // number of ACKed PSFCH receptions
  uint8_t  num_psfch_nack;             // number of NACKed PSFCH receptions
  uint8_t  num_psfch_missed;           // missed PSFCHs in this CG period
} sl_config_grant_t;


#endif
