# ESP32-C6 802.15.4 repeater: a Zigbee router or a Thread router (chosen at build time).
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Keep the build small: only pull in what "main" needs.
idf_build_set_property(MINIMAL_BUILD ON)

project(esp_802154_repeater)
