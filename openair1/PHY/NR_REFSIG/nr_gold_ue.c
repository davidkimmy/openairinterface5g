/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "sl_refsig_defs.h"
#include "openair1/PHY/gold.h"

void sl_init_psbch_dmrs_gold_sequences(PHY_VARS_NR_UE *UE)
{
  unsigned int x1, x2;
  uint16_t slss_id;
  uint8_t reset;

  for (slss_id = 0; slss_id < SL_NR_NUM_SLSS_IDs; slss_id++) {
    reset = 1;
    x2 = slss_id;

#ifdef SL_DEBUG_INIT
    printf("\nPSBCH DMRS GOLD SEQ for SLSSID :%d  :\n", slss_id);
#endif

    for (uint8_t n = 0; n < SL_NR_NUM_PSBCH_DMRS_RE_DWORD; n++) {
      UE->SL_UE_PHY_PARAMS.init_params.psbch_dmrs_gold_sequences[slss_id][n] = gold_generic(&x1, &x2, reset);
      reset = 0;

#ifdef SL_DEBUG_INIT_DATA
      printf("%x\n", SL_UE_INIT_PARAMS.sl_psbch_dmrs_gold_sequences[slss_id][n]);
#endif
    }
  }
}

// episys SL data-plane port: PSSCH DMRS gold sequence for one (slot, symbol), scrambled by N_id
// (from the PSCCH CRC, 38.211 8.4.1.1). One-shot generation (no precompute table). Used by the
// PSSCH TX RE mapper (nr_ue_slsch_procedures).
void nr_init_pssch_dmrs_oneshot(const NR_DL_FRAME_PARMS *fp, uint16_t N_id, uint32_t *pssch_dmrs, int slot, int symb)
{
  uint32_t x1 = 0;
  uint32_t x2 = ((1U << 17) * (fp->symbols_per_slot * slot + symb + 1) * ((N_id << 1) + 1) + (N_id << 1));
  const int len = ((fp->N_RB_UL * 12) >> 5) + 1;
  int reset = 1;
  for (int n = 0; n < len; n++) {
    pssch_dmrs[n] = gold_generic(&x1, &x2, reset);
    reset = 0;
  }
}
