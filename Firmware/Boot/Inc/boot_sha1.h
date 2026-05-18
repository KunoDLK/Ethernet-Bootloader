#ifndef BOOT_SHA1_H
#define BOOT_SHA1_H

#include <stddef.h>
#include <stdint.h>

#define BOOT_SHA1_DIGEST_BYTES 20U

void boot_sha1_compute(const void *data, size_t length, uint8_t digest_out[BOOT_SHA1_DIGEST_BYTES]);

#endif /* BOOT_SHA1_H */
