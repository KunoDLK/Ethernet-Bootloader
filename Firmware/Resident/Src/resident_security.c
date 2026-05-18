#include "resident_security.h"

void resident_security_init(void)
{
}

bool resident_security_programming_is_unlocked(void)
{
  return true;
}

void resident_security_set_programming_unlocked(bool unlocked, uint32_t timeout_ms)
{
  (void)unlocked;
  (void)timeout_ms;
}

void resident_security_poll(void)
{
}
