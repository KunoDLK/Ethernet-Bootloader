function(add_ccmram_app_image)
  set(options)
  set(oneValueArgs TARGET_NAME APP_NAME LINKER_SCRIPT PACKAGE_SCRIPT VERSION)
  set(multiValueArgs SOURCES INCLUDE_DIRS COMPILE_DEFINITIONS)
  cmake_parse_arguments(APP "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT APP_TARGET_NAME)
    message(FATAL_ERROR "add_ccmram_app_image: TARGET_NAME is required")
  endif()
  if(NOT APP_APP_NAME)
    message(FATAL_ERROR "add_ccmram_app_image: APP_NAME is required")
  endif()
  if(NOT APP_LINKER_SCRIPT)
    message(FATAL_ERROR "add_ccmram_app_image: LINKER_SCRIPT is required")
  endif()
  if(NOT APP_PACKAGE_SCRIPT)
    message(FATAL_ERROR "add_ccmram_app_image: PACKAGE_SCRIPT is required")
  endif()
  if(NOT APP_SOURCES)
    message(FATAL_ERROR "add_ccmram_app_image: SOURCES is required")
  endif()

  add_executable(${APP_TARGET_NAME}.elf ${APP_SOURCES})

  target_include_directories(${APP_TARGET_NAME}.elf PRIVATE ${APP_INCLUDE_DIRS})
  target_compile_definitions(${APP_TARGET_NAME}.elf PRIVATE ${APP_COMPILE_DEFINITIONS})

  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_app_opt_level -Og)
    set(_app_debug_flag -g)
  else()
    set(_app_opt_level -Os)
    set(_app_debug_flag)
  endif()

  target_compile_options(${APP_TARGET_NAME}.elf PRIVATE
    -mcpu=cortex-m4
    -mthumb
    -mfpu=fpv4-sp-d16
    -mfloat-abi=hard
    ${_app_opt_level}
    ${_app_debug_flag}
    -ffunction-sections
    -fdata-sections
    -Wall
    -Wextra
  )

  target_link_options(${APP_TARGET_NAME}.elf PRIVATE
    -T${APP_LINKER_SCRIPT}
    -nostartfiles
    -Wl,--gc-sections
    -Wl,-Map=${APP_TARGET_NAME}.map
    -mcpu=cortex-m4
    -mthumb
    -mfpu=fpv4-sp-d16
    -mfloat-abi=hard
  )

  set(bin_path "${CMAKE_CURRENT_BINARY_DIR}/${APP_APP_NAME}.bin")
  set(appimg_path "${CMAKE_CURRENT_BINARY_DIR}/${APP_APP_NAME}.appimg")

  add_custom_command(
    OUTPUT "${bin_path}"
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${APP_TARGET_NAME}.elf> "${bin_path}"
    DEPENDS ${APP_TARGET_NAME}.elf
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${appimg_path}"
    COMMAND ${Python3_EXECUTABLE} "${APP_PACKAGE_SCRIPT}"
      --elf $<TARGET_FILE:${APP_TARGET_NAME}.elf>
      --bin "${bin_path}"
      --out "${appimg_path}"
      --nm "${CMAKE_NM}"
      --version ${APP_VERSION}
    DEPENDS ${APP_TARGET_NAME}.elf "${bin_path}" "${APP_PACKAGE_SCRIPT}"
    VERBATIM
  )

  add_custom_target(${APP_TARGET_NAME}.bin ALL DEPENDS "${bin_path}")
  add_custom_target(${APP_TARGET_NAME}.appimg ALL DEPENDS "${appimg_path}")

  add_custom_command(
    TARGET ${APP_TARGET_NAME}.elf
    POST_BUILD
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${APP_TARGET_NAME}.elf>
    VERBATIM
  )
endfunction()

