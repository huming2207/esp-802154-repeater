/*
 * ESP32-C6 802.15.4 repeater - entry point.
 */

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"

#include "boot_gpio.h"
#include "ota_portal.h"
#include "repeater.h"

static const char *TAG = "repeater";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "802.15.4 repeater starting");

    const boot_action_t action = boot_gpio_sample();
    if (action == BOOT_ACTION_OTA_PORTAL) {
        /* Update mode: Wi-Fi access point + web page, no 802.15.4 stack. */
        ota_portal_start();
        esp_ota_mark_app_valid_cancel_rollback();
        return;
    }

    const bool reset_and_pair = (action == BOOT_ACTION_RESET_AND_PAIR);

#if CONFIG_RPT_PROTOCOL_ZIGBEE
    zigbee_router_start(reset_and_pair);
#elif CONFIG_RPT_PROTOCOL_THREAD
    thread_router_start(reset_and_pair);
#endif

    /* Got this far: keep this image (cancels a pending rollback after an update). */
    esp_ota_mark_app_valid_cancel_rollback();
}
