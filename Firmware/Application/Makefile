TOOLCHAIN ?= arm-none-eabi
CC := $(TOOLCHAIN)-gcc
OBJCOPY := $(TOOLCHAIN)-objcopy
NM := $(TOOLCHAIN)-nm
SIZE := $(TOOLCHAIN)-size

APP_NAME := example_app
BUILD_DIR := build

CFLAGS := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard \
          -Os -ffunction-sections -fdata-sections -Wall -Wextra \
          -I../AppAbi -IInc
LDFLAGS := -Tapp_linker.ld -nostartfiles -Wl,--gc-sections -Wl,-Map=$(BUILD_DIR)/$(APP_NAME).map

SRCS := Src/app_main.c
OBJS := $(SRCS:Src/%.c=$(BUILD_DIR)/%.o)

.PHONY: all clean

all: $(BUILD_DIR)/$(APP_NAME).elf $(BUILD_DIR)/$(APP_NAME).bin $(BUILD_DIR)/$(APP_NAME).appimg
	$(SIZE) $(BUILD_DIR)/$(APP_NAME).elf

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: Src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/$(APP_NAME).elf: $(OBJS) app_linker.ld
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@

$(BUILD_DIR)/$(APP_NAME).bin: $(BUILD_DIR)/$(APP_NAME).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/$(APP_NAME).appimg: $(BUILD_DIR)/$(APP_NAME).elf $(BUILD_DIR)/$(APP_NAME).bin package_app.py
	python package_app.py --elf $< --bin $(BUILD_DIR)/$(APP_NAME).bin --out $@ --nm "$(NM)"

clean:
	rm -rf $(BUILD_DIR)
