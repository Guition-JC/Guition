#include "memory_monitor.h"

#include <inttypes.h>
#include <stdint.h>

#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"

#define TAG "MemoryMonitor"

/** @brief Report memory capacity every 30 seconds. */
#define MEMORY_MONITOR_PERIOD_MS 30000U

static esp_timer_handle_t s_report_timer;

static uint32_t flash_partition_end(void)
{
    uint32_t partition_end = 0;
    esp_partition_iterator_t iterator = esp_partition_find(ESP_PARTITION_TYPE_ANY,
                                                           ESP_PARTITION_SUBTYPE_ANY,
                                                           NULL);
    while (iterator != NULL) {
        const esp_partition_t *partition = esp_partition_get(iterator);
        if (partition != NULL) {
            const uint64_t end = (uint64_t)partition->address + partition->size;
            if (end > partition_end) {
                partition_end = end > UINT32_MAX ? UINT32_MAX : (uint32_t)end;
            }
        }
        iterator = esp_partition_next(iterator);
    }
    return partition_end;
}

void memory_monitor_report(const char *phase)
{
    const size_t sram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t sram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t sram_dma_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    const size_t sram_dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    const size_t sram_dma_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    const size_t sram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    uint32_t rom_total = 0;
    uint32_t rom_free = 0;

    if (esp_flash_get_physical_size(esp_flash_default_chip, &rom_total) == ESP_OK) {
        const uint32_t partition_end = flash_partition_end();
        rom_free = partition_end < rom_total ? rom_total - partition_end : 0;
    }

    ESP_LOGI(TAG,
             "memory[%s]: SRAM free=%" PRIu32 "/ total=%" PRIu32 " largest=%" PRIu32 " bytes, "
             "SRAM-DMA free=%" PRIu32 " largest=%" PRIu32 " min=%" PRIu32 " bytes, "
             "PSRAM free=%" PRIu32 "/ total=%" PRIu32 " largest=%" PRIu32 " bytes, "
             "ROM(Flash) free=%" PRIu32 "/ total=%" PRIu32 " bytes",
             phase == NULL ? "unknown" : phase,
             (uint32_t)sram_free,
             (uint32_t)sram_total,
             (uint32_t)sram_largest,
             (uint32_t)sram_dma_free,
             (uint32_t)sram_dma_largest,
             (uint32_t)sram_dma_min,
             (uint32_t)psram_free,
             (uint32_t)psram_total,
             (uint32_t)psram_largest,
             rom_free,
             rom_total);
}

static void memory_monitor_tick(void *arg)
{
    (void)arg;
    memory_monitor_report("tick");
}

esp_err_t memory_monitor_start(void)
{
    if (s_report_timer != NULL) {
        return ESP_OK;
    }

    const esp_timer_create_args_t args = {
        .callback = memory_monitor_tick,
        .name = "memory_monitor",
    };
    esp_err_t ret = esp_timer_create(&args, &s_report_timer);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "periodic report unavailable: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_timer_start_periodic(s_report_timer, (uint64_t)MEMORY_MONITOR_PERIOD_MS * 1000ULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "periodic report not started: %s", esp_err_to_name(ret));
        esp_timer_delete(s_report_timer);
        s_report_timer = NULL;
    }
    return ret;
}
