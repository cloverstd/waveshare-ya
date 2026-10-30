"""Replay the pinned SPIFFS implementation, without hardware or private fixtures."""
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]

def test_legacy_spiffs_extent(tmp_path):
    spiffs = ROOT / '.tools/esp-idf/components/spiffs'
    (tmp_path / 'sdkconfig.h').write_text('''
#define CONFIG_SPIFFS_CACHE 1
#define CONFIG_SPIFFS_CACHE_WR 1
#define CONFIG_SPIFFS_PAGE_CHECK 1
#define CONFIG_SPIFFS_GC_MAX_RUNS 10
#define CONFIG_SPIFFS_OBJ_NAME_LEN 32
#define CONFIG_SPIFFS_META_LENGTH 4
#define CONFIG_SPIFFS_PAGE_SIZE 256
#define CONFIG_SPIFFS_USE_MAGIC 1
#define CONFIG_SPIFFS_USE_MAGIC_LENGTH 1
''')
    (tmp_path / 'esp_log.h').write_text('#define ESP_LOGD(...)\n')
    (tmp_path / 'esp_assert.h').write_text('#define ESP_STATIC_ASSERT _Static_assert\n')
    (tmp_path / 'esp_partition.h').write_text('''
#include <stddef.h>
#define ESP_OK 0
typedef struct { size_t size; char label[17]; } esp_partition_t;
int esp_partition_read(const esp_partition_t *, size_t, void *, size_t);
''')
    binary = tmp_path / 'spiffs-test'
    subprocess.run(['cc', '-std=c11', '-g', '-fsanitize=address',
        '-I' + str(tmp_path), '-I' + str(spiffs / 'include'), '-I' + str(spiffs / 'spiffs/src'),
        str(ROOT / 'tests/spiffs_extent_test.c'), str(ROOT / 'main/spiffs_extent.c'),
        *map(str, sorted((spiffs / 'spiffs/src').glob('*.c'))), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
