#include "boot_iap.h"

#include "boot_app_manager.h"
#include "boot_iap_payload.h"
#include "proto_program_tcp.h"

int boot_iap_app_start(void)
{
  BootIapPayloadHeader payload_header;
  if ((boot_iap_payload_read_header(&payload_header) != 0) ||
      (boot_iap_payload_validate_header(&payload_header) != 0))
  {
    return -1;
  }

  (void)boot_app_manager_stop();
  proto_program_tcp_stop();

  return boot_app_manager_start_force();
}
