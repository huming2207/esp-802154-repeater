/*
 * Thread router (repeater) back-end, using the OpenThread port in ESP-IDF.
 *
 * The device is a Full Thread Device (router eligible). It joins a network with
 * the standard Thread Joiner procedure: a commissioner (e.g. an OpenThread
 * Border Router) admits it with the PSKd from CONFIG_RPT_OT_JOINER_PSKD.
 * No IP stack runs on the SoC: it only forwards Thread traffic.
 */

#include <stdint.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_types.h"
#include "esp_timer.h"
#include "esp_vfs_eventfd.h"
#include "nvs.h"

#include "openthread/dataset.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/joiner.h"
#include "openthread/link.h"
#include "openthread/thread.h"

#if CONFIG_OPENTHREAD_PLATFORM_NETIF
#include "esp_netif.h"
#include "esp_openthread_netif_glue.h"
#endif

#include "repeater.h"

#if !CONFIG_OPENTHREAD_FTD
#error "A Thread repeater must be a Full Thread Device (CONFIG_OPENTHREAD_FTD)"
#endif

/* NVS namespace used by the ESP-IDF OpenThread settings port (esp_openthread_settings.c). */
#define OT_NVS_NAMESPACE "openthread"
#define OT_NVS_PARTITION "nvs"
#define US_PER_MS        1000ULL

static const char *TAG = "ot_router";

/*
 * State is only touched with the OpenThread lock held: either from OpenThread
 * callbacks (which run in the OpenThread task with the lock held) or from the
 * timer callbacks below, which take the lock.
 */
static otInstance *s_ot;
static esp_timer_handle_t s_retry_timer;  /* restarts the Joiner after a failure */
static esp_timer_handle_t s_window_timer; /* ends a GPIO-triggered pairing window */
static bool s_window_armed;
static bool s_pairing;

static esp_openthread_config_t s_ot_config = {
#if CONFIG_OPENTHREAD_PLATFORM_NETIF
    .netif_config = ESP_NETIF_DEFAULT_OPENTHREAD(),
#endif
    .platform_config = {
        .radio_config = { .radio_mode = RADIO_MODE_NATIVE },
        .host_config  = { .host_connection_mode = HOST_CONNECTION_MODE_NONE },
        .port_config  = {
            .storage_partition_name = OT_NVS_PARTITION,
            .netif_queue_size       = 10,
            .task_queue_size        = 10,
        },
    },
};

/* Wipe the stored Thread network before OpenThread loads it (same as a factory reset). */
static void erase_stored_network(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(OT_NVS_PARTITION, OT_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return; /* nothing stored yet */
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(nvs_erase_all(handle));
    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);
    ESP_LOGI(TAG, "Stored Thread network erased");
}

/* ---- Joiner ------------------------------------------------------------------------ */

static void joiner_start(void);

static void pairing_stop(void)
{
    s_pairing = false;
    esp_timer_stop(s_window_timer);
    esp_timer_stop(s_retry_timer);
}

static void joiner_done_cb(otError error, void *ctx)
{
    (void)ctx;

    if (error == OT_ERROR_NONE) {
        pairing_stop();
        ESP_LOGI(TAG, "Joiner succeeded, attaching to the Thread network");
        otThreadSetEnabled(s_ot, true);
    } else if (s_pairing) {
        ESP_LOGI(TAG, "Joiner failed (%s), retrying in %d ms", otThreadErrorToString(error), CONFIG_RPT_JOIN_RETRY_MS);
        esp_timer_stop(s_retry_timer);
        ESP_ERROR_CHECK(esp_timer_start_once(s_retry_timer, CONFIG_RPT_JOIN_RETRY_MS * US_PER_MS));
    }
}

static void joiner_start(void)
{
    const otError error = otJoinerStart(s_ot, CONFIG_RPT_OT_JOINER_PSKD, NULL, CONFIG_RPT_VENDOR_NAME,
                                        CONFIG_RPT_MODEL_NAME, NULL, NULL, joiner_done_cb, NULL);
    if (error != OT_ERROR_NONE) {
        joiner_done_cb(error, NULL);
    }
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    esp_openthread_lock_acquire(portMAX_DELAY);
    if (s_pairing) {
        joiner_start();
    }
    esp_openthread_lock_release();
}

static void window_timer_cb(void *arg)
{
    (void)arg;
    esp_openthread_lock_acquire(portMAX_DELAY);
    if (s_pairing) {
        pairing_stop();
        otJoinerStop(s_ot);
        ESP_LOGW(TAG, "Pairing window of %d s expired without joining a network. "
                 "Idle until the next reboot.", CONFIG_RPT_PAIRING_TIMEOUT_S);
    }
    esp_openthread_lock_release();
}

static void pairing_start(void)
{
    otExtAddress eui64;
    otLinkGetFactoryAssignedIeeeEui64(s_ot, &eui64);

    s_pairing = true;
    if (s_window_armed) {
        s_window_armed = false;
        ESP_ERROR_CHECK(esp_timer_start_once(s_window_timer, CONFIG_RPT_PAIRING_TIMEOUT_S * 1000ULL * US_PER_MS));
        ESP_LOGI(TAG, "Pairing for %d s", CONFIG_RPT_PAIRING_TIMEOUT_S);
    } else {
        ESP_LOGI(TAG, "No network stored, pairing until joined");
    }
    ESP_LOGI(TAG, "Joiner EUI-64 %02x%02x%02x%02x%02x%02x%02x%02x, PSKd %s", eui64.m8[0], eui64.m8[1], eui64.m8[2],
             eui64.m8[3], eui64.m8[4], eui64.m8[5], eui64.m8[6], eui64.m8[7], CONFIG_RPT_OT_JOINER_PSKD);

    joiner_start();
}

/* ---- state logging ----------------------------------------------------------------- */

static void state_changed_cb(otChangedFlags flags, void *ctx)
{
    (void)ctx;
    if (flags & OT_CHANGED_THREAD_ROLE) {
        ESP_LOGI(TAG, "Thread role: %s", otThreadDeviceRoleToString(otThreadGetDeviceRole(s_ot)));
    }
}

void thread_router_start(bool reset_and_pair)
{
    if (reset_and_pair) {
        erase_stored_network();
    }
    s_window_armed = reset_and_pair;

    const esp_timer_create_args_t retry_args = { .callback = retry_timer_cb, .name = "ot_retry" };
    const esp_timer_create_args_t window_args = { .callback = window_timer_cb, .name = "ot_window" };
    ESP_ERROR_CHECK(esp_timer_create(&retry_args, &s_retry_timer));
    ESP_ERROR_CHECK(esp_timer_create(&window_args, &s_window_timer));

    /* eventfds used by the OpenThread port: task queue, radio driver (+ netif if enabled). */
    const esp_vfs_eventfd_config_t eventfd_config = { .max_fds = 3 };
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));
    ESP_ERROR_CHECK(esp_event_loop_create_default());
#if CONFIG_OPENTHREAD_PLATFORM_NETIF
    ESP_ERROR_CHECK(esp_netif_init());
#endif

    ESP_LOGI(TAG, "Starting Thread router");
    ESP_ERROR_CHECK(esp_openthread_start(&s_ot_config)); /* spawns the OpenThread task */

    esp_openthread_lock_acquire(portMAX_DELAY);
    s_ot = esp_openthread_get_instance();
    otSetStateChangedCallback(s_ot, state_changed_cb, NULL);
    otIp6SetEnabled(s_ot, true);

    if (otDatasetIsCommissioned(s_ot)) {
        ESP_LOGI(TAG, "Rejoining stored Thread network, routing");
        otThreadSetEnabled(s_ot, true);
    } else {
        pairing_start();
    }
    esp_openthread_lock_release();
}
