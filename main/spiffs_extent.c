// Use the pinned SPIFFS probe to preserve volumes created with a smaller size.
// Compiled into IDF's SPIFFS target; no partition-table or on-flash migration.
#include "esp_partition.h"
#include "spiffs.h"

static _Thread_local const esp_partition_t *probe_partition;
static s32_t probe_read(spiffs *fs, u32_t offset, u32_t size, u8_t *out) {
    (void)fs;
    return esp_partition_read(probe_partition, offset, out, size) == ESP_OK ? SPIFFS_OK : -1;
}

u32_t ya_spiffs_extent(const esp_partition_t *partition, const spiffs_config *config) {
#if SPIFFS_USE_MAGIC && SPIFFS_USE_MAGIC_LENGTH && SPIFFS_SINGLETON == 0
    if (strcmp(partition->label, "storage") == 0) {
        spiffs_config probe = *config;
        probe.hal_read_f = probe_read;
        probe_partition = partition;
        s32_t size = SPIFFS_probe_fs(&probe);
        probe_partition = NULL;
        // A corrupt/foreign/oversized volume still takes the normal mount-fail
        // path. Never extend a legacy filesystem or format its existing bytes.
        if (size > 0 && (u32_t)size >= 3 * config->log_block_size && (u32_t)size <= partition->size &&
            size % config->log_block_size == 0) {
            return size;
        }
    }
#endif
    return partition->size;
}
