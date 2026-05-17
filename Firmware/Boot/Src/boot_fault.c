#include "boot_fault.h"

#include "boot_kv_nodes.h"
#include "boot_metadata.h"
#include "boot_memory_map.h"
#include "cmsis_gcc.h"
#include "stm32f4xx_hal.h"

#include <stdint.h>

typedef struct
{
  uint32_t r0;
  uint32_t r1;
  uint32_t r2;
  uint32_t r3;
  uint32_t r12;
  uint32_t lr;
  uint32_t pc;
  uint32_t xpsr;
} BootExceptionFrame;

static void write_u32_le(uint8_t out[4], uint32_t value)
{
  out[0] = (uint8_t)(value & 0xFFU);
  out[1] = (uint8_t)((value >> 8U) & 0xFFU);
  out[2] = (uint8_t)((value >> 16U) & 0xFFU);
  out[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

static void store_u32_setting(uint32_t key, uint32_t value)
{
  uint8_t encoded[4];
  write_u32_le(encoded, value);
  (void)boot_metadata_set(key, encoded, sizeof(encoded));
}

bool boot_fault_pc_is_application(uint32_t pc)
{
  return boot_mem_addr_is_app_exec(pc);
}

static BootExceptionFrame *active_exception_frame(uint32_t exc_return)
{
  if ((exc_return & (1UL << 2U)) != 0U)
  {
    return (BootExceptionFrame *)__get_PSP();
  }

  return (BootExceptionFrame *)__get_MSP();
}

void boot_fault_handle(BootAppFaultReason reason, uint32_t exc_return)
{
  BootExceptionFrame *frame = active_exception_frame(exc_return);
  const uint32_t pc = frame->pc;
  const uint32_t lr = frame->lr;

  if (boot_fault_pc_is_application(pc) || boot_fault_pc_is_application(lr))
  {
    store_u32_setting(BOOT_KV_APP_VALID, 0U);
    store_u32_setting(BOOT_KV_APP_DISABLED, 1U);
    store_u32_setting(BOOT_KV_FAULT_REASON, (uint32_t)reason);
    store_u32_setting(BOOT_KV_FAULT_PC, pc);
    store_u32_setting(BOOT_KV_FAULT_LR, lr);
    (void)boot_metadata_save_to_flash();
    NVIC_SystemReset();
  }

  while (1)
  {
  }
}
