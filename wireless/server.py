"""Local-first multi-tenant card service: MQTT assignment + binary WebSocket images."""
import asyncio
import contextlib
import json
import logging
import os
import re
import secrets
import sqlite3
import time
import zlib
from collections import defaultdict, deque
from pathlib import Path
from urllib.parse import urlsplit

from aiohttp import web, WSMsgType
from amqtt.broker import Broker
from amqtt.client import MQTTClient
from amqtt.mqtt.constants import QOS_1

from . import broker as broker_plugins
from .store import Store, digest, password_matches

IMAGE_BYTES = 240 * 280 * 2
DEVICE_ID = re.compile(r"(?:card|sim)-[a-f0-9]{12}\Z")
STATIC = Path(__file__).with_name("static")
LOG = logging.getLogger("card.cloud")


def json_response(value, status=200):
    return web.json_response(value, status=status, headers={"Cache-Control": "no-store"})


@web.middleware
async def guard(request, handler):
    try:
        origin = request.headers.get("Origin")
        if request.method not in ("GET", "HEAD", "OPTIONS") and origin:
            parsed = urlsplit(origin)
            if parsed.scheme not in ("http", "https") or parsed.netloc != request.host:
                raise web.HTTPForbidden(text="跨站请求已拒绝")
        if request.path.startswith("/api/") and request.method == "POST":
            key = (request.remote, request.path)
            bucket = request.app["limits"][key]
            now = time.monotonic()
            while bucket and now - bucket[0] > 60:
                bucket.popleft()
            if len(bucket) >= 30:
                raise web.HTTPTooManyRequests(text="请求过于频繁，请稍后重试")
            bucket.append(now)
        response = await handler(request)
    except web.HTTPException as exc:
        response = json_response({"error": exc.text}, exc.status)
    except (ValueError, TypeError, KeyError, json.JSONDecodeError):
        response = json_response({"error": "请求格式或参数不正确"}, 400)
    if not response.prepared:
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["Referrer-Policy"] = "no-referrer"
        response.headers["X-Frame-Options"] = "DENY"
        response.headers["Content-Security-Policy"] = (
            "default-src 'self'; img-src 'self' blob: data:; connect-src 'self'; "
            "style-src 'self'; script-src 'self'; frame-ancestors 'none'")
    return response


def current_user(request):
    user = request.app["store"].user(request.cookies.get("card_session"))
    if not user:
        raise web.HTTPUnauthorized(text="请先登录")
    return user


async def object_json(request):
    value = await request.json()
    if not isinstance(value, dict):
        raise web.HTTPBadRequest(text="请求正文必须是 JSON 对象")
    return value


def require_owner(request, device_id):
    user = current_user(request)
    if not request.app["store"].owned(device_id, user["tenant_id"]):
        raise web.HTTPNotFound(text="设备不存在或不属于当前租户")
    return user


def public_devices(app, tenant):
    result = app["store"].devices(tenant)
    for device in result:
        device["online"] = device["id"] in app["device_sockets"]
        device["mqtt_recent"] = time.time() - device["last_seen"] < 45
        device["status"] = json.loads(device["status"])
    return result


async def notify(app, tenant):
    data = {"type": "devices", "devices": public_devices(app, tenant)}
    for user, socket, token in list(app["browser_sockets"]):
        if user["tenant_id"] == tenant and not socket.closed:
            if not app["store"].user(token):
                await socket.close(code=1008, message=b"session expired")
                continue
            with contextlib.suppress(ConnectionError, RuntimeError):
                await socket.send_json(data)


async def assignment(app, device_id):
    device = app["store"].device(device_id)
    if not device or not device["tenant_id"]:
        return
    base = app["public_base"]
    ws_base = ("wss://" if base.startswith("https://") else "ws://") + base.split("://", 1)[1]
    payload = {"type": "assignment", "device_id": device_id,
               "tenant_id": device["tenant_id"], "websocket_url": ws_base + "/ws/device",
               "protocol": "CARD-WIFI/1"}
    await app["mqtt"].publish(f"cards/{device_id}/assignment",
                              json.dumps(payload).encode(), qos=QOS_1, retain=True)


async def mqtt_reader(app):
    while True:
        packet = await app["mqtt"].deliver_message()
        topic = packet.publish_packet.variable_header.topic_name
        parts = topic.split("/")
        if len(parts) != 3 or parts[0] != "cards" or parts[2] != "status":
            continue
        device_id = parts[1]
        payload = bytes(packet.publish_packet.payload.data)
        if len(payload) > 4096:
            continue
        try:
            status = json.loads(payload)
            if not isinstance(status, dict):
                continue
            # The authenticated MQTT topic, rather than a claimed JSON ID, identifies the device.
            status = {key: status[key] for key in ("state", "ip", "firmware", "rssi") if key in status}
            device = app["store"].device(device_id)
            if not device:
                continue
            app["store"].write("UPDATE devices SET last_seen=?,status=? WHERE id=?",
                                 (time.time(), json.dumps(status), device_id))
            if device["tenant_id"]:
                # Re-send after device or broker restart; the payload carries the server-owned tenant.
                await assignment(app, device_id)
                await notify(app, device["tenant_id"])
        except (ValueError, TypeError, KeyError):
            LOG.warning("Rejected malformed device status")


async def health(request):
    return json_response({"ok": True, "mqtt": True, "image_bytes": IMAGE_BYTES,
                          "public_base_url": request.app["public_base"], "protocol": "CARD-WIFI/1"})


async def auth(request):
    action = request.match_info.get("action", "session")
    store = request.app["store"]
    if action == "session":
        return json_response({"user": store.user(request.cookies.get("card_session"))})
    if action == "logout":
        token = request.cookies.get("card_session", "")
        store.write("DELETE FROM sessions WHERE token_hash=?", (digest(token),))
        for _, socket, session_token in list(request.app["browser_sockets"]):
            if session_token == token:
                await socket.close(code=1008, message=b"logged out")
        response = json_response({"ok": True})
        response.del_cookie("card_session")
        return response
    data = await object_json(request)
    email = str(data.get("email", "")).strip().lower()
    password = str(data.get("password", ""))
    if not re.fullmatch(r"[^\s@]{1,100}@[^\s@]{1,150}", email) or not 10 <= len(password) <= 128:
        raise web.HTTPBadRequest(text="请输入邮箱和 10–128 位密码")
    if action == "signup":
        name = str(data.get("tenant_name", "")).strip()
        if not 1 <= len(name) <= 64:
            raise web.HTTPBadRequest(text="请输入 1–64 字的工作空间名称")
        try:
            store.signup(email, password, name)
        except sqlite3.IntegrityError:
            raise web.HTTPConflict(text="该邮箱已注册") from None
    elif action == "login":
        row = store.one("SELECT password FROM users WHERE email=?", (email,))
        if not row or not password_matches(password, row["password"]):
            raise web.HTTPUnauthorized(text="邮箱或密码不正确")
    else:
        raise web.HTTPNotFound()
    token = store.session(email)
    response = json_response({"user": store.user(token)})
    response.set_cookie("card_session", token, httponly=True, samesite="Strict", max_age=86400,
                        secure=request.app["public_base"].startswith("https://"))
    return response


async def register_device(request):
    data = await object_json(request)
    device_id, secret = data.get("device_id", ""), data.get("secret", "")
    model = data.get("model", "ESP32-S3")
    if not isinstance(device_id, str) or not DEVICE_ID.fullmatch(device_id):
        raise web.HTTPBadRequest(text="设备编号格式不正确")
    if not isinstance(secret, str) or not re.fullmatch(r"[a-f0-9]{64}", secret):
        raise web.HTTPBadRequest(text="设备凭据格式不正确")
    if model not in ("ESP32-S3", "simulator"):
        raise web.HTTPBadRequest(text="不支持该设备类型")
    try:
        device = request.app["store"].register(device_id, secret, model)
    except PermissionError:
        raise web.HTTPForbidden(text="设备凭据不匹配") from None
    return json_response({"device_id": device_id, "claim_code": device["claim_code"],
                          "claim_expires": device["claim_expires"],
                          "claimed": bool(device["tenant_id"]), "mqtt_uri": request.app["mqtt_public"]})


async def claim_device(request):
    user = current_user(request)
    data = await object_json(request)
    device_id = str(data.get("device_id", "")).strip().lower()
    code = str(data.get("claim_code", "")).replace("-", "").strip().upper()
    if not request.app["store"].claim(device_id, code, user["tenant_id"]):
        raise web.HTTPConflict(text="认领码不正确、已过期，或设备已被认领")
    await assignment(request.app, device_id)
    await notify(request.app, user["tenant_id"])
    return json_response({"ok": True, "devices": public_devices(request.app, user["tenant_id"])})


async def list_devices(request):
    user = current_user(request)
    return json_response({"devices": public_devices(request.app, user["tenant_id"])})


async def get_image(request):
    device_id = request.match_info["device_id"]
    user = require_owner(request, device_id)
    device = request.app["store"].owned(device_id, user["tenant_id"])
    if device["image"] is None:
        raise web.HTTPNotFound(text="设备还没有图片")
    return web.Response(body=device["image"], content_type="application/octet-stream",
                        headers={"Cache-Control": "no-store"})


async def dispatch_image(app, device_id):
    async with app["device_locks"][device_id]:
        socket = app["device_sockets"].get(device_id)
        device = app["store"].device(device_id)
        if socket is None or socket.closed or not device or device["image"] is None:
            return
        transfer = device["image_request"]
        try:
            await socket.send_json({"type": "image_begin", "request_id": transfer,
                                    "size": IMAGE_BYTES, "crc32": device["image_crc"]})
            data = device["image"]
            for offset in range(0, len(data), 2048):
                await socket.send_bytes(data[offset:offset + 2048])
            await socket.send_json({"type": "image_commit", "request_id": transfer})
            app["store"].write("UPDATE devices SET image_state='sent' WHERE id=? AND image_request=?",
                                 (device_id, transfer))
            await notify(app, device["tenant_id"])
        except (ConnectionError, RuntimeError):
            # Persisted bytes remain queued for the next authenticated connection.
            app["store"].write("UPDATE devices SET image_state='queued' WHERE id=? AND image_request=?",
                                 (device_id, transfer))


def background(app, coroutine):
    task = asyncio.create_task(coroutine)
    app["tasks"].add(task)
    task.add_done_callback(app["tasks"].discard)
    return task


async def device_ws(request):
    device_id = request.headers.get("X-Device-ID", "")
    authorization = request.headers.get("Authorization", "")
    secret = authorization.removeprefix("Bearer ") if authorization.startswith("Bearer ") else ""
    app = request.app
    if not app["store"].device_auth(device_id, secret):
        raise web.HTTPUnauthorized(text="设备认证失败")
    device = app["store"].device(device_id)
    if not device["tenant_id"]:
        raise web.HTTPForbidden(text="设备尚未分配租户")
    socket = web.WebSocketResponse(heartbeat=20, max_msg_size=4096)
    await socket.prepare(request)
    old_socket = app["device_sockets"].get(device_id)
    app["device_sockets"][device_id] = socket
    if old_socket is not None:
        await old_socket.close(code=1000, message=b"reconnected")
    await socket.send_json({"type": "hello", "tenant_id": device["tenant_id"], "protocol": "CARD-WIFI/1"})
    await notify(app, device["tenant_id"])
    background(app, dispatch_image(app, device_id))
    try:
        async for message in socket:
            if message.type == WSMsgType.TEXT:
                try:
                    data = json.loads(message.data)
                    if data.get("type") != "image_ack":
                        continue
                    row = app["store"].device(device_id)
                    if data.get("request_id") != row["image_request"]:
                        continue
                    valid_crc = data.get("crc32") == row["image_crc"]
                    state = "applied" if data.get("ok") is True and valid_crc else "failed"
                    app["store"].write("UPDATE devices SET image_state=? WHERE id=?", (state, device_id))
                    await notify(app, row["tenant_id"])
                except (ValueError, AttributeError):
                    await socket.close(code=1007, message=b"invalid json")
    finally:
        if app["device_sockets"].get(device_id) is socket:
            app["device_sockets"].pop(device_id, None)
        await notify(app, device["tenant_id"])
    return socket


async def browser_ws(request):
    user = current_user(request)
    origin = urlsplit(request.headers.get("Origin", ""))
    if origin.netloc != request.host or origin.scheme not in ("http", "https"):
        raise web.HTTPForbidden(text="WebSocket 来源不匹配")
    socket = web.WebSocketResponse(heartbeat=20, max_msg_size=IMAGE_BYTES + 4096)
    await socket.prepare(request)
    app = request.app
    token = request.cookies.get("card_session")
    entry = (user, socket, token)
    app["browser_sockets"].append(entry)
    upload = None
    await socket.send_json({"type": "devices", "devices": public_devices(app, user["tenant_id"])})
    try:
        async for message in socket:
            if not app["store"].user(token):
                await socket.close(code=1008, message=b"session expired")
                break
            try:
                if message.type == WSMsgType.TEXT:
                    data = json.loads(message.data)
                    kind = data.get("type")
                    if kind == "image_begin":
                        device_id = data["device_id"]
                        if not app["store"].owned(device_id, user["tenant_id"]):
                            raise ValueError("设备不属于当前工作空间")
                        if data.get("size") != IMAGE_BYTES or type(data.get("crc32")) is not int or not 0 <= data["crc32"] <= 0xffffffff:
                            raise ValueError("图片必须是 240×280 RGB565")
                        upload = {"device_id": device_id, "request_id": secrets.token_hex(8),
                                  "crc32": data["crc32"], "data": bytearray(), "started": time.monotonic()}
                        await socket.send_json({"type": "image_ready", "request_id": upload["request_id"]})
                    elif kind == "image_commit":
                        if not upload or data.get("request_id") != upload["request_id"]:
                            raise ValueError("没有匹配的上传任务")
                        payload = bytes(upload["data"])
                        if len(payload) != IMAGE_BYTES or zlib.crc32(payload) != upload["crc32"]:
                            raise ValueError("图片长度或 CRC32 校验失败")
                        device_id, transfer = upload["device_id"], upload["request_id"]
                        app["store"].write("""UPDATE devices SET image=?,image_crc=?,image_request=?,
                            image_state='queued' WHERE id=? AND tenant_id=?""",
                            (payload, upload["crc32"], transfer, device_id, user["tenant_id"]))
                        upload = None
                        await socket.send_json({"type": "image_queued", "request_id": transfer})
                        await notify(app, user["tenant_id"])
                        background(app, dispatch_image(app, device_id))
                    else:
                        raise ValueError("不支持的消息类型")
                elif message.type == WSMsgType.BINARY:
                    if not upload or time.monotonic() - upload["started"] > 60:
                        raise ValueError("上传任务不存在或已超时")
                    if len(upload["data"]) + len(message.data) > IMAGE_BYTES:
                        raise ValueError("图片超过允许大小")
                    upload["data"].extend(message.data)
            except (ValueError, KeyError, TypeError, AttributeError) as exc:
                upload = None
                await socket.send_json({"type": "error", "message": str(exc) or "消息格式错误"})
    finally:
        app["browser_sockets"].remove(entry)
    return socket


async def startup(app):
    secret = secrets.token_hex(32)
    broker_plugins.STORE, broker_plugins.SERVICE_SECRET = app["store"], secret
    broker = Broker({"listeners": {"default": {"type": "tcp", "bind": app["mqtt_bind"],
                                                "max_connections": 100}},
                     "plugins": {"wireless.broker.DeviceAuthPlugin": {},
                                 "wireless.broker.DeviceAclPlugin": {}}})
    await broker.start()
    app["broker"] = broker
    mqtt = MQTTClient(client_id="card-cloud", config={"auto_reconnect": True})
    port = app["mqtt_bind"].rsplit(":", 1)[1]
    await mqtt.connect(f"mqtt://cloud:{secret}@127.0.0.1:{port}")
    await mqtt.subscribe([("cards/+/status", QOS_1)])
    app["mqtt"] = mqtt
    app["mqtt_task"] = asyncio.create_task(mqtt_reader(app))


async def cleanup(app):
    for _, socket, _ in list(app["browser_sockets"]):
        await socket.close(code=1001, message=b"server shutdown")
    for socket in list(app["device_sockets"].values()):
        await socket.close(code=1001, message=b"server shutdown")
    tasks = [app["mqtt_task"], *list(app["tasks"])]
    for task in tasks:
        task.cancel()
    await asyncio.gather(*tasks, return_exceptions=True)
    await app["mqtt"].disconnect()
    await app["broker"].shutdown()
    app["store"].db.close()


async def index(request):
    return web.FileResponse(STATIC / "index.html")


def make_app(data_dir=None, public_base=None, mqtt_bind=None, mqtt_public=None):
    base = (public_base or os.environ.get("CARD_PUBLIC_URL", "http://localhost:8787")).rstrip("/")
    parsed = urlsplit(base)
    if parsed.scheme not in ("http", "https") or not parsed.hostname or parsed.path:
        raise ValueError("CARD_PUBLIC_URL must be an http(s) origin without a path")
    app = web.Application(middlewares=[guard], client_max_size=8192)
    app["public_base"] = base
    app["mqtt_bind"] = mqtt_bind or os.environ.get("CARD_MQTT_BIND", "0.0.0.0:1883")
    app["mqtt_public"] = mqtt_public or os.environ.get("CARD_MQTT_URL", f"mqtt://{parsed.hostname}:1883")
    app["store"] = Store(Path(data_dir or os.environ.get("CARD_DATA_DIR", "wireless-data")) / "cards.sqlite3")
    app["limits"] = defaultdict(deque)
    app["device_sockets"], app["browser_sockets"] = {}, []
    app["device_locks"] = defaultdict(asyncio.Lock)
    app["tasks"] = set()
    app.router.add_get("/api/health", health)
    app.router.add_get("/api/auth/session", auth)
    app.router.add_post("/api/auth/{action:signup|login|logout}", auth)
    app.router.add_post("/api/device/register", register_device)
    app.router.add_post("/api/devices/claim", claim_device)
    app.router.add_get("/api/devices", list_devices)
    app.router.add_get("/api/devices/{device_id}/image", get_image)
    app.router.add_get("/ws/device", device_ws)
    app.router.add_get("/ws/browser", browser_ws)
    app.router.add_get("/", index)
    app.router.add_static("/static/", STATIC, show_index=False)
    app.on_startup.append(startup)
    app.on_cleanup.append(cleanup)
    return app


if __name__ == "__main__":
    logging.basicConfig(level=logging.WARNING)
    web.run_app(make_app(), host="0.0.0.0", port=int(os.environ.get("PORT", "8787")), access_log=None)
