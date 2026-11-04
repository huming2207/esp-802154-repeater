/*
 * Boot-time jumpers: GPIO2 = reset and pair, GPIO3 = firmware update portal.
 */
#pragma once

typedef enum {
    BOOT_ACTION_NONE = 0,       /* normal start */
    BOOT_ACTION_RESET_AND_PAIR, /* CONFIG_RPT_PAIR_GPIO was held low */
    BOOT_ACTION_OTA_PORTAL,     /* CONFIG_RPT_OTA_GPIO was held low */
} boot_action_t;

/**
 * Sample both jumper GPIOs (internal pull-ups) for CONFIG_RPT_BOOT_SAMPLE_WINDOW_MS.
 *
 * A pin counts as "held" if it stays low for CONFIG_RPT_BOOT_HOLD_MS without
 * interruption at some point inside the window; shorter pulses are glitches.
 * If both pins are held, the request is ambiguous and both are ignored.
 * The pins are released (pull-ups off) before returning, so jumpers left in
 * place do not draw current.
 */
boot_action_t boot_gpio_sample(void);
