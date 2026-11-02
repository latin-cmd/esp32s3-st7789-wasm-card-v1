# Lineless / 无线电子名片

本分支在原有 USB 图片与 WASM 插件管理之上，增加设备热点配网、Wi-Fi 联网、
租户管理、MQTT 分配和 WebSocket 图片传输。固件仍使用 ESP-IDF **5.3.2**。

## 先启动本机 Docker 服务

在项目 PowerShell 中执行：

```powershell
.\tools\wireless.ps1 up
```

若电脑有 VPN、虚拟网卡或多个默认网关，明确选择与 ESP32 同网的 WLAN 地址：

```powershell
.\tools\wireless.ps1 up -Address 192.168.1.10
```

浏览器访问 `http://localhost:8787`。脚本还会显示设备应该填写的管理服务地址，
例如 `http://192.168.1.10:8787`。该地址不能是 `localhost` 或 Docker 内部地址。
电脑与 ESP32 要在可互通的网络上；公共 Wi-Fi 的客户端隔离可能阻止设备连接电脑。

服务提供 HTTP/WebSocket 端口 8787、MQTT 端口 1883，账户、设备归属和最新图片保存在
Docker 命名卷 `wireless-data` 中。`down` 只停止服务，保留数据：

```powershell
.\tools\wireless.ps1 status
.\tools\wireless.ps1 logs
.\tools\wireless.ps1 down
```

这是可信局域网中的开发配置，HTTP、WS、MQTT 使用明文传输。部署公网前必须配置
HTTPS/WSS、MQTT TLS、注册策略和网络访问边界；不要直接将这两个开发端口开放到公网。

## 编译与设备配网

```powershell
.\tools\idf.ps1 build
```

烧录生成的无线固件后：

1. 没有保存网络配置时，设备启动 `Card-XXXXXX` WPA2 热点。
2. 屏幕显示热点名称、独立随机热点密码和 `http://192.168.4.1`。
3. 手机连接该热点，手动打开该地址。选择/输入 2.4 GHz Wi-Fi，填写密码和管理服务地址。
4. 保存后设备重启并连接目标热点。联网失败约 60 秒后重新开放配网入口。
5. 连接管理服务后，设备再次开放配网热点并显示设备号和 12 位认领码。
6. 在无线网站创建工作空间，输入设备号和认领码。
7. 服务端通过 MQTT 向该设备发布租户分配，设备主动建立 WebSocket 图片连接。
8. 正常认领完成后关闭自动配网热点，恢复名片。长按 BOOT 5 秒可重新开启配网；
   按住 BOOT 启动仍保留原有跳过已保存内容的恢复行为，并进入手动配网。

配网 HTTP 接口只接受发往设备 AP 地址的连接；不会通过路由器侧接口暴露设置。
设备凭据、Wi-Fi 密码保存在设备 NVS，配网页不会返回设备凭据或已保存 Wi-Fi 密码。
本版配网支持手动访问 IP，不包含 BluFi、SmartConfig 或 DNS 劫持式自动弹窗。

## 租户与传输

- 每个注册账号创建一个工作空间（租户）。本版不包含成员邀请、角色管理或计费。
- 固件用 MAC 生成稳定设备号，同时首次启动生成独立 256 位随机凭据。
- 设备用凭据登记服务；服务保存凭据哈希，返回 15 分钟有效的一次性认领码。
- 设备号本身不能用于认证；已登记设备再次登记必须提供原凭据。
- 认领采用数据库条件更新，两个租户同时认领只会成功一个。
- MQTT 客户端 ID 与用户名必须等于认证设备号。设备只能发布自己的
  `cards/{device_id}/status`，订阅/接收自己的 `cards/{device_id}/assignment`。
- 租户 ID 来自服务端归属记录，不能由设备上报或网页自行指定。
- 浏览器使用 HttpOnly 会话 Cookie，WebSocket 校验 Origin 和租户归属。
- 图片在浏览器裁剪为 240×280 RGB565（高字节在前，共 134,400 字节）。
- 浏览器到服务、服务到设备都使用 WebSocket 二进制分块。JSON 包含总长度、
  请求号和 CRC32；图片不会塞进 MQTT，也不做 Base64 扩容。
- 图片离线入队，服务/设备重连后重新发送。只保留每台设备最新一张图片。
- 设备先写临时文件、校验长度和 CRC32，再原子替换 LittleFS 文件并显示；只有设备
  返回与当前请求和 CRC 相符的成功回执，网页才显示“已保存并显示”。
- USB 和无线激活内容共用互斥锁，两个传输使用不同临时文件。
- Wi-Fi 模式仅新增图片传输；WASM 上传、执行和清除仍可通过原 USB 工具使用。

## 无硬件联调

模拟器是独立的软件测试设备，不代表实体 ESP32 已烧录或联网：

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r wireless/requirements.txt
.\.venv\Scripts\python.exe -m wireless.simulator --server http://localhost:8787
```

它会打印以 `sim-` 开头的设备号与认领码，网站明确标注“模拟设备”。
模拟器会走真实 MQTT 和 WebSocket 链路校验图片并回执，但不驱动屏幕或写设备 Flash。

```powershell
.\.venv\Scripts\python.exe -m unittest wireless.tests.test_integration -v
```

测试覆盖租户认领、MQTT 订阅/发布 ACL、跨租户图片拒绝、错误凭据、Origin 检查、
完整图片字节和回执校验、坏 CRC 拒绝，以及离线图片在服务重启后的恢复。

## 上游参考

参考的是小智 **v2.5.2** 的联网交互，使用 ESP-IDF 5.3.2 C API 实现本项目所需子集，
没有直接引入要求 IDF 6 的整套小智固件：

- [小智 WifiBoard](https://github.com/78/xiaozhi-esp32/blob/v2.5.2/main/boards/common/wifi_board.cc)
- [78/esp-wifi-connect](https://github.com/78/esp-wifi-connect)
- [ESP-MQTT 5.3.2](https://docs.espressif.com/projects/esp-idf/en/v5.3.2/esp32s3/api-reference/protocols/mqtt.html)
