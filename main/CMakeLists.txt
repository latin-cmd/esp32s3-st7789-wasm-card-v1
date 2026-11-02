# img_test.c (~17 MB of source for an unused test image) is no longer compiled.
idf_component_register(
    SRCS
        "main.c"
        "font_card_name_24.c"
        "font_card_cjk_16.c"
        "card_storage.c"
        "card_display.c"
        "card_serial.c"
        "wasm_plugin.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_driver_usb_serial_jtag
)
