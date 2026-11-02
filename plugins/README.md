# WASM card plugin ABI (V1)

## Building a plugin

This folder contains:

- `example/plugin.c`: C source of the sample plugin
- `example/plugin.wat`: the same sample in WebAssembly text format
- `example/card_plugin.wasm`: a prebuilt copy for immediate upload testing

Build from C with Clang (wasm32 target):

```powershell
clang --target=wasm32 -O2 -nostdlib -Wl,--no-entry -Wl,--allow-undefined `
      -o card_plugin.wasm example/plugin.c
```

Or from the text format with WABT:

```powershell
wat2wasm example/plugin.wat -o card_plugin.wasm
```

Upload the result from the computer-side editor in `../host`.

## ABI

The module must export `plugin_render()` and may export `plugin_init()`. Both
take no arguments and return no value. The firmware exposes these imports from
module `env`:

```c
void host_clear(int32_t rgb565);
void host_fill_rect(int32_t x, int32_t y, int32_t width, int32_t height,
                    int32_t rgb565);
int32_t host_blit_image(int32_t image_id, int32_t x, int32_t y);
int32_t host_present(void);
```

`image_id == 0` selects the 240x280 RGB565 image uploaded through the page.
Rectangles and blits are clipped to the screen; any int32 coordinate is safe.
If `plugin_render` returns without calling `host_present`, the firmware
presents the frame itself.

The plugin cannot access LVGL, SPI, files, Wi-Fi, or native memory directly.

## Limits (per exported call)

| Limit | Value |
| --- | --- |
| Module file size | 8 B to 128 KB |
| Linear memory (module + 64 KB app heap) | 16 pages (1 MB), including `memory.grow` |
| WebAssembly instructions | 2,000,000 |
| `host_present` calls | 2 |
| `host_blit_image` calls | 4 |
| Pixels written by clear/fill/blit | 16 full screens |

Exceeding a limit aborts the call with an error such as
`host budget exceeded: present`. A plugin that fails while being uploaded is
not saved, and the previous screen is redrawn.

## Test plugins

`tests/` holds small modules that must each be rejected or handled safely
(`tests/*.wat` is the source):

| File | Expected result |
| --- | --- |
| `infinite_loop.wasm` | `instruction limit exceeded` |
| `present_spam.wasm` | `host budget exceeded: present` |
| `blit_spam.wasm` | `host budget exceeded: blit_image` |
| `fill_spam.wasm` | `host budget exceeded: pixels` |
| `big_memory.wasm` | `Plugin memory exceeds 1024 KB` |
| `grow_memory.wasm` | loads; the oversized `memory.grow` returns -1 |
| `blit_overflow.wasm` | loads; extreme coordinates draw nothing and do not crash |
| `draw_then_trap.wasm` | `unreachable`; the previous content is redrawn |
| `malformed.wasm` | rejected by the loader |
