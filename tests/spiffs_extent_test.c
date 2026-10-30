#include "spiffs.h"
#include "esp_partition.h"
#include <assert.h>
#include <stdio.h>

extern u32_t ya_spiffs_extent(const esp_partition_t *, const spiffs_config *);
static unsigned char flash[0x3d0000];
static unsigned writes, reads;
static int fail_read;
static esp_partition_t partition = {.size = sizeof flash, .label = "storage"};
int esp_partition_read(const esp_partition_t *p, size_t off, void *out, size_t n) {
    ++reads;
    if (fail_read || off + n > p->size) return -1;
    memcpy(out, flash + off, n); return 0;
}
void spiffs_api_lock(spiffs *fs) {(void)fs;}
void spiffs_api_unlock(spiffs *fs) {(void)fs;}
static s32_t read_flash(spiffs *fs, u32_t off, u32_t n, u8_t *out) {
    (void)fs; return esp_partition_read(&partition, off, out, n);
}
static s32_t write_flash(spiffs *fs, u32_t off, u32_t n, u8_t *in) {
    (void)fs; assert(off + n <= sizeof flash); ++writes;
    for (unsigned i = 0; i < n; ++i) flash[off + i] &= in[i];
    return 0;
}
static s32_t erase_flash(spiffs *fs, u32_t off, u32_t n) {
    (void)fs; assert(off + n <= sizeof flash); ++writes;
    memset(flash + off, 255, n); return 0;
}
int main(int argc, char **argv) {
    memset(flash, 255, sizeof flash);
    spiffs_config cfg = {.hal_read_f = read_flash, .hal_write_f = write_flash,
        .hal_erase_f = erase_flash, .phys_size = 0x300000, .phys_erase_block = 4096,
        .log_block_size = 4096, .log_page_size = 256};
    spiffs fs;
    u8_t work[512], fds[1024], cache[2048];
    assert(SPIFFS_mount(&fs, &cfg, work, fds, sizeof fds, cache, sizeof cache, NULL) != 0);
    assert(SPIFFS_format(&fs) == 0);
    if (argc > 1) {
        // Optional local-only replay of a backed-up legacy partition.
        FILE *f = fopen(argv[1], "rb"); assert(f);
        assert(fread(flash, 1, sizeof flash, f) == sizeof flash); fclose(f);
    }
    assert(SPIFFS_mount(&fs, &cfg, work, fds, sizeof fds, cache, sizeof cache, NULL) == 0);
    const char message[] = "这是测试，🙂";
    spiffs_file f = SPIFFS_open(&fs, "legacy", SPIFFS_CREAT | SPIFFS_RDWR, 0); assert(f >= 0);
    assert(SPIFFS_write(&fs, f, (void *)message, sizeof message) == sizeof message);
    assert(SPIFFS_close(&fs, f) == 0); SPIFFS_unmount(&fs);
    cfg.phys_size = sizeof flash;
    assert(SPIFFS_mount(&fs, &cfg, work, fds, sizeof fds, cache, sizeof cache, NULL) != 0);
    unsigned before = writes;
    cfg.phys_size = ya_spiffs_extent(&partition, &cfg);
    assert(cfg.phys_size == 0x300000 && writes == before);
    assert(SPIFFS_mount(&fs, &cfg, work, fds, sizeof fds, cache, sizeof cache, NULL) == 0);
    char out[sizeof message] = {0};
    f = SPIFFS_open(&fs, "legacy", SPIFFS_RDONLY, 0); assert(f >= 0);
    assert(SPIFFS_read(&fs, f, out, sizeof out) == sizeof out && !memcmp(out, message, sizeof out));
    assert(SPIFFS_close(&fs, f) == 0);
    f = SPIFFS_open(&fs, "info1", SPIFFS_CREAT | SPIFFS_RDWR, 0); assert(f >= 0);
    assert(SPIFFS_write(&fs, f, (void *)message, sizeof message) == sizeof message);
    assert(SPIFFS_close(&fs, f) == 0); SPIFFS_unmount(&fs);
    assert(SPIFFS_mount(&fs, &cfg, work, fds, sizeof fds, cache, sizeof cache, NULL) == 0);
    f = SPIFFS_open(&fs, "info1", SPIFFS_RDONLY, 0); assert(f >= 0);
    assert(SPIFFS_read(&fs, f, out, sizeof out) == sizeof out && !memcmp(out, message, sizeof out));
    assert(SPIFFS_close(&fs, f) == 0); SPIFFS_unmount(&fs);
    before = writes;
    fail_read = 1; assert(ya_spiffs_extent(&partition, &cfg) == sizeof flash); fail_read = 0;
    partition.size = 0x200000; assert(ya_spiffs_extent(&partition, &cfg) == partition.size);
    partition.size = sizeof flash;
    // Probe tolerates one interrupted erase, as upstream SPIFFS specifies.
    memset(flash + 252, 255, 2); assert(ya_spiffs_extent(&partition, &cfg) == 0x300000);
    memset(flash + 4096 + 252, 255, 2); assert(ya_spiffs_extent(&partition, &cfg) == sizeof flash);
    memset(flash, 255, sizeof flash); assert(ya_spiffs_extent(&partition, &cfg) == sizeof flash);
    assert(writes == before && reads > 0);
    puts("legacy mount, retained data, UTF-8 write/remount and failed probe preservation passed");
}
