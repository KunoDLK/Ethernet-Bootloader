/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define nESTOP_Pin GPIO_PIN_15
#define nESTOP_GPIO_Port GPIOC
#define Output_Rail_1_Tristate_Mode_Pin GPIO_PIN_2
#define Output_Rail_1_Tristate_Mode_GPIO_Port GPIOC
#define Output_Rail_2_Tristate_Mode_Pin GPIO_PIN_3
#define Output_Rail_2_Tristate_Mode_GPIO_Port GPIOC
#define Button_Input_Pin GPIO_PIN_4
#define Button_Input_GPIO_Port GPIOA
#define Output_Rail_1_Current_Sense_Pin GPIO_PIN_0
#define Output_Rail_1_Current_Sense_GPIO_Port GPIOB
#define Output_Rail_2_Current_Sense_Pin GPIO_PIN_1
#define Output_Rail_2_Current_Sense_GPIO_Port GPIOB
#define RS485_TX_Pin GPIO_PIN_8
#define RS485_TX_GPIO_Port GPIOD
#define RS485_RX_Pin GPIO_PIN_9
#define RS485_RX_GPIO_Port GPIOD
#define Modbus_TX_Pin GPIO_PIN_6
#define Modbus_TX_GPIO_Port GPIOC
#define Modbus_RX_Pin GPIO_PIN_7
#define Modbus_RX_GPIO_Port GPIOC
#define USART8_RS485_Mode_Pin GPIO_PIN_8
#define USART8_RS485_Mode_GPIO_Port GPIOC
#define Expansion_GPIO_1_Pin GPIO_PIN_9
#define Expansion_GPIO_1_GPIO_Port GPIOA
#define Expansion_GPIO_2_Pin GPIO_PIN_10
#define Expansion_GPIO_2_GPIO_Port GPIOA
#define TTL_TX_Pin GPIO_PIN_12
#define TTL_TX_GPIO_Port GPIOC
#define UART5_TTL_TX_Enable_Pin GPIO_PIN_0
#define UART5_TTL_TX_Enable_GPIO_Port GPIOD
#define UART5_TTL_RX_Disable_Pin GPIO_PIN_1
#define UART5_TTL_RX_Disable_GPIO_Port GPIOD
#define TTL_RX_Pin GPIO_PIN_2
#define TTL_RX_GPIO_Port GPIOD
#define Enable_5V_Rail_Pin GPIO_PIN_8
#define Enable_5V_Rail_GPIO_Port GPIOB
#define Expansion_GPIO_3_Pin GPIO_PIN_9
#define Expansion_GPIO_3_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
