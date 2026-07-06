/*
Author: Ejaz Ahmed
Email ID: ejaz.ahmed@applied.co
*/

#include "nr_srap_manager.h"
#include "nr_srap_oai_api.h"
#include <softmodem-common.h>

nr_srap_manager_t *nr_srap_manager;

nr_srap_manager_t *new_nr_srap_manager(bool gNB_flag) {
  nr_srap_manager_internal_t *ret;

  ret = calloc(1, sizeof(nr_srap_manager_internal_t));
  if (ret == NULL) {
    LOG_E(NR_SRAP, "%s:%d:%s: out of memory\n", __FILE__, __LINE__, __FUNCTION__);
    exit(EXIT_FAILURE);
  }
  bool is_relay_ue = get_softmodem_params()->is_relay_ue;
  uint8_t relay_type = get_softmodem_params()->relay_type;
  uint8_t num_of_entities = (is_relay_ue && relay_type == U2N) ? 2 : 1; // 38.351 - 4.2.2
  ret->srap_entity = (nr_srap_entity_t**)malloc16_clear(sizeof(nr_srap_entity_t *));

  if (!ret->srap_entity) {
      LOG_E(NR_SRAP, "Memory allocation failed\n");
      exit(EXIT_FAILURE);
  }

  for (uint8_t i = 0; i < num_of_entities; i++) {
    // Now allocate memory for the actual structure
    ret->srap_entity[i] = (nr_srap_entity_t*)malloc16_clear(sizeof(nr_srap_entity_t));
    if (ret->srap_entity[i] == NULL) {
        LOG_E(NR_SRAP, "Memory allocation failed\n");
        for (int j = 0; j < i; j++) {
          free(ret->srap_entity[j]);
        }
      free(ret->srap_entity);
      exit(EXIT_FAILURE);
    }
    if (pthread_mutex_init(&ret->lock, NULL)) abort();
    ret->gNB_flag = gNB_flag;
  }
  return ret;
}

int nr_srap_manager_get_gnb_flag(nr_srap_manager_t *_m) {
  nr_srap_manager_internal_t *m = _m;
  return m->gNB_flag;
}

nr_srap_entity_t* nr_srap_get_entity(nr_srap_manager_t *_m, nr_srap_entity_type_t entity_type) {
  nr_srap_manager_internal_t *m = _m;
  bool is_relay_ue = get_softmodem_params()->is_relay_ue;
  uint8_t relay_type = get_softmodem_params()->relay_type;
  uint8_t num_of_entities = (is_relay_ue && relay_type == U2N) ? 2 : 1; // 38.351 - 4.2.2
  for (int i = 0; i < num_of_entities; i++) {
    AssertFatal((m->srap_entity[i] != NULL), "SRAP entity is not initialized!!!");
    if (m->srap_entity[i]->type == entity_type) {
      LOG_D(NR_SRAP, "%s m->srap_entity[%d] %p\n", __FUNCTION__, i, (m->srap_entity[i]));
      return (m->srap_entity[i]);
    }
  }
  return NULL;
}

void nr_srap_manager_lock(nr_srap_manager_t *_m) {
  nr_srap_manager_internal_t *m = _m;
  if (pthread_mutex_lock(&m->lock)) {
    LOG_E(NR_SRAP, "%s:%d:%s: fatal\n", __FILE__, __LINE__, __FUNCTION__);
    exit(EXIT_FAILURE);
  }
}

void nr_srap_manager_unlock(nr_srap_manager_t *_m) {
  nr_srap_manager_internal_t *m = _m;
  if (pthread_mutex_unlock(&m->lock)) {
    LOG_E(NR_SRAP, "%s:%d:%s: fatal\n", __FILE__, __LINE__, __FUNCTION__);
    exit(EXIT_FAILURE);
  }
}

/* must be called with lock acquired */
srap_mapping_t *nr_srap_manager_get_ue_mapping(nr_srap_manager_t *_m, int rnti, nr_srap_entity_type_t entity_type) {
  nr_srap_manager_internal_t *m = _m;
  nr_srap_entity_t *srap_entity = nr_srap_get_entity(m, entity_type);
  if (srap_entity != NULL) {
    for (int k = 0; k < srap_entity->bearer_to_rlc_map.size; k++) {
      if ((srap_entity->bearer_to_rlc_map.array + k)->ue_id == rnti) {
        return (srap_entity->bearer_to_rlc_map.array + k);
      }
    }
  } else {
    LOG_W(NR_SRAP, "SRAP entity does not exist!!!\n");
  }
  return NULL;
}

nr_srap_manager_t *get_nr_srap_manager() {
    return nr_srap_manager;
}
