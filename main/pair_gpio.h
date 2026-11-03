/*
 * Boot-time "reset and pair" GPIO.
 */
#pragma once

#include <stdbool.h>

/**
 * Sample CONFIG_RPT_PAIR_GPIO (internal pull-up) for CONFIG_RPT_PAIR_SAMPLE_WINDOW_MS.
 *
 * Returns as soon as the pin has been held low for CONFIG_RPT_PAIR_HOLD_MS without
 * interruption, or when the window ends. Shorter pulses are treated as glitches.
 * The pin is released (pull-up off) before returning, so a jumper left in place
 * does not draw current.
 *
 * @return true if a reset-and-pair was requested.
 */
bool pair_gpio_reset_requested(void);
