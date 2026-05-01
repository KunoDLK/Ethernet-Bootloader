#include "boot_fault.h"

#include "boot_memory_map.h"
#include "cmsis_gcc.h"
#include "stm32f4xx_hal.h"

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
    (void)boot_metadata_disable_app(reason, pc, lr);
    NVIC_SystemReset();
  }

  while (1)
  {
  }
}
