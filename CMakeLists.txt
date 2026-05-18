cmake_minimum_required(VERSION 3.22)

project(EthernetBootloader LANGUAGES C ASM)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not supported. Use: cmake -S . -B build")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(Firmware/Application)
add_subdirectory(Firmware/IapApp)

