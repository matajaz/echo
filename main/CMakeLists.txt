idf_component_register(
    SRCS "app_main.c" "ota.c" "display.c" "touch.c" "weblog.c" "ui.c" "sensors.c" "audio.c"
    INCLUDE_DIRS "." "../secrets"
    EMBED_TXTFILES "certs/ota_ca.pem"
    REQUIRES
        esp_event esp_netif esp_wifi nvs_flash
        esp_http_client esp_http_server esp_https_ota app_update
        esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_spi
        esp_lcd
        esp_io_expander esp_lcd_touch_cst3530 esp_codec_dev esp_lvgl_port
)
