/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "mac_defs.h"
#include "mac_proto.h"
#include "executables/softmodem-common.h"  // episys SL PSFCH port (Stage 3): get_softmodem_params()
#include "common/utils/nr/nr_common.h"     // get_nb_periods_per_frame()

#define SL_DEBUG

// episys SL PSFCH port (Stage 3): PSFCH cyclic-shift for {ACK, NACK} when HARQ feedback is 1 bit
// (38.213 Table 16.3-1 -> sequence cyclic shift 0 for ACK, 6 for NACK).
static const int sequence_cyclic_shift_harq_ack_or_ack_or_only_nack[2] = {0, 6};

// develop's global get_first_ul_slot() takes a frame_structure_t; the episys SL PSFCH code used a 3-arg
// TDD-pattern form. Keep an SL-local helper replicating the episys logic to avoid the API clash.
// slots-per-frame on develop is not an exported array (nr_slots_per_frame is file-local in
// nr_mac_common_tdd.c); use the standard (10 << mu) formula instead.
static inline int sl_first_ul_slot(int nrofDownlinkSlots, int nrofDownlinkSymbols, int nrofUplinkSymbols)
{
  return nrofDownlinkSlots + (nrofDownlinkSymbols != 0 && nrofUplinkSymbols == 0);
}
#define SL_SLOTS_PER_FRAME(mu) (10 << (mu))

uint8_t sl_process_TDD_UL_DL_config_patterns(NR_TDD_UL_DL_ConfigCommon_t *TDD_UL_DL_Config,
                                             uint8_t mu,
                                             double *slot_period_P,
                                             uint8_t *w)
{

  uint8_t return_value = 255;
  *w = 0;
  int pattern1_dlul_period = TDD_UL_DL_Config->pattern1.dl_UL_TransmissionPeriodicity;

#ifdef SL_DEBUG

  printf("INPUT VALUES: function: %s\n", __func__);
  printf("pattern1 periodicity:%d\n", pattern1_dlul_period);
  if (TDD_UL_DL_Config->pattern1.ext1 != NULL && TDD_UL_DL_Config->pattern1.ext1->dl_UL_TransmissionPeriodicity_v1530 != NULL )
    printf("pattern1 periodicity_v1530:%ld\n", *TDD_UL_DL_Config->pattern1.ext1->dl_UL_TransmissionPeriodicity_v1530);
  if (TDD_UL_DL_Config->pattern2 != NULL) {
    printf("mu:%d, pattern2 periodicity:%d\n", mu, pattern1_dlul_period);
    if (TDD_UL_DL_Config->pattern2->ext1 != NULL && TDD_UL_DL_Config->pattern2->ext1->dl_UL_TransmissionPeriodicity_v1530 != NULL )
      printf("pattern2 periodicity_v1530:%ld\n", *TDD_UL_DL_Config->pattern2->ext1->dl_UL_TransmissionPeriodicity_v1530);
  }

#endif

  return_value = pattern1_dlul_period;
  switch (pattern1_dlul_period) {
    case 0:
      *slot_period_P = 0.5;
      break;
    case 1:
      *slot_period_P = 0.625;
      break;
    case 2:
      *slot_period_P = 1.0;
      break;
    case 3:
      *slot_period_P = 1.25;
      break;
    case 4:
      *slot_period_P = 2.0;
      break;
    case 5:
      *slot_period_P = 2.5;
      break;
    case 6:
      *slot_period_P = 5.0;
      return_value = 7;
      break;
    case 7:
      *slot_period_P = 10.0;
      return_value = 8;
      break;
    default:
      AssertFatal(1==0,"Incorrect value of dl_UL_TransmissionPeriodicity\n");
      break;
  }

  if (TDD_UL_DL_Config->pattern1.ext1 != NULL &&
      TDD_UL_DL_Config->pattern1.ext1->dl_UL_TransmissionPeriodicity_v1530 != NULL ) {
    if (*TDD_UL_DL_Config->pattern1.ext1->dl_UL_TransmissionPeriodicity_v1530 == 1) {
      *slot_period_P = 4.0;
      return_value = 6;
    } else {
      *slot_period_P = 3.0;
      return_value = 255;
    }
  }

  if (TDD_UL_DL_Config->pattern2 != NULL) {

    return_value = 255;
    *w = 1;

    if ((*slot_period_P == 4.0 ) && (TDD_UL_DL_Config->pattern2->dl_UL_TransmissionPeriodicity == 1)) {
      return_value = 13;
      *w = (mu == 3)? 2: 1;
    } else if ((*slot_period_P == 3.0 ) && (TDD_UL_DL_Config->pattern2->dl_UL_TransmissionPeriodicity == 4)) {
      return_value = 12;
      *w = (mu == 3)? 2: 1;
    } else if ((*slot_period_P == 3.0 ) && (TDD_UL_DL_Config->pattern2->dl_UL_TransmissionPeriodicity == 2)) {
      return_value = 8;
      *w = (mu == 3)? 2: 1;
    } else {

      switch (pattern1_dlul_period) {
        case 7:
          if (TDD_UL_DL_Config->pattern2->dl_UL_TransmissionPeriodicity == 7) {
            return_value = 15;
            *w = 1<<mu;
          }
          break;
        case 6:
          if (TDD_UL_DL_Config->pattern2->dl_UL_TransmissionPeriodicity == 6) {
            return_value = 14;
            *w = (mu==0)?1:1<<(mu-1);
          }
          break;
        case 5:
          if (TDD_UL_DL_Config->pattern2->dl_UL_TransmissionPeriodicity == 5) {
            return_value = 11;
            *w = (mu == 3)? 2: 1;
          }
          break;
        case 4:
          if (TDD_UL_DL_Config->pattern2->dl_UL_TransmissionPeriodicity == 0) {
            return_value = 5;
          }
          else if (TDD_UL_DL_Config->pattern2->dl_UL_TransmissionPeriodicity == 4) {
            return_value = 7;
          }
          else if (TDD_UL_DL_Config->pattern2->ext1 != NULL && *TDD_UL_DL_Config->pattern2->ext1->dl_UL_TransmissionPeriodicity_v1530 == 0) {
            return_value = 10;
            *w = (mu == 3)? 2: 1;
          }
          break;
        case 3:
          if (TDD_UL_DL_Config->pattern2->dl_UL_TransmissionPeriodicity == 3) {
            return_value = 4;
          }
          break;
        case 2:
          if (TDD_UL_DL_Config->pattern2->dl_UL_TransmissionPeriodicity == 2) {
            return_value = 2;
          }
          else if (TDD_UL_DL_Config->pattern2->ext1 != NULL && *TDD_UL_DL_Config->pattern2->ext1->dl_UL_TransmissionPeriodicity_v1530 == 0) {
            return_value = 6;
            *w = (mu == 3)? 2: 1;
          }
          else if (TDD_UL_DL_Config->pattern2->ext1 != NULL && *TDD_UL_DL_Config->pattern2->ext1->dl_UL_TransmissionPeriodicity_v1530 == 1) {
            return_value = 9;
            *w = (mu == 3)? 2: 1;
          }
          break;
        case 1:
          if (TDD_UL_DL_Config->pattern2->dl_UL_TransmissionPeriodicity == 1) {
            return_value = 1;
          }
          break;
        case 0:
          if (TDD_UL_DL_Config->pattern2->dl_UL_TransmissionPeriodicity == 0) {
            return_value = 0;
          }
          else if (TDD_UL_DL_Config->pattern2->dl_UL_TransmissionPeriodicity == 4) {
            return_value = 3;
          }
          break;
        default:
          AssertFatal(1==0,"Incorrect value of dl_UL_TransmissionPeriodicity");
      }
    }
  }

#ifdef SL_DEBUG
  printf("OUTPUT VALUES: function %s\n",__func__);
  printf("return_value:%d, *w:%d, slot_period_P:%f\n", return_value, *w, *slot_period_P);
#endif

  return return_value;
}

/*
This procedures prepares the psbch payload of tdd configuration according
to section 16.1 in 38.213
*/
void sl_prepare_psbch_payload(NR_TDD_UL_DL_ConfigCommon_t *TDD_UL_DL_Config,
                              uint8_t *bits_0_to_7, uint8_t *bits_8_to_11,
                              uint8_t mu, uint8_t L, uint8_t Y)
{

  uint8_t w = 0, a1_to_a4 = 0;
  uint8_t mu_ref = 0, diff = 0;
  uint8_t u_slots = 0, u_sym = 0, I1 = 0;
  uint8_t u_sl_slots = 0, u_sl_slots_2 = 0;
  double slot_period_P = 0.0;

  *bits_0_to_7  = 0xFF; // If TDD_UL_DL_Config = NULL all 12 bits are set to 1
  *bits_8_to_11 = 0xF0;

  if (TDD_UL_DL_Config != NULL) {

    mu_ref = TDD_UL_DL_Config->referenceSubcarrierSpacing;
    diff = 1 << (mu-mu_ref);
    u_slots = TDD_UL_DL_Config->pattern1.nrofUplinkSlots;
    u_sym = TDD_UL_DL_Config->pattern1.nrofUplinkSymbols;
    I1 = ((u_sym * diff) % L >= (L-Y)) ? 1 : 0;

#ifdef SL_DEBUG
    printf("INPUT VALUES: function %s\n", __func__);
    printf("numerology:%d, number of symbols:%d, sl-startSymbol:%d\n", mu, L, Y);
    printf("mu_ref:%d, u_slots:%d, u_sym:%d\n", mu_ref, u_slots, u_sym);
    if (TDD_UL_DL_Config->pattern2 != NULL)
      printf("u_slots_2:%ld, u_sym_2:%ld\n", TDD_UL_DL_Config->pattern2->nrofUplinkSlots,
                                             TDD_UL_DL_Config->pattern2->nrofUplinkSymbols);
#endif

    u_sl_slots = (u_slots * diff) + floor((u_sym*diff)/L) + I1;
    a1_to_a4 = sl_process_TDD_UL_DL_config_patterns(TDD_UL_DL_Config, mu, &slot_period_P, &w);
    AssertFatal(a1_to_a4 != 255,"Incorrect return value, wrong configuration.\n");

#ifdef SL_DEBUG
    printf("I1:%d, a1_to_a2:%d, u_sl_slots:%d\n", I1, a1_to_a4, u_sl_slots);
#endif

    if (TDD_UL_DL_Config->pattern2 != NULL) {

      uint8_t u_slots_2 = TDD_UL_DL_Config->pattern2->nrofUplinkSlots;
      uint8_t u_sym_2 = TDD_UL_DL_Config->pattern2->nrofUplinkSymbols;
      uint8_t I2 = ((u_sym_2 * diff) % L >= (L-Y)) ? 1 : 0;
      uint16_t val = floor(((u_slots_2 * diff) + floor((u_sym_2*diff)/L) + I2)/w);

      u_sl_slots_2 = val * ceil((slot_period_P*(1<<mu)+1)/w) + floor(u_sl_slots/w);

      *bits_0_to_7 = 0x80 | (a1_to_a4 << 3) | ((u_sl_slots_2 & 0x70) >> 4);
      *bits_8_to_11 = (u_sl_slots_2 & 0x0F) << 4;

#ifdef SL_DEBUG
    printf("I2:%d, val:%d, u_sl_slots_2:%d\n", I2, val, u_sl_slots_2);
#endif

    } else {
      *bits_0_to_7 = 0x00 | (a1_to_a4 << 3) | ((u_sl_slots & 0x70) >> 4);
      *bits_8_to_11 = (u_sl_slots & 0x0F) << 4;
    }
  }

#ifdef SL_DEBUG
    printf("OUTPUT VALUES: function %s\n", __func__);
    printf("12 bits payload buf[0]:%x, buf[1]:%x\n", *bits_0_to_7, *bits_8_to_11);
#endif

}

/*
This procedures prepares the psbch payload of tdd configuration according
to section 16.1 in 38.213
*/
uint8_t sl_decode_sl_TDD_Config(NR_TDD_UL_DL_ConfigCommon_t *TDD_UL_DL_Config,
                                uint8_t bits_0_to_7, uint8_t bits_8_to_11,
                                uint8_t mu, uint8_t L, uint8_t Y)
{

  AssertFatal(TDD_UL_DL_Config, "TDD_UL_DL_Config cannot be null");
  uint16_t num_SL_slots = 0, mixed_slot_numsym = 0;

  TDD_UL_DL_Config->pattern1.nrofDownlinkSlots = 0;
  TDD_UL_DL_Config->pattern1.nrofDownlinkSymbols = 0;
  TDD_UL_DL_Config->pattern1.nrofUplinkSlots = 0;
  TDD_UL_DL_Config->pattern1.nrofUplinkSymbols = 0;
  TDD_UL_DL_Config->referenceSubcarrierSpacing = mu;
  TDD_UL_DL_Config->pattern1.ext1 = NULL;

  LOG_D(MAC, "bits_0_to_7:%x, bits_8_to_11:%x, mu:%d, L:%d, Y:%d\n",
                                                  bits_0_to_7, bits_8_to_11,mu, L, Y);

  //If all bits are 1 - indicates that no TDD config was present.
  if ((bits_0_to_7 == 0xFF) && ((bits_8_to_11 & 0xF0) == 0xF0)) {
    //If no TDD config present - use all slots for Sidelink.
    //Spec not clear -- TBD....
    return 0;
  }

  //Bit A0 if 1 indicates pattern2 as present.
  if (bits_0_to_7 & 0x80) {
    //Pattern1 and Pattern2 Present.
    TDD_UL_DL_Config->pattern2 = malloc16_clear(sizeof(*TDD_UL_DL_Config->pattern2));
    AssertFatal(1==0,"Decoding Pattern2 - NOT YET IMPLEMENTED\n");
  } else {

    //Only Pattern1 Present. bits a1..a4 identify the periodicity.
    uint8_t val = (bits_0_to_7 & 0x78) >> 3;
    if (val >= 7)
      TDD_UL_DL_Config->pattern1.dl_UL_TransmissionPeriodicity = val-1;

    if (val == 6) {
      if (TDD_UL_DL_Config->pattern1.ext1 == NULL)
        TDD_UL_DL_Config->pattern1.ext1 = calloc(1, sizeof(*TDD_UL_DL_Config->pattern1.ext1));
      if (TDD_UL_DL_Config->pattern1.ext1->dl_UL_TransmissionPeriodicity_v1530 == NULL)
        TDD_UL_DL_Config->pattern1.ext1->dl_UL_TransmissionPeriodicity_v1530 = calloc(1, sizeof(long));
      *TDD_UL_DL_Config->pattern1.ext1->dl_UL_TransmissionPeriodicity_v1530 = 1;
    }

    //a5,a6..a11 bits from the 7th to 1st LSB of num SL slots
    num_SL_slots = ((bits_0_to_7 & 0x07) << 4 ) | ((bits_8_to_11 & 0xF0) >> 4);

    TDD_UL_DL_Config->pattern1.nrofUplinkSlots = num_SL_slots;
    TDD_UL_DL_Config->pattern1.nrofUplinkSymbols = mixed_slot_numsym;

    LOG_D(MAC, "SIDELINK: EXtracted TDD config from 12 bits - Sidelink Slots:%ld, Mixed_slot_symbols:%ld,dl_UL_TransmissionPeriodicity:%ld\n",
                                TDD_UL_DL_Config->pattern1.nrofUplinkSlots, TDD_UL_DL_Config->pattern1.nrofUplinkSymbols,
                                TDD_UL_DL_Config->pattern1.dl_UL_TransmissionPeriodicity);
  }
  return 1;
}

/*Function used to prepare Sidelink MIB*/
uint32_t sl_prepare_MIB(NR_TDD_UL_DL_ConfigCommon_t *TDD_UL_DL_Config,
                        uint8_t incoverage, uint8_t mu,
                        uint8_t start_symbol, uint8_t L)
{

  uint8_t  sl_mib_payload[4] = {0,0,0,0};
  //int mu = UE->sl_frame_params.numerology_index, start_symbol = UE->start_symbol;
  uint8_t byte0, byte1;
  //int L = (UE->sl_frame_params.Ncp == 0) ? 14 : 12;
  uint32_t sl_mib=0;

  sl_prepare_psbch_payload(TDD_UL_DL_Config, &byte0, &byte1, mu, L, start_symbol);
  sl_mib_payload[0] = byte0;
  sl_mib_payload[1] = byte1;

  AssertFatal(incoverage <= 1, "Invalid value for incoverage paramter for SL-MIB. Accepted values 0 or 1\n");
  sl_mib_payload[1] |= (incoverage << 3);

  sl_mib =  sl_mib_payload[1]<<8  | sl_mib_payload[0];

#ifdef SL_DEBUG
  printf("SIDELINK PSBCH SIM: NUM SYMBOLS:%d, mu:%d, start_symbol:%d incoverage:%d \n",
                                      L, mu, start_symbol, incoverage);
  printf("SIDELINK PSBCH PAYLOAD: psbch_a:%x, sl_mib_payload:%x %x %x %x\n",
                                sl_mib, sl_mib_payload[0],sl_mib_payload[1], sl_mib_payload[2], sl_mib_payload[3]);
#endif

  return sl_mib;
}

uint16_t sl_get_num_subch(NR_SL_ResourcePool_r16_t *rpool)
{

  //sl-NumSubchannel - Indicates the number of subchannels in the corresponding resource pool
  //which consists of contiguous PRBs only.
  uint16_t num_subch = (rpool->sl_NumSubchannel_r16) ? *rpool->sl_NumSubchannel_r16 : 0;

  AssertFatal(num_subch,"NUM Subchannels cannot be 0. Resource Pool Configuration Error\n");

  return num_subch;
}

uint16_t sl_get_subchannel_size(NR_SL_ResourcePool_r16_t *rpool)
{

  uint16_t num_subch = sl_get_num_subch(rpool);

  //sl-RB-Number - Indicates the number of PRBs in the corresponding resource pool.
  //which consists of contiguous PRBs only.The remaining RB cannot be used
  uint16_t num_rbs = (rpool->sl_RB_Number_r16) ? *rpool->sl_RB_Number_r16 : 0;

  AssertFatal(num_rbs,"NumRbs in rpool cannot be 0.Resource Pool Configuration Error\n");

  uint16_t subch_size = 0;

  subch_size = num_rbs/num_subch;

  LOG_D(NR_MAC, "Subch_size:%d, numRBS:%d, num_subch:%d\n",
                                          subch_size,num_rbs,num_subch);

  return (subch_size);
}

//This function determines SCI 1A Len in bits based on the configuration in the resource pool.
uint8_t sl_determine_sci_1a_len(uint16_t *num_subchannels,
                                NR_SL_ResourcePool_r16_t *rpool,
                                sidelink_sci_format_1a_fields_t *sci_1a)
{

  uint8_t num_bits = 0;

  //Size of Fixed fields prio (3), sci_2ndstage(2),
  //betaoffsetindicator(2), num dmrs ports (1), mcs (5bits)
  uint8_t sci_1a_len = SL_SCI_FORMAT_1A_LEN_IN_BITS_FIXED_FIELDS;

  *num_subchannels = sl_get_num_subch(rpool);

  uint16_t n_subch = *num_subchannels;

  LOG_D(NR_MAC,"Determine SCI-1A len - Num Subch:%d, sci 1A len fixed fields:%d\n",
                                                           *num_subchannels, sci_1a_len);

  NR_SL_UE_SelectedConfigRP_r16_t *selectedconfigRP = rpool->sl_UE_SelectedConfigRP_r16;
  const uint8_t maxnum_values[] = {2,3};
  uint8_t sl_MaxNumPerReserve =   (selectedconfigRP &&
                                   selectedconfigRP->sl_MaxNumPerReserve_r16)
                                   ? maxnum_values[*selectedconfigRP->sl_MaxNumPerReserve_r16]
                                   : 0;

  //Determine bits for Freq and Time Resource assignment
  if (sl_MaxNumPerReserve == 3) {
    num_bits = ceil(log2(n_subch * (n_subch + 1) * (2*n_subch + 1)/6));
    sci_1a_len += num_bits;
    sci_1a->frequency_resource_assignment.nbits = num_bits;
    sci_1a_len += 9;
    sci_1a->time_resource_assignment.nbits = 9;
  } else {
    num_bits = ceil(log2((n_subch * (n_subch + 1)) >> 1));
    sci_1a_len += num_bits;
    sci_1a->frequency_resource_assignment.nbits = num_bits;
    sci_1a_len += 5;
    sci_1a->time_resource_assignment.nbits = 5;
  }

  LOG_D(NR_MAC,"sci 1A - sl_MaxNumPerReserve:%d, sci 1a len:%d, FRA nbits:%d, TRA nbits:%d\n",
                                                                    sl_MaxNumPerReserve,sci_1a_len,
                                                                    sci_1a->frequency_resource_assignment.nbits,
                                                                    sci_1a->time_resource_assignment.nbits);

  //Determine bits for res reservation period
  uint8_t n_rsvperiod =  (selectedconfigRP &&
                          selectedconfigRP->sl_ResourceReservePeriodList_r16)
                          ? selectedconfigRP->sl_ResourceReservePeriodList_r16->list.count : 0;

  #define SL_IE_ENABLED 0
  if (selectedconfigRP &&
      selectedconfigRP->sl_MultiReserveResource_r16 == SL_IE_ENABLED) {
    num_bits = ceil(log2(n_rsvperiod));
    sci_1a_len += num_bits;
    sci_1a->resource_reservation_period.nbits = num_bits;
  } else
    sci_1a->resource_reservation_period.nbits = 0;

  LOG_D(NR_MAC,"sci 1A - n_rsvperiod:%d, sci 1a len:%d, res reserve period.nbits:%d\n",
                                                      n_rsvperiod, sci_1a_len,
                                                      sci_1a->resource_reservation_period.nbits);


  uint8_t n_dmrspatterns = 0;
  if (rpool->sl_PSSCH_Config_r16 &&
      rpool->sl_PSSCH_Config_r16->present == NR_SetupRelease_SL_PSSCH_Config_r16_PR_setup) {
    NR_SL_PSSCH_Config_r16_t *pssch_cfg = rpool->sl_PSSCH_Config_r16->choice.setup;

    //Determine bits for DMRS PATTERNS
    n_dmrspatterns = (pssch_cfg && pssch_cfg->sl_PSSCH_DMRS_TimePatternList_r16)
                         ? pssch_cfg->sl_PSSCH_DMRS_TimePatternList_r16->list.count : 0;
  }

  AssertFatal((n_dmrspatterns>=1) && (n_dmrspatterns <=3),
                          "Number of DMRS Patterns should be 1or2or3. Resource Pool Configuration Error.\n");

  if (n_dmrspatterns) {
    num_bits = ceil(log2(n_dmrspatterns));
    sci_1a_len += num_bits;
    sci_1a->dmrs_pattern.nbits = num_bits;
  }

  LOG_D(NR_MAC,"sci 1A -  n_dmrspatterns:%d, sci 1a len:%d, dmrs_pattern.nbits:%d\n",
                                                  n_dmrspatterns, sci_1a_len, sci_1a->dmrs_pattern.nbits);

  //Determine bits for Additional MCS table
  if (rpool->sl_Additional_MCS_Table_r16) {
    int numbits = (*rpool->sl_Additional_MCS_Table_r16 > 1) ? 2 : 1;
    sci_1a_len += numbits;
    sci_1a->additional_mcs_table_indicator.nbits = numbits;
    AssertFatal(*rpool->sl_Additional_MCS_Table_r16<=2, "additional table value cannot be > 2. Resource Pool Configuration Error.\n");
  }

  LOG_D(NR_MAC,
        "sci 1A - additional_table:%ld, sci 1a len:%d, additional table nbits:%d\n",
        rpool->sl_Additional_MCS_Table_r16 ? *rpool->sl_Additional_MCS_Table_r16 : 0,
        sci_1a_len,
        sci_1a->additional_mcs_table_indicator.nbits);

  uint8_t psfch_period = 0;
  if (rpool->sl_PSFCH_Config_r16 &&
      rpool->sl_PSFCH_Config_r16->present == NR_SetupRelease_SL_PSFCH_Config_r16_PR_setup) {
    NR_SL_PSFCH_Config_r16_t *psfch_config = rpool->sl_PSFCH_Config_r16->choice.setup;

    //Determine bits for PSFCH overhead indication
    const uint8_t psfch_periods[] = {0,1,2,4};
    psfch_period = (psfch_config->sl_PSFCH_Period_r16)
                          ? psfch_periods[*psfch_config->sl_PSFCH_Period_r16] : 0;
  }

  if ((psfch_period == 2) || (psfch_period == 4)) {
    sci_1a_len += 1;
    sci_1a->psfch_overhead_indication.nbits = 1;
  } else
    sci_1a->psfch_overhead_indication.nbits = 0;

  LOG_D(NR_MAC,"sci 1A - psfch_period:%d, sci 1a len:%d, psfch overhead nbits:%d\n",
                                                            psfch_period, sci_1a_len,
                                                            sci_1a->psfch_overhead_indication.nbits);

  //Determine number of reserved bits
  uint8_t num_reservedbits =  0;
  if (rpool->sl_PSCCH_Config_r16 &&
      rpool->sl_PSCCH_Config_r16->present == NR_SetupRelease_SL_PSCCH_Config_r16_PR_setup) {
    NR_SL_PSCCH_Config_r16_t *pscch_config = rpool->sl_PSCCH_Config_r16->choice.setup;

    num_reservedbits = (pscch_config->sl_NumReservedBits_r16)
                          ? *pscch_config->sl_NumReservedBits_r16 : 0;
  }

  AssertFatal((num_reservedbits >= 2) && (num_reservedbits <= 4),
              "Num Reserved bits can only be 2 or 3 or 4. Resource Pool Configuration Error.\n");
  sci_1a_len += num_reservedbits;
  sci_1a->reserved_bits.nbits = num_reservedbits;
  LOG_D(NR_MAC,
        "sci 1A - reserved_bits:%d, sci 1a len:%d, sci_1a->reserved_bits.nbits:%d\n",
        num_reservedbits,
        sci_1a_len,
        sci_1a->reserved_bits.nbits);

  LOG_D(NR_MAC,"sci 1A Length in bits: %d \n",sci_1a_len);

  return sci_1a_len;
}

// ========================================================================================
// episys SL PSFCH port (Stage 3b): RX-UE HARQ-feedback build. On decoding a PSSCH, the receiver
// computes the PSFCH (PUCCH-format-0) resource + cyclic shift carrying the ACK/NACK and stores it in
// UE_sched_ctrl.sched_psfch[]; the SL PSFCH scheduler (Stage 4) later emits the actual TX_PSFCH.
// DORMANT until PSFCH is provisioned (sl_PSFCH_Config_r16 + sci_pdu_rx.harq_feedback) in Stage 4.
// ========================================================================================

uint8_t count_on_bits(uint8_t *buf, size_t size)
{
  uint8_t count = 0;
  for (size_t i = 0; i < size; i++) {
    uint8_t byte = buf[i];
    while (byte) {
      count += byte & 1;
      byte >>= 1;
    }
  }
  return count;
}

int64_t normalize(frameslot_t *frame_slot, uint8_t mu)
{
  uint8_t slots_per_frame = SL_SLOTS_PER_FRAME(mu);
  return (int64_t)frame_slot->slot + (int64_t)frame_slot->frame * slots_per_frame;
}

void de_normalize(int64_t abs_slot_idx, uint8_t mu, frameslot_t *frame_slot)
{
  uint8_t slots_per_frame = SL_SLOTS_PER_FRAME(mu);
  frame_slot->frame = (abs_slot_idx / slots_per_frame) & 1023;
  frame_slot->slot = (abs_slot_idx % slots_per_frame);
}

void print_prb_set_allocation(psfch_params_t *psfch_params, uint8_t psfch_period, uint8_t num_subchannels)
{
  LOG_D(NR_PHY, "PSSCH Slot mod PSFCH period |   Subchannel   |   Start PRB   |    End PRB\n");
  for (int i = 0; i < psfch_period; i++)
    for (int j = 0; j < num_subchannels; j++)
      LOG_D(NR_PHY, "\t\t    %d \t\t|\t%d\t|\t%d\t| \t %d\n", i, j,
            psfch_params->prbs_sets->start_prb[i][j], psfch_params->prbs_sets->end_prb[i][j]);
}

// TS 38.213 16.3: derive the PSFCH PRB-set allocation + base cyclic shift (m0) for the RX resource pool.
static void compute_params(int module_idP, psfch_params_t *psfch_params)
{
  NR_UE_MAC_INST_t *mac = get_mac_inst(module_idP);
  if (!mac->sl_tx_res_pool->sl_PSFCH_Config_r16 &&
      mac->sl_tx_res_pool->sl_PSFCH_Config_r16->present != NR_SetupRelease_SL_PSFCH_Config_r16_PR_setup)
    return;

  psfch_params->prbs_sets = calloc(1, sizeof(prbs_set_t));
  NR_SL_PSFCH_Config_r16_t *sl_psfch_config = mac->sl_tx_res_pool->sl_PSFCH_Config_r16->choice.setup;
  const int sl_num_muxcs_pair[4] = {1, 2, 3, 6};
  uint8_t sci2_src_id = mac->sci_pdu_rx.source_id;
  uint8_t *rb_buf = sl_psfch_config->sl_PSFCH_RB_Set_r16->buf;
  size_t size = sl_psfch_config->sl_PSFCH_RB_Set_r16->size / sizeof(rb_buf[0]);
  uint8_t m_psfch_prb_set = count_on_bits(rb_buf, size);
  long sl_numsubchannel = *mac->sl_tx_res_pool->sl_NumSubchannel_r16;
  const uint8_t psfch_periods[] = {0, 1, 2, 4};
  long n_psfch_pssch = (sl_psfch_config->sl_PSFCH_Period_r16)
                         ? psfch_periods[*sl_psfch_config->sl_PSFCH_Period_r16] : 0;
  long n_psfch_cs = *sl_psfch_config->sl_NumMuxCS_Pair_r16;

  double m_psfch_subch_slot = m_psfch_prb_set / (sl_numsubchannel * n_psfch_pssch);
  // FIXME: 38.213 16.3 second condition — assumes single subchannel.
  long n_psfch_type = *sl_psfch_config->sl_PSFCH_CandidateResourceType_r16 ? sl_numsubchannel : 1;
  uint16_t r_psfch_prb_cs = n_psfch_type * m_psfch_subch_slot * sl_num_muxcs_pair[n_psfch_cs];
  uint8_t psfch_rsc_idx = (sci2_src_id + mac->src_id) / r_psfch_prb_cs;
  psfch_params->m0 = table_16_3_1[n_psfch_cs][psfch_rsc_idx];

  psfch_params->prbs_sets->start_prb = (uint16_t **)calloc(n_psfch_pssch, sizeof(uint16_t *));
  psfch_params->prbs_sets->end_prb = (uint16_t **)calloc(n_psfch_pssch, sizeof(uint16_t *));
  for (int k = 0; k < n_psfch_pssch; k++) {
    psfch_params->prbs_sets->start_prb[k] = (uint16_t *)calloc(sl_numsubchannel, sizeof(uint16_t));
    psfch_params->prbs_sets->end_prb[k] = (uint16_t *)calloc(sl_numsubchannel, sizeof(uint16_t));
  }
  for (int i = 0; i < n_psfch_pssch; i++) {
    for (int j = 0; j < sl_numsubchannel; j++) {
      psfch_params->prbs_sets->start_prb[i][j] = (i + j * n_psfch_pssch) * m_psfch_subch_slot;
      psfch_params->prbs_sets->end_prb[i][j] = (i + 1 + j * n_psfch_pssch) * m_psfch_subch_slot - 1;
    }
  }
}

// Free everything compute_params() allocated into psfch_params (the nested start_prb/end_prb rows, the
// pointer arrays, prbs_sets, and psfch_params itself). n_psfch_pssch = the PSFCH period value used to
// size the row arrays. Prevents the per-call leak of the prbs_sets allocations.
static void free_psfch_params(psfch_params_t *pp, long n_psfch_pssch)
{
  if (!pp)
    return;
  if (pp->prbs_sets) {
    if (pp->prbs_sets->start_prb) {
      for (long k = 0; k < n_psfch_pssch; k++)
        free(pp->prbs_sets->start_prb[k]);
      free(pp->prbs_sets->start_prb);
    }
    if (pp->prbs_sets->end_prb) {
      for (long k = 0; k < n_psfch_pssch; k++)
        free(pp->prbs_sets->end_prb[k]);
      free(pp->prbs_sets->end_prb);
    }
    free(pp->prbs_sets);
  }
  free(pp);
}

// Is the given absolute slot a sidelink slot? Develop-native: consult sl_slot_bitmap (per-frame bit i set
// when slot i is a SL slot), rather than episys's phy_sl_bitmap (which relied on ulsch_slot_bitmap that
// develop never populates).
bool is_sl_slot(NR_UE_MAC_INST_t *mac, uint64_t abs_slot)
{
  const uint8_t mu = get_softmodem_params()->numerology;
  int slot_in_frame = abs_slot % SL_SLOTS_PER_FRAME(mu);
  return (mac->SL_MAC_PARAMS->sl_slot_bitmap >> slot_in_frame) & 1;
}

// TS 38.213 16.3: does the given (absolute) slot carry PSFCH resources for this period?
bool slot_has_psfch(NR_UE_MAC_INST_t *mac,
                    uint64_t abs_index_cur_slot,
                    uint8_t psfch_period,
                    NR_TDD_UL_DL_ConfigCommon_t *tdd)
{
  if (psfch_period == 0)
    return false;
  const uint8_t mu = get_softmodem_params()->numerology;
  frameslot_t fs0;
  de_normalize(abs_index_cur_slot, mu, &fs0);
  const int nr_slots_frame = SL_SLOTS_PER_FRAME(mu);
  const int nr_slots_period =
      tdd ? nr_slots_frame / get_nb_periods_per_frame(tdd->pattern1.dl_UL_TransmissionPeriodicity) : nr_slots_frame;
  // (episys assumed UL=4/DL=6; this is diagnostic-only here — warn instead of aborting on other TDDs.)
  if (tdd && (tdd->pattern1.nrofUplinkSlots != 4 || tdd->pattern1.nrofDownlinkSlots != 6))
    LOG_D(NR_MAC, "slot_has_psfch: TDD UL=%ld DL=%ld (episys reference assumed 4/6)\n",
          tdd->pattern1.nrofUplinkSlots, tdd->pattern1.nrofDownlinkSlots);
  bool sl_slot = is_sl_slot(mac, abs_index_cur_slot);
  int slot_in_period = fs0.slot % nr_slots_period;
  int first_ul_slot =
      tdd ? sl_first_ul_slot(tdd->pattern1.nrofDownlinkSlots, tdd->pattern1.nrofDownlinkSymbols, tdd->pattern1.nrofUplinkSymbols) : 0;
  int psfch_slot_offset = (first_ul_slot + psfch_period - 1) % nr_slots_period;
  bool has_psfch = sl_slot && (slot_in_period == psfch_slot_offset);
  LOG_D(NR_MAC, "slot %d (in_period %d) has_psfch %d, psfch_offset %d, first_ul %d, abs slot %ld\n",
        fs0.slot, slot_in_period, has_psfch, psfch_slot_offset, first_ul_slot, (long)abs_index_cur_slot);
  return has_psfch;
}

// Index into the per-period PSFCH scheduling buffer for a given (frame,slot) UL slot.
int get_psfch_index(int frame, int slot, int n_slots_frame, const NR_TDD_UL_DL_Pattern_t *tdd, int sched_psfch_max_size)
{
  const int first_ul_slot_period =
      tdd ? sl_first_ul_slot(tdd->nrofDownlinkSlots, tdd->nrofDownlinkSymbols, tdd->nrofUplinkSymbols) : 0;
  const int n_ul_slots_period = tdd ? tdd->nrofUplinkSlots + (tdd->nrofUplinkSymbols > 0 ? 1 : 0) : n_slots_frame;
  const int nr_slots_period = tdd ? n_slots_frame / get_nb_periods_per_frame(tdd->dl_UL_TransmissionPeriodicity) : n_slots_frame;
  const int n_ul_slots_frame = n_slots_frame / nr_slots_period * n_ul_slots_period;
  const int frame_start = frame * n_ul_slots_frame;
  const int ul_period_start = (slot / nr_slots_period) * n_ul_slots_period;
  const int ul_period_slot = (slot % nr_slots_period) - first_ul_slot_period;
  return (frame_start + ul_period_start + ul_period_slot) % sched_psfch_max_size;
}

int nr_ue_sl_acknack_scheduling(NR_UE_MAC_INST_t *mac,
                                sl_nr_rx_indication_t *rx_ind,
                                long psfch_period,
                                uint16_t frame,
                                uint16_t slot,
                                const int nr_slots_frame)
{
  int psfch_frame, psfch_slot;
  sl_nr_ue_mac_params_t *sl_mac = mac->SL_MAC_PARAMS;
  NR_TDD_UL_DL_Pattern_t *tdd = &sl_mac->sl_TDD_config->pattern1;
  const int n_ul_slots_period = tdd ? tdd->nrofUplinkSlots + (tdd->nrofUplinkSymbols > 0 ? 1 : 0) : nr_slots_frame;

  uint16_t num_subch = sl_get_num_subch(mac->sl_tx_res_pool);
  int n_ul_buf_max_size = n_ul_slots_period * num_subch;

  // Derive the PSFCH feedback slot from the OVER-THE-AIR PSSCH slot (rx_ind->slot) so the receiver's
  // ACK/NACK lands exactly where the PSSCH transmitter listens (which also uses get_feedback_slot on its
  // own tx_slot == this rx_ind->slot). Ignore the DURATION-shifted `slot`/`frame` params for this.
  (void)slot;
  (void)frame;
  // Receiver sends PSFCH in ITS OWN TX half (SyncRef -> {6-9}, Nearby -> {16-19}).
  bool use_first_half = get_softmodem_params()->sync_ref;
  psfch_slot = get_feedback_slot(psfch_period, rx_ind->slot, use_first_half);
  const int psfch_index = get_psfch_index(rx_ind->sfn, rx_ind->slot, nr_slots_frame, tdd, n_ul_buf_max_size);
  NR_SL_UE_sched_ctrl_t *sched_ctrl = &mac->sl_info.list[0]->UE_sched_ctrl;
  SL_sched_feedback_t *curr_psfch = &sched_ctrl->sched_psfch[psfch_index];
  psfch_frame = rx_ind->sfn;
  if (psfch_slot >= 0 && psfch_slot < rx_ind->slot)
    psfch_frame = (psfch_frame + 1) & 1023; // feedback wraps into the next frame
  frameslot_t fs = {.frame = psfch_frame, .slot = psfch_slot};
  uint8_t pool_id = 0;
  uint64_t tx_abs_slot = normalize(&fs, get_softmodem_params()->numerology);
  (void)pool_id;
  bool sl_has_psfch = slot_has_psfch(mac, tx_abs_slot, psfch_period, mac->SL_MAC_PARAMS->sl_TDD_config);
  LOG_D(NR_MAC, "%s %4d.%2d sl_has_psfch %d\n", __FUNCTION__, psfch_frame, psfch_slot, sl_has_psfch);
  curr_psfch->feedback_frame = psfch_frame;
  curr_psfch->feedback_slot = psfch_slot;
  curr_psfch->dai_c = psfch_index;
  LOG_D(NR_MAC, "Rx SLSCH %4d.%2d, SL_ACK %4d.%2d PSFCH: idx %d, n_ul_slots_period %d dai_c %u\n",
        rx_ind->sfn, rx_ind->slot, psfch_frame, psfch_slot, psfch_index, n_ul_slots_period, curr_psfch->dai_c);
  return psfch_index;
}

void fill_psfch_params_tx(NR_UE_MAC_INST_t *mac,
                          sl_nr_rx_indication_t *rx_ind,
                          long psfch_period,
                          uint16_t frame,
                          uint16_t slot,
                          uint8_t ack_nack,
                          psfch_params_t *psfch_params,
                          const int nr_slots_frame,
                          int psfch_index)
{
  // develop stores the SL BWP-generic config in SL_MAC_PARAMS->sl_bwp_generic (mac->sl_bwp is unused/NULL).
  NR_SL_BWP_Generic_r16_t *sl_bwp_generic = (NR_SL_BWP_Generic_r16_t *)mac->SL_MAC_PARAMS->sl_bwp_generic;
  SL_sched_feedback_t *sched_psfch = &mac->sl_info.list[0]->UE_sched_ctrl.sched_psfch[psfch_index];
  sched_psfch->initial_cyclic_shift = psfch_params->m0;
  if ((mac->sci1_pdu.second_stage_sci_format == 0 && (mac->sci_pdu_rx.cast_type == 1 || mac->sci_pdu_rx.cast_type == 2))
      || mac->sci1_pdu.second_stage_sci_format == 2) {
    sched_psfch->mcs = sequence_cyclic_shift_harq_ack_or_ack_or_only_nack[ack_nack];
    sched_psfch->bit_len_harq = 1;
  } else if (mac->sci1_pdu.second_stage_sci_format == 1
             || (mac->sci1_pdu.second_stage_sci_format == 0 && mac->sci_pdu_rx.cast_type == 3)) {
    sched_psfch->mcs = sequence_cyclic_shift_harq_ack_or_ack_or_only_nack[0];
    sched_psfch->bit_len_harq = 0;
  }
  const uint8_t values[] = {7, 8, 9, 10, 11, 12, 13, 14};
  uint8_t sl_num_symbols = *sl_bwp_generic->sl_LengthSymbols_r16 ? values[*sl_bwp_generic->sl_LengthSymbols_r16] : 0;
  // start_symbol_index used as lprime check (38.213 16.3).
  sched_psfch->start_symbol_index = *sl_bwp_generic->sl_StartSymbol_r16 + sl_num_symbols - 2;
  // Hopping id from the provisioned TX pool's PSFCH config (develop-native; mac->sl_bwp chains are NULL).
  sched_psfch->hopping_id = *mac->sl_tx_res_pool->sl_PSFCH_Config_r16->choice.setup->sl_PSFCH_HopID_r16;

  // FIXME [0] assumes number of subchannels = 1 (channel id 0).
  sched_psfch->prb = psfch_params->prbs_sets->start_prb[rx_ind->slot % psfch_period][0];
  print_prb_set_allocation(psfch_params, psfch_period, 1);
  int locbw = sl_bwp_generic->sl_BWP_r16->locationAndBandwidth;
  sched_psfch->sl_bwp_start = NRRIV2PRBOFFSET(locbw, MAX_BWP_SIZE);
  sched_psfch->freq_hop_flag = 0;
  sched_psfch->group_hop_flag = 0;
  sched_psfch->second_hop_prb = 0;
  sched_psfch->sequence_hop_flag = 0;
  sched_psfch->harq_feedback = mac->sci_pdu_rx.harq_feedback;
  LOG_D(NR_MAC, "Filled psfch pdu\n");
}

// Entry: on a received SLSCH, build the PSFCH (ACK/NACK) feedback resource into sched_psfch[].
void configure_psfch_params_tx(int module_idP, NR_UE_MAC_INST_t *mac, sl_nr_rx_indication_t *rx_ind, int pdu_id)
{
  const uint8_t psfch_periods[] = {0, 1, 2, 4};
  NR_SL_PSFCH_Config_r16_t *sl_psfch_config = mac->sl_tx_res_pool->sl_PSFCH_Config_r16->choice.setup;
  long psfch_period = (sl_psfch_config->sl_PSFCH_Period_r16) ? psfch_periods[*sl_psfch_config->sl_PSFCH_Period_r16] : 0;

  int scs = get_softmodem_params()->numerology;
  uint16_t tx_slot = (rx_ind->slot + DURATION_RX_TO_TX) % SL_SLOTS_PER_FRAME(scs);
  uint16_t tx_frame = (rx_ind->sfn + (rx_ind->slot + DURATION_RX_TO_TX) / SL_SLOTS_PER_FRAME(scs)) % 1024;

  uint8_t ack_nack = (rx_ind->rx_indication_body + pdu_id)->rx_slsch_pdu.ack_nack;
  psfch_params_t *psfch_params = calloc(1, sizeof(psfch_params_t));
  compute_params(module_idP, psfch_params);
  const int nr_slots_frame = SL_SLOTS_PER_FRAME(scs);
  int psfch_index = nr_ue_sl_acknack_scheduling(mac, rx_ind, psfch_period, tx_frame, tx_slot, nr_slots_frame);
  if (psfch_index != -1)
    fill_psfch_params_tx(mac, rx_ind, psfch_period, tx_frame, tx_slot, ack_nack, psfch_params, nr_slots_frame, psfch_index);
  free_psfch_params(psfch_params, psfch_period);
}

// episys SL PSFCH port (Stage 4d): build the PSFCH RX decode config for this slot. For each HARQ process
// awaiting feedback at (frame,slot), compute the SAME PSFCH resource the peer transmits (m0 from
// sci_pdu_rx.source_id+mac->src_id — commutative, so both sides agree) and fill an RX psfch_pdu. Returns
// the number scheduled (0 = nothing to receive). The PHY decodes via nr_ue_decode_psfch0 (Stage 2).
int configure_psfch_params_rx(int module_idP, NR_UE_MAC_INST_t *mac, int frame, int slot, sl_nr_rx_config_request_t *rx_config)
{
  if (!mac->sl_info.list[0] || !mac->sl_tx_res_pool || !mac->sl_tx_res_pool->sl_PSFCH_Config_r16
      || !mac->SL_MAC_PARAMS || !mac->SL_MAC_PARAMS->sl_bwp_generic)
    return 0;
  NR_SL_UE_sched_ctrl_t *sc = &mac->sl_info.list[0]->UE_sched_ctrl;
  const uint8_t psfch_periods[] = {0, 1, 2, 4};
  long psfch_period = psfch_periods[*mac->sl_tx_res_pool->sl_PSFCH_Config_r16->choice.setup->sl_PSFCH_Period_r16];
  if (psfch_period == 0)
    return 0;
  NR_SL_BWP_Generic_r16_t *sl_bwp_generic = (NR_SL_BWP_Generic_r16_t *)mac->SL_MAC_PARAMS->sl_bwp_generic;
  const uint8_t values[] = {7, 8, 9, 10, 11, 12, 13, 14};
  uint8_t sl_num_symbols = *sl_bwp_generic->sl_LengthSymbols_r16 ? values[*sl_bwp_generic->sl_LengthSymbols_r16] : 0;
  int locbw = sl_bwp_generic->sl_BWP_r16->locationAndBandwidth;
  sl_nr_tx_rx_config_psfch_pdu_t *list = NULL;
  int k = 0;
  for (int pid = 0; pid < NR_MAX_HARQ_PROCESSES; pid++) {
    NR_UE_sl_harq_t *h = &sc->sl_harq_processes[pid];
    if (!h->is_waiting || h->feedback_frame != frame || h->feedback_slot != slot)
      continue;
    psfch_params_t *pp = calloc(1, sizeof(*pp));
    compute_params(module_idP, pp);
    if (!list)
      list = calloc(NR_MAX_HARQ_PROCESSES, sizeof(*list));
    sl_nr_tx_rx_config_psfch_pdu_t *pdu = &list[k];
    pdu->initial_cyclic_shift = pp->m0;
    pdu->start_symbol_index = *sl_bwp_generic->sl_StartSymbol_r16 + sl_num_symbols - 2;
    pdu->hopping_id = *mac->sl_tx_res_pool->sl_PSFCH_Config_r16->choice.setup->sl_PSFCH_HopID_r16;
    int index = (psfch_period > 0) ? (h->sched_pssch.slot % psfch_period) : 0;
    if (pp->prbs_sets && pp->prbs_sets->start_prb)
      pdu->prb = pp->prbs_sets->start_prb[index][0];
    pdu->sl_bwp_start = NRRIV2PRBOFFSET(locbw, MAX_BWP_SIZE);
    pdu->freq_hop_flag = 0;
    pdu->group_hop_flag = 0;
    pdu->second_hop_prb = 0;
    pdu->sequence_hop_flag = 0;
    pdu->bit_len_harq = 1;
    pdu->nr_of_symbols = 1; // period 1: 3 PSFCH symbols - 2 (AGC+guard)
    LOG_D(NR_MAC, "SL PSFCH RX %d.%d pid=%d ics=%d prb=%d ssym=%d hop=%d nsym=%d psschslot=%d\n",
          frame, slot, pid, pdu->initial_cyclic_shift, pdu->prb, pdu->start_symbol_index, pdu->hopping_id,
          pdu->nr_of_symbols, h->sched_pssch.slot);
    k++;
    free_psfch_params(pp, psfch_period); // free prbs_sets nested arrays too (no leak)
  }
  if (k > 0) {
    // Attach to the (blind) PSSCH RX config already built for this slot and upgrade the type so the PHY
    // does PSSCH-blind-RX + PSFCH decode together. If NO PSSCH config was built (PSFCH-only feedback slot),
    // ZERO the SCI/PSSCH configs so pssch_numsym=0 and the PHY does NOT attempt a PSSCH decode with garbage
    // params (would drive nr_slot_fep past the slot -> assert).
    if (rx_config->number_pdus == 0) {
      // The list holds ONE PDU now that the receive path is staged, so there is no list[1] to clear.
      memset(&rx_config->sl_rx_config_list[0], 0, sizeof(rx_config->sl_rx_config_list[0]));
      rx_config->number_pdus = 1;
    }
    rx_config->sl_rx_config_list[0].pdu_type = SL_NR_CONFIG_TYPE_RX_PSSCH_SLSCH_PSFCH;
    rx_config->sl_rx_config_list[0].psfch_pdu_list = (struct sl_nr_tx_rx_config_psfch_pdu *)list;
    rx_config->sl_rx_config_list[0].num_psfch_pdus = k;
    LOG_D(NR_MAC, "%d.%d CMD to PHY: RX PSFCH %d pdu(s)\n", frame, slot, k);
  }
  return k;
}

// episys SL PSFCH port (Stage 3c): collect the TX-side HARQ processes awaiting feedback in (frame,slot).
int find_current_slot_harqs(frame_t frame, sub_frame_t slot, NR_SL_UE_sched_ctrl_t *sched_ctrl, NR_UE_sl_harq_t **matched_harqs)
{
  int cur = sched_ctrl->feedback_sl_harq.head;
  int k = 0;
  while (cur != -1) {
    NR_UE_sl_harq_t *harq = &sched_ctrl->sl_harq_processes[cur];
    if (harq->feedback_frame == frame && harq->feedback_slot == slot) {
      if (matched_harqs)
        matched_harqs[k] = harq;
      k++;
    }
    cur = sched_ctrl->feedback_sl_harq.next[cur];
  }
  return k;
}
