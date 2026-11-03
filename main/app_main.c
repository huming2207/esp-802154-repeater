/*
 * ESP32-C6 802.15.4 repeater - entry point.
 */

#include "esp_log.h"
#include "nvs_flash.h"

#include "pair_gpio.h"
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

    const bool reset_and_pair = pair_gpio_reset_requested();

#if CONFIG_RPT_PROTOCOL_ZIGBEE
    zigbee_router_start(reset_and_pair);
#elif CONFIG_RPT_PROTOCOL_THREAD
    thread_router_start(reset_and_pair);
#endif
}
