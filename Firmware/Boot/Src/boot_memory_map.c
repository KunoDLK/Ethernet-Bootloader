#include "boot_memory_map.h"

typedef struct
{
  uint32_t base;
  uint32_t size;
} BootFlashSectorInfo;

static const BootFlashSectorInfo kFlashSectors[] = {
  {0x08000000UL, 16UL * 1024UL},
  {0x08004000UL, 16UL * 1024UL},
  {0x08008000UL, 16UL * 1024UL},
  {0x0800C000UL, 16UL * 1024UL},
  {0x08010000UL, 64UL * 1024UL},
  {0x08020000UL, 128UL * 1024UL},
  {0x08040000UL, 128UL * 1024UL},
  {0x08060000UL, 128UL * 1024UL},
  {0x08080000UL, 128UL * 1024UL},
  {0x080A0000UL, 128UL * 1024UL},
  {0x080C0000UL, 128UL * 1024UL},
  {0x080E0000UL, 128UL * 1024UL},
};

uint32_t boot_mem_resident_base(void)
{
  return BOOT_RESIDENT_BASE_ADDR;
}

uint32_t boot_mem_resident_limit(void)
{
  return BOOT_RESIDENT_LIMIT_ADDR;
}

uint32_t boot_mem_app_store_base(void)
{
  return BOOT_APP_STORE_BASE_ADDR;
}

uint32_t boot_mem_app_store_limit(void)
{
  return BOOT_APP_STORE_LIMIT_ADDR;
}

uint32_t boot_mem_app_exec_base(void)
{
  return BOOT_APP_EXEC_BASE_ADDR;
}

uint32_t boot_mem_app_exec_limit(void)
{
  return BOOT_APP_EXEC_LIMIT_ADDR;
}

uint32_t boot_mem_metadata_base(void)
{
  return BOOT_METADATA_BASE_ADDR;
}

uint32_t boot_mem_metadata_limit(void)
{
  return BOOT_METADATA_LIMIT_ADDR;
}

bool boot_mem_range_is_valid(uint32_t base, uint32_t length)
{
  if (length == 0U)
  {
    return false;
  }

  return (base <= (UINT32_MAX - length));
}

static bool range_inside(uint32_t base, uint32_t length, uint32_t region_base, uint32_t region_limit)
{
  if (!boot_mem_range_is_valid(base, length))
  {
    return false;
  }

  const uint32_t limit = base + length;
  return (base >= region_base) && (limit <= region_limit);
}

bool boot_mem_is_resident_range(uint32_t base, uint32_t length)
{
  return range_inside(base, length, BOOT_RESIDENT_BASE_ADDR, BOOT_RESIDENT_LIMIT_ADDR);
}

bool boot_mem_is_app_store_range(uint32_t base, uint32_t length)
{
  return range_inside(base, length, BOOT_APP_STORE_BASE_ADDR, BOOT_APP_STORE_LIMIT_ADDR);
}

bool boot_mem_is_app_exec_range(uint32_t base, uint32_t length)
{
  return range_inside(base, length, BOOT_APP_EXEC_BASE_ADDR, BOOT_APP_EXEC_LIMIT_ADDR);
}

bool boot_mem_is_metadata_range(uint32_t base, uint32_t length)
{
  return range_inside(base, length, BOOT_METADATA_BASE_ADDR, BOOT_METADATA_LIMIT_ADDR);
}

bool boot_mem_addr_is_app_exec(uint32_t address)
{
  return (address >= BOOT_APP_EXEC_BASE_ADDR) && (address < BOOT_APP_EXEC_LIMIT_ADDR);
}

uint32_t boot_mem_sector_for_addr(uint32_t address)
{
  for (uint32_t index = 0U; index < (sizeof(kFlashSectors) / sizeof(kFlashSectors[0])); index++)
  {
    const uint32_t base = kFlashSectors[index].base;
    const uint32_t limit = base + kFlashSectors[index].size;
    if ((address >= base) && (address < limit))
    {
      return index;
    }
  }

  return BOOT_SECTOR_INVALID;
}

uint32_t boot_mem_sector_base(uint32_t sector)
{
  if (sector >= (sizeof(kFlashSectors) / sizeof(kFlashSectors[0])))
  {
    return 0U;
  }

  return kFlashSectors[sector].base;
}

uint32_t boot_mem_sector_size(uint32_t sector)
{
  if (sector >= (sizeof(kFlashSectors) / sizeof(kFlashSectors[0])))
  {
    return 0U;
  }

  return kFlashSectors[sector].size;
}
