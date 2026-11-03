set(srcs "app_main.c" "pair_gpio.c")

if(CONFIG_RPT_PROTOCOL_ZIGBEE)
    list(APPEND srcs "zigbee_router.c")
elseif(CONFIG_RPT_PROTOCOL_THREAD)
    list(APPEND srcs "thread_router.c")
endif()

idf_component_register(SRCS ${srcs}
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash esp_driver_gpio esp_timer)
