# Только SDK bootstrap; active39 собирает tools/build.py по ordered recipe.
idf_component_register(SRCS "bootstrap.c" INCLUDE_DIRS "." REQUIRES esp_timer nvs_flash esp_rom esp_wifi protocomm driver wpa_supplicant esp_driver_sdio esp_pm openthread app_update protobuf-c)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_wifi_init")
