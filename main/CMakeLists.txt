idf_component_register(
    SRCS
        "app_main.c"
        "usb_cdc_source.c"
        "nmea_source.c"
        "mosaic_config.c"
        "sbf_parser.c"
        "cutfill.c"
        "fieldmap.c"
        "gnss_state.c"
        "leveler.c"
        "ntrip_client.c"
        "debug_console.c"
        "net_mdns.c"
        "board_power.c"
        "wifi_sta.c"
        "display.c"
        "backlight.c"
        "touch.c"
        "tab5_ili9881c_init.c"
        "esp_lcd_st7123.c"
        "tab5_st7123_init.c"
        "status_screen.c"
        "map_view.c"
        "gnss_view.c"
        "web_server.c"
    INCLUDE_DIRS "."
    REQUIRES console esp_timer mdns esp_driver_i2c esp_netif esp_event
             esp_wifi esp_wifi_remote nvs_flash lwip esp_http_server esp_http_client mbedtls
             esp_lcd esp_lcd_ili9881c esp_driver_ledc esp_lvgl_port esp_mm
)
