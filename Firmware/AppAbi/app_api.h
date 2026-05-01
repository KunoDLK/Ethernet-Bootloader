#ifndef APP_API_H
#define APP_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_API_VERSION                 (1U)

typedef void *AppTaskHandle;
typedef void *AppMutexHandle;
typedef void *AppQueueHandle;
typedef void *AppUdpHandle;
typedef void *AppDeviceTreeMount;

typedef void (*AppTaskEntry)(void *argument);
typedef void (*AppUdpReceiveCallback)(const uint8_t *payload, size_t length,
                                      const uint8_t remote_ip[4], uint16_t remote_port,
                                      void *context);
typedef int (*AppDeviceTreeActionCallback)(const char *args, void *context);

typedef struct
{
  AppTaskHandle (*create_task)(const char *name, AppTaskEntry entry, void *argument,
                               uint32_t stack_bytes, uint32_t priority);
  void (*delete_task)(AppTaskHandle task);
  void (*delay_ms)(uint32_t delay_ms);
  uint32_t (*uptime_ms)(void);
} AppApiRtos;

typedef struct
{
  int (*open_udp)(uint16_t local_port, AppUdpReceiveCallback callback,
                  void *context, AppUdpHandle *handle);
  int (*send_udp)(AppUdpHandle handle, const uint8_t remote_ip[4], uint16_t remote_port,
                  const void *payload, size_t length);
  void (*close_udp)(AppUdpHandle handle);
  int (*get_ipv4)(uint8_t ip[4], uint8_t netmask[4], uint8_t gateway[4]);
  int (*get_mac)(uint8_t mac[6]);
  bool (*link_is_up)(void);
} AppApiNet;

typedef struct
{
  int (*mount)(const char *name, AppDeviceTreeMount *mount);
  int (*set_value)(AppDeviceTreeMount mount, const char *path, const char *value);
  int (*register_action)(AppDeviceTreeMount mount, const char *path,
                         AppDeviceTreeActionCallback callback, void *context);
  void (*unmount)(AppDeviceTreeMount mount);
} AppApiDeviceTree;

typedef enum
{
  APP_GPIO_EXPANSION_1 = 0,
  APP_GPIO_EXPANSION_2,
  APP_GPIO_EXPANSION_3,
  APP_GPIO_ENABLE_5V_RAIL,
  APP_GPIO_OUTPUT_RAIL_1_TRISTATE,
  APP_GPIO_OUTPUT_RAIL_2_TRISTATE,
} AppGpioId;

typedef enum
{
  APP_DIGITAL_INPUT_BUTTON = 0,
  APP_DIGITAL_INPUT_NESTOP,
} AppDigitalInputId;

typedef enum
{
  APP_ADC_OUTPUT_RAIL_1_CURRENT = 0,
  APP_ADC_OUTPUT_RAIL_2_CURRENT,
} AppAdcChannelId;

typedef enum
{
  APP_UART_TTL = 0,
  APP_UART_RS485,
  APP_UART_MODBUS,
} AppUartId;

typedef enum
{
  APP_LED_STATUS_0 = 0,
  APP_LED_STATUS_1,
  APP_LED_COUNT,
} AppLedId;

typedef enum
{
  APP_LED_COLOR_RED = 0,
  APP_LED_COLOR_GREEN,
  APP_LED_COLOR_BOTH,
} AppLedColor;

typedef enum
{
  APP_LED_MODE_OFF = 0,
  APP_LED_MODE_STATIC,
  APP_LED_MODE_BLINK,
} AppLedMode;

typedef struct
{
  int (*gpio_write)(AppGpioId id, bool level);
  int (*gpio_read)(AppGpioId id, bool *level);
  int (*digital_input_read)(AppDigitalInputId id, bool *level);
  int (*adc_read_raw)(AppAdcChannelId id, uint32_t *raw_value);
  int (*uart_write)(AppUartId id, const uint8_t *data, size_t length);
  int (*uart_read)(AppUartId id, uint8_t *data, size_t max_length, size_t *length);
  int (*led_set_status)(AppLedId id, AppLedColor color, AppLedMode mode, uint32_t blink_period_ms);
} AppApiHardware;

typedef struct
{
  int (*read)(const char *key, void *data, size_t max_length, size_t *length);
  int (*write)(const char *key, const void *data, size_t length);
} AppApiStorage;

typedef struct
{
  void (*write)(const char *message);
} AppApiLog;

typedef struct
{
  uint32_t version;
  AppApiRtos rtos;
  AppApiNet net;
  AppApiDeviceTree device_tree;
  AppApiHardware hardware;
  AppApiStorage storage;
  AppApiLog log;
} AppApi;

typedef int (*AppEntryPoint)(const AppApi *api);

#endif /* APP_API_H */
