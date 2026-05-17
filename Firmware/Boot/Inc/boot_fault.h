#ifndef BOOT_FAULT_H
#define BOOT_FAULT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
  BOOT_APP_FAULT_NONE = 0,
  BOOT_APP_FAULT_MEMMANAGE = 1,
  BOOT_APP_FAULT_BUS = 2,
  BOOT_APP_FAULT_USAGE = 3,
  BOOT_APP_FAULT_HARD = 4,
} BootAppFaultReason;

bool boot_fault_pc_is_application(uint32_t pc);
void boot_fault_handle(BootAppFaultReason reason, uint32_t exc_return);

#endif /* BOOT_FAULT_H */
