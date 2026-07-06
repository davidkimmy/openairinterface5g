/*
Author: Ejaz Ahmed
Email ID: ejaz.ahmed@applied.co
*/

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "nr_srap_sdu.h"

nr_srap_sdu_t *nr_srap_new_sdu(char *buffer, int size)
{
  nr_srap_sdu_t *ret = calloc(1, sizeof(nr_srap_sdu_t));
  if (ret == NULL)
    exit(1);
  ret->buffer = malloc(size);
  if (ret->buffer == NULL)
    exit(1);
  memcpy(ret->buffer, buffer, size);
  ret->size = size;
  return ret;
}

void nr_srap_free_sdu(nr_srap_sdu_t *sdu)
{
  free(sdu->buffer);
  free(sdu);
}
