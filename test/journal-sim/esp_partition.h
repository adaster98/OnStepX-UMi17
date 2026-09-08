// Fake flash with real NOR semantics, for host testing of Journal.cpp.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#define ESP_OK 0
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_ANY 0xff

typedef struct { uint32_t size; } esp_partition_t;

#define FLASH_BYTES (64u * 4096u)
extern uint8_t g_flash[FLASH_BYTES];
extern int g_powerCut;          // when >0, counts writes remaining before "power loss"
extern int g_tornPartial;       // if set, the cutting write lands half-programmed
extern unsigned long g_eraseOps, g_eraseSectors, g_writeOps;

const esp_partition_t *esp_partition_find_first(int, int, const char *);
int esp_partition_read(const esp_partition_t *, size_t, void *, size_t);
int esp_partition_write(const esp_partition_t *, size_t, const void *, size_t);
int esp_partition_erase_range(const esp_partition_t *, size_t, size_t);
