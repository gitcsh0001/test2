# test2 — ESP32 Modbus RTU 从站 (ESP-IDF)

一个基于 **ESP-IDF 6.0** 和 **esp-modbus v2.1.2** 的 Modbus RTU **从站(slave)** 示例。
通过串口对外提供 4 类标准 Modbus 寄存器区,可被任意 Modbus 主站读写。

## 功能

对外暴露以下参数区:

| 区域 | 类型 | 权限 | 数量 | 说明 |
|------|------|------|------|------|
| 保持寄存器 Holding | `MB_PARAM_HOLDING` | 读/写 | 10 × uint16 | 起始偏移 0 |
| 输入寄存器 Input | `MB_PARAM_INPUT` | 只读 | 10 × uint16 | 起始偏移 0,初值 `0x1000+i` |
| 线圈 Coils | `MB_PARAM_COIL` | 读/写 | 16 bit | 2 字节 |
| 离散输入 Discrete | `MB_PARAM_DISCRETE` | 只读 | 16 bit | 2 字节,初值 `0xA5` |

主站每次读写时,从站会在串口打印被访问的区域、偏移和长度。

## 目录结构

```
.
├── CMakeLists.txt          # 顶层工程文件
├── main
│   ├── CMakeLists.txt      # main 组件构建脚本
│   ├── Kconfig.projbuild   # menuconfig 配置项(从站地址 / UART / 波特率)
│   ├── idf_component.yml   # 托管组件依赖(espressif/esp-modbus ==2.1.2)
│   └── modbus_slave.c      # 主程序
└── .gitignore
```

## 环境要求

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32/get-started/) **v6.0**
- [esp-modbus](https://components.espressif.com/components/espressif/esp-modbus) **v2.1.2**(首次 `idf.py build` 时由组件管理器自动下载)
- 一块 ESP32 系列开发板 + RS485 收发器(如 MAX485)接到 Modbus 总线

## 配置

```bash
idf.py menuconfig
```

- **Modbus Slave Configuration**(本工程提供):
  - **Modbus slave address (UID)**:从站地址,默认 `1`
  - **UART port number**:UART 端口号,默认 `1`
  - **UART baud rate**:波特率,默认 `115200`(需与主站一致)
- **Component config → Modbus configuration**(esp-modbus 组件提供):
  - 配置 **TXD / RXD / RTS** 引脚等串口物理参数

## 编译、烧录与监视

```bash
# 设置目标芯片
idf.py set-target esp32

# 编译(首次会自动拉取 esp-modbus 组件)
idf.py build

# 烧录并打开串口监视器(按实际串口替换 PORT)
idf.py -p PORT flash monitor
```

退出串口监视器:`Ctrl + ]`。

## 预期现象

启动后串口打印:

```
I (xxx) mb_slave: Modbus RTU slave started: addr=1, uart=1, baud=115200
```

主站读写寄存器时:

```
I (xxx) mb_slave: HOLDING WR: offset=0, size=2, addr=0x...
I (xxx) mb_slave: INPUT RD: offset=0, size=20, addr=0x...
```

## 说明

- 本例为 **RTU(串口)** 从站。若需 **Modbus TCP** 从站,可将创建接口换成
  `mbc_slave_create_tcp()` 并改用 `mb_communication_info_t.tcp_opts`。
- RS485 需要硬件收发器;RTS 引脚用于自动收发方向切换,可在组件配置中设置。
