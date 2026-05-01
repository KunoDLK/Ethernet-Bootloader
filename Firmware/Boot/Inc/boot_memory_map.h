#ifndef BOOT_MEMORY_MAP_H
#define BOOT_MEMORY_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOOT_FLASH_BASE_ADDR          (0x08000000UL)
#define BOOT_FLASH_SIZE_BYTES         (1024UL * 1024UL)

#define BOOT_RESIDENT_BASE_ADDR       (0x08000000UL)
#define BOOT_RESIDENT_SIZE_BYTES      (256UL * 1024UL)
#define BOOT_RESIDENT_LIMIT_ADDR      (BOOT_RESIDENT_BASE_ADDR + BOOT_RESIDENT_SIZE_BYTES)

#define BOOT_APP_STORE_BASE_ADDR      (0x08040000UL)
#define BOOT_APP_STORE_SIZE_BYTES     (640UL * 1024UL)
#define BOOT_APP_STORE_LIMIT_ADDR     (BOOT_APP_STORE_BASE_ADDR + BOOT_APP_STORE_SIZE_BYTES)

#define BOOT_METADATA_BASE_ADDR       (0x080E0000UL)
#define BOOT_METADATA_SIZE_BYTES      (128UL * 1024UL)
#define BOOT_METADATA_LIMIT_ADDR      (BOOT_METADATA_BASE_ADDR + BOOT_METADATA_SIZE_BYTES)

#define BOOT_APP_EXEC_BASE_ADDR       (0x10000000UL)
#define BOOT_APP_EXEC_SIZE_BYTES      (64UL * 1024UL)
#define BOOT_APP_EXEC_LIMIT_ADDR      (BOOT_APP_EXEC_BASE_ADDR + BOOT_APP_EXEC_SIZE_BYTES)

#define BOOT_SRAM_BASE_ADDR           (0x20000000UL)
#define BOOT_SRAM_SIZE_BYTES          (128UL * 1024UL)
#define BOOT_SRAM_LIMIT_ADDR          (BOOT_SRAM_BASE_ADDR + BOOT_SRAM_SIZE_BYTES)

#define BOOT_SECTOR_INVALID           (0xFFFFFFFFUL)

uint32_t boot_mem_resident_base(void);
uint32_t boot_mem_resident_limit(void);
uint32_t boot_mem_app_store_base(void);
uint32_t boot_mem_app_store_limit(void);
uint32_t boot_mem_app_exec_base(void);
uint32_t boot_mem_app_exec_limit(void);
uint32_t boot_mem_metadata_base(void);
uint32_t boot_mem_metadata_limit(void);

bool boot_mem_range_is_valid(uint32_t base, uint32_t length);
bool boot_mem_is_resident_range(uint32_t base, uint32_t length);
bool boot_mem_is_app_store_range(uint32_t base, uint32_t length);
bool boot_mem_is_app_exec_range(uint32_t base, uint32_t length);
bool boot_mem_is_metadata_range(uint32_t base, uint32_t length);
bool boot_mem_addr_is_app_exec(uint32_t address);

uint32_t boot_mem_sector_for_addr(uint32_t address);
uint32_t boot_mem_sector_base(uint32_t sector);
uint32_t boot_mem_sector_size(uint32_t sector);

#endif /* BOOT_MEMORY_MAP_H */
