/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.0  (the "License"); you may not use this file
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

/*! \file PHY/NR_TRANSPORT/nr_tbs_tools.c
* \brief Top-level routines for implementing LDPC-coded (DLSCH) transport channels from 38-212, 15.2
* \author H.Wang
* \date 2018
* \version 0.1
* \company Eurecom
* \email:
* \note
* \warning
*/

#include "nr_transport_common_proto.h"
#include "PHY/CODING/coding_defs.h"
#include "PHY/defs_nr_common.h"
#include "common/utils/LOG/log.h"

uint32_t nr_get_G(uint16_t nb_rb, uint16_t nb_symb_sch,uint8_t nb_re_dmrs,uint16_t length_dmrs, uint8_t Qm, uint8_t Nl) {
	uint32_t G;
	G = ((NR_NB_SC_PER_RB*nb_symb_sch)-(nb_re_dmrs*length_dmrs))*nb_rb*Qm*Nl;
	return(G);
}

uint32_t nr_get_G_SL(uint16_t nb_rb, uint16_t nb_symb_sch, uint8_t nb_re_dmrs, uint16_t length_dmrs, uint8_t sci1_dmrs_overlap, uint16_t sci1_re, uint16_t sci1_rb, uint16_t sci2_re, uint16_t csi_rs_re, uint8_t Qm, uint8_t Nl) {

  uint32_t G_SL, slsch_re;
  slsch_re = ((NR_NB_SC_PER_RB*nb_symb_sch) - (nb_re_dmrs*length_dmrs)) * nb_rb;
  if (sci1_dmrs_overlap > 0) slsch_re += (nb_re_dmrs * sci1_rb); // return the dmrs that are not transmitted due to SCI1
  slsch_re -= sci1_re; // REs taken by SCI1
  slsch_re -= sci2_re; // REs taken by SCI2
  slsch_re -= csi_rs_re; // REs taken by CSI_RS
  G_SL = slsch_re * Qm * Nl;
  return(G_SL);
}

uint32_t nr_get_E(uint32_t G, uint8_t C, uint8_t Qm, uint8_t Nl, uint8_t r) {
  uint32_t E;
  uint8_t Cprime = C; //assume CBGTI not present

  /* Under bad RF, MAC/PHY can supply corrupted DCI (Nl=0 or Qm=0). Asserting crashes the UE/gNB;
   * callers must check for E==0 and abort the current TB. */
  if (Nl == 0 || Qm == 0 || Cprime == 0) {
    LOG_E(PHY, "nr_get_E: invalid params Nl=%u Qm=%u C=%u (G=%u r=%u)\n", Nl, Qm, Cprime, G, r);
    return 0;
  }
  if (r <= Cprime - ((G/(Nl*Qm))%Cprime) - 1)
      E = Nl*Qm*(G/(Nl*Qm*Cprime));
  else
      E = Nl*Qm*((G/(Nl*Qm*Cprime))+1);

  LOG_D(PHY,"nr_get_E : (G %d, C %d, Qm %d, Nl %d, r %d), E %d\n",G, C, Qm, Nl, r, E);
  return E;
}
