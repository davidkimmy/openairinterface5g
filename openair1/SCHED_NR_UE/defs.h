/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*
 * \brief NR UE PHY functions prototypes
 */

#ifndef __openair_SCHED_H__
#define __openair_SCHED_H__

#include "PHY/defs_nr_UE.h"


/*enum THREAD_INDEX { OPENAIR_THREAD_INDEX = 0,
                    TOP_LEVEL_SCHEDULER_THREAD_INDEX,
                    DLC_SCHED_THREAD_INDEX,
                    openair_SCHED_NB_THREADS
                  };*/ // do not modify this line

#define OPENAIR_THREAD_STACK_SIZE     PTHREAD_STACK_MIN //4096 //RTL_PTHREAD_STACK_MIN*6
//#define DLC_THREAD_STACK_SIZE        4096 //DLC stack size
//#define UE_SLOT_PARALLELISATION
//#define UE_DLSCH_PARALLELISATION

/*enum openair_SCHED_STATUS {
  openair_SCHED_STOPPED=1,
  openair_SCHED_STARTING,
  openair_SCHED_STARTED,
  openair_SCHED_STOPPING
};*/

/*enum openair_ERROR {
  // HARDWARE CAUSES
  openair_ERROR_HARDWARE_CLOCK_STOPPED= 1,

  // SCHEDULER CAUSE
  openair_ERROR_OPENAIR_RUNNING_LATE,
  openair_ERROR_OPENAIR_SCHEDULING_FAILED,

  // OTHERS
  openair_ERROR_OPENAIR_TIMING_OFFSET_OUT_OF_BOUNDS,
};*/

/*enum openair_SYNCH_STATUS {
  openair_NOT_SYNCHED=1,
  openair_SYNCHED,
  openair_SCHED_EXIT
};*/

/*enum openair_HARQ_TYPE {
  openair_harq_DL = 0,
  openair_harq_UL,
  openair_harq_RA
};*/

/** @addtogroup _PHY_PROCEDURES_
 * @{
 */

/*! \brief Scheduling for UE TX procedures in normal subframes.
  @param ue Pointer to UE variables on which to act
  @param proc Pointer to RXn-TXnp4 proc information
@param phy_data
*/
void phy_procedures_nrUE_TX(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc, nr_phy_data_tx_t *phy_data, c16_t **txp);

int pbch_processing(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc, nr_phy_data_t *phy_data);
void pdcch_processing(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc, nr_phy_data_t *phy_data);

void pdsch_processing(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc, nr_phy_data_t *phy_data);

void processSlotTX(void *arg);

/*! \brief UL time alignment procedures for TA application
  @param ue
  @param slot_tx
  @param frame_tx
*/
void ue_ta_procedures(PHY_VARS_NR_UE *ue, int slot_tx, int frame_tx);

void set_tx_harq_id(NR_UE_ULSCH_t *ulsch, int harq_pid, int slot_tx);
int get_tx_harq_id(NR_UE_ULSCH_t *ulsch, int slot_tx);

int is_pbch_in_slot(fapi_nr_config_request_t *config, int frame, int slot, NR_DL_FRAME_PARMS *fp);
int is_ssb_in_slot(fapi_nr_config_request_t *config, int frame, int slot, NR_DL_FRAME_PARMS *fp);
bool is_csi_rs_in_symbol(fapi_nr_dl_config_csirs_pdu_rel15_t csirs_config_pdu, int symbol);

/*@}*/

/*! \brief This function prepares the dl rx indication
 */
void nr_fill_rx_indication(fapi_nr_rx_indication_t *rx_ind,
                           uint8_t pdu_type,
                           PHY_VARS_NR_UE *ue,
                           int cw_idx,
                           int harq_pid,
                           NR_UE_DLSCH_t *dlsch,
                           const UE_nr_rxtx_proc_t *proc,
                           void *typeSpecific);

void nr_pdcch_generate_llr(PHY_VARS_NR_UE *ue,
                           const UE_nr_rxtx_proc_t *proc,
                           int symbol,
                           nr_phy_data_t *phy_data,
                           int llr_size_symbol,
                           int num_monitoring_occ,
                           int max_symb,
                           c16_t rxdataF[ue->frame_parms.nb_antennas_rx][ue->frame_parms.ofdm_symbol_size],
                           c16_t pdcch_llr[phy_data->phy_pdcch_config.nb_search_space][num_monitoring_occ][max_symb * llr_size_symbol]);

void nr_pdcch_dci_indication(const UE_nr_rxtx_proc_t *proc,
                             int llr_size,
                             int max_monOcc,
                             PHY_VARS_NR_UE *ue,
                             nr_phy_data_t *phy_data,
                             c16_t llr[phy_data->phy_pdcch_config.nb_search_space][max_monOcc][llr_size]);

void nr_ue_csi_im_procedures(PHY_VARS_NR_UE *ue,
                             const c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP],
                             const fapi_nr_dl_config_csiim_pdu_rel15_t *csiim_config_pdu);

void nr_ue_csi_rs_procedures(PHY_VARS_NR_UE *ue,
                             const UE_nr_rxtx_proc_t *proc,
                             const c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP],
                             fapi_nr_dl_config_csirs_pdu_rel15_t *csirs_config_pdu,
                             c16_t trs_estimates[][1][ue->frame_parms.ofdm_symbol_size],
                             const int res_idx,
                             const int trs_sym0);

/* episys SL CSI-RS port: sidelink (PC5) CSI-RS receive procedure. Defined in csi_rx.c alongside the Uu
   nr_ue_csi_rs_procedures so it can reuse that file's static CSI helpers. Measures every active
   sl_csirs_vars[] resource carried in phy_data (populated by fapi_nr_ue_l1.c). */
void nr_ue_sl_csi_rs_procedures(PHY_VARS_NR_UE *ue,
                                const UE_nr_rxtx_proc_t *proc,
                                const c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP],
                                nr_phy_data_t *phy_data);

/* episys SL CSI-RS port: sidelink SINR->CQI mapping (signed SINR). Defined in csi_rx.c. */
int nr_csi_rs_cqi_estimation_sl(const int32_t precoded_sinr, uint8_t *cqi);

void trs_freq_correction(PHY_VARS_NR_UE *ue, int cfo);

int psbch_pscch_pssch_processing(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc, nr_phy_data_t *phy_data);
void phy_procedures_nrUE_SL_TX(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc, nr_phy_data_tx_t *phy_data, c16_t **txp);

/* episys SL data-plane port: UE-native PSSCH demodulator (fills ue->pssch_vars[].llr_layers) and the
   SLSCH decode entry (layer-demap -> unscramble -> LDPC decode). Both defined in nr_pscch_pssch_rx.c. */
void nr_rx_pssch(PHY_VARS_NR_UE *ue,
                 const UE_nr_rxtx_proc_t *proc,
                 NR_DL_FRAME_PARMS *fp,
                 nr_phy_data_t *phy_data,
                 int rxFsize,
                 c16_t rxdataF[][rxFsize],
                 uint8_t slsch_id);
int nr_slsch_procedures(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc, nr_phy_data_t *phy_data, uint8_t slsch_id);
/* SCI-1A (PSCCH) receive: decodes the control channel that carries the PSSCH configuration, and returns
   the Nid the transmitter derived from the PSCCH CRC (38.211 8.3.1.1) for PSSCH DMRS + SLSCH scrambling.
   Defined in nr_pscch_pssch_rx.c. 0 = CRC OK. */
int nr_rx_pscch(PHY_VARS_NR_UE *ue,
                const UE_nr_rxtx_proc_t *proc,
                const NR_DL_FRAME_PARMS *fp,
                const sl_nr_rx_config_pscch_pdu_t *pscch,
                int rxFsize,
                c16_t rxdataF[][rxFsize],
                uint64_t *sci1_payload,
                uint16_t *pssch_Nid,
                int16_t *pscch_rsrp_dBm);
/* episys SL data-plane port: PSCCH SCI-1 encode (PC5). Defined in nr_pscch_tx.c. Returns pscch Nid (low 16b). */
uint32_t nr_generate_sci1(const PHY_VARS_NR_UE *ue,
                          c16_t *txdataF,
                          const NR_DL_FRAME_PARMS *frame_parms,
                          const int16_t amp,
                          const int nr_slot_tx,
                          const sl_nr_tx_config_pscch_pssch_pdu_t *pscch_pssch_pdu);
/* episys SL data-plane port: UE-native PSSCH data transmit (SLSCH encode + SCI-2 + DMRS + RE map). In nr_pscch_tx.c. */
void nr_ue_slsch_procedures(PHY_VARS_NR_UE *ue, uint32_t frame, uint8_t slot, nr_phy_data_tx_t *phy_data, c16_t **txdataF);
/* place a sidelink CSI-RS resource onto the PSSCH grid (after the SLSCH RE map
   punctured its REs). Defined in nr_pscch_tx.c. Called only on the SL_NR_CONFIG_TYPE_TX_PSCCH_PSSCH_CSI_RS action. */
void nr_generate_csi_rs_sl(PHY_VARS_NR_UE *ue,
                           c16_t **txdataF,
                           const NR_DL_FRAME_PARMS *fp,
                           const int slot,
                           const uint16_t scramb_id,
                           const sl_nr_tti_csi_rs_pdu_t *sl_csi);
/* episys SL PSFCH port (Stage 1 PHY TX): generate the PSFCH (HARQ feedback) on PC5. In nr_psfch_tx.c. */
void nr_generate_psfch0(const PHY_VARS_NR_UE *ue,
                        c16_t **txdataF,
                        const NR_DL_FRAME_PARMS *frame_parms,
                        const int16_t amp,
                        const int nr_slot_tx,
                        const sl_nr_tx_rx_config_psfch_pdu_t *psfch_pdu);
/*! \brief This function prepares the sl indication to pass to the MAC
 */
void nr_fill_sl_indication(nr_sidelink_indication_t *sl_ind,
                           sl_nr_rx_indication_t *rx_ind,
                           sl_nr_sci_indication_t *sci_ind,
                           const UE_nr_rxtx_proc_t *proc,
                           PHY_VARS_NR_UE *ue,
                           void *phy_data);
void nr_fill_sl_rx_indication(sl_nr_rx_indication_t *rx_ind,
                              uint8_t pdu_type,
                              PHY_VARS_NR_UE *ue,
                              uint16_t n_pdus,
                              void *typeSpecific,
                              uint16_t rx_slss_id);

/* episys SL data-plane port: PSSCH (SLSCH) receive-decode status + post-decode delivery.
   rdata points into the gNB LDPC decode job (struct LDPCDecode_s, defs_gNB.h) reused for SLSCH RX. */
struct LDPCDecode_s;
typedef struct {
  struct LDPCDecode_s *rdata;
  bool rxok;
} slsch_status_t;

void nr_postDecode_slsch(PHY_VARS_NR_UE *UE,
                         notifiedFIFO_elt_t *req,
                         UE_nr_rxtx_proc_t *proc,
                         nr_phy_data_t *phy_data,
                         int8_t *ack_nack_rcvd,
                         uint8_t num_acks);

#endif
/** @}*/
