# ESP32-S3 ST7789 WASM Card V1

ESP32-S3 电子名片插件系统第一版。

## 已实现

- ESP32-S3 + ST7789，240 x 280，RGB565，LVGL 8.4
- 16 MB Flash 分区和 8 MB PSRAM
- 内置静态电子名片，作为任何失败情况下的回退界面
- 通过原生 USB（Web Serial，CARD/1 协议 + CRC32）上传图片和插件
- 图片和插件保存在 LittleFS，重启后自动恢复
- WAMR 解释器运行 WebAssembly 渲染插件：完整加载器校验，限制指令数、内存
  和宿主调用次数
- 安全模式：启动时崩溃过的插件会被自动隔离，按住 BOOT 键启动可跳过已保存内容
- 触摸功能关闭

使用方法、恢复方式和插件接口见 `README-V1.md` 与 `plugins/README.md`。

## 构建（Docker / ESP-IDF 5.3.2）

在 Windows 上使用 VS Code 编辑源码，使用官方 `espressif/idf:v5.3.2`
镜像编译。先启动 Docker Desktop，确认 Linux 容器引擎正在运行。

在项目目录的 PowerShell 中运行：

```powershell
# 首次运行会自动下载镜像；之后复用镜像和编译缓存
.\tools\idf.ps1 build

# 查看固件空间占用
.\tools\idf.ps1 size

# 打开固件配置界面（需要交互终端）
.\tools\idf.ps1 menuconfig

# 进入已激活 ESP-IDF 环境的容器终端
.\tools\idf.ps1 shell
```

也可以直接运行：

```powershell
docker compose run --rm -T idf idf.py -B build-docker build
```

- `compose.yaml`：固定 ESP-IDF 5.3.2 和编译缓存配置。
- `tools/idf.ps1`：构建、配置和容器终端入口。
- `build-docker/`：当前源码的 Linux 编译结果，保存在本地且不提交 Git。
- 依赖组件与 ccache 使用 Docker 命名卷，和本地旧依赖目录隔离。
- 修改源码后重新执行 `build`，会进行增量编译。
- 容器中的项目目录是 `/project`。在容器终端中也使用
  `idf.py -B build-docker build`，避免复用旧 Windows 构建目录。
- 烧录和串口调试在 Windows 上进行；编译命令不会自动烧录开发板。
- 电脑端图片/插件管理页面仍可通过 `host/serve.ps1` 启动。

此环境保留项目当前的 IDF 版本；升级到 6.1 应在独立分支中迁移并验证。

## 目录

- `main/main.c`：LCD、LVGL、内置名片界面、启动恢复与安全模式
- `main/card_serial.c`：USB CARD/1 协议
- `main/card_storage.c`：LittleFS 存储
- `main/card_display.c`：图片和界面切换
- `main/wasm_plugin.c`：WAMR 插件运行时与宿主接口
- `main/font_card_name_24.c`：24 px 英文姓名字体
- `main/font_card_cjk_16.c`：16 px 中文子集字体
- `host/`：电脑端 Web Serial 管理页面
- `plugins/`：插件示例和安全测试插件

上传图片使用 240 x 280 RGB565，像素按高字节在前存储，以匹配本工程的
`CONFIG_LV_COLOR_16_SWAP=y` 配置。

`partitions.csv` 中的 `ota_0`、`ota_1` 预留给后续 OTA 升级；`coredump`
分区用于保存崩溃现场。
