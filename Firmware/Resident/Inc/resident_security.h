#ifndef RESIDENT_SECURITY_H
#define RESIDENT_SECURITY_H

#include <stdbool.h>
#include <stdint.h>

void resident_security_init(void);
bool resident_security_programming_is_unlocked(void);
void resident_security_set_programming_unlocked(bool unlocked, uint32_t timeout_ms);
void resident_security_poll(void);

#endif /* RESIDENT_SECURITY_H */
