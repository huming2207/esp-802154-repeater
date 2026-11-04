/*
 * Boot-time jumpers: GPIO2 = reset and pair, GPIO3 = firmware update portal.
 */

#include "boot_gpio.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

_Static_assert(CONFIG_RPT_BOOT_HOLD_MS < CONFIG_RPT_BOOT_SAMPLE_WINDOW_MS,
               "The hold time must fit inside the boot sample window");
_Static_assert(CONFIG_RPT_PAIR_GPIO != CONFIG_RPT_OTA_GPIO, "The two boot jumpers need different GPIOs");

#define POLL_MS   10
#define US_PER_MS 1000LL

typedef struct {
    gpio_num_t pin;
    int64_t low_since_us; /* -1 while high */
    bool held;            /* was low for at least the hold time */
} jumper_t;

static const char *TAG = "boot_gpio";

static void jumper_poll(jumper_t *j, int64_t now_us)
{
    if (gpio_get_level(j->pin) != 0) {
        j->low_since_us = -1; /* released, or just a glitch: start over */
        return;
    }
    if (j->low_since_us < 0) {
        j->low_since_us = now_us;
    }
    if (now_us - j->low_since_us >= CONFIG_RPT_BOOT_HOLD_MS * US_PER_MS) {
        j->held = true;
    }
}

boot_action_t boot_gpio_sample(void)
{
    jumper_t pair = { .pin = (gpio_num_t)CONFIG_RPT_PAIR_GPIO, .low_since_us = -1 };
    jumper_t ota = { .pin = (gpio_num_t)CONFIG_RPT_OTA_GPIO, .low_since_us = -1 };

    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pair.pin) | (1ULL << ota.pin),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    ESP_LOGI(TAG, "Hold GPIO%d low (reset + pair) or GPIO%d low (update portal) for %d ms within %d ms",
             pair.pin, ota.pin, CONFIG_RPT_BOOT_HOLD_MS, CONFIG_RPT_BOOT_SAMPLE_WINDOW_MS);

    /* Give the pull-ups a moment to charge the pins before the first sample. */
    vTaskDelay(pdMS_TO_TICKS(POLL_MS));

    /* Always sample the whole window, so that "both held" can be told apart. */
    const int64_t start_us = esp_timer_get_time();
    for (;;) {
        const int64_t now_us = esp_timer_get_time();
        jumper_poll(&pair, now_us);
        jumper_poll(&ota, now_us);
        if (now_us - start_us >= CONFIG_RPT_BOOT_SAMPLE_WINDOW_MS * US_PER_MS) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }

    gpio_reset_pin(pair.pin);
    gpio_set_pull_mode(pair.pin, GPIO_FLOATING);
    gpio_reset_pin(ota.pin);
    gpio_set_pull_mode(ota.pin, GPIO_FLOATING);

    if (pair.held && ota.held) {
        ESP_LOGW(TAG, "Both GPIO%d and GPIO%d held low: ambiguous, ignoring both", pair.pin, ota.pin);
        return BOOT_ACTION_NONE;
    }
    if (pair.held) {
        ESP_LOGI(TAG, "Reset and pair REQUESTED");
        return BOOT_ACTION_RESET_AND_PAIR;
    }
    if (ota.held) {
        ESP_LOGI(TAG, "Firmware update portal REQUESTED");
        return BOOT_ACTION_OTA_PORTAL;
    }
    ESP_LOGI(TAG, "No jumper, normal start");
    return BOOT_ACTION_NONE;
}
