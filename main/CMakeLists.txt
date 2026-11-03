set(srcs "app_main.c" "pair_gpio.c")

idf_component_register(SRCS ${srcs}
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash esp_driver_gpio esp_timer)
