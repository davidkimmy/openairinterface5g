/*
Author: Ejaz Ahmed
Email ID: ejaz.ahmed@applied.co
*/

#ifndef _NR_SRAP_MANAGER_H_
#define _NR_SRAP_MANAGER_H_

#include "nr_srap_entity.h"

typedef void nr_srap_manager_t;

typedef struct {
  pthread_mutex_t lock;
  nr_srap_entity_t **srap_entity;
  bool             gNB_flag;
} nr_srap_manager_internal_t;

/***********************************************************************/
/* manager functions                                                   */
/***********************************************************************/

nr_srap_manager_t *new_nr_srap_manager(bool gNB_flag);

nr_srap_manager_t *get_nr_srap_manager();

nr_srap_entity_t* nr_srap_get_entity(nr_srap_manager_t *_m, nr_srap_entity_type_t entity_type);

int nr_srap_manager_get_gnb_flag(nr_srap_manager_t *m);

void nr_srap_manager_lock(nr_srap_manager_t *m);

void nr_srap_manager_unlock(nr_srap_manager_t *m);

srap_mapping_t *nr_srap_manager_get_ue_mapping(nr_srap_manager_t *_m, int rnti, nr_srap_entity_type_t entity_type);

void nr_srap_manager_remove_ue(nr_srap_manager_t *m, int rnti);

#endif /* _NR_SRAP_MANAGER_H_ */
