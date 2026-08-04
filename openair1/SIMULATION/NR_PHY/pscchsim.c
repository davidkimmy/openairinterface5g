/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/* Sidelink PSCCH (SCI-1A) transmit->receive round trip, with no radio, no scheduler and no ping.
 *
 * Why this exists: the PSCCH receiver was previously validated only through end-to-end ping tests, and every
 * defect that shipped that way cost a build+test cycle. All of them are visible here in a second.
 *
 * The harness feeds nr_generate_sci1()'s own output straight back into nr_rx_pscch() and checks:
 *   1. CRC passes and the decoded payload is bit-exact;
 *   2. the Nid the receiver derives equals the one the transmitter returned (38.211 8.3.1.1);
 *   3. an all-zero slot and a noise-only slot are both REJECTED (there is no presence gate: the CRC and the
 *      false-detection check do that work);
 *   4. behaviour versus SNR, i.e. where SCI-1A stops working on this link.
 *
 *   ./nr_pscchsim              # clean + all-zero + noise-only + AWGN sweep
 *   ./nr_pscchsim -s 10        # single SNR point
 *   ./nr_pscchsim -n 200       # trials per point
 */

#include <string.h>
#include <math.h>
#include <unistd.h>
#include "common/config/config_userapi.h"
#include "common/ran_context.h"
#include "PHY/types.h"
#include "PHY/defs_nr_common.h"
#include "PHY/defs_nr_UE.h"
#include "PHY/defs_gNB.h"
#include "PHY/phy_vars.h"
#include "PHY/INIT/phy_init.h"
#include "openair1/SIMULATION/TOOLS/sim.h"
#include "common/utils/nr/nr_common.h"
#include "openair1/SCHED_NR_UE/defs.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h"
#include "NR_MasterInformationBlockSidelink.h"
#include "executables/softmodem-common.h"
#include "executables/nr-uesoftmodem.h"
#include "openair2/COMMON/e1ap_messages_types.h"
#include "PHY/NR_REFSIG/nr_refsig.h"

/* Globals and link stubs. Mirrors psbchsim.c: the PHY/MAC/SCHED library group referenced below pulls in
 * symbols this bench never exercises, so they are satisfied here rather than by linking the softmodem. */
double cpuf;
uint64_t downlink_frequency[MAX_NUM_CCs][4];
int64_t uplink_frequency_offset[MAX_NUM_CCs][4];
THREAD_STRUCT thread_struct;
openair0_config_t openair0_cfg[MAX_CARDS];
RAN_CONTEXT_t RC;
char *uecap_file;
int NB_UE_INST = 1;   // matches the extern in nr-uesoftmodem.h
configmodule_interface_t *uniqCfg = NULL;
nrUE_params_t nrUE_params = {0};

nrUE_params_t *get_nrUE_params(void)
{
  return &nrUE_params;
}
int8_t nr_rrc_RA_succeeded(const module_id_t mod_id, const uint8_t gNB_index)
{
  return 1;
}
void nr_rrc_ue_generate_RRCSetupRequest(module_id_t module_id, const uint8_t gNB_index)
{
}
int8_t nr_mac_rrc_data_req_ue(const module_id_t Mod_idP,
                              const int CC_id,
                              const uint8_t gNB_id,
                              const frame_t frameP,
                              const rb_id_t Srb_id,
                              uint8_t *buffer_pP)
{
  return 0;
}
uint8_t check_if_ue_is_sl_syncsource(void)
{
  return 0;
}
void nr_rrc_mac_config_req_sl_mib(module_id_t module_id,
                                  NR_SL_SSB_TimeAllocation_r16_t *ssb_ta,
                                  uint16_t rx_slss_id,
                                  uint8_t *sl_mib)
{
}
void get_num_re_dmrs(nfapi_nr_ue_pusch_pdu_t *pusch_pdu, uint8_t *nb_dmrs_re_per_rb, uint16_t *number_dmrs_symbols)
{
}
void e1_bearer_release_cmd(const e1ap_bearer_release_cmd_t *cmd)
{
  abort();
}
/* AssertFatal/exit_fun route here; the other NR sims get it from the softmodem they link. This bench links
 * only the PHY/MAC libraries, so provide it directly. */
void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  if (s != NULL)
    printf("%s:%d %s() Exiting: %s\n", file, line, function, s);
  exit(assert ? EXIT_FAILURE : EXIT_SUCCESS);
}

// Geometry under test: matches sl_ue1.conf / sl_sync_ref.conf as deployed (100-RB subchannel, PSCCH 12 RBs
// x 2 symbols). Kept here as named constants so a config change shows up as a harness edit, not a surprise.
#define TEST_N_RB          106
#define TEST_PSCCH_NUMRBS  12
#define TEST_PSCCH_NUMSYM  2
#define TEST_PSCCH_STARTRB 0
#define TEST_DMRS_ID       0
#define TEST_SCI1_LEN      22   // sci_1a_len reported by both nodes in the field logs
#define TEST_SLOT          6

typedef struct {
  int trials, decoded, fp_rejected, crc_failed, nid_mismatch, payload_mismatch;
} result_t;

/* One TX->RX round trip. with_signal = false leaves the grid empty (all-zero, or noise only if add_noise),
 * which is what the receiver sees on the great majority of sidelink RX slots. */
static void run_trial(PHY_VARS_NR_UE *ue,
                      NR_DL_FRAME_PARMS *fp,
                      double snr_dB,
                      bool add_noise,
                      bool with_signal,
                      result_t *res)
{
  const int samples_per_slot_wCP = fp->symbols_per_slot * fp->ofdm_symbol_size;
  c16_t txdataF[samples_per_slot_wCP];
  memset(txdataF, 0, sizeof(txdataF));

  // ---- TX: build the SCI-1A payload and let the transmitter map it onto the grid
  sl_nr_tx_config_pscch_pssch_pdu_t tx_pdu = {0};
  tx_pdu.startrb = TEST_PSCCH_STARTRB;
  tx_pdu.pscch_numrbs = TEST_PSCCH_NUMRBS;
  tx_pdu.pscch_numsym = TEST_PSCCH_NUMSYM;
  tx_pdu.pscch_dmrs_scrambling_id = TEST_DMRS_ID;
  tx_pdu.pscch_sci_payload_len = TEST_SCI1_LEN;
  uint64_t payload_in = 0;
  for (int i = 0; i < TEST_SCI1_LEN; i++)
    if (taus() & 1)
      payload_in |= (1ULL << i);
  memcpy(tx_pdu.pscch_sci_payload, &payload_in, sizeof(uint64_t));

  const uint32_t tx_crc = nr_generate_sci1(ue, txdataF, fp, AMP, TEST_SLOT, &tx_pdu);
  const uint16_t tx_Nid = (uint16_t)(tx_crc & 0xFFFF);

  // ---- channel: ideal, AWGN, or silence
  c16_t rxdataF[1][samples_per_slot_wCP];
  if (with_signal)
    memcpy(rxdataF[0], txdataF, sizeof(txdataF));
  else
    memset(rxdataF[0], 0, sizeof(rxdataF[0])); // a silent slot in rfsim/vrtsim is EXACTLY zero
  if (add_noise) {
    // Per-RE complex AWGN. AMP is the transmitted DMRS/data amplitude, so sigma is set relative to it.
    const double sigma = (double)AMP / pow(10.0, snr_dB / 20.0);
    for (int i = 0; i < samples_per_slot_wCP; i++) {
      rxdataF[0][i].r = (int16_t)(rxdataF[0][i].r + sigma * gaussdouble(0.0, 1.0));
      rxdataF[0][i].i = (int16_t)(rxdataF[0][i].i + sigma * gaussdouble(0.0, 1.0));
    }
  }

  // ---- RX
  sl_nr_rx_config_pscch_pdu_t rx_pdu = {0};
  rx_pdu.pscch_startrb = TEST_PSCCH_STARTRB;
  rx_pdu.pscch_numrbs = TEST_PSCCH_NUMRBS;
  rx_pdu.pscch_numsym = TEST_PSCCH_NUMSYM;
  rx_pdu.pscch_dmrs_scrambling_id = TEST_DMRS_ID;
  rx_pdu.sci_1a_length = TEST_SCI1_LEN;
  rx_pdu.num_subch = 1;
  rx_pdu.subchannel_size = 100;
  rx_pdu.l_subch = 1;

  UE_nr_rxtx_proc_t proc = {0};
  proc.frame_rx = 0;
  proc.nr_slot_rx = TEST_SLOT;

  // rx_errors counts CRC-consistent decodes rejected by the false-detection check, not CRC mismatches.
  const uint32_t err_before = ue->SL_UE_PHY_PARAMS.pscch.rx_errors;
  uint64_t payload_out = 0;
  uint16_t rx_Nid = 0;
  int16_t rx_rsrp_dBm = 0;
  const int rc =
      nr_rx_pscch(ue, &proc, fp, &rx_pdu, samples_per_slot_wCP, rxdataF, &payload_out, &rx_Nid, &rx_rsrp_dBm);
  const bool counted_false_positive = ue->SL_UE_PHY_PARAMS.pscch.rx_errors > err_before;

  res->trials++;
  if (rc == 0) {
    res->decoded++;
    // Sensing feeds on this: a decode that reports no RSRP would make every sensed resource look equal.
    LOG_D(PHY, "pscchsim: decoded, Nid %u, pscch_rsrp %d dBm\n", rx_Nid, rx_rsrp_dBm);
    // Nid is what the whole port exists for: PSSCH DMRS + SLSCH scrambling key it (38.211 8.3.1.1).
    if (rx_Nid != tx_Nid)
      res->nid_mismatch++;
    const uint64_t mask = (TEST_SCI1_LEN >= 64) ? ~0ULL : ((1ULL << TEST_SCI1_LEN) - 1);
    if ((payload_out & mask) != (payload_in & mask))
      res->payload_mismatch++;
  } else if (counted_false_positive) {
    res->fp_rejected++; // CRC passed, then the false-detection check rejected it
  } else {
    res->crc_failed++; // no CRC-consistent codeword: nothing here
  }
}

static void report(const char *label, const result_t *r)
{
  printf("  %-22s trials %4d | decoded %4d | CRC-fail %4d | false-pos rejected %4d | Nid-mismatch %d | payload-mismatch %d\n",
         label, r->trials, r->decoded, r->crc_failed, r->fp_rejected, r->nid_mismatch, r->payload_mismatch);
}

int main(int argc, char **argv)
{
  int n_trials = 100;
  double single_snr = NAN;
  int loglvl = OAILOG_INFO;

  if ((uniqCfg = load_configmodule(argc, argv, CONFIG_ENABLECMDLINEONLY)) == 0)
    exit_fun("[NR_PSCCHSIM] Error, configuration module init failed\n");
  randominit();
  logInit();

  int c;
  while ((c = getopt(argc, argv, "--:O:hn:s:L:")) != -1) {
    if (c == 1 || c == '-' || c == 'O')
      continue;
    switch (c) {
      case 'n': n_trials = atoi(optarg); break;
      case 's': single_snr = atof(optarg); break;
      case 'L': loglvl = atoi(optarg); break;
      default:
        printf("Usage: %s [-n trials] [-s snr_dB] [-L loglevel]\n", argv[0]);
        printf("  PSCCH (SCI-1A) TX->RX round trip: CRC, Nid derivation, and false-detection behaviour.\n");
        exit(1);
    }
  }
  set_glog(loglvl);

  /* The QPSK/QAM constellation tables are generated at runtime (nr_gen_mod_table.c); the softmodem
   * does this in PHY init. Without it nr_modulation() emits zeros and the transmitter is silent. */
  nr_generate_modulation_table();

  // Minimal UE: nr_generate_sci1 and nr_rx_pscch only read frame_parms (plus the SL stats counters),
  // so the full init_nr_ue_signal() path is deliberately not used - it would drag in the whole PHY.
  PHY_VARS_NR_UE *ue = calloc(1, sizeof(PHY_VARS_NR_UE));
  AssertFatal(ue != NULL, "out of memory\n");
  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  fp->N_RB_DL = TEST_N_RB;
  fp->N_RB_UL = TEST_N_RB;
  fp->ofdm_symbol_size = 1536;                 // 106 PRB @ 30 kHz
  fp->first_carrier_offset = fp->ofdm_symbol_size - (TEST_N_RB * NR_NB_SC_PER_RB) / 2;
  fp->symbols_per_slot = 14;
  fp->slots_per_frame = 20;
  fp->nb_antennas_rx = 1;
  fp->nb_antennas_tx = 1;
  fp->samples_per_slot_wCP = fp->symbols_per_slot * fp->ofdm_symbol_size;
  fp->subcarrier_spacing = 30000;
  fp->Ncp = 0; // normal CP

  printf("\nPSCCH (SCI-1A) round trip: %d RB grid, PSCCH %d RB x %d sym from RB %d, SCI-1A %d bits, DMRS id %d\n",
         TEST_N_RB, TEST_PSCCH_NUMRBS, TEST_PSCCH_NUMSYM, TEST_PSCCH_STARTRB, TEST_SCI1_LEN, TEST_DMRS_ID);
  printf("Expected coded length G = %d REs x 2 bits = %d\n\n",
         TEST_PSCCH_NUMRBS * TEST_PSCCH_NUMSYM * 9, TEST_PSCCH_NUMRBS * TEST_PSCCH_NUMSYM * 18);

  int failures = 0;

  // 1. Noiseless round trip. Anything other than 100% decoded is a construction bug, not a channel effect.
  result_t clean = {0};
  for (int i = 0; i < n_trials; i++)
    run_trial(ue, fp, 0, false, true, &clean);
  report("clean (no noise)", &clean);
  if (clean.decoded != clean.trials) {
    printf("    FAIL: a noiseless round trip must decode every time\n");
    failures++;
  }
  if (clean.nid_mismatch || clean.payload_mismatch) {
    printf("    FAIL: decoded but Nid or payload differs from the transmitter\n");
    failures++;
  }

  // 2. Silence, exactly zero as in rfsim/vrtsim. Nothing may be reported as decoded: a single accept here
  //    means every empty slot fabricates an SCI-1A, which then programs the PSSCH stages with garbage.
  result_t silent = {0};
  for (int i = 0; i < n_trials; i++)
    run_trial(ue, fp, 0, false, false, &silent);
  report("all-zero (silence)", &silent);
  if (silent.decoded != 0) {
    printf("    FAIL: a silent slot must never decode (%d of %d did)\n", silent.decoded, silent.trials);
    failures++;
  }

  // 3. Noise only, no transmission - the over-the-air version of an empty slot, and the case a bare
  //    "crc == 0" accept gets wrong: the list polar decoder searches for a CRC-consistent path and finds
  //    one often enough to matter. This is the false-detection check's reason for existing.
  result_t noise_only = {0};
  for (int i = 0; i < n_trials; i++)
    run_trial(ue, fp, 0 /* sigma == AMP */, true, false, &noise_only);
  report("noise only (no TX)", &noise_only);
  if (noise_only.decoded != 0) {
    printf("    FAIL: a noise-only slot must never decode (%d of %d did)\n", noise_only.decoded, noise_only.trials);
    failures++;
  }

  // 4. SNR sweep. The decode curve tells us where SCI-1A actually stops working, which is the number to
  //    compare against the field logs.
  if (!isnan(single_snr)) {
    result_t r = {0};
    for (int i = 0; i < n_trials; i++)
      run_trial(ue, fp, single_snr, true, true, &r);
    char label[32];
    snprintf(label, sizeof(label), "AWGN %.0f dB", single_snr);
    report(label, &r);
  } else {
    printf("\n  SNR sweep (a real transmission must decode down to a sensible SNR):\n");
    for (double snr = -6; snr <= 20; snr += 2) {
      result_t r = {0};
      for (int i = 0; i < n_trials; i++)
        run_trial(ue, fp, snr, true, true, &r);
      char label[32];
      snprintf(label, sizeof(label), "AWGN %+.0f dB", snr);
      report(label, &r);
      // Assert on the sweep, not only the corner cases: a threshold that silently rejects valid
      // transmissions shows up nowhere else than in the decode rate.
      if (snr >= 6 && r.decoded < (r.trials * 95) / 100) {
        printf("    FAIL: only %d of %d decoded at %+.0f dB - a real transmission is being rejected\n",
               r.decoded, r.trials, snr);
        failures++;
      }
    }
  }

  printf("\n%s\n\n", failures ? "RESULT: FAIL" : "RESULT: PASS");
  free(ue);
  return failures ? 1 : 0;
}
