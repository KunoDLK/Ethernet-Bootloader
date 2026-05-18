cmake_minimum_required(VERSION 3.22)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

include(${CMAKE_SOURCE_DIR}/cmake/ccmram_app.cmake)

set(APP_INCLUDE_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}/../AppAbi
  ${CMAKE_CURRENT_SOURCE_DIR}/Inc
)

set(APP_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/Src/app_main.c
)

add_ccmram_app_image(
  TARGET_NAME application
  APP_NAME example_app
  LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/app_linker.ld
  PACKAGE_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/package_app.py
  VERSION 1
  SOURCES ${APP_SOURCES}
  INCLUDE_DIRS ${APP_INCLUDE_DIRS}
)

