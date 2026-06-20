# test2 — ESP32 Modbus 从站 / Web 配置 + RTU/TCP 热切换 (ESP-IDF)

基于 **ESP-IDF 6.0** 和 **esp-modbus v2.1.2** 的 Modbus **从站(slave)**。
单个固件同时支持 **RTU(串口)** 和 **TCP(网络)**,并内置一个**网页配置界面**:
在浏览器里即可切换传输方式、设置 RTU/TCP 全部参数,点一下**实时热切换**生效。
配置保存在 **NVS**,掉电不丢。

## 功能

- **网页配置 UI**(`http://<设备IP>/`):
  - 选择模式:**RTU / TCP**
  - 设置从站地址(1–247)
  - **RTU 参数**:UART 端口、波特率、校验(None/Odd/Even)、数据位(8/7)、停止位(1/2)
  - **TCP 参数**:监听端口(默认 502)
  - 点击「Apply & hot-switch」→ 后台拆掉当前从站、用新参数重建并重启
- **热切换**:`mbc_slave_delete()` → `mbc_slave_create_serial/tcp()` → `mbc_slave_start()`,无需重启/重新烧录
- **持久化**:配置写入 NVS,重启后自动加载
- 暴露 **保持寄存器(Holding Registers)** 1 类区:10 × uint16,读/写,起始偏移 0

## HTTP 接口

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/` | 配置网页(单页) |
| GET | `/api/status` | 返回当前配置 JSON |
| POST | `/api/config` | 提交新配置(url-encoded),触发热切换 |

`/api/status` 返回示例:

```json
{"mode":"rtu","addr":1,"uart":1,"baud":115200,"parity":0,"databits":8,"stopbits":1,"tcpport":502}
```

## 架构说明

```
        浏览器 ──HTTP──▶ esp_http_server ──▶ modbus_apply_config()
                                                  │ 加锁
                                                  ▼
                                   stop(delete) → 改 cfg → start(create+start)
                                                  │
                                                  ▼  保存 NVS
```

- **Wi-Fi 常驻**:Web 服务器需要联网,所以开机即连 Wi-Fi;Modbus 传输(RTU/TCP)独立于此。
- 配置与启停由互斥锁保护,Web 请求与从站状态切换串行执行。
- 寄存器内存由 esp-modbus 协议栈后台读写;热切换不影响寄存器数据。

## 目录结构

```
.
├── CMakeLists.txt          # 顶层工程(含 protocol_examples_common 路径)
├── main
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild   # 出厂默认值(可被网页覆盖)
│   ├── idf_component.yml   # 依赖 espressif/esp-modbus ==2.1.2
│   ├── modbus_slave.h/.c   # 从站核心 + 运行时配置 + NVS
│   └── web_server.h/.c     # HTTP 服务器 + 网页
└── .gitignore
```

## 环境要求

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32/get-started/) **v6.0**
- [esp-modbus](https://components.espressif.com/components/espressif/esp-modbus) **v2.1.2**(首次构建自动下载)
- 可联网(Wi-Fi/以太网)的 ESP32 开发板;RTU 模式还需 RS485 收发器

## 配置与编译

```bash
idf.py menuconfig   # 设置出厂默认值 + Wi-Fi 凭据(Example Connection Configuration)
idf.py set-target esp32
idf.py build
idf.py -p PORT flash monitor
```

- **Modbus Slave Configuration**:出厂默认地址/模式/RTU/TCP 参数(之后均可在网页改)
- **Example Connection Configuration**:Wi-Fi SSID/密码
- **Component config → Modbus configuration**:RTU 的 TXD/RXD/RTS 引脚

## 使用流程

1. 烧录后看串口日志,等待 Wi-Fi 获取 IP:

   ```
   I (xxx) example_connect: Got IPv4 ... 192.168.x.y
   I (xxx) web: Web UI started on port 80
   ```

2. 浏览器打开 `http://192.168.x.y/`
3. 选择模式、填参数,点「Apply & hot-switch」
4. 串口出现:

   ```
   W (xxx) mb_slave: Applying config -> TCP (hot switch)
   I (xxx) mb_slave: Slave running: TCP addr=1
   ```

## 说明

- esp-modbus v2.x 的结构体字段(`ser_opts`/`tcp_opts`)按 v2.1.2 官方示例编写;
  若本地工具链编译报字段不匹配,把报错贴来即可快速修正。
- 若要清空已保存的网页配置、回到出厂默认:`idf.py erase-flash` 后重新烧录。
