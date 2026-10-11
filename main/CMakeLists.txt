idf_component_register(SRCS "main.c" "board.c" "sl_api.c" "ui.c" "controls.c" "metro_data.c" "screenshot.c" "serial.c" "improv.c" "wifi_setup.c" "web.c"
                            "fonts/sl_font_12.c" "fonts/sl_font_16.c" "fonts/sl_font_22.c"
                       PRIV_REQUIRES esp_lcd esp_driver_spi esp_driver_gpio esp_timer
                                     esp_wifi esp_netif esp_event nvs_flash
                                     esp_http_client mbedtls esp_driver_usb_serial_jtag esp_app_format esp_http_server dns_server
                       INCLUDE_DIRS ".")
