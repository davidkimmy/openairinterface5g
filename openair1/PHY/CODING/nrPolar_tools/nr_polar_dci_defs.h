/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*!
 * \brief Defines the constant variables for polar coding of the DCI from 38-212, V15.1.1 2018-04.
 */

#ifndef __NR_POLAR_DCI_DEFS__H__
#define __NR_POLAR_DCI_DEFS__H__

#define NR_POLAR_DCI_MESSAGE_TYPE 1 // int8_t
// episys SL data-plane port: sidelink SCI polar coding (38.212 8.3/8.4). SCI-1A (PSCCH) and SCI-2 (PSSCH)
// reuse the DCI polar parameters (nMax=9, i_il=1, CRC24C) and differ only in encoderLength (see nr_polar_init.c).
// Values follow PBCH=0, DCI=1, UCI_PUCCH=2, PSBCH=3 -> SCI=4, SCI2=5.
#define NR_POLAR_SCI_MESSAGE_TYPE 4  // int8_t
#define NR_POLAR_SCI2_MESSAGE_TYPE 5 // int8_t
#define NR_POLAR_DCI_CRC_PARITY_BITS 24
#define NR_POLAR_DCI_CRC_ERROR_CORRECTION_BITS 3

// Sec. 7.3.3: Channel Coding
#define NR_POLAR_DCI_N_MAX 9 // uint8_t
#define NR_POLAR_DCI_I_IL 1 // uint8_t
#define NR_POLAR_DCI_I_SEG 0 // uint8_t
#define NR_POLAR_DCI_N_PC 0 // uint8_t
#define NR_POLAR_DCI_N_PC_WM 0 // uint8_t

// Sec. 7.3.4: Rate Matching
#define NR_POLAR_DCI_I_BIL 0 // uint8_t

#endif
