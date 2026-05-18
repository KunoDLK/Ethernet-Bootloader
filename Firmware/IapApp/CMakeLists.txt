cmake_minimum_required(VERSION 3.22)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

include(${CMAKE_SOURCE_DIR}/cmake/ccmram_app.cmake)

set(IAP_INCLUDE_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}/../AppAbi
  ${CMAKE_CURRENT_SOURCE_DIR}/../Boot/Inc
  ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/CMSIS/Include
  ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/CMSIS/Device/ST/STM32F4xx/Include
  ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32F4xx_HAL_Driver/Inc
  ${CMAKE_CURRENT_SOURCE_DIR}/Inc
)

set(IAP_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/Src/app_main.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../Boot/Src/boot_flash_resident.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../Boot/Src/boot_iap_payload.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../Boot/Src/boot_memory_map.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../Boot/Src/boot_sha1.c
)

add_ccmram_app_image(
  TARGET_NAME iap_app
  APP_NAME iap_app
  LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/app_linker.ld
  PACKAGE_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/package_app.py
  VERSION 1
  SOURCES ${IAP_SOURCES}
  INCLUDE_DIRS ${IAP_INCLUDE_DIRS}
)

