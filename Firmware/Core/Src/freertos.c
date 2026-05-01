/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include <stdio.h>

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

/* USER CODE END Variables */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* Call from GDB when halted: call debug_freertos_print_stack_watermarks()
 * (ITM/SWO or semihosting printf must work for output.) */
void debug_freertos_print_stack_watermarks(void)
{
  static TaskStatus_t rows[24];
  const UBaseType_t cap = (UBaseType_t)(sizeof(rows) / sizeof(rows[0]));
  const UBaseType_t n = uxTaskGetSystemState(rows, cap, NULL);

  (void)printf("Task stack high water (FreeRTOS \"words\", typically 4 bytes each)\r\n");
  for (UBaseType_t i = 0U; i < n; i++)
  {
    (void)printf("  %-16s  min_free_words=%u\r\n", rows[i].pcTaskName,
                   (unsigned int)rows[i].usStackHighWaterMark);
  }
}

/* USER CODE END Application */

