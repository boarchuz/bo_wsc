#ifndef BO_WSC_H
#define BO_WSC_H

#include <stdarg.h>
#include <stdint.h>

#include "esp_wifi.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BO_WSC_NVS_MODE_MANUAL,
    BO_WSC_NVS_MODE_AUTO,
} bo_wsc_nvs_mode_t;

/**
 * Set NVS write mode.
 * 
 * Manual (default): Changes will only be saved to NVS flash by bo_wsc_nvs_save.
 * Auto: Save any pending changes and, thereafter, automatically save as requested by the WiFi driver.
 * 
 * In a low-latency application with persistence, this should be left in Manual mode until WiFi
 * completes initialisation and/or connection for efficiency, then changed to Auto mode to keep NVS
 * up-to-date.
 */
#ifdef CONFIG_BO_WSC_NVS_DISABLED
__attribute__((error ("NVS support disabled")))
#endif
esp_err_t bo_wsc_nvs_mode(bo_wsc_nvs_mode_t mode);

/**
 * Write changes to flash when NVS mode is BO_WSC_NVS_MODE_MANUAL (default)
 */
#ifdef CONFIG_BO_WSC_NVS_DISABLED
__attribute__((error ("NVS support disabled")))
#endif
esp_err_t bo_wsc_nvs_save(void);

/**
 * Enable WiFi Storage Cache by setting functions in OSI struct (typically &g_wifi_osi_funcs).
 */
esp_err_t bo_wsc_set(wifi_osi_funcs_t *osi_funcs);

#ifdef __cplusplus
}
#endif

#endif /* BO_WSC_H */
