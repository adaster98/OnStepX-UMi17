#include "esp_partition.h"
#include <setjmp.h>

uint8_t g_flash[FLASH_BYTES];
int g_powerCut = -1;
int g_tornPartial = 0;
unsigned long g_eraseOps = 0, g_eraseSectors = 0, g_writeOps = 0;
extern jmp_buf g_pwr;

static esp_partition_t g_part = { FLASH_BYTES };

const esp_partition_t *esp_partition_find_first(int, int, const char *) { return &g_part; }

int esp_partition_read(const esp_partition_t *, size_t off, void *dst, size_t n) {
  if (off + n > FLASH_BYTES) return -1;
  memcpy(dst, g_flash + off, n);
  return ESP_OK;
}

int esp_partition_write(const esp_partition_t *, size_t off, const void *src, size_t n) {
  if (off + n > FLASH_BYTES) return -1;
  const uint8_t *s = (const uint8_t *)src;
  size_t limit = n;
  int cut = 0;
  if (g_powerCut == 0) { limit = g_tornPartial ? n / 2 : 0; cut = 1; }
  // NOR: a write can only clear bits
  for (size_t i = 0; i < limit; i++) g_flash[off + i] &= s[i];
  g_writeOps++;
  if (g_powerCut > 0) g_powerCut--;
  if (cut) longjmp(g_pwr, 1);
  return ESP_OK;
}

int esp_partition_erase_range(const esp_partition_t *, size_t off, size_t n) {
  if (off % 4096 || n % 4096 || off + n > FLASH_BYTES) {
    printf("  !! BAD ERASE off=%zu n=%zu\n", off, n);
    return -1;
  }
  g_eraseOps++; g_eraseSectors += n / 4096;
  memset(g_flash + off, 0xFF, n);
  return ESP_OK;
}
