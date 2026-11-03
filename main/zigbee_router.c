/*
 * Zigbee router (repeater) back-end, using esp-zigbee-lib 2.x.
 *
 * The device exposes one HA "Range Extender" endpoint with the Basic and Identify
 * clusters, which is all coordinators such as Zigbee2MQTT or ZHA need to
 * interview it. Everything else is plain Zigbee routing done by the stack.
 */

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_zigbee.h"
#include "ezbee/zha.h"

#include "repeater.h"

#define ZB_ENDPOINT_ID     1
#define ZB_TASK_STACK_SIZE 4096
#define ZB_TASK_PRIORITY   5
#define ZCL_STR_MAX_LEN    32
#define US_PER_MS          1000ULL

_Static_assert(sizeof(CONFIG_RPT_VENDOR_NAME) - 1 <= ZCL_STR_MAX_LEN, "Vendor name too long");
_Static_assert(sizeof(CONFIG_RPT_MODEL_NAME) - 1 <= ZCL_STR_MAX_LEN, "Model name too long");

static const char *TAG = "zb_router";

/*
 * All state is static and only touched from the Zigbee task (signal handler and
 * the callbacks posted with esp_zigbee_task_queue_post()), so no locking is needed.
 */
static esp_timer_handle_t s_retry_timer;  /* re-runs a BDB step after a failure */
static esp_timer_handle_t s_window_timer; /* ends a GPIO-triggered pairing window */
static ezb_bdb_comm_mode_mask_t s_retry_mode;
static bool s_leave_requested; /* GPIO reset: leave the stored network first */
static bool s_window_armed;    /* the next pairing attempt is time limited */
static bool s_pairing;         /* currently trying to join a network */

/* ZCL character strings: length byte followed by the characters. */
static uint8_t s_zcl_vendor[1 + ZCL_STR_MAX_LEN];
static uint8_t s_zcl_model[1 + ZCL_STR_MAX_LEN];

static void zcl_string_set(uint8_t *dst, const char *src)
{
    const size_t len = strlen(src);
    dst[0] = (uint8_t)len;
    memcpy(&dst[1], src, len);
}

static void log_network(const char *what)
{
    ezb_extpanid_t ext_pan_id;
    ezb_nwk_get_extended_panid(&ext_pan_id);
    ESP_LOGI(TAG, "%s: PAN 0x%04hx (ext 0x%016llx), channel %d, short address 0x%04hx", what,
             ezb_nwk_get_panid(), ext_pan_id.u64, ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
}

/* ---- timers: their callbacks only hop over to the Zigbee task ---------------------- */

static void retry_in_zigbee_task(void *ctx)
{
    (void)ctx;
    if (s_retry_mode == EZB_BDB_MODE_NETWORK_STEERING && !s_pairing) {
        return; /* the pairing window closed in the meantime */
    }
    ezb_bdb_start_top_level_commissioning(s_retry_mode);
}

static void window_expired_in_zigbee_task(void *ctx)
{
    (void)ctx;
    if (!s_pairing) {
        return;
    }
    s_pairing = false;
    esp_timer_stop(s_retry_timer);
    ezb_bdb_cancel_steering();
    ESP_LOGW(TAG, "Pairing window of %d s expired without joining a network. "
             "Idle until the next reboot.", CONFIG_RPT_PAIRING_TIMEOUT_S);
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    if (esp_zigbee_task_queue_post(retry_in_zigbee_task, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Zigbee task queue full, retry dropped");
    }
}

static void window_timer_cb(void *arg)
{
    (void)arg;
    if (esp_zigbee_task_queue_post(window_expired_in_zigbee_task, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Zigbee task queue full, pairing window not closed");
    }
}

static void schedule_retry(ezb_bdb_comm_mode_mask_t mode)
{
    s_retry_mode = mode;
    esp_timer_stop(s_retry_timer); /* fine if it was not running */
    ESP_ERROR_CHECK(esp_timer_start_once(s_retry_timer, CONFIG_RPT_JOIN_RETRY_MS * US_PER_MS));
}

/* ---- pairing ---------------------------------------------------------------------- */

static void pairing_start(void)
{
    s_pairing = true;
    if (s_window_armed) {
        s_window_armed = false;
        ESP_ERROR_CHECK(esp_timer_start_once(s_window_timer, CONFIG_RPT_PAIRING_TIMEOUT_S * 1000ULL * US_PER_MS));
        ESP_LOGI(TAG, "Pairing: searching for an open Zigbee network for %d s", CONFIG_RPT_PAIRING_TIMEOUT_S);
    } else {
        ESP_LOGI(TAG, "Pairing: no network stored, searching for an open Zigbee network until joined");
    }
    ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
}

static void pairing_done(void)
{
    s_pairing = false;
    esp_timer_stop(s_window_timer);
    esp_timer_stop(s_retry_timer);
}

static bool zb_signal_handler(const ezb_app_signal_t *signal)
{
    const ezb_app_signal_type_t type = ezb_app_signal_get_type(signal);

    switch (type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;

    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        const ezb_bdb_comm_status_t status = *(const ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
        if (status != EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "Stack initialisation failed (0x%02x), retrying", status);
            schedule_retry(EZB_BDB_MODE_INITIALIZATION);
        } else if (ezb_bdb_is_factory_new()) {
            s_leave_requested = false;
            pairing_start();
        } else if (s_leave_requested) {
            /* Leave properly (keeps the NWK frame counter); EZB_ZDO_SIGNAL_LEAVE follows. */
            s_leave_requested = false;
            log_network("Reset requested, leaving network");
            ezb_bdb_reset_via_local_action();
        } else {
            log_network("Rejoined stored network, routing");
        }
    } break;

    case EZB_ZDO_SIGNAL_LEAVE: {
        const ezb_zdo_signal_leave_params_t *params = ezb_app_signal_get_params(signal);
        if (params->leave_type == EZB_ZDO_LEAVE_TYPE_RESET) {
            /* Either our own reset or the coordinator removed us: look for a network again. */
            ESP_LOGI(TAG, "Left the network");
            pairing_start();
        }
    } break;

    case EZB_BDB_SIGNAL_STEERING: {
        const ezb_bdb_comm_status_t status = *(const ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
        if (status == EZB_BDB_STATUS_SUCCESS) {
            pairing_done();
            log_network("Joined network, routing");
        } else if (s_pairing) {
            ESP_LOGI(TAG, "No joinable network found (0x%02x), retrying in %d ms", status, CONFIG_RPT_JOIN_RETRY_MS);
            schedule_retry(EZB_BDB_MODE_NETWORK_STEERING);
        }
    } break;

    default:
        ESP_LOGD(TAG, "Signal %s (0x%02x)", ezb_app_signal_to_string(type), (unsigned)type);
        break;
    }
    return true;
}

/* ---- device description ------------------------------------------------------------ */

static void zb_register_device(void)
{
    const ezb_af_ep_config_t ep_config = {
        .ep_id              = ZB_ENDPOINT_ID,
        .app_profile_id     = EZB_AF_HA_PROFILE_ID,
        .app_device_id      = EZB_ZHA_RANGE_EXTENDER_DEVICE_ID,
        .app_device_version = 0,
    };
    ezb_zcl_basic_cluster_server_config_t basic_cfg = {
        .zcl_version  = EZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = EZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE,
    };
    ezb_zcl_identify_cluster_server_config_t identify_cfg = {
        .identify_time = EZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE,
    };

    zcl_string_set(s_zcl_vendor, CONFIG_RPT_VENDOR_NAME);
    zcl_string_set(s_zcl_model, CONFIG_RPT_MODEL_NAME);

    /* Allocated once by the stack here, never freed while running. */
    ezb_af_device_desc_t dev_desc = ezb_af_create_device_desc();
    ezb_af_ep_desc_t ep_desc = ezb_af_create_endpoint_desc(&ep_config);

    ezb_zcl_cluster_desc_t basic_desc = ezb_zcl_basic_create_cluster_desc(&basic_cfg, EZB_ZCL_CLUSTER_SERVER);
    ESP_ERROR_CHECK(ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, s_zcl_vendor));
    ESP_ERROR_CHECK(ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, s_zcl_model));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep_desc, basic_desc));

    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(
        ep_desc, ezb_zcl_identify_create_cluster_desc(&identify_cfg, EZB_ZCL_CLUSTER_SERVER)));

    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev_desc, ep_desc));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(dev_desc));
}

static void zigbee_task(void *arg)
{
    (void)arg;

    const esp_zigbee_config_t config = {
        .device_config = {
            .device_type         = EZB_NWK_DEVICE_TYPE_ROUTER,
            .install_code_policy = false,
            .zczr_config         = { .max_children = CONFIG_RPT_ZB_MAX_CHILDREN },
        },
        .platform_config = {
            .storage_partition_name = "nvs",
            .radio_config           = { .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE },
        },
    };

    ESP_ERROR_CHECK(esp_zigbee_init(&config));

    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(CONFIG_RPT_ZB_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(zb_signal_handler));
    zb_register_device();

    ESP_ERROR_CHECK(esp_zigbee_start(false));
    esp_zigbee_launch_mainloop(); /* runs forever */

    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

void zigbee_router_start(bool reset_and_pair)
{
    s_leave_requested = reset_and_pair;
    s_window_armed = reset_and_pair;

    const esp_timer_create_args_t retry_args = { .callback = retry_timer_cb, .name = "zb_retry" };
    const esp_timer_create_args_t window_args = { .callback = window_timer_cb, .name = "zb_window" };
    ESP_ERROR_CHECK(esp_timer_create(&retry_args, &s_retry_timer));
    ESP_ERROR_CHECK(esp_timer_create(&window_args, &s_window_timer));

    ESP_LOGI(TAG, "Starting Zigbee router (max %d children)", CONFIG_RPT_ZB_MAX_CHILDREN);
    xTaskCreate(zigbee_task, "zigbee", ZB_TASK_STACK_SIZE, NULL, ZB_TASK_PRIORITY, NULL);
}
