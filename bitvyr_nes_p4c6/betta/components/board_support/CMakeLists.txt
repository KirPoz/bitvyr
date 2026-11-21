# Фиксированный Board ABI:3native, PSRAM RGB, синхронный GT911 и partial UI bars.
idf_component_register(
    SRCS "Board.cpp" "display/Display.cpp" "display/Text.cpp"
         "input/Touch.cpp" "input/BluetoothInput.cpp" "input/HidGamepad.cpp"
         "audio/Audio.cpp" "storage/Storage.cpp"
    INCLUDE_DIRS "include"
    REQUIRES log efuse esp_lcd esp_psram esp_hw_support esp_driver_gpio
             esp_driver_i2c esp_driver_i2s esp_driver_ppa esp_driver_sdmmc
             sdmmc fatfs bt nvs_flash esp_event esp_timer esp_mm espressif__esp_hosted
)
include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/NesCxxStandard.cmake")
nes_require_cxx17(${COMPONENT_LIB} PUBLIC)
