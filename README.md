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
- **自定义寄存器回调**:覆盖 esp-modbus 的 weak 回调,对主站读写做校验并返回异常码

## 自定义回调 / 异常码

本项目用**两种互补**的机制接管保持寄存器,以便校验数据并向主站返回精确的异常码。

### 机制一:weak 回调覆盖(内存搬运 + 越界)

esp-modbus 把默认寄存器回调 `mbc_reg_holding_slave_cb()` 声明为 **weak 符号**,
本项目定义同名**强符号**函数在链接时覆盖它,负责实际的读写内存搬运与越界检查。
但寄存器回调只能返回 `mb_err_enum_t`,经 `mb_error_to_exception()` 映射后**异常码有限**:

| 回调返回 | 异常码 |
|----------|--------|
| `MB_ENOERR` | 0x00 正常 |
| `MB_ENOREG` | **0x02** 非法数据地址 |
| `MB_ETIMEDOUT` | **0x06** 从站忙 |
| 其它 | **0x04** 从站设备故障 |

> 寄存器回调**拿不到 0x03**(非法数据值)——这正是需要机制二的原因。

### 机制二:功能码 handler 覆盖(可返回任意异常码,含 0x03)

esp-modbus 提供**公开** API `mbc_set_handler()` / `mbc_get_handler()`,可替换某个
功能码的处理函数。本项目为写功能码 **0x06**(写单个)和 **0x10**(写多个)各装了一个
包装 handler:它们**直接返回 `mb_exception_t`**,因此能返回**任意**异常码,包括 **0x03**。
流程为:先用 `mbc_get_handler` 保存默认 handler → 用 `mbc_set_handler` 装上包装 handler
→ 包装 handler 校验请求,不合法直接返回异常码,**合法则委托回默认 handler** 完成写入。

校验逻辑集中在 `validate_holding_write()`,内置示例策略(可调整):

| 情况 | 返回异常码 |
|------|-----------|
| 地址越界(≥ 10) | **0x02** 非法数据地址 |
| 写**只读**寄存器(默认寄存器 0) | **0x02** 非法数据地址 |
| 写入值 **> 1000** | **0x03** 非法数据值 ✅ |
| 合法写入 | 0x00,委托默认 handler 写入 |

> 这样所有标准异常码(0x02 / 0x03 / 0x04 / 0x06)都能按需返回。handler 在每次
> 启动/热切换后由 `install_custom_handlers()` 重新注册。

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
