set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(ARM_TOOLCHAIN_PREFIX arm-none-eabi)

set(_arm_toolchain_hints)

if(DEFINED ENV{ARM_GCC_ROOT})
  list(APPEND _arm_toolchain_hints "$ENV{ARM_GCC_ROOT}" "$ENV{ARM_GCC_ROOT}/bin")
endif()
if(DEFINED ENV{GNUARMEMB_TOOLCHAIN_PATH})
  list(APPEND _arm_toolchain_hints "$ENV{GNUARMEMB_TOOLCHAIN_PATH}" "$ENV{GNUARMEMB_TOOLCHAIN_PATH}/bin")
endif()

list(APPEND _arm_toolchain_hints
  "C:/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/bin"
  "C:/Program Files/Arm GNU Toolchain arm-none-eabi/bin"
)

find_program(ARM_NONE_EABI_GCC NAMES ${ARM_TOOLCHAIN_PREFIX}-gcc HINTS ${_arm_toolchain_hints})
find_program(ARM_NONE_EABI_OBJCOPY NAMES ${ARM_TOOLCHAIN_PREFIX}-objcopy HINTS ${_arm_toolchain_hints})
find_program(ARM_NONE_EABI_NM NAMES ${ARM_TOOLCHAIN_PREFIX}-nm HINTS ${_arm_toolchain_hints})
find_program(ARM_NONE_EABI_SIZE NAMES ${ARM_TOOLCHAIN_PREFIX}-size HINTS ${_arm_toolchain_hints})

if(NOT ARM_NONE_EABI_GCC OR NOT ARM_NONE_EABI_OBJCOPY OR NOT ARM_NONE_EABI_NM OR NOT ARM_NONE_EABI_SIZE)
  message(FATAL_ERROR
    "ARM GNU toolchain not found. Ensure arm-none-eabi tools are in PATH, or set ARM_GCC_ROOT "
    "to the toolchain root (containing bin/arm-none-eabi-gcc).")
endif()

set(CMAKE_C_COMPILER "${ARM_NONE_EABI_GCC}")
set(CMAKE_ASM_COMPILER "${ARM_NONE_EABI_GCC}")
set(CMAKE_OBJCOPY "${ARM_NONE_EABI_OBJCOPY}" CACHE FILEPATH "objcopy tool")
set(CMAKE_NM "${ARM_NONE_EABI_NM}" CACHE FILEPATH "nm tool")
set(CMAKE_SIZE "${ARM_NONE_EABI_SIZE}" CACHE FILEPATH "size tool")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

