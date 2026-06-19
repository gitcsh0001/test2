# test2 — ESP32 LED Blink (ESP-IDF)

一个最基础的 ESP32 入门程序:让一个 LED 周期性闪烁。基于 **ESP-IDF** 框架开发。

## 功能

- 在可配置的 GPIO 上控制一个 LED 周期性亮灭。
- GPIO 引脚号和闪烁周期均可通过 `menuconfig` 配置。
- 通过串口日志打印当前 LED 状态(ON/OFF)。

## 目录结构

```
.
├── CMakeLists.txt          # 顶层工程文件
├── main
│   ├── CMakeLists.txt      # main 组件构建脚本
│   ├── Kconfig.projbuild   # menuconfig 配置项(GPIO / 周期)
│   └── blink.c             # 主程序
└── .gitignore
```

## 环境要求

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32/get-started/) v4.4 或更新版本
- 一块 ESP32 / ESP32-S3 / ESP32-C3 等开发板

## 配置

```bash
idf.py menuconfig
```

在 **Blink Configuration** 菜单中可设置:

- **Blink GPIO number**:LED 连接的 GPIO,默认 `2`(很多 ESP32 DevKit 板载 LED 在 GPIO2;ESP32-C3/S3 常用 GPIO8)。
- **Blink period in milliseconds**:闪烁周期,默认 `1000` ms。

## 编译、烧录与监视

```bash
# 设置目标芯片(例如 esp32 / esp32s3 / esp32c3)
idf.py set-target esp32

# 编译
idf.py build

# 烧录并打开串口监视器(按实际串口替换 PORT)
idf.py -p PORT flash monitor
```

退出串口监视器:`Ctrl + ]`。

## 预期现象

板载 LED 以默认 1 秒为周期闪烁,串口同时输出:

```
I (xxx) blink: Turning the LED ON
I (xxx) blink: Turning the LED OFF
```
