# test2 — ESP32 Modbus 从站 / RTU + TCP 热切换 (ESP-IDF)

一个基于 **ESP-IDF 6.0** 和 **esp-modbus v2.1.2** 的 Modbus **从站(slave)** 示例。
单个固件同时编译 **RTU(串口)** 和 **TCP(网络)** 两种传输,支持**运行时热切换**:
按一下按钮即可拆掉当前从站、用另一种传输重建并重启,无需重新烧录或重启。

## 功能

两种模式都对外暴露以下参数区:

| 区域 | 类型 | 权限 | 数量 | 说明 |
|------|------|------|------|------|
| 保持寄存器 Holding | `MB_PARAM_HOLDING` | 读/写 | 10 × uint16 | 起始偏移 0 |
| 输入寄存器 Input | `MB_PARAM_INPUT` | 只读 | 10 × uint16 | 初值 `0x1000+i` |
| 线圈 Coils | `MB_PARAM_COIL` | 读/写 | 16 bit | 2 字节 |
| 离散输入 Discrete | `MB_PARAM_DISCRETE` | 只读 | 16 bit | 初值 `0xA5` |

**热切换**:按下切换按钮(默认板载 BOOT 键 GPIO0),从站在 RTU ⇄ TCP 之间切换。
切换时调用 `mbc_slave_delete()` 拆除当前控制器,再用另一种传输 `mbc_slave_create_*()`
重新创建并 `mbc_slave_start()`。网络在首次进入 TCP 模式时惰性初始化,之后常驻。

## 工作原理

```
                       按钮(GPIO0)按下
   ┌──────────┐  delete + create_tcp + start   ┌──────────┐
   │ RTU 从站  │ ─────────────────────────────▶ │ TCP 从站  │
   │ (串口)    │ ◀───────────────────────────── │ (网络)    │
   └──────────┘  delete + create_serial + start └──────────┘
```

- 寄存器内存由 esp-modbus 协议栈在后台自动读写,因此主循环只需做按钮去抖与切换。
- 切换不影响寄存器数据(`s_holding_regs` 等为全局,跨模式保留)。

## 目录结构

```
.
├── CMakeLists.txt          # 顶层工程(含 protocol_examples_common 路径)
├── main
│   ├── CMakeLists.txt      # main 组件构建脚本
│   ├── Kconfig.projbuild   # menuconfig 配置项
│   ├── idf_component.yml   # 托管组件依赖(espressif/esp-modbus ==2.1.2)
│   └── modbus_slave.c      # 主程序
└── .gitignore
```

## 环境要求

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32/get-started/) **v6.0**
- [esp-modbus](https://components.espressif.com/components/espressif/esp-modbus) **v2.1.2**(首次 `idf.py build` 自动下载)
- RTU 模式:RS485 收发器(如 MAX485);TCP 模式:可联网(Wi-Fi/以太网)的开发板

## 配置

```bash
idf.py menuconfig
```

- **Modbus Slave Configuration**(本工程提供):
  - **Modbus slave address (UID)**:从站地址,默认 `1`
  - **Initial communication mode**:开机初始模式(RTU / TCP)
  - **Mode switch button GPIO**:切换按钮引脚,默认 `0`(BOOT 键)
  - RTU:UART 端口号、波特率
  - TCP:监听端口(默认 `502`)
- **Component config → Modbus configuration**(esp-modbus 提供):RTU 的 TXD/RXD/RTS 引脚
- **Example Connection Configuration**(protocol_examples_common 提供):TCP 的 Wi-Fi SSID/密码

## 编译、烧录与监视

```bash
idf.py set-target esp32
idf.py build
idf.py -p PORT flash monitor
```

退出串口监视器:`Ctrl + ]`。

## 预期现象

启动并按下切换按钮时,串口输出类似:

```
I (xxx) mb_slave: Mode switch button on GPIO0 (press to toggle RTU/TCP)
I (xxx) mb_slave: Modbus slave running in RTU mode (addr=1)
... 按下按钮 ...
W (xxx) mb_slave: Hot-switching RTU -> TCP ...
I (xxx) mb_slave: Modbus slave running in TCP mode (addr=1)
```

## 说明

- 切换按钮采用主循环轮询 + 简单去抖(50 ms);如需更可靠可改用 GPIO 中断。
- 若只用 RTU,可不配置 Wi-Fi——网络仅在首次切到 TCP 时才初始化。
- esp-modbus v2.x 的结构体字段(`ser_opts` / `tcp_opts`)按 v2.1.x API 编写;
  若本地工具链编译报字段不匹配,把报错贴来即可快速修正。
