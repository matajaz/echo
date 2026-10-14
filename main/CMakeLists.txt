idf_component_register(
    SRCS "app_main.c" "ota.c" "maintenance_api.c" "display.c" "touch.c" "weblog.c" "ui.c" "ui_graphs.c" "sensors.c" "audio.c" "ha_client.c" "board_services.c" "echo_sun_20.c" "../core/ha_parse.c" "../core/dashboard_model.c" "ha_energy.c"
        "ui/ui_shell.c" "ui/ui_widgets.c"
        "ui/pages/overview.c" "ui/pages/energy_now.c" "ui/pages/energy_today.c"
        "ui/pages/energy_prices.c" "ui/pages/energy_costs.c" "ui/pages/dashboard.c" "ui/pages/power_history.c" "ui/fonts/font_montserrat_14_sv.c" "ui/pages/temperatures.c" "ui/pages/batteries.c"
        "ui/pages/melodies.c" "ui/pages/device_info.c"
    INCLUDE_DIRS "." "ui" "../secrets" "../core"
    EMBED_TXTFILES "certs/ota_ca.pem"
    REQUIRES
        esp_event esp_netif esp_wifi nvs_flash espressif__cjson espressif__esp_websocket_client
        esp_http_client esp_http_server esp_https_ota app_update
        esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_spi
        esp_lcd
        esp_io_expander esp_lcd_touch_cst3530 esp_codec_dev esp_lvgl_port
)
