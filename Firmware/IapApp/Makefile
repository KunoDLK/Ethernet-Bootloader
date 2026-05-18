TOOLCHAIN ?= arm-none-eabi
CC := $(TOOLCHAIN)-gcc
OBJCOPY := $(TOOLCHAIN)-objcopy
NM := $(TOOLCHAIN)-nm
SIZE := $(TOOLCHAIN)-size

APP_NAME := iap_app
BUILD_DIR := build

CFLAGS := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard \
          -Os -ffunction-sections -fdata-sections -Wall -Wextra \
          -I../AppAbi \
          -I../Boot/Inc \
          -I../Drivers/CMSIS/Include \
          -I../Drivers/CMSIS/Device/ST/STM32F4xx/Include \
          -I../Drivers/STM32F4xx_HAL_Driver/Inc \
          -IInc
LDFLAGS := -Tapp_linker.ld -nostartfiles -Wl,--gc-sections -Wl,-Map=$(BUILD_DIR)/$(APP_NAME).map

SRCS_LOCAL := Src/app_main.c
SRCS_BOOT := \
  ../Boot/Src/boot_flash_resident.c \
  ../Boot/Src/boot_iap_payload.c \
  ../Boot/Src/boot_memory_map.c \
  ../Boot/Src/boot_sha1.c
OBJS_LOCAL := $(SRCS_LOCAL:Src/%.c=$(BUILD_DIR)/Src/%.o)
OBJS_BOOT := $(SRCS_BOOT:../Boot/Src/%.c=$(BUILD_DIR)/Boot/%.o)
OBJS := $(OBJS_LOCAL) $(OBJS_BOOT)

.PHONY: all clean

all: $(BUILD_DIR)/$(APP_NAME).elf $(BUILD_DIR)/$(APP_NAME).bin $(BUILD_DIR)/$(APP_NAME).appimg
	$(SIZE) $(BUILD_DIR)/$(APP_NAME).elf

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/Src
	mkdir -p $(BUILD_DIR)/Boot

$(BUILD_DIR)/Src/%.o: Src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/Boot/%.o: ../Boot/Src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/$(APP_NAME).elf: $(OBJS) app_linker.ld
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@

$(BUILD_DIR)/$(APP_NAME).bin: $(BUILD_DIR)/$(APP_NAME).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/$(APP_NAME).appimg: $(BUILD_DIR)/$(APP_NAME).elf $(BUILD_DIR)/$(APP_NAME).bin package_app.py
	python package_app.py --elf $< --bin $(BUILD_DIR)/$(APP_NAME).bin --out $@ --nm "$(NM)"

clean:
	rm -rf $(BUILD_DIR)
