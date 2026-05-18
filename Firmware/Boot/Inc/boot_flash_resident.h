#ifndef BOOT_FLASH_RESIDENT_H
#define BOOT_FLASH_RESIDENT_H

#include <stddef.h>
#include <stdint.h>

int boot_flash_resident_erase_sectors_0_to_5(void);
int boot_flash_resident_program(uint32_t destination, const uint8_t *source, size_t length);
int boot_flash_resident_verify_sha1(uint32_t base, size_t length, const uint8_t expected_sha1[20]);

#endif /* BOOT_FLASH_RESIDENT_H */
