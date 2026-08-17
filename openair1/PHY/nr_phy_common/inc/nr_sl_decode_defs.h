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
 */

/* \file nr_sl_decode_defs.h
 * \brief Shared NR shared-channel LDPC decode-job / UL-HARQ / ULSCH structs.
 *        Moved out of PHY/defs_gNB.h into nr_phy_common so BOTH the gNB PHY (PHY_NR) and the
 *        UE PHY (PHY_NR_UE) can use them without the UE binary linking the gNB PHY lib.
 *        (episys SL data-plane port: PSSCH RX decode reuses this decode infrastructure.)
 *        The gNB back-pointer in ldpcDecode_t is kept OPAQUE (struct PHY_VARS_gNB_s *) so this
 *        header carries no gNB-specific dependency; gNB TUs that dereference it include defs_gNB.h.
 */

#ifndef __PHY_NR_COMMON_NR_SL_DECODE_DEFS_H__
#define __PHY_NR_COMMON_NR_SL_DECODE_DEFS_H__

#include <stdint.h>
#include <stdbool.h>
#include "nfapi_nr_interface_scf.h"
#include "PHY/CODING/coding_defs.h"
#include "PHY/CODING/nrLDPC_decoder/nrLDPC_types.h"
#include "common/utils/threadPool/task_ans.h"
#include "common/utils/nr/nr_common.h"          // delay_t
#include "PHY/TOOLS/tools_defs.h"                // c16_t
#include "nfapi/open-nFAPI/nfapi/public_inc/sidelink_nr_ue_interface.h"

// Opaque: gNB TUs complete this via PHY/defs_gNB.h; UE TUs never dereference it.
struct PHY_VARS_gNB_s;

typedef struct {
  /// Nfapi ULSCH PDU
  nfapi_nr_pusch_pdu_t ulsch_pdu; // !!
  /// episys SL data-plane port: PSSCH SCI + SLSCH PDUs (this HARQ reused for UE sidelink RX decode)
  sl_nr_rx_config_pssch_sci_pdu_t *pssch_pdu;
  sl_nr_rx_config_pssch_pdu_t *slsch_pdu;
  /// Index of current HARQ round for this DLSCH
  uint8_t round;
  bool new_rx;
  /////////////////////// ulsch decoding ///////////////////////
  /// flag used to clear d properly
  /// set to true in nr_fill_ulsch() when new_data_indicator is received
  bool harq_to_be_cleared;
  /// Pointer to the payload (38.212 V15.4.0 section 5.1)
  uint8_t *b;
  /// Pointer to aggregated code blocks after code block segmentation and CRC attachment (38.212 V15.4.0 section 5.2.2)
  uint8_t *c;
  /// Number of bits in each code block (38.212 V15.4.0 section 5.2.2)
  uint32_t K;
  /// Number of "Filler" bits added in the code block segmentation (38.212 V15.4.0 section 5.2.2)
  uint32_t F;
  /// Number of code blocks after code block segmentation (38.212 V15.4.0 section 5.2.2)
  uint32_t C;
  /// Pointers to aggregated code blocks after LDPC coding (38.212 V15.4.0 section 5.3.2)
  int16_t *d;
  /// LDPC lifting size (38.212 V15.4.0 table 5.3.2-1)
  uint32_t Z;
  /// Number of bits in each code block after rate matching for LDPC code (38.212 V15.4.0 section 5.4.2.1)
  uint32_t E;
  /// Number of segments processed so far
  uint32_t processedSegments;
  /// episys SL data-plane port: decoded transport block size in bytes (SLSCH RX delivery)
  uint32_t TBS;
  decode_abort_t abort_decode;
  /// Last index of LLR buffer that contains information.
  /// Used for computing LDPC decoder R
  int llrLen;
  //////////////////////////////////////////////////////////////
} NR_UL_gNB_HARQ_t;

// Tagged (NR_gNB_ULSCH_s) so UE headers can forward-declare it without pulling this header.
typedef struct NR_gNB_ULSCH_s {
  uint32_t frame;
  uint32_t slot;
  uint32_t unav_res;
  /// Pointers to 16 HARQ processes for the ULSCH
  NR_UL_gNB_HARQ_t *harq_process;
  /// HARQ process mask, indicates which processes are currently active
  int harq_pid;
  /// Allocated RNTI for this ULSCH
  uint16_t rnti;
  /// Maximum number of LDPC iterations
  uint8_t max_ldpc_iterations;
  /// number of iterations used in last LDPC decoding
  int8_t last_iteration_cnt;
  /// Status Flag indicating for this ULSCH
  bool active;
  /// episys SL data-plane port: set when a SLSCH decode round is handled (in error) by the UE
  int handled;
} NR_gNB_ULSCH_t;

typedef struct LDPCDecode_s {
  struct PHY_VARS_gNB_s *gNB;
  NR_UL_gNB_HARQ_t *ulsch_harq;
  t_nrLDPC_dec_params decoderParms;
  NR_gNB_ULSCH_t *ulsch;
  int16_t *ulsch_llr;
  int ulsch_id;
  int harq_pid;
  int rv_index;
  int A;
  int E;
  int Kc;
  int Qm;
  int Kr_bytes;
  int nbSegments;
  int segment_r;
  int r_offset;
  int offset;
  int decodeIterations;
  uint32_t tbslbrm;
  task_ans_t *ans;
} ldpcDecode_t;

// PUSCH/PSSCH receive vars (channel estimates, LLR scratch). Moved from PHY/defs_gNB.h; the UE
// sidelink PSSCH RX path uses this as PHY_VARS_NR_UE.pssch_vars. Tagged struct so UE headers can
// forward-declare it without pulling this header.
typedef struct NR_gNB_PUSCH_s {
  /// \brief Hold the channel estimates in frequency domain based on DRS.
  int32_t **ul_ch_estimates;
  /// \brief Holds the compensated signal.
  c16_t **rxdataF_comp;
  /// \f$\log_2(\max|H_i|^2)\f$
  int16_t log2_maxh;
  /// measured RX power based on DRS
  uint32_t ulsch_power[8];
  /// total signal over antennas
  uint32_t ulsch_power_tot;
  /// measured RX noise power
  uint32_t ulsch_noise_power[8];
  /// total noise over antennas
  uint32_t ulsch_noise_power_tot;
  /// \brief llr values.
  int16_t *llr;
  /// episys SL data-plane port: per-layer llr scratch (PSSCH layer demapping)
  int16_t **llr_layers;
  // PTRS symbol index, to be updated every PTRS symbol within a slot.
  uint8_t ptrs_symbol_index;
  /// bit mask of PT-RS ofdm symbol indicies
  uint16_t ptrs_symbols;
  // PTRS subcarriers per OFDM symbol
  int32_t ptrs_re_per_slot;
  /// \brief Estimated phase error based upon PTRS on each symbol .
  int32_t **ptrs_phase_per_slot;
  /// \brief Total RE count after DMRS/PTRS RE's are extracted from respective symbol.
  int16_t *ul_valid_re_per_slot;
  /// \brief offset for llr corresponding to each symbol
  int llr_offset[14];
  /// flag to indicate DTX on reception
  int DTX;
  /// delay estimation
  delay_t delay;
} NR_gNB_PUSCH;

#endif /* __PHY_NR_COMMON_NR_SL_DECODE_DEFS_H__ */
