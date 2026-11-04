/*
 * Firmware update portal: Wi-Fi access point + web page to upload a new image.
 */
#pragma once

/**
 * Start the access point and the HTTP server, then return.
 *
 * The 802.15.4 stack is not started in this mode. After a successful upload
 * the device reboots into the new image. Without an upload the device reboots
 * after CONFIG_RPT_OTA_PORTAL_TIMEOUT_MIN minutes (0 = stay forever).
 */
void ota_portal_start(void);
