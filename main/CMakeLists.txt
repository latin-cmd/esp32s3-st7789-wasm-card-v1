# img_test.c (~17 MB of source for an unused test image) is no longer compiled.
idf_component_register(
    SRCS
        "main.c"
        "font_card_name_24.c"
        "font_card_cjk_16.c"
        "card_storage.c"
        "card_display.c"
        "card_serial.c"
        "card_network.c"
        "wasm_plugin.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_driver_usb_serial_jtag esp_wifi esp_netif esp_event
                  esp_http_server esp_http_client mqtt mbedtls esp_timer json
    EMBED_TXTFILES "wifi_portal.html"
)
