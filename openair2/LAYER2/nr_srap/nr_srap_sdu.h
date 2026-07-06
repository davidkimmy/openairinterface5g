/*
Author: Ejaz Ahmed
Email ID: ejaz.ahmed@applied.co
*/

#ifndef _NR_SRAP_SDU_H_
#define _NR_SRAP_SDU_H_

typedef struct nr_srap_sdu {
  uint32_t             count;
  char                 *buffer;
  int                  size;
  struct nr_srap_sdu *next;
} nr_srap_sdu_t;

nr_srap_sdu_t *nr_srap_new_sdu(char *buffer, int size);
void nr_srap_free_sdu(nr_srap_sdu_t *sdu);

#endif /* _NR_SRAP_SDU_H_ */
