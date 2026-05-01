#include "resident_hardware.h"

#include "main.h"
#include "resident_led.h"

extern ADC_HandleTypeDef hadc1;
extern UART_HandleTypeDef huart5;
extern UART_HandleTypeDef huart3;
extern UART_HandleTypeDef huart6;

typedef struct
{
  GPIO_TypeDef *port;
  uint16_t pin;
} ResidentGpio;

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

void resident_hardware_init(void)
{
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
  (void)id;

  if (raw_value == 0)
  {
    return -1;
  }

  if (HAL_ADC_Start(&hadc1) != HAL_OK)
  {
    return -1;
  }

  if (HAL_ADC_PollForConversion(&hadc1, 10U) != HAL_OK)
  {
    (void)HAL_ADC_Stop(&hadc1);
    return -1;
  }

  *raw_value = HAL_ADC_GetValue(&hadc1);
  (void)HAL_ADC_Stop(&hadc1);
  return 0;
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
