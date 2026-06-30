/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "nr_pdcp_asn1_utils.h"
#include "common/utils/LOG/log.h"
#include "nr_pdcp_entity.h"
#include "nr_pdcp_configuration.h"

#define ENCODE_DECODE(a, b)                          \
int decode_##a(int v)                                \
{                                                    \
  static const int tab[] = { VALUES_NR_PDCP_##b };   \
  AssertFatal(v >= 0 && v < SIZEOF_NR_PDCP_##b,      \
              "bad encoded value " #a " %d\n", v);   \
  return tab[v];                                     \
}                                                    \
                                                     \
int encode_##a(int v)                                \
{                                                    \
  static const int tab[] = { VALUES_NR_PDCP_##b };   \
  for (int ret = 0; ret < SIZEOF_NR_PDCP_##b; ret++) \
    if (tab[ret] == v)                               \
      return ret;                                    \
  AssertFatal(0, "bad " #a " value %d\n", v);        \
}

ENCODE_DECODE(t_reordering, T_REORDERING)
ENCODE_DECODE(sn_size_ul, SN_SIZE)
ENCODE_DECODE(sn_size_dl, SN_SIZE)
ENCODE_DECODE(discard_timer, DISCARD_TIMER)

/* Sidelink PDCP discard timer (NR_SL_PDCP_Config_r16.sl_DiscardTimer_r16), value in ms; -1 = infinity.
 * (episys SL data-plane port; standalone since the SL enum is wider than the Uu DISCARD_TIMER table.) */
int decode_discard_timer_sl(long v)
{
  static const int tab[18] = {3, 10, 20, 25, 30, 40, 50, 60, 75, 100, 150, 200, 250, 300, 500, 750, 1500, -1};
  AssertFatal(v >= 0 && v <= 17, "bad sl discard_timer value %ld\n", v);
  return tab[v];
}
