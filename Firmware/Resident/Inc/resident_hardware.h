#ifndef RESIDENT_HARDWARE_H
#define RESIDENT_HARDWARE_H

#include "app_api.h"

typedef enum
{
  RESIDENT_HARDWARE_RAIL_A = 0,
  RESIDENT_HARDWARE_RAIL_B,
} ResidentHardwareRailId;

typedef enum
{
  RESIDENT_HARDWARE_RAIL_MODE_DISABLED = 0,
  RESIDENT_HARDWARE_RAIL_MODE_ENABLED,
  RESIDENT_HARDWARE_RAIL_MODE_FOLLOW_ESTOP,
} ResidentHardwareRailMode;

void resident_hardware_init(void);
int resident_hardware_gpio_write(AppGpioId id, bool level);
int resident_hardware_gpio_read(AppGpioId id, bool *level);
int resident_hardware_digital_input_read(AppDigitalInputId id, bool *level);
int resident_hardware_adc_read_raw(AppAdcChannelId id, uint32_t *raw_value);
int resident_hardware_uart_write(AppUartId id, const uint8_t *data, size_t length);
int resident_hardware_uart_read(AppUartId id, uint8_t *data, size_t max_length, size_t *length);
int resident_hardware_led_set_status(AppLedId id, AppLedColor color, AppLedMode mode, uint32_t blink_period_ms);
uint32_t resident_hardware_get_poll_period_ms(void);
int resident_hardware_set_poll_period_ms(uint32_t period_ms);
int resident_hardware_get_poll_period_text(char *text, size_t text_size);
int resident_hardware_get_estop_text(char *text, size_t text_size);
int resident_hardware_get_button_text(char *text, size_t text_size);
int resident_hardware_get_cpu_temp_text(char *text, size_t text_size);
int resident_hardware_get_rail_mode(ResidentHardwareRailId rail, ResidentHardwareRailMode *mode);
int resident_hardware_set_rail_mode(ResidentHardwareRailId rail, ResidentHardwareRailMode mode);
int resident_hardware_get_rail_mode_text(ResidentHardwareRailId rail, char *text, size_t text_size);
int resident_hardware_get_rail_output_text(ResidentHardwareRailId rail, char *text, size_t text_size);

#endif /* RESIDENT_HARDWARE_H */
