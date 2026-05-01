#ifndef RESIDENT_LED_H
#define RESIDENT_LED_H

#include "app_api.h"

void resident_led_init(void);
int resident_led_set_status(AppLedId id, AppLedColor color, AppLedMode mode, uint32_t blink_period_ms);

#endif /* RESIDENT_LED_H */
