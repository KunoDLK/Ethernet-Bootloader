#include "resident_security.h"

#include "cmsis_os.h"

static bool g_programming_unlocked;
static uint32_t g_unlock_until_ms;

void resident_security_init(void)
{
  g_programming_unlocked = false;
  g_unlock_until_ms = 0U;
}

bool resident_security_programming_is_unlocked(void)
{
  resident_security_poll();
  return g_programming_unlocked;
}

void resident_security_set_programming_unlocked(bool unlocked, uint32_t timeout_ms)
{
  g_programming_unlocked = unlocked;
  g_unlock_until_ms = unlocked ? (osKernelGetTickCount() + timeout_ms) : 0U;
}

void resident_security_poll(void)
{
  if (g_programming_unlocked && (g_unlock_until_ms != 0U))
  {
    const uint32_t now = osKernelGetTickCount();
    if ((int32_t)(now - g_unlock_until_ms) >= 0)
    {
      g_programming_unlocked = false;
      g_unlock_until_ms = 0U;
    }
  }
}
