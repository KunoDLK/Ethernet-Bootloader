#include "app_api.h"
#include "app_config.h"
#include "boot_flash_resident.h"
#include "boot_iap_payload.h"
#include "boot_memory_map.h"
#include "boot_sha1.h"
#include "stm32f4xx.h"

#include <string.h>

static const AppApi *g_api;

static void log_text(const char *text)
{
  if ((g_api != 0) && (g_api->log.write != 0) && (text != 0))
  {
    g_api->log.write(text);
  }
}

int app_start(const AppApi *api)
{
  if ((api == 0) || (api->version != APP_API_VERSION))
  {
    return -1;
  }
  g_api = api;

  if (g_api->device_tree.mount != 0)
  {
    AppDeviceTreeMount mount = 0;
    (void)g_api->device_tree.mount(IAP_APP_NAME, &mount);
    (void)g_api->device_tree.set_value(mount, "status", "applying");
  }

  log_text("IAP app start");

  BootIapPayloadHeader header;
  if ((boot_iap_payload_read_header(&header) != 0) || (boot_iap_payload_validate_header(&header) != 0))
  {
    log_text("IAP payload header invalid");
    return -1;
  }

  const uint8_t *payload = boot_iap_payload_image_ptr(&header);
  if (payload == 0)
  {
    log_text("IAP payload pointer invalid");
    return -1;
  }

  uint8_t payload_sha1[BOOT_SHA1_DIGEST_BYTES];
  boot_sha1_compute(payload, header.image_length, payload_sha1);
  if (memcmp(payload_sha1, header.sha1, BOOT_SHA1_DIGEST_BYTES) != 0)
  {
    log_text("IAP payload sha1 mismatch");
    return -1;
  }

  log_text("IAP erase resident sectors 0-5");
  if (boot_flash_resident_erase_sectors_0_to_5() != 0)
  {
    log_text("IAP erase failed");
    return -1;
  }

  log_text("IAP program resident");
  if (boot_flash_resident_program(BOOT_RESIDENT_BASE_ADDR, payload, header.image_length) != 0)
  {
    log_text("IAP program failed");
    return -1;
  }

  log_text("IAP verify resident sha1");
  if (boot_flash_resident_verify_sha1(BOOT_RESIDENT_BASE_ADDR, header.image_length, header.sha1) != 0)
  {
    log_text("IAP verify failed");
    return -1;
  }

  log_text("IAP done reset");
  __DSB();
  __ISB();
  NVIC_SystemReset();
  return 0;
}

void app_stop(void)
{
}
