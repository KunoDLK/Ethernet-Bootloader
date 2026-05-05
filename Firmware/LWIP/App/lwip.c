/* USER CODE BEGIN Header */
/**
 ******************************************************************************
  * File Name          : LWIP.c
  * Description        : This file provides initialization code for LWIP
  *                      middleWare.
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
#include "lwip.h"
#include "boot_metadata.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#if defined ( __CC_ARM )  /* MDK ARM Compiler */
#include "lwip/sio.h"
#endif /* MDK ARM Compiler */
#include "ethernetif.h"
#include "FreeRTOS.h"
#if LWIP_DHCP
#include "lwip/dhcp.h"
#endif
#include "lwipopts.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */
/* Private function prototypes -----------------------------------------------*/
static void ethernet_link_status_updated(struct netif *netif);
/* ETH Variables initialization ----------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/* Variables Initialization */
struct netif gnetif;
ip4_addr_t ipaddr;
ip4_addr_t netmask;
ip4_addr_t gw;
/* USER CODE BEGIN OS_THREAD_ATTR_CMSIS_RTOS_V2 */
#define INTERFACE_THREAD_STACK_SIZE ( 1024 )
static StaticTask_t EthLinkTaskControlBlock;
static StackType_t EthLinkTaskStack[INTERFACE_THREAD_STACK_SIZE / sizeof(StackType_t)];
osThreadAttr_t attributes;
/* USER CODE END OS_THREAD_ATTR_CMSIS_RTOS_V2 */

/* USER CODE BEGIN 2 */

/* USER CODE END 2 */

/**
  * LwIP initialization function
  */
void MX_LWIP_Init(void)
{
  /* Initialize the LwIP stack with RTOS */
  tcpip_init( NULL, NULL );

  uint8_t stored_ip[4];
  uint8_t stored_netmask[4];
  uint8_t stored_gw[4];
  const uint8_t use_dhcp = boot_metadata_get_net_dhcp_enabled();

  if (use_dhcp != 0U)
  {
    memset(stored_ip, 0, sizeof(stored_ip));
    memset(stored_netmask, 0, sizeof(stored_netmask));
    memset(stored_gw, 0, sizeof(stored_gw));
    printf("lwIP: DHCP enabled (preference from metadata)\r\n");
  }
  else
  {
    boot_metadata_get_ipv4(stored_ip, stored_netmask, stored_gw);
    if (boot_metadata_ipv4_is_unusable_reserved(stored_ip) ||
        boot_metadata_ipv4_is_unusable_reserved(stored_netmask) ||
        boot_metadata_ipv4_is_unusable_reserved(stored_gw))
    {
      printf("lwIP: stored IPv4/mask/gateway is 0.0.0.0 or 255.255.255.255; using build defaults\r\n");
      stored_ip[0] = RESIDENT_IPV4_ADDR0;
      stored_ip[1] = RESIDENT_IPV4_ADDR1;
      stored_ip[2] = RESIDENT_IPV4_ADDR2;
      stored_ip[3] = RESIDENT_IPV4_ADDR3;
      stored_netmask[0] = RESIDENT_IPV4_MASK0;
      stored_netmask[1] = RESIDENT_IPV4_MASK1;
      stored_netmask[2] = RESIDENT_IPV4_MASK2;
      stored_netmask[3] = RESIDENT_IPV4_MASK3;
      stored_gw[0] = RESIDENT_IPV4_GW0;
      stored_gw[1] = RESIDENT_IPV4_GW1;
      stored_gw[2] = RESIDENT_IPV4_GW2;
      stored_gw[3] = RESIDENT_IPV4_GW3;
    }
    printf("lwIP static IPv4: %u.%u.%u.%u mask %u.%u.%u.%u gw %u.%u.%u.%u\r\n",
           stored_ip[0], stored_ip[1], stored_ip[2], stored_ip[3],
           stored_netmask[0], stored_netmask[1], stored_netmask[2], stored_netmask[3],
           stored_gw[0], stored_gw[1], stored_gw[2], stored_gw[3]);
  }

  IP4_ADDR(&ipaddr, stored_ip[0], stored_ip[1], stored_ip[2], stored_ip[3]);
  IP4_ADDR(&netmask, stored_netmask[0], stored_netmask[1], stored_netmask[2], stored_netmask[3]);
  IP4_ADDR(&gw, stored_gw[0], stored_gw[1], stored_gw[2], stored_gw[3]);

  /* add the network interface (IPv4/IPv6) with RTOS */
  netif_add(&gnetif, &ipaddr, &netmask, &gw, NULL, &ethernetif_init, &tcpip_input);

  /* Registers the default network interface */
  netif_set_default(&gnetif);

  /* We must always bring the network interface up connection or not... */
  netif_set_up(&gnetif);

#if LWIP_DHCP
  if (use_dhcp != 0U)
  {
    if (dhcp_start(&gnetif) != ERR_OK)
    {
      printf("lwIP: dhcp_start failed\r\n");
    }
  }
#endif

  /* Set the link callback function, this function is called on change of link status*/
  netif_set_link_callback(&gnetif, ethernet_link_status_updated);

  /* Create the Ethernet link handler thread */
/* USER CODE BEGIN H7_OS_THREAD_NEW_CMSIS_RTOS_V2 */
  memset(&attributes, 0x0, sizeof(osThreadAttr_t));
  attributes.name = "EthLink";
  attributes.cb_mem = &EthLinkTaskControlBlock;
  attributes.cb_size = sizeof(EthLinkTaskControlBlock);
  attributes.stack_mem = EthLinkTaskStack;
  attributes.stack_size = sizeof(EthLinkTaskStack);
  attributes.priority = osPriorityBelowNormal;
  osThreadNew(ethernet_link_thread, &gnetif, &attributes);
/* USER CODE END H7_OS_THREAD_NEW_CMSIS_RTOS_V2 */

/* USER CODE BEGIN 3 */

/* USER CODE END 3 */
}

#ifdef USE_OBSOLETE_USER_CODE_SECTION_4
/* Kept to help code migration. (See new 4_1, 4_2... sections) */
/* Avoid to use this user section which will become obsolete. */
/* USER CODE BEGIN 4 */
/* USER CODE END 4 */
#endif

/**
  * @brief  Notify the User about the network interface config status
  * @param  netif: the network interface
  * @retval None
  */
static void ethernet_link_status_updated(struct netif *netif)
{
  if (netif_is_up(netif))
  {
/* USER CODE BEGIN 5 */
/* USER CODE END 5 */
  }
  else /* netif is down */
  {
/* USER CODE BEGIN 6 */
/* USER CODE END 6 */
  }
}

#if defined ( __CC_ARM )  /* MDK ARM Compiler */
/**
 * Opens a serial device for communication.
 *
 * @param devnum device number
 * @return handle to serial device if successful, NULL otherwise
 */
sio_fd_t sio_open(u8_t devnum)
{
  sio_fd_t sd;

/* USER CODE BEGIN 7 */
  sd = 0; // dummy code
/* USER CODE END 7 */

  return sd;
}

/**
 * Sends a single character to the serial device.
 *
 * @param c character to send
 * @param fd serial device handle
 *
 * @note This function will block until the character can be sent.
 */
void sio_send(u8_t c, sio_fd_t fd)
{
/* USER CODE BEGIN 8 */
/* USER CODE END 8 */
}

/**
 * Reads from the serial device.
 *
 * @param fd serial device handle
 * @param data pointer to data buffer for receiving
 * @param len maximum length (in bytes) of data to receive
 * @return number of bytes actually received - may be 0 if aborted by sio_read_abort
 *
 * @note This function will block until data can be received. The blocking
 * can be cancelled by calling sio_read_abort().
 */
u32_t sio_read(sio_fd_t fd, u8_t *data, u32_t len)
{
  u32_t recved_bytes;

/* USER CODE BEGIN 9 */
  recved_bytes = 0; // dummy code
/* USER CODE END 9 */
  return recved_bytes;
}

/**
 * Tries to read from the serial device. Same as sio_read but returns
 * immediately if no data is available and never blocks.
 *
 * @param fd serial device handle
 * @param data pointer to data buffer for receiving
 * @param len maximum length (in bytes) of data to receive
 * @return number of bytes actually received
 */
u32_t sio_tryread(sio_fd_t fd, u8_t *data, u32_t len)
{
  u32_t recved_bytes;

/* USER CODE BEGIN 10 */
  recved_bytes = 0; // dummy code
/* USER CODE END 10 */
  return recved_bytes;
}
#endif /* MDK ARM Compiler */

