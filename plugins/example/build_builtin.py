"""Create the dependency-free V1 example plugin as a WebAssembly binary."""
from pathlib import Path


def uleb(value: int) -> bytes:
    encoded = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        encoded.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(encoded)


def name(value: str) -> bytes:
    raw = value.encode("utf-8")
    return uleb(len(raw)) + raw


def vector(items: list[bytes]) -> bytes:
    return uleb(len(items)) + b"".join(items)


def section(section_id: int, payload: bytes) -> bytes:
    return bytes([section_id]) + uleb(len(payload)) + payload


i32 = b"\x7f"
types = vector([
    b"\x60" + vector([i32]) + vector([]),
    b"\x60" + vector([i32, i32, i32]) + vector([i32]),
    b"\x60" + vector([]) + vector([i32]),
    b"\x60" + vector([]) + vector([]),
])
imports = vector([
    name("env") + name("host_clear") + b"\x00" + uleb(0),
    name("env") + name("host_blit_image") + b"\x00" + uleb(1),
    name("env") + name("host_present") + b"\x00" + uleb(2),
])
functions = vector([uleb(3), uleb(3)])
exports = vector([
    name("plugin_init") + b"\x00" + uleb(3),
    name("plugin_render") + b"\x00" + uleb(4),
])
init_body = b"\x00\x0b"
render_body = bytes([
    0x00,             # local declarations
    0x41, 0x00,       # i32.const 0
    0x10, 0x00,       # call host_clear
    0x41, 0x00, 0x41, 0x00, 0x41, 0x00,
    0x10, 0x01, 0x1A, # call host_blit_image; drop
    0x10, 0x02, 0x1A, # call host_present; drop
    0x0B,
])
code = vector([uleb(len(init_body)) + init_body,
               uleb(len(render_body)) + render_body])

module = (b"\x00asm\x01\x00\x00\x00" + section(1, types) +
          section(2, imports) + section(3, functions) +
          section(7, exports) + section(10, code))
output = Path(__file__).with_name("card_plugin.wasm")
output.write_bytes(module)
print(f"Built {output} ({len(module)} bytes)")
