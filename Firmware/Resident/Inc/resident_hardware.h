#ifndef RESIDENT_HARDWARE_H
#define RESIDENT_HARDWARE_H

#include "app_api.h"

void resident_hardware_init(void);
int resident_hardware_gpio_write(AppGpioId id, bool level);
int resident_hardware_gpio_read(AppGpioId id, bool *level);
int resident_hardware_digital_input_read(AppDigitalInputId id, bool *level);
int resident_hardware_adc_read_raw(AppAdcChannelId id, uint32_t *raw_value);
int resident_hardware_uart_write(AppUartId id, const uint8_t *data, size_t length);
int resident_hardware_uart_read(AppUartId id, uint8_t *data, size_t max_length, size_t *length);
int resident_hardware_led_set_status(AppLedId id, AppLedColor color, AppLedMode mode, uint32_t blink_period_ms);

#endif /* RESIDENT_HARDWARE_H */
