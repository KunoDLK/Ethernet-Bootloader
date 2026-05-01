#ifndef BOOT_FAULT_H
#define BOOT_FAULT_H

#include "boot_metadata.h"

#include <stdbool.h>
#include <stdint.h>

bool boot_fault_pc_is_application(uint32_t pc);
void boot_fault_handle(BootAppFaultReason reason, uint32_t exc_return);

#endif /* BOOT_FAULT_H */
