/*
 * Boot-time "reset and pair" GPIO.
 */

#include "pair_gpio.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

_Static_assert(CONFIG_RPT_PAIR_HOLD_MS < CONFIG_RPT_PAIR_SAMPLE_WINDOW_MS,
               "The hold time must fit inside the boot sample window");

#define PAIR_GPIO    ((gpio_num_t)CONFIG_RPT_PAIR_GPIO)
#define POLL_MS      10
#define US_PER_MS    1000LL

static const char *TAG = "pair_gpio";

bool pair_gpio_reset_requested(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PAIR_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    ESP_LOGI(TAG, "Hold GPIO%d low for %d ms within the next %d ms to reset and pair",
             PAIR_GPIO, CONFIG_RPT_PAIR_HOLD_MS, CONFIG_RPT_PAIR_SAMPLE_WINDOW_MS);

    /* Give the pull-up a moment to charge the pin before the first sample. */
    vTaskDelay(pdMS_TO_TICKS(POLL_MS));

    const int64_t start_us = esp_timer_get_time();
    int64_t low_since_us = -1;
    bool requested = false;

    for (;;) {
        const int64_t now_us = esp_timer_get_time();

        if (gpio_get_level(PAIR_GPIO) == 0) {
            if (low_since_us < 0) {
                low_since_us = now_us;
            }
            if (now_us - low_since_us >= CONFIG_RPT_PAIR_HOLD_MS * US_PER_MS) {
                requested = true;
                break;
            }
        } else {
            low_since_us = -1; /* released (or a glitch): start over */
        }

        if (now_us - start_us >= CONFIG_RPT_PAIR_SAMPLE_WINDOW_MS * US_PER_MS) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }

    gpio_reset_pin(PAIR_GPIO);
    gpio_set_pull_mode(PAIR_GPIO, GPIO_FLOATING);

    ESP_LOGI(TAG, "Reset-and-pair %s", requested ? "REQUESTED" : "not requested");
    return requested;
}
