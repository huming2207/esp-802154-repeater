set(srcs "app_main.c" "boot_gpio.c" "ota_portal.c")

if(CONFIG_RPT_PROTOCOL_ZIGBEE)
    list(APPEND srcs "zigbee_router.c")
elseif(CONFIG_RPT_PROTOCOL_THREAD)
    list(APPEND srcs "thread_router.c")
endif()

idf_component_register(SRCS ${srcs}
                       PRIV_INCLUDE_DIRS "."
                       EMBED_TXTFILES "ota_portal.html"
                       PRIV_REQUIRES nvs_flash esp_driver_gpio esp_timer app_update
                                     esp_wifi esp_netif esp_http_server bootloader_support
                                     openthread vfs esp_event)
