#ifndef BOOT_FLASH_H
#define BOOT_FLASH_H

#include <stddef.h>
#include <stdint.h>

int boot_flash_begin_app_storage_programming(void);
void boot_flash_end_app_storage_programming(void);
int boot_flash_erase_app_storage(void);
int boot_flash_write_app_storage(uint32_t offset, const void *data, size_t length);
int boot_flash_read_app_storage(uint32_t offset, void *data, size_t length);

#endif /* BOOT_FLASH_H */
