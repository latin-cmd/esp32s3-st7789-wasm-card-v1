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

### 固件状态机回归（无需开发板）

安装 Python 3 和支持 C11 的主机 C 编译器后，在仓库根目录执行：

```sh
python -m unittest wireless.tests.test_firmware_network -v
```

默认调用 `cc`，可用 `CC` 环境变量指定编译器。测试从 `main/card_network.c`
直接提取状态声明及网络任务、断线回调、热点关闭、内容恢复和 WebSocket 启动函数，
编译并执行原始 C 函数体；时钟、GPIO、RTOS 和设备 I/O 使用测试替身。
这不是完整 ESP-IDF 固件编译，也不会验证射频、Flash、LCD 或真实任务并发。

回归覆盖：断网超过 60 秒后自动热点在 Wi-Fi 恢复时关闭（无需 MQTT 分配，且不依赖
WebSocket 对象是否存在）、短暂断网、重复掉线、手动配网、长按 BOOT 将自动热点
转为手动热点、认领入口保留、WebSocket 启动失败重试、已保存图片/内置名片/插件恢复，
以及断线时只清理未完成的无线图片传输。现有服务端集成测试还覆盖设备 WebSocket
断开期间图片入队、重连后按原始字节重传并确认成功。

自动热点记录其开启原因：仅断网超时新开的热点在 Wi-Fi 恢复时关闭；已有的配置或
认领入口继续保留，认领成功后才关闭。BOOT 手动开启的热点继续保留到配置重启。
断网恢复路径不依赖 MQTT、认领结果或分配队列，可在后续 HTTPS 状态查询架构中复用。

### 实体 ESP32-S3 验证清单（需另行执行）

1. 用 ESP-IDF 5.3.2 完整编译并烧录固件，设备正常联网并建立 WebSocket。
2. 分别在无已保存图片、已保存 USB/无线图片和已加载 WASM 插件的情况下，关闭路由器
   超过 60 秒，确认自动热点和配网页出现；恢复路由器后，确认热点关闭并恢复对应内容。
3. 在掉线期间通过网站上传图片，确认联网和 WebSocket 恢复后图片完整显示并收到成功回执。
4. 联网时长按 BOOT 5 秒，以及自动热点打开后长按 BOOT 5 秒；恢复 Wi-Fi 并收到分配后，
   确认手动热点仍保留。另测按住 BOOT 启动的恢复模式。
5. 重复断网/恢复，并测试首次配网和未认领状态，确认认领入口保留。

## 上游参考

参考的是小智 **v2.5.2** 的联网交互，使用 ESP-IDF 5.3.2 C API 实现本项目所需子集，
没有直接引入要求 IDF 6 的整套小智固件：

- [小智 WifiBoard](https://github.com/78/xiaozhi-esp32/blob/v2.5.2/main/boards/common/wifi_board.cc)
- [78/esp-wifi-connect](https://github.com/78/esp-wifi-connect)
- [ESP-MQTT 5.3.2](https://docs.espressif.com/projects/esp-idf/en/v5.3.2/esp32s3/api-reference/protocols/mqtt.html)
