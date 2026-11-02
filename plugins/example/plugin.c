#include <stdint.h>

/* Host API exposed by the ESP32 firmware. Colors are RGB565 values. */
extern void host_clear(int32_t color);
extern void host_fill_rect(int32_t x, int32_t y, int32_t width, int32_t height,
                           int32_t color);
extern int32_t host_blit_image(int32_t image_id, int32_t x, int32_t y);
extern int32_t host_present(void);

__attribute__((export_name("plugin_init")))
void plugin_init(void)
{
    /* Reserved for future plugin state initialization. */
}

__attribute__((export_name("plugin_render")))
void plugin_render(void)
{
    host_clear(0x0000);
    if (host_blit_image(0, 0, 0) != 0) {
        /* Visible fallback when no uploaded image exists. */
        host_clear(0x18E3);
        host_fill_rect(12, 12, 216, 256, 0x2124);
        host_fill_rect(20, 20, 200, 6, 0xFDC0);
        host_fill_rect(20, 254, 200, 6, 0xFDC0);
    }
    host_present();
}
