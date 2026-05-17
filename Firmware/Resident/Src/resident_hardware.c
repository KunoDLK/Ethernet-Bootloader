#include "resident_hardware.h"

#include "boot_metadata.h"
#include "cmsis_os.h"
#include "main.h"
#include "resident_led.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HARDWARE_POLL_TASK_STACK_WORDS  (256U)
#define HARDWARE_POLL_WAKE_FLAG         (1U)
#define ADC_MAX_COUNTS                  (4095U)
#define ADC_VREF_MV                     (3300U)
#define CPU_TEMP_V25_MV                 (760)
#define CPU_TEMP_AVG_SLOPE_UV_PER_C     (2500)

extern ADC_HandleTypeDef hadc1;
extern UART_HandleTypeDef huart5;
extern UART_HandleTypeDef huart3;
extern UART_HandleTypeDef huart6;

typedef struct
{
  GPIO_TypeDef *port;
  uint16_t pin;
} ResidentGpio;

typedef struct
{
  uint32_t poll_period_ms;
  bool estop_level;
  bool button_level;
  ResidentHardwareRailMode rail_a_mode;
  ResidentHardwareRailMode rail_b_mode;
  char poll_period_text[12];
  char estop_text[6];
  char button_text[6];
  char rail_a_mode_text[16];
  char rail_b_mode_text[16];
  char rail_a_output_text[16];
  char rail_b_output_text[16];
  char cpu_temp_text[16];
} ResidentHardwareSnapshot;

static ResidentHardwareSnapshot g_snapshot;
static volatile uint32_t g_poll_period_ms = BOOT_METADATA_DEFAULT_HARDWARE_POLL_PERIOD_MS;
static StaticTask_t g_poll_task_cb;
static StackType_t g_poll_task_stack[HARDWARE_POLL_TASK_STACK_WORDS];
static osThreadId_t g_poll_task_handle;
static StaticSemaphore_t g_adc_mutex_cb;
static osMutexId_t g_adc_mutex;

static uint32_t clamp_poll_period(uint32_t period_ms)
{
  return (period_ms < BOOT_METADATA_DEFAULT_HARDWARE_POLL_PERIOD_MS)
    ? BOOT_METADATA_DEFAULT_HARDWARE_POLL_PERIOD_MS
    : period_ms;
}

static uint32_t read_u32_le(const uint8_t *value)
{
  return (uint32_t)value[0] | ((uint32_t)value[1] << 8U) |
         ((uint32_t)value[2] << 16U) | ((uint32_t)value[3] << 24U);
}

static uint32_t metadata_read_u32(uint32_t key, uint32_t fallback)
{
  BootMetadataValueView value;
  if ((boot_metadata_get(key, &value) != 0) || (value.value_len != sizeof(uint32_t)))
  {
    return fallback;
  }
  return read_u32_le(value.value);
}

static uint8_t metadata_read_u8(uint32_t key, uint8_t fallback)
{
  BootMetadataValueView value;
  if ((boot_metadata_get(key, &value) != 0) || (value.value_len != sizeof(uint8_t)))
  {
    return fallback;
  }
  return value.value[0];
}

static const char *bool_text(bool value)
{
  return value ? "High" : "Low";
}

static const char *rail_mode_text(ResidentHardwareRailMode mode)
{
  switch (mode)
  {
    case RESIDENT_HARDWARE_RAIL_MODE_ENABLED:
      return "Enabled";
    case RESIDENT_HARDWARE_RAIL_MODE_FOLLOW_ESTOP:
      return "Follow Estop";
    case RESIDENT_HARDWARE_RAIL_MODE_DISABLED:
    default:
      return "Disabled";
  }
}

static int gpio_for_rail(ResidentHardwareRailId rail, ResidentGpio *gpio)
{
  if (gpio == 0)
  {
    return -1;
  }

  switch (rail)
  {
    case RESIDENT_HARDWARE_RAIL_A:
      gpio->port = Output_Rail_1_Tristate_Mode_GPIO_Port;
      gpio->pin = Output_Rail_1_Tristate_Mode_Pin;
      return 0;
    case RESIDENT_HARDWARE_RAIL_B:
      gpio->port = Output_Rail_2_Tristate_Mode_GPIO_Port;
      gpio->pin = Output_Rail_2_Tristate_Mode_Pin;
      return 0;
    default:
      return -1;
  }
}

static int gpio_for_id(AppGpioId id, ResidentGpio *gpio)
{
  if (gpio == 0)
  {
    return -1;
  }

  switch (id)
  {
    case APP_GPIO_EXPANSION_1:
      gpio->port = Expansion_GPIO_1_GPIO_Port;
      gpio->pin = Expansion_GPIO_1_Pin;
      return 0;
    case APP_GPIO_EXPANSION_2:
      gpio->port = Expansion_GPIO_2_GPIO_Port;
      gpio->pin = Expansion_GPIO_2_Pin;
      return 0;
    case APP_GPIO_EXPANSION_3:
      gpio->port = Expansion_GPIO_3_GPIO_Port;
      gpio->pin = Expansion_GPIO_3_Pin;
      return 0;
    case APP_GPIO_ENABLE_5V_RAIL:
      gpio->port = Enable_5V_Rail_GPIO_Port;
      gpio->pin = Enable_5V_Rail_Pin;
      return 0;
    case APP_GPIO_OUTPUT_RAIL_1_TRISTATE:
      gpio->port = Output_Rail_1_Tristate_Mode_GPIO_Port;
      gpio->pin = Output_Rail_1_Tristate_Mode_Pin;
      return 0;
    case APP_GPIO_OUTPUT_RAIL_2_TRISTATE:
      gpio->port = Output_Rail_2_Tristate_Mode_GPIO_Port;
      gpio->pin = Output_Rail_2_Tristate_Mode_Pin;
      return 0;
    default:
      return -1;
  }
}

static int input_for_id(AppDigitalInputId id, ResidentGpio *gpio)
{
  if (gpio == 0)
  {
    return -1;
  }

  switch (id)
  {
    case APP_DIGITAL_INPUT_BUTTON:
      gpio->port = Button_Input_GPIO_Port;
      gpio->pin = Button_Input_Pin;
      return 0;
    case APP_DIGITAL_INPUT_NESTOP:
      gpio->port = nESTOP_GPIO_Port;
      gpio->pin = nESTOP_Pin;
      return 0;
    default:
      return -1;
  }
}

static UART_HandleTypeDef *uart_for_id(AppUartId id)
{
  switch (id)
  {
    case APP_UART_TTL:
      return &huart5;
    case APP_UART_RS485:
      return &huart3;
    case APP_UART_MODBUS:
      return &huart6;
    default:
      return 0;
  }
}

static uint32_t adc_channel_for_id(AppAdcChannelId id)
{
  return (id == APP_ADC_OUTPUT_RAIL_2_CURRENT) ? ADC_CHANNEL_9 : ADC_CHANNEL_8;
}

static int adc_read_channel(uint32_t channel, uint32_t sampling_time, uint32_t *raw_value)
{
  ADC_ChannelConfTypeDef config = {0};
  int result = -1;

  if (raw_value == 0)
  {
    return -1;
  }

  if ((g_adc_mutex != 0) && (osMutexAcquire(g_adc_mutex, osWaitForever) != osOK))
  {
    return -1;
  }

  config.Channel = channel;
  config.Rank = 1U;
  config.SamplingTime = sampling_time;
  if (HAL_ADC_ConfigChannel(&hadc1, &config) != HAL_OK)
  {
    goto done;
  }

  if (HAL_ADC_Start(&hadc1) != HAL_OK)
  {
    goto done;
  }

  if (HAL_ADC_PollForConversion(&hadc1, 10U) != HAL_OK)
  {
    (void)HAL_ADC_Stop(&hadc1);
    goto done;
  }

  *raw_value = HAL_ADC_GetValue(&hadc1);
  (void)HAL_ADC_Stop(&hadc1);
  result = 0;

done:
  if (g_adc_mutex != 0)
  {
    (void)osMutexRelease(g_adc_mutex);
  }
  return result;
}

static void format_amps_from_raw(uint32_t raw, char *text, size_t text_size)
{
  uint32_t milliamps = (raw * ADC_VREF_MV) / ADC_MAX_COUNTS;
  (void)snprintf(text, text_size, "%lu.%03lu", (unsigned long)(milliamps / 1000U),
                 (unsigned long)(milliamps % 1000U));
}

static void format_cpu_temp_from_raw(uint32_t raw, char *text, size_t text_size)
{
  int32_t sensor_mv = (int32_t)((raw * ADC_VREF_MV) / ADC_MAX_COUNTS);
  int32_t temp_c_x10 = 250 + (((sensor_mv - CPU_TEMP_V25_MV) * 10000) / CPU_TEMP_AVG_SLOPE_UV_PER_C);
  (void)snprintf(text, text_size, "%ld.%ld C", (long)(temp_c_x10 / 10), labs(temp_c_x10 % 10));
}

static void store_snapshot(const ResidentHardwareSnapshot *snapshot)
{
  __disable_irq();
  g_snapshot = *snapshot;
  __enable_irq();
}

static ResidentHardwareSnapshot load_snapshot(void)
{
  ResidentHardwareSnapshot snapshot;

  __disable_irq();
  snapshot = g_snapshot;
  __enable_irq();

  return snapshot;
}

static int apply_rail_mode(ResidentHardwareRailId rail, ResidentHardwareRailMode mode)
{
  ResidentGpio gpio;
  GPIO_InitTypeDef init = {0};

  if (gpio_for_rail(rail, &gpio) != 0)
  {
    return -1;
  }

  init.Pin = gpio.pin;
  init.Pull = GPIO_NOPULL;
  init.Speed = GPIO_SPEED_FREQ_LOW;

  if (mode == RESIDENT_HARDWARE_RAIL_MODE_FOLLOW_ESTOP)
  {
    /* Release the pin to high-Z (input); do not drive low first or we glitch the line. */
    init.Mode = GPIO_MODE_INPUT;
    HAL_GPIO_Init(gpio.port, &init);
    return 0;
  }

  /* Latch desired level in ODR while pin may still be input, then enable push-pull output. */
  HAL_GPIO_WritePin(gpio.port, gpio.pin,
                    (mode == RESIDENT_HARDWARE_RAIL_MODE_ENABLED) ? GPIO_PIN_SET : GPIO_PIN_RESET);
  init.Mode = GPIO_MODE_OUTPUT_PP;
  HAL_GPIO_Init(gpio.port, &init);
  return 0;
}

static void resident_hardware_poll_once(void)
{
  ResidentHardwareSnapshot next = load_snapshot();
  uint32_t raw;

  next.poll_period_ms = g_poll_period_ms;
  (void)snprintf(next.poll_period_text, sizeof(next.poll_period_text), "%lu",
                 (unsigned long)next.poll_period_ms);

  next.estop_level = (HAL_GPIO_ReadPin(nESTOP_GPIO_Port, nESTOP_Pin) == GPIO_PIN_SET);
  next.button_level = (HAL_GPIO_ReadPin(Button_Input_GPIO_Port, Button_Input_Pin) == GPIO_PIN_SET);
  (void)snprintf(next.estop_text, sizeof(next.estop_text), "%s", bool_text(next.estop_level));
  (void)snprintf(next.button_text, sizeof(next.button_text), "%s", bool_text(next.button_level));
  (void)snprintf(next.rail_a_mode_text, sizeof(next.rail_a_mode_text), "%s", rail_mode_text(next.rail_a_mode));
  (void)snprintf(next.rail_b_mode_text, sizeof(next.rail_b_mode_text), "%s", rail_mode_text(next.rail_b_mode));

  if (adc_read_channel(ADC_CHANNEL_8, ADC_SAMPLETIME_3CYCLES, &raw) == 0)
  {
    format_amps_from_raw(raw, next.rail_a_output_text, sizeof(next.rail_a_output_text));
  }
  else
  {
    (void)snprintf(next.rail_a_output_text, sizeof(next.rail_a_output_text), "unavailable");
  }

  if (adc_read_channel(ADC_CHANNEL_9, ADC_SAMPLETIME_3CYCLES, &raw) == 0)
  {
    format_amps_from_raw(raw, next.rail_b_output_text, sizeof(next.rail_b_output_text));
  }
  else
  {
    (void)snprintf(next.rail_b_output_text, sizeof(next.rail_b_output_text), "unavailable");
  }

  if (adc_read_channel(ADC_CHANNEL_TEMPSENSOR, ADC_SAMPLETIME_480CYCLES, &raw) == 0)
  {
    format_cpu_temp_from_raw(raw, next.cpu_temp_text, sizeof(next.cpu_temp_text));
  }
  else
  {
    (void)snprintf(next.cpu_temp_text, sizeof(next.cpu_temp_text), "unavailable");
  }

  store_snapshot(&next);
}

static void resident_hardware_poll_task(void *argument)
{
  (void)argument;

  for (;;)
  {
    resident_hardware_poll_once();
    (void)osThreadFlagsWait(HARDWARE_POLL_WAKE_FLAG, osFlagsWaitAny, g_poll_period_ms);
  }
}

void resident_hardware_init(void)
{
  ResidentHardwareSnapshot initial;

  memset(&initial, 0, sizeof(initial));
  initial.poll_period_ms = clamp_poll_period(
      metadata_read_u32(BOOT_KV_HW_POLL_MS, BOOT_METADATA_DEFAULT_HARDWARE_POLL_PERIOD_MS));
  initial.rail_a_mode = (ResidentHardwareRailMode)metadata_read_u8(BOOT_KV_RAIL_A,
                                                                   BOOT_METADATA_DEFAULT_RAIL_MODE);
  initial.rail_b_mode = (ResidentHardwareRailMode)metadata_read_u8(BOOT_KV_RAIL_B,
                                                                   BOOT_METADATA_DEFAULT_RAIL_MODE);
  (void)snprintf(initial.poll_period_text, sizeof(initial.poll_period_text), "%lu",
                 (unsigned long)initial.poll_period_ms);
  (void)snprintf(initial.estop_text, sizeof(initial.estop_text), "Low");
  (void)snprintf(initial.button_text, sizeof(initial.button_text), "Low");
  (void)snprintf(initial.rail_a_mode_text, sizeof(initial.rail_a_mode_text), "%s",
                 rail_mode_text(initial.rail_a_mode));
  (void)snprintf(initial.rail_b_mode_text, sizeof(initial.rail_b_mode_text), "%s",
                 rail_mode_text(initial.rail_b_mode));
  (void)snprintf(initial.rail_a_output_text, sizeof(initial.rail_a_output_text), "unavailable");
  (void)snprintf(initial.rail_b_output_text, sizeof(initial.rail_b_output_text), "unavailable");
  (void)snprintf(initial.cpu_temp_text, sizeof(initial.cpu_temp_text), "unavailable");
  g_poll_period_ms = initial.poll_period_ms;
  store_snapshot(&initial);

  const osMutexAttr_t adc_mutex_attr = {
    .name = "HardwareAdc",
    .cb_mem = &g_adc_mutex_cb,
    .cb_size = sizeof(g_adc_mutex_cb),
  };
  g_adc_mutex = osMutexNew(&adc_mutex_attr);

  /* Board needs the 5 V rail enabled for downstream analog / outputs to operate. */
  HAL_GPIO_WritePin(Enable_5V_Rail_GPIO_Port, Enable_5V_Rail_Pin, GPIO_PIN_SET);

  (void)apply_rail_mode(RESIDENT_HARDWARE_RAIL_A, initial.rail_a_mode);
  (void)apply_rail_mode(RESIDENT_HARDWARE_RAIL_B, initial.rail_b_mode);
  resident_hardware_poll_once();

  const osThreadAttr_t poll_task_attr = {
    .name = "HardwarePoll",
    .cb_mem = &g_poll_task_cb,
    .cb_size = sizeof(g_poll_task_cb),
    .stack_mem = g_poll_task_stack,
    .stack_size = sizeof(g_poll_task_stack),
    .priority = osPriorityLow,
  };
  g_poll_task_handle = osThreadNew(resident_hardware_poll_task, 0, &poll_task_attr);
}

int resident_hardware_gpio_write(AppGpioId id, bool level)
{
  ResidentGpio gpio;
  if (gpio_for_id(id, &gpio) != 0)
  {
    return -1;
  }

  HAL_GPIO_WritePin(gpio.port, gpio.pin, level ? GPIO_PIN_SET : GPIO_PIN_RESET);
  return 0;
}

int resident_hardware_gpio_read(AppGpioId id, bool *level)
{
  ResidentGpio gpio;
  if ((level == 0) || (gpio_for_id(id, &gpio) != 0))
  {
    return -1;
  }

  *level = (HAL_GPIO_ReadPin(gpio.port, gpio.pin) == GPIO_PIN_SET);
  return 0;
}

int resident_hardware_digital_input_read(AppDigitalInputId id, bool *level)
{
  ResidentGpio gpio;
  if ((level == 0) || (input_for_id(id, &gpio) != 0))
  {
    return -1;
  }

  *level = (HAL_GPIO_ReadPin(gpio.port, gpio.pin) == GPIO_PIN_SET);
  return 0;
}

int resident_hardware_adc_read_raw(AppAdcChannelId id, uint32_t *raw_value)
{
  return adc_read_channel(adc_channel_for_id(id), ADC_SAMPLETIME_3CYCLES, raw_value);
}

int resident_hardware_uart_write(AppUartId id, const uint8_t *data, size_t length)
{
  UART_HandleTypeDef *uart = uart_for_id(id);
  if ((uart == 0) || (data == 0) || (length > UINT16_MAX))
  {
    return -1;
  }

  return (HAL_UART_Transmit(uart, (uint8_t *)data, (uint16_t)length, 100U) == HAL_OK) ? 0 : -1;
}

int resident_hardware_uart_read(AppUartId id, uint8_t *data, size_t max_length, size_t *length)
{
  UART_HandleTypeDef *uart = uart_for_id(id);
  if ((uart == 0) || (data == 0) || (length == 0) || (max_length > UINT16_MAX))
  {
    return -1;
  }

  HAL_StatusTypeDef status = HAL_UART_Receive(uart, data, (uint16_t)max_length, 1U);
  *length = (status == HAL_OK) ? max_length : 0U;
  return (status == HAL_OK) ? 0 : -1;
}

int resident_hardware_led_set_status(AppLedId id, AppLedColor color, AppLedMode mode, uint32_t blink_period_ms)
{
  return resident_led_set_status(id, color, mode, blink_period_ms);
}

uint32_t resident_hardware_get_poll_period_ms(void)
{
  return g_poll_period_ms;
}

int resident_hardware_set_poll_period_ms(uint32_t period_ms)
{
  ResidentHardwareSnapshot snapshot;

  period_ms = clamp_poll_period(period_ms);
  g_poll_period_ms = period_ms;
  snapshot = load_snapshot();
  snapshot.poll_period_ms = period_ms;
  (void)snprintf(snapshot.poll_period_text, sizeof(snapshot.poll_period_text), "%lu",
                 (unsigned long)period_ms);
  store_snapshot(&snapshot);

  if (g_poll_task_handle != 0)
  {
    (void)osThreadFlagsSet(g_poll_task_handle, HARDWARE_POLL_WAKE_FLAG);
  }

  return 0;
}

int resident_hardware_get_poll_period_text(char *text, size_t text_size)
{
  ResidentHardwareSnapshot snapshot = load_snapshot();
  if ((text == 0) || (text_size == 0U))
  {
    return -1;
  }

  (void)snprintf(text, text_size, "%s", snapshot.poll_period_text);
  return 0;
}

int resident_hardware_get_estop_text(char *text, size_t text_size)
{
  ResidentHardwareSnapshot snapshot = load_snapshot();
  if ((text == 0) || (text_size == 0U))
  {
    return -1;
  }

  (void)snprintf(text, text_size, "%s", snapshot.estop_text);
  return 0;
}

int resident_hardware_get_button_text(char *text, size_t text_size)
{
  ResidentHardwareSnapshot snapshot = load_snapshot();
  if ((text == 0) || (text_size == 0U))
  {
    return -1;
  }

  (void)snprintf(text, text_size, "%s", snapshot.button_text);
  return 0;
}

int resident_hardware_get_cpu_temp_text(char *text, size_t text_size)
{
  ResidentHardwareSnapshot snapshot = load_snapshot();
  if ((text == 0) || (text_size == 0U))
  {
    return -1;
  }

  (void)snprintf(text, text_size, "%s", snapshot.cpu_temp_text);
  return 0;
}

int resident_hardware_get_rail_mode(ResidentHardwareRailId rail, ResidentHardwareRailMode *mode)
{
  ResidentHardwareSnapshot snapshot = load_snapshot();
  if (mode == 0)
  {
    return -1;
  }

  if (rail == RESIDENT_HARDWARE_RAIL_A)
  {
    *mode = snapshot.rail_a_mode;
    return 0;
  }

  if (rail == RESIDENT_HARDWARE_RAIL_B)
  {
    *mode = snapshot.rail_b_mode;
    return 0;
  }

  return -1;
}

int resident_hardware_set_rail_mode(ResidentHardwareRailId rail, ResidentHardwareRailMode mode)
{
  ResidentHardwareSnapshot snapshot;
  ResidentHardwareRailMode previous_mode;

  if (mode > RESIDENT_HARDWARE_RAIL_MODE_FOLLOW_ESTOP)
  {
    return -1;
  }

  snapshot = load_snapshot();
  if (rail == RESIDENT_HARDWARE_RAIL_A)
  {
    previous_mode = snapshot.rail_a_mode;
  }
  else if (rail == RESIDENT_HARDWARE_RAIL_B)
  {
    previous_mode = snapshot.rail_b_mode;
  }
  else
  {
    return -1;
  }

  if (mode == previous_mode)
  {
    return 0;
  }

  if (apply_rail_mode(rail, mode) != 0)
  {
    return -1;
  }

  const uint32_t key = (rail == RESIDENT_HARDWARE_RAIL_A) ? BOOT_KV_RAIL_A : BOOT_KV_RAIL_B;
  const uint8_t stored_mode = (uint8_t)mode;
  if ((boot_metadata_set(key, &stored_mode, sizeof(stored_mode)) != 0) ||
      (boot_metadata_save_to_flash() != 0))
  {
    (void)apply_rail_mode(rail, previous_mode);
    return -1;
  }

  snapshot = load_snapshot();
  if (rail == RESIDENT_HARDWARE_RAIL_A)
  {
    snapshot.rail_a_mode = mode;
    (void)snprintf(snapshot.rail_a_mode_text, sizeof(snapshot.rail_a_mode_text), "%s", rail_mode_text(mode));
  }
  else
  {
    snapshot.rail_b_mode = mode;
    (void)snprintf(snapshot.rail_b_mode_text, sizeof(snapshot.rail_b_mode_text), "%s", rail_mode_text(mode));
  }

  store_snapshot(&snapshot);
  if (g_poll_task_handle != 0)
  {
    (void)osThreadFlagsSet(g_poll_task_handle, HARDWARE_POLL_WAKE_FLAG);
  }

  return 0;
}

int resident_hardware_get_rail_mode_text(ResidentHardwareRailId rail, char *text, size_t text_size)
{
  ResidentHardwareSnapshot snapshot = load_snapshot();
  if ((text == 0) || (text_size == 0U))
  {
    return -1;
  }

  if (rail == RESIDENT_HARDWARE_RAIL_A)
  {
    (void)snprintf(text, text_size, "%s", snapshot.rail_a_mode_text);
    return 0;
  }

  if (rail == RESIDENT_HARDWARE_RAIL_B)
  {
    (void)snprintf(text, text_size, "%s", snapshot.rail_b_mode_text);
    return 0;
  }

  return -1;
}

int resident_hardware_get_rail_output_text(ResidentHardwareRailId rail, char *text, size_t text_size)
{
  ResidentHardwareSnapshot snapshot = load_snapshot();
  if ((text == 0) || (text_size == 0U))
  {
    return -1;
  }

  if (rail == RESIDENT_HARDWARE_RAIL_A)
  {
    (void)snprintf(text, text_size, "%s", snapshot.rail_a_output_text);
    return 0;
  }

  if (rail == RESIDENT_HARDWARE_RAIL_B)
  {
    (void)snprintf(text, text_size, "%s", snapshot.rail_b_output_text);
    return 0;
  }

  return -1;
}
