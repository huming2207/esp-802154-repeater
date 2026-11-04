/*
 * Firmware update portal: Wi-Fi access point + web page to upload a new image.
 *
 * GET  /        the upload page (embedded ota_portal.html)
 * GET  /info    JSON with the running firmware details
 * POST /update  raw application image (the "-app.bin"), written to the idle OTA slot
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#include "ota_portal.h"

#define AP_MAX_CLIENTS    2
#define RECV_BUF_SIZE     4096
#define RECV_MAX_TIMEOUTS 5    /* consecutive socket timeouts before giving up */
#define REBOOT_DELAY_MS   1500 /* lets the HTTP response reach the browser */
#define US_PER_MS         1000ULL
#define US_PER_MIN        (60ULL * 1000ULL * US_PER_MS)

/* The image starts with these headers; the app descriptor follows them. */
#define IMG_DESC_OFFSET (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))
#define IMG_MIN_SIZE    (IMG_DESC_OFFSET + sizeof(esp_app_desc_t))

_Static_assert(RECV_BUF_SIZE >= IMG_MIN_SIZE, "First chunk must hold the image headers");
_Static_assert(sizeof(CONFIG_RPT_OTA_AP_PASSWORD) == 1 || sizeof(CONFIG_RPT_OTA_AP_PASSWORD) >= 9,
               "The access point password must be empty (open network) or at least 8 characters");

#if CONFIG_RPT_PROTOCOL_ZIGBEE
#define PROTOCOL_NAME "zigbee"
#elif CONFIG_RPT_PROTOCOL_THREAD
#define PROTOCOL_NAME "thread"
#endif

extern const char portal_html_start[] asm("_binary_ota_portal_html_start");
extern const char portal_html_end[] asm("_binary_ota_portal_html_end");

static const char *TAG = "ota_portal";

/* Static buffers: the HTTP server handles one request at a time. */
static uint8_t s_recv_buf[RECV_BUF_SIZE];
static char s_info_json[384];
static esp_timer_handle_t s_reboot_timer;
static esp_timer_handle_t s_idle_timer;
static volatile bool s_upload_active;

static void reboot_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Rebooting");
    esp_restart();
}

static void idle_timer_cb(void *arg)
{
    (void)arg;
    if (s_upload_active) {
        ESP_ERROR_CHECK(esp_timer_start_once(s_idle_timer, US_PER_MIN)); /* check again later */
        return;
    }
    ESP_LOGI(TAG, "Portal timeout without an update");
    esp_restart();
}

/* ---- HTTP handlers ----------------------------------------------------------------- */

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* EMBED_TXTFILES appends a NUL terminator; do not send it. */
    return httpd_resp_send(req, portal_html_start, portal_html_end - portal_html_start - 1);
}

static esp_err_t info_get_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    snprintf(s_info_json, sizeof(s_info_json),
             "{\"project\":\"%s\",\"version\":\"%s\",\"protocol\":\"%s\",\"idf\":\"%s\","
             "\"build\":\"%s %s\",\"running\":\"%s\",\"target\":\"%s\",\"max_size\":%lu}",
             app->project_name, app->version, PROTOCOL_NAME, app->idf_ver, app->date, app->time,
             running ? running->label : "?", next ? next->label : "?",
             next ? (unsigned long)next->size : 0UL);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, s_info_json);
}

/* Receive exactly len bytes (or fail). */
static esp_err_t recv_exact(httpd_req_t *req, uint8_t *buf, size_t len)
{
    size_t got = 0;
    int timeouts = 0;

    while (got < len) {
        const int n = httpd_req_recv(req, (char *)buf + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= RECV_MAX_TIMEOUTS) {
            continue;
        }
        if (n <= 0) {
            return ESP_FAIL;
        }
        timeouts = 0;
        got += (size_t)n;
    }
    return ESP_OK;
}

/* Reject anything that is not an application image of this project for this chip. */
static const char *check_image(const uint8_t *buf)
{
    const esp_image_header_t *hdr = (const esp_image_header_t *)buf;
    const esp_app_desc_t *desc = (const esp_app_desc_t *)(buf + IMG_DESC_OFFSET);

    if (hdr->magic != ESP_IMAGE_HEADER_MAGIC) {
        return "Not an ESP application image (use the -app.bin, not the -full.bin)";
    }
    if (hdr->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        return "Image is built for a different chip";
    }
    if (desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return "Image has no application descriptor";
    }
    if (strncmp(desc->project_name, esp_app_get_description()->project_name, sizeof(desc->project_name)) != 0) {
        return "Image belongs to a different project";
    }
    return NULL;
}

static esp_err_t update_fail(httpd_req_t *req, const char *status, const char *msg)
{
    ESP_LOGE(TAG, "Update failed: %s", msg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, msg);
    return ESP_OK; /* keep the connection handling sane; the error went to the client */
}

static esp_err_t update_post_handler(httpd_req_t *req)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    const size_t total = req->content_len;

    if (target == NULL) {
        return update_fail(req, "500 Internal Server Error", "No OTA partition available");
    }
    if (total < IMG_MIN_SIZE) {
        return update_fail(req, "400 Bad Request", "File too small to be a firmware image");
    }
    if (total > target->size) {
        return update_fail(req, "413 Payload Too Large", "Image does not fit into the OTA partition");
    }

    ESP_LOGI(TAG, "Receiving %u bytes into %s", (unsigned)total, target->label);
    s_upload_active = true;

    esp_ota_handle_t ota = 0;
    size_t done = 0;
    const char *error = NULL;

    while (done < total) {
        const size_t chunk = MIN(sizeof(s_recv_buf), total - done);
        if (recv_exact(req, s_recv_buf, chunk) != ESP_OK) {
            error = "Upload interrupted";
            break;
        }

        if (done == 0) {
            error = check_image(s_recv_buf);
            if (error != NULL) {
                break;
            }
            const esp_app_desc_t *desc = (const esp_app_desc_t *)(s_recv_buf + IMG_DESC_OFFSET);
            ESP_LOGI(TAG, "New image: %s %s", desc->project_name, desc->version);
            if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
                error = "Could not start writing the OTA partition";
                break;
            }
        }

        if (esp_ota_write(ota, s_recv_buf, chunk) != ESP_OK) {
            error = "Flash write failed";
            break;
        }
        done += chunk;
    }

    if (error != NULL) {
        if (ota != 0) {
            esp_ota_abort(ota);
        }
        s_upload_active = false;
        return update_fail(req, "400 Bad Request", error);
    }

    const esp_err_t err = esp_ota_end(ota); /* verifies the whole image */
    if (err != ESP_OK) {
        s_upload_active = false;
        return update_fail(req, "400 Bad Request",
                           err == ESP_ERR_OTA_VALIDATE_FAILED ? "Image is corrupted" : "Could not finish the update");
    }
    if (esp_ota_set_boot_partition(target) != ESP_OK) {
        s_upload_active = false;
        return update_fail(req, "500 Internal Server Error", "Could not select the new image for boot");
    }

    ESP_LOGI(TAG, "Update written to %s, rebooting in %d ms", target->label, REBOOT_DELAY_MS);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Update OK, rebooting");
    ESP_ERROR_CHECK(esp_timer_start_once(s_reboot_timer, REBOOT_DELAY_MS * US_PER_MS));
    return ESP_OK;
}

/* ---- start-up ---------------------------------------------------------------------- */

static void wifi_ap_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));

    wifi_config_t ap_cfg = {
        .ap = {
            .channel        = CONFIG_RPT_OTA_AP_CHANNEL,
            .max_connection = AP_MAX_CLIENTS,
            .authmode       = (sizeof(CONFIG_RPT_OTA_AP_PASSWORD) > 1) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
        },
    };
    const int ssid_len = snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "%s-%02X%02X",
                                  CONFIG_RPT_OTA_AP_SSID_PREFIX, mac[4], mac[5]);
    ap_cfg.ap.ssid_len = (uint8_t)MIN(ssid_len, (int)sizeof(ap_cfg.ap.ssid));
    strlcpy((char *)ap_cfg.ap.password, CONFIG_RPT_OTA_AP_PASSWORD, sizeof(ap_cfg.ap.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi AP \"%s\" (%s) on channel %d, open http://192.168.4.1/", (char *)ap_cfg.ap.ssid,
             ap_cfg.ap.authmode == WIFI_AUTH_OPEN ? "open" : "WPA2", CONFIG_RPT_OTA_AP_CHANNEL);
}

static void http_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 3;
    cfg.recv_wait_timeout = 10; /* seconds per recv while uploading */
    cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    static const httpd_uri_t uris[] = {
        { .uri = "/",       .method = HTTP_GET,  .handler = root_get_handler },
        { .uri = "/info",   .method = HTTP_GET,  .handler = info_get_handler },
        { .uri = "/update", .method = HTTP_POST, .handler = update_post_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uris[i]));
    }
}

void ota_portal_start(void)
{
    const esp_timer_create_args_t reboot_args = { .callback = reboot_timer_cb, .name = "ota_reboot" };
    const esp_timer_create_args_t idle_args = { .callback = idle_timer_cb, .name = "ota_idle" };
    ESP_ERROR_CHECK(esp_timer_create(&reboot_args, &s_reboot_timer));
    ESP_ERROR_CHECK(esp_timer_create(&idle_args, &s_idle_timer));

    wifi_ap_start();
    http_server_start();

#if CONFIG_RPT_OTA_PORTAL_TIMEOUT_MIN > 0
    ESP_ERROR_CHECK(esp_timer_start_once(s_idle_timer, CONFIG_RPT_OTA_PORTAL_TIMEOUT_MIN * US_PER_MIN));
    ESP_LOGI(TAG, "Portal closes in %d min if no update is uploaded", CONFIG_RPT_OTA_PORTAL_TIMEOUT_MIN);
#endif
}
