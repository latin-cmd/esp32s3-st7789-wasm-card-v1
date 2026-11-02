"""Small CARD/1 smoke test used during firmware development."""
import binascii
import json
import sys
import time

import serial


def request(port, request_id, command, payload=b"", timeout=15):
    crc = binascii.crc32(payload) & 0xFFFFFFFF
    port.write(f"CARD/1 {request_id} {command} {len(payload)} {crc:08x}\n".encode())
    for offset in range(0, len(payload), 1024):
        port.write(payload[offset:offset + 1024])
        port.flush()
        time.sleep(0.005)
    port.flush()
    deadline = time.monotonic() + timeout
    prefix = f"@CARD {request_id} ".encode()
    while time.monotonic() < deadline:
        line = port.readline()
        if not line.startswith(prefix):
            continue
        _, _, status, raw_json = line.decode(errors="replace").strip().split(" ", 3)
        data = json.loads(raw_json)
        if status != "OK":
            raise RuntimeError(data.get("message", "device error"))
        return data
    raise TimeoutError(f"timeout waiting for {command}")


if __name__ == "__main__":
    com_port = sys.argv[1] if len(sys.argv) > 1 else "COM10"
    plugin_path = sys.argv[2] if len(sys.argv) > 2 else "../plugins/example/card_plugin.wasm"
    with serial.Serial(com_port, 115200, timeout=0.5, write_timeout=10) as device:
        time.sleep(0.4)
        print(request(device, 1, "STATUS"))
        payload = open(plugin_path, "rb").read()
        print(request(device, 2, "PUT_PLUGIN", payload, timeout=30))
        print(request(device, 3, "STATUS"))
