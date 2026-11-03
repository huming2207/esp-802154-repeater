/*
 * Protocol back-ends. Exactly one is compiled in, selected by CONFIG_RPT_PROTOCOL_*.
 */
#pragma once

#include <stdbool.h>

#include "sdkconfig.h"

#if CONFIG_ZB_ENABLED && CONFIG_OPENTHREAD_ENABLED
#error "Zigbee and OpenThread cannot be linked into one image: disable the stack you do not use " \
       "(menuconfig: Component config -> Zigbee / OpenThread) or delete sdkconfig and rebuild."
#endif

/**
 * Start the repeater stack in its own task and return.
 *
 * Behaviour (identical for both protocols):
 *  - reset_and_pair == true: forget the stored network, then try to join a new
 *    network for CONFIG_RPT_PAIRING_TIMEOUT_S seconds. If that fails, stay idle
 *    until the next reboot.
 *  - reset_and_pair == false and a network is stored: rejoin it and route forever.
 *  - reset_and_pair == false and nothing is stored (factory state): keep trying
 *    to join until it succeeds.
 */
void zigbee_router_start(bool reset_and_pair);
void thread_router_start(bool reset_and_pair);
