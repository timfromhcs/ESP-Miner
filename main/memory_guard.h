#ifndef MEMORY_GUARD_H_
#define MEMORY_GUARD_H_

#include <stdbool.h>
#include <stdint.h>

#define MEMORY_GUARD_OK         0
#define MEMORY_GUARD_WARNING    1
#define MEMORY_GUARD_OVERLOADED 2

typedef struct {
    uint32_t internal_free;
    uint32_t internal_largest_block;
    uint32_t internal_min_free_ever;
    uint32_t psram_free;
    uint8_t  state;
} memory_guard_status_t;

void memory_guard_task(void *pvParameters);

void memory_guard_get_status(memory_guard_status_t *out);

/*
 * Prueft den internen DRAM auf einen kritischen Zustand.
 *
 * Bewusst ohne Hysterese und ohne Neustart: fuer Aufrufer, die vor einer
 * riskanten Allokation (grosse DMA-Puffer, DMA-Transfer) wissen wollen, ob
 * noch genug interner RAM da ist. Gibt true zurueck, wenn die Allokation
 * sicher vermieden werden sollte.
 */
bool memory_guard_internal_is_critical(uint32_t needed_bytes);

#endif /* MEMORY_GUARD_H_ */