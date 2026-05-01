#include "resident_led.h"

#include "FreeRTOS.h"
#include "cmsis_os.h"
#include "main.h"
#include "task.h"

#include <string.h>

#ifndef RESIDENT_LED_TASK_STACK_WORDS
#define RESIDENT_LED_TASK_STACK_WORDS 128U
#endif

#ifndef RESIDENT_LED_DEFAULT_BLINK_PERIOD_MS
#define RESIDENT_LED_DEFAULT_BLINK_PERIOD_MS 500U
#endif

typedef struct
{
  GPIO_TypeDef *red_port;
  uint16_t red_pin;
  GPIO_TypeDef *green_port;
  uint16_t green_pin;
} ResidentLedPins;

typedef struct
{
  AppLedColor color;
  AppLedMode mode;
  uint32_t blink_period_ms;
  uint32_t next_toggle_ms;
  bool blink_on;
} ResidentLedState;

static const ResidentLedPins k_led_pins[APP_LED_COUNT] = {
  {
    .red_port = Engineer_LED_Red_GPIO_Port,
    .red_pin = Engineer_LED_Red_Pin,
    .green_port = Engineer_LED_Green_GPIO_Port,
    .green_pin = Engineer_LED_Green_Pin,
  },
  {
    .red_port = 0,
    .red_pin = 0,
    .green_port = 0,
    .green_pin = 0,
  },
};

static ResidentLedState g_led_state[APP_LED_COUNT];
static StaticTask_t g_led_task_cb;
static StackType_t g_led_task_stack[RESIDENT_LED_TASK_STACK_WORDS];

static bool led_id_is_valid(AppLedId id)
{
  return ((uint32_t)id < (uint32_t)APP_LED_COUNT) &&
         ((k_led_pins[id].red_port != 0) || (k_led_pins[id].green_port != 0));
}

static void led_write_pins(AppLedId id, bool red_on, bool green_on)
{
  const ResidentLedPins *pins = &k_led_pins[id];

  if (pins->red_port != 0)
  {
    HAL_GPIO_WritePin(pins->red_port, pins->red_pin, red_on ? GPIO_PIN_SET : GPIO_PIN_RESET);
  }

  if (pins->green_port != 0)
  {
    HAL_GPIO_WritePin(pins->green_port, pins->green_pin, green_on ? GPIO_PIN_SET : GPIO_PIN_RESET);
  }
}

static void led_apply_state(AppLedId id)
{
  const ResidentLedState *state = &g_led_state[id];
  const bool output_on = (state->mode == APP_LED_MODE_STATIC) ||
                         ((state->mode == APP_LED_MODE_BLINK) && state->blink_on);
  bool red_on = false;
  bool green_on = false;

  if (output_on)
  {
    red_on = (state->color == APP_LED_COLOR_RED) || (state->color == APP_LED_COLOR_BOTH);
    green_on = (state->color == APP_LED_COLOR_GREEN) || (state->color == APP_LED_COLOR_BOTH);
  }

  led_write_pins(id, red_on, green_on);
}

static void resident_led_task(void *argument)
{
  (void)argument;

  for (;;)
  {
    const uint32_t now = osKernelGetTickCount();

    taskENTER_CRITICAL();
    for (uint32_t i = 0U; i < (uint32_t)APP_LED_COUNT; i++)
    {
      if (!led_id_is_valid((AppLedId)i))
      {
        continue;
      }

      ResidentLedState *state = &g_led_state[i];
      if ((state->mode == APP_LED_MODE_BLINK) && ((int32_t)(now - state->next_toggle_ms) >= 0))
      {
        state->blink_on = !state->blink_on;
        state->next_toggle_ms = now + (state->blink_period_ms / 2U);
        led_apply_state((AppLedId)i);
      }
    }
    taskEXIT_CRITICAL();

    osDelay(10U);
  }
}

void resident_led_init(void)
{
  memset(g_led_state, 0, sizeof(g_led_state));

  for (uint32_t i = 0U; i < (uint32_t)APP_LED_COUNT; i++)
  {
    g_led_state[i].color = APP_LED_COLOR_GREEN;
    g_led_state[i].mode = APP_LED_MODE_OFF;
    g_led_state[i].blink_period_ms = RESIDENT_LED_DEFAULT_BLINK_PERIOD_MS;
    led_write_pins((AppLedId)i, false, false);
  }

  const osThreadAttr_t task_attr = {
    .name = "LedStatus",
    .cb_mem = &g_led_task_cb,
    .cb_size = sizeof(g_led_task_cb),
    .stack_mem = g_led_task_stack,
    .stack_size = sizeof(g_led_task_stack),
    .priority = osPriorityLow,
  };
  (void)osThreadNew(resident_led_task, 0, &task_attr);
}

int resident_led_set_status(AppLedId id, AppLedColor color, AppLedMode mode, uint32_t blink_period_ms)
{
  if (!led_id_is_valid(id) ||
      (color > APP_LED_COLOR_BOTH) ||
      (mode > APP_LED_MODE_BLINK))
  {
    return -1;
  }

  if (blink_period_ms == 0U)
  {
    blink_period_ms = RESIDENT_LED_DEFAULT_BLINK_PERIOD_MS;
  }
  else if (blink_period_ms < 20U)
  {
    blink_period_ms = 20U;
  }

  taskENTER_CRITICAL();
  ResidentLedState *state = &g_led_state[id];
  state->color = color;
  state->mode = mode;
  state->blink_period_ms = blink_period_ms;
  state->blink_on = (mode == APP_LED_MODE_BLINK) || (mode == APP_LED_MODE_STATIC);
  state->next_toggle_ms = osKernelGetTickCount() + (blink_period_ms / 2U);

  led_apply_state(id);
  taskEXIT_CRITICAL();
  return 0;
}
