#ifndef BOOT_APP_MANAGER_H
#define BOOT_APP_MANAGER_H

#include <stdbool.h>

void boot_app_manager_init(void);
int boot_app_manager_start_if_valid(void);
int boot_app_manager_stop(void);
bool boot_app_manager_is_running(void);

#endif /* BOOT_APP_MANAGER_H */
