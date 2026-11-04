"""Explicitly labelled software device for MQTT/WebSocket integration testing."""
import argparse
import asyncio
import contextlib
import json
import secrets
import zlib
from urllib.parse import urlsplit, quote

from aiohttp import ClientSession, WSMsgType
from amqtt.client import MQTTClient

from .server import IMAGE_BYTES


async def run(base, device_id=None):
    device_id = device_id or "sim-" + secrets.token_hex(6)
    secret = secrets.token_hex(32)
    async with ClientSession() as session:
        async with session.post(base.rstrip("/") + "/api/device/register", json={
            "device_id": device_id, "secret": secret, "model": "simulator"
        }) as response:
            response.raise_for_status()
            enrollment = await response.json()
        print(f"SIMULATOR (not physical ESP32): {device_id}", flush=True)
        print(f"Claim code: {enrollment['claim_code']}", flush=True)
        mqtt_url = urlsplit(enrollment["mqtt_uri"])
        mqtt = MQTTClient(client_id=device_id)
        await mqtt.connect(f"mqtt://{quote(device_id)}:{secret}@{mqtt_url.hostname}:{mqtt_url.port or 1883}")
        await mqtt.subscribe([(f"cards/{device_id}/assignment", 1)])
        await mqtt.publish(f"cards/{device_id}/status", json.dumps({
            "state": "online", "firmware": "simulator", "ip": "software-device"
        }).encode(), qos=1)
        try:
            packet = await mqtt.deliver_message()
            assignment = json.loads(bytes(packet.publish_packet.payload.data))
            print(f"MQTT tenant assigned: {assignment['tenant_id']}", flush=True)
            async with session.ws_connect(assignment["websocket_url"], headers={
                "Authorization": "Bearer " + secret, "X-Device-ID": device_id
            }, heartbeat=15) as socket:
                transfer, payload = None, bytearray()
                async def heartbeat():
                    while True:
                        await asyncio.sleep(20)
                        await mqtt.publish(f"cards/{device_id}/status", b'{"state":"online","firmware":"simulator"}', qos=1)
                task = asyncio.create_task(heartbeat())
                try:
                    async for message in socket:
                        if message.type == WSMsgType.TEXT:
                            data = json.loads(message.data)
                            if data.get("type") == "image_begin":
                                transfer, payload = data, bytearray()
                            elif data.get("type") == "image_commit" and transfer:
                                crc = zlib.crc32(payload)
                                ok = (len(payload) == IMAGE_BYTES and crc == transfer["crc32"] and
                                      data.get("request_id") == transfer["request_id"])
                                await socket.send_json({"type": "image_ack", "request_id": transfer["request_id"],
                                                        "ok": ok, "crc32": crc})
                                print(f"SIMULATED image apply: {len(payload)} bytes, crc32={crc:08x}, ok={ok}", flush=True)
                                transfer = None
                        elif message.type == WSMsgType.BINARY and transfer:
                            payload.extend(message.data)
                            if len(payload) > IMAGE_BYTES:
                                raise ValueError("Image exceeds limit")
                finally:
                    task.cancel()
                    with contextlib.suppress(asyncio.CancelledError):
                        await task
        finally:
            await mqtt.disconnect()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", default="http://localhost:8787")
    parser.add_argument("--device-id")
    args = parser.parse_args()
    asyncio.run(run(args.server, args.device_id))
