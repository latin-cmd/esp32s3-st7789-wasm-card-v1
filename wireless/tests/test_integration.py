"""Real HTTP + MQTT + WebSocket tests, including cross-tenant rejection."""
import asyncio
import json
import secrets
import socket
import tempfile
import unittest
import zlib

from aiohttp import web, ClientSession, CookieJar, WSServerHandshakeError, WSMsgType
from amqtt.client import MQTTClient

from wireless.server import make_app, IMAGE_BYTES


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class WirelessIntegration(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.port, self.mqtt_port = free_port(), free_port()
        self.base = f"http://127.0.0.1:{self.port}"
        self.app = make_app(self.temp.name, self.base, f"127.0.0.1:{self.mqtt_port}",
                            f"mqtt://127.0.0.1:{self.mqtt_port}")
        self.runner = web.AppRunner(self.app)
        await self.runner.setup()
        await web.TCPSite(self.runner, "127.0.0.1", self.port).start()
        self.clients = [ClientSession(cookie_jar=CookieJar(unsafe=True)) for _ in range(3)]

    async def asyncTearDown(self):
        for client in self.clients:
            await client.close()
        await self.runner.cleanup()
        self.temp.cleanup()

    async def post(self, client, path, data, status=200):
        async with client.post(self.base + path, json=data) as response:
            value = await response.json()
            self.assertEqual(response.status, status, value)
            return value

    async def test_tenant_assignment_and_binary_image_roundtrip(self):
        a, b, device_http = self.clients
        async with a.get(self.base + "/api/auth/session") as response:
            self.assertEqual(response.status, 200)
            self.assertIsNone((await response.json())["user"])
        tenant = (await self.post(a, "/api/auth/signup", {"email":"a@example.test",
            "password":"integration-password-a", "tenant_name":"Workspace A"}))["user"]["tenant_id"]
        async with a.get(self.base + "/api/auth/session") as response:
            self.assertEqual(response.status, 200)
            self.assertEqual((await response.json())["user"]["tenant_id"], tenant)
        await self.post(b, "/api/auth/signup", {"email":"b@example.test",
            "password":"integration-password-b", "tenant_name":"Workspace B"})
        device_id, secret = "card-123456abcdef", secrets.token_hex(32)
        registration = {"device_id":device_id,"secret":secret,"model":"ESP32-S3"}
        enrollment = await self.post(device_http, "/api/device/register", registration)
        await self.post(device_http, "/api/device/register", {**registration,"secret":"f"*64}, 403)
        mqtt = MQTTClient(client_id=device_id)
        await mqtt.connect(f"mqtt://{device_id}:{secret}@127.0.0.1:{self.mqtt_port}")
        try:
            result = await mqtt.subscribe([(f"cards/{device_id}/assignment",1)])
            self.assertEqual(result, [1])
            denied = await mqtt.subscribe([("cards/other/assignment",1)])
            self.assertEqual(denied, [128], "A device must not subscribe to another device")
            other_id = "card-000000000001"
            await self.post(device_http, "/api/device/register", {"device_id":other_id,"secret":secrets.token_hex(32)})
            await mqtt.publish(f"cards/{other_id}/status",b'{"state":"forged"}',qos=1)
            await mqtt.publish(f"cards/{device_id}/status",b'{"state":"online"}',qos=1)
            for _ in range(30):
                if self.app["store"].device(device_id)["last_seen"]:break
                await asyncio.sleep(.02)
            self.assertGreater(self.app["store"].device(device_id)["last_seen"],0)
            self.assertEqual(self.app["store"].device(other_id)["last_seen"],0,
                             "Cross-device MQTT publishing must be rejected")
            await self.post(a, "/api/devices/claim", {"device_id":device_id,"claim_code":enrollment["claim_code"]})
            packet = await asyncio.wait_for(mqtt.deliver_message(), 5)
            assignment = json.loads(bytes(packet.publish_packet.payload.data))
            self.assertEqual(assignment["tenant_id"], tenant)
            await self.post(b, "/api/devices/claim", {"device_id":device_id,"claim_code":enrollment["claim_code"]},409)
            async with b.get(self.base+f"/api/devices/{device_id}/image") as response:
                self.assertEqual(response.status,404)
            with self.assertRaises(WSServerHandshakeError):
                await device_http.ws_connect(self.base+"/ws/device", headers={"X-Device-ID":device_id,"Authorization":"Bearer wrong"})
            with self.assertRaises(WSServerHandshakeError):
                await a.ws_connect(self.base+"/ws/browser",headers={"Origin":"https://foreign.example"})
            async with device_http.ws_connect(assignment["websocket_url"], headers={
                "X-Device-ID":device_id,"Authorization":"Bearer "+secret
            }) as device_ws, a.ws_connect(self.base+"/ws/browser",headers={"Origin":self.base}) as browser:
                await device_ws.receive_json(timeout=3)
                await browser.receive_json(timeout=3)
                payload = bytes(range(256)) * 525  # exactly 134,400 bytes
                self.assertEqual(len(payload),IMAGE_BYTES)
                crc = zlib.crc32(payload)
                await browser.send_json({"type":"image_begin","device_id":device_id,"size":IMAGE_BYTES,"crc32":crc})
                ready = await browser.receive_json(timeout=3)
                self.assertEqual(ready["type"],"image_ready")
                for offset in range(0,len(payload),2048):
                    await browser.send_bytes(payload[offset:offset+2048])
                await browser.send_json({"type":"image_commit","request_id":ready["request_id"]})
                begin = await device_ws.receive_json(timeout=5)
                self.assertEqual(begin["type"],"image_begin")
                received=bytearray()
                while True:
                    message = await device_ws.receive(timeout=5)
                    if message.type == WSMsgType.BINARY:
                        received.extend(message.data)
                    else:
                        commit=json.loads(message.data)
                        self.assertEqual(commit["type"],"image_commit")
                        break
                self.assertEqual(bytes(received),payload)
                await device_ws.send_json({"type":"image_ack","request_id":begin["request_id"],"ok":True,"crc32":crc})
                for _ in range(30):
                    if self.app["store"].device(device_id)["image_state"]=="applied":break
                    await asyncio.sleep(.02)
                self.assertEqual(self.app["store"].device(device_id)["image_state"],"applied")
                async with a.get(self.base+f"/api/devices/{device_id}/image") as response:
                    self.assertEqual(await response.read(),payload)
            async with b.ws_connect(self.base+"/ws/browser",headers={"Origin":self.base}) as forbidden:
                await forbidden.receive_json(timeout=3)
                await forbidden.send_json({"type":"image_begin","device_id":device_id,"size":IMAGE_BYTES,"crc32":crc})
                self.assertEqual((await forbidden.receive_json(timeout=3))["type"],"error")
        finally:
            await mqtt.disconnect()

    async def test_unclaimed_and_corrupt_images_are_rejected(self):
        a, _, device_http = self.clients
        await self.post(a,"/api/auth/signup",{"email":"c@example.test","password":"integration-password-c","tenant_name":"C"})
        device_id, secret="card-abcdef123456",secrets.token_hex(32)
        enrollment=await self.post(device_http,"/api/device/register",{"device_id":device_id,"secret":secret})
        with self.assertRaises(WSServerHandshakeError):
            await device_http.ws_connect(self.base+"/ws/device",headers={"X-Device-ID":device_id,"Authorization":"Bearer "+secret})
        await self.post(a,"/api/devices/claim",{"device_id":device_id,"claim_code":enrollment["claim_code"]})
        async with a.ws_connect(self.base+"/ws/browser",headers={"Origin":self.base}) as browser:
            await browser.receive_json(timeout=3)
            await browser.send_json({"type":"image_begin","device_id":device_id,"size":IMAGE_BYTES,"crc32":1})
            ready=await browser.receive_json(timeout=3)
            await browser.send_bytes(bytes(IMAGE_BYTES))
            await browser.send_json({"type":"image_commit","request_id":ready["request_id"]})
            self.assertEqual((await browser.receive_json(timeout=3))["type"],"error")
            self.assertIsNone(self.app["store"].device(device_id)["image"])

    async def test_offline_queue_survives_server_restart(self):
        a, _, device_http = self.clients
        await self.post(a,"/api/auth/signup",{"email":"persist@example.test","password":"integration-password-p","tenant_name":"Persist"})
        device_id,secret="card-000000000002",secrets.token_hex(32)
        enrollment=await self.post(device_http,"/api/device/register",{"device_id":device_id,"secret":secret})
        await self.post(a,"/api/devices/claim",{"device_id":device_id,"claim_code":enrollment["claim_code"]})
        payload=bytes([0x12,0x34])*(IMAGE_BYTES//2)
        crc=zlib.crc32(payload)
        async with a.ws_connect(self.base+"/ws/browser",headers={"Origin":self.base}) as browser:
            await browser.receive_json(timeout=3)
            await browser.send_json({"type":"image_begin","device_id":device_id,"size":IMAGE_BYTES,"crc32":crc})
            ready=await browser.receive_json(timeout=3)
            await browser.send_bytes(payload)
            await browser.send_json({"type":"image_commit","request_id":ready["request_id"]})
            self.assertEqual((await browser.receive_json(timeout=3))["type"],"image_queued")
        self.assertEqual(self.app["store"].device(device_id)["image_state"],"queued")
        await self.runner.cleanup()
        self.app=make_app(self.temp.name,self.base,f"127.0.0.1:{self.mqtt_port}",f"mqtt://127.0.0.1:{self.mqtt_port}")
        self.runner=web.AppRunner(self.app)
        await self.runner.setup()
        await web.TCPSite(self.runner,"127.0.0.1",self.port).start()
        again=await self.post(device_http,"/api/device/register",{"device_id":device_id,"secret":secret})
        self.assertTrue(again["claimed"])
        async with device_http.ws_connect(self.base+"/ws/device",headers={"X-Device-ID":device_id,"Authorization":"Bearer "+secret}) as ws:
            await ws.receive_json(timeout=3)
            begin=await ws.receive_json(timeout=3)
            received=bytearray()
            while True:
                message=await ws.receive(timeout=3)
                if message.type==WSMsgType.BINARY:received.extend(message.data)
                else:break
            self.assertEqual(bytes(received),payload)
            # A mismatched checksum in a device receipt must never become "applied".
            await ws.send_json({"type":"image_ack","request_id":begin["request_id"],"ok":True,"crc32":crc^1})
            for _ in range(30):
                if self.app["store"].device(device_id)["image_state"]=="failed":break
                await asyncio.sleep(.02)
            self.assertEqual(self.app["store"].device(device_id)["image_state"],"failed")


if __name__=="__main__":
    unittest.main()
