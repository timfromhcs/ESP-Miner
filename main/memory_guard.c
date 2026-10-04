#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_psram.h"

#include "memory_guard.h"

static const char *TAG = "memory_guard";

/*
 * Warteintervall. 5s ist kurz genug, um einen echten Leck nicht lange
 * unentdeckt zu lassen, und lang genug, dass der Wächter selbst praktisch
 * keine CPU-Zeit kostet (zwei heap_caps-Aufrufe pro Iteration).
 */
#define GUARD_INTERVAL_MS       5000

/*
 * Kritische Schwelle auf dem GROESSTEN freien Block, nicht auf dem Gesamt-Freiheap.
 *
 * Der groesste Block ist die entscheidende Groesse: FreeRTOS alloziiert
 * Task-Stacks als zusammenhaengenden Block. Ein Gesamt-Freiheap von 40 KB, der
 * nur aus 5 KB fragmentierten Resten besteht, ist fuer eine Task mit 4 KB Stack
 * genauso unbrauchbar wie ein leerer Heap. Genau daraus entstehen die
 * "stack overflow in task IDLE1"-Panics.
 *
 * 16 KB ist gewaehlt, weil die groessten Stacks im Projekt (main_task 8 KB,
 * httpd-Threads 4-6 KB) sonst nicht mehr zuverlaessig passen.
 */
#define CRITICAL_LARGEST_BLOCK  (16 * 1024)

/*
 * Anzahl aufeinanderfolgender kritischer Messungen bis zum Neustart.
 * Schützt vor Fehlalarmen durch einzelne Allokationen (z.B. TLS-Handshake,
 * HTTP-Request) und gibt dem Heap Zeit, sich wieder zu erholen.
 */
#define CRITICAL_STREAK_TO_RESTART  3

/*
 * Warnschwelle auf dem Gesamt-Freiheap. Nur Logging, kein Eingriff.
 */
#define WARNING_INTERNAL_FREE   (24 * 1024)

static memory_guard_status_t s_status = {
    .internal_free = 0,
    .internal_largest_block = 0,
    .internal_min_free_ever = 0,
    .psram_free = 0,
    .state = MEMORY_GUARD_OK,
};

static uint32_t internal_cap(void)
{
    return MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
}

static void sample(memory_guard_status_t *out)
{
    out->internal_free = heap_caps_get_free_size(internal_cap());
    out->internal_largest_block = heap_caps_get_largest_free_block(internal_cap());
    out->internal_min_free_ever = heap_caps_get_minimum_free_size(internal_cap());
    out->psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

bool memory_guard_internal_is_critical(uint32_t needed_bytes)
{
    uint32_t largest = heap_caps_get_largest_free_block(internal_cap());
    return largest < needed_bytes;
}

void memory_guard_get_status(memory_guard_status_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_status;
}

void memory_guard_task(void *pvParameters)
{
    uint32_t critical_streak = 0;
    uint32_t last_warn_free = UINT32_MAX;
    uint8_t last_state = MEMORY_GUARD_OK;

    (void)pvParameters;

    for (;;) {
        memory_guard_status_t now = {0};
        sample(&now);

        uint8_t state = MEMORY_GUARD_OK;

        if (now.internal_largest_block < CRITICAL_LARGEST_BLOCK) {
            state = MEMORY_GUARD_OVERLOADED;
            critical_streak++;
        } else {
            critical_streak = 0;
            if (now.internal_free < WARNING_INTERNAL_FREE) {
                state = MEMORY_GUARD_WARNING;
            }
        }

        now.state = state;
        s_status = now;

        /* Zustandswechsel loggen, nicht jeden Zyklus wiederholen. */
        if (state != last_state) {
            if (state == MEMORY_GUARD_OVERLOADED) {
                ESP_LOGE(TAG, "internal RAM overloaded: largest block %" PRIu32 " B < %d B",
                         now.internal_largest_block, CRITICAL_LARGEST_BLOCK);
            } else if (state == MEMORY_GUARD_WARNING) {
                ESP_LOGW(TAG, "internal RAM low: free %" PRIu32 " B, largest block %" PRIu32 " B, PSRAM %" PRIu32 " B",
                         now.internal_free, now.internal_largest_block, now.psram_free);
            } else {
                ESP_LOGI(TAG, "internal RAM recovered: free %" PRIu32 " B, largest block %" PRIu32 " B (min ever %" PRIu32 " B)",
                         now.internal_free, now.internal_largest_block, now.internal_min_free_ever);
            }
            last_state = state;
        } else if (state == MEMORY_GUARD_WARNING && now.internal_free + 4096 < last_warn_free) {
            /* Nur bei weiter sinkendem Freihheap melden, nicht bei jedem Tick. */
            ESP_LOGW(TAG, "internal RAM still low: free %" PRIu32 " B, largest block %" PRIu32 " B",
                     now.internal_free, now.internal_largest_block);
            last_warn_free = now.internal_free;
        }

        if (state == MEMORY_GUARD_OK) {
            last_warn_free = UINT32_MAX;
        }

        if (critical_streak >= CRITICAL_STREAK_TO_RESTART) {
            /*
             * Kontrollierter Neustart statt Panic.
             *
             * Ohne das endet ein echter Speicherengpass in einem
             * "stack overflow in task IDLE1"-Panic: der Absturz ist dann
             * schwerer zu deuten und der Reboot erfolgt unkontrolliert.
             * Der Log-Bringt den Grund mit, und der Neustart ist deterministisch.
             */
            ESP_LOGE(TAG, "persistent internal RAM overload (%u consecutive samples), restarting", critical_streak);
            ESP_LOGE(TAG, "free=%" PRIu32 " largest=%" PRIu32 " min_ever=%" PRIu32 " psram=%" PRIu32,
                     now.internal_free, now.internal_largest_block, now.internal_min_free_ever, now.psram_free);

            /* Dem Log-Bring Zeit geben, die Diagnose zu leeren. */
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }

        vTaskDelay(pdMS_TO_TICKS(GUARD_INTERVAL_MS));
    }
}