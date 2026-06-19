# test2 — ESP32 WS2812 Blink (ESP-IDF)

一个 ESP32 入门程序:驱动 **WS2812(NeoPixel)可寻址 RGB LED** 周期性闪烁,每次点亮时切换不同颜色。基于 **ESP-IDF** 框架,使用官方 `led_strip` 组件 + RMT 后端。

## 功能

- 在可配置的 GPIO 上驱动一个或多个 WS2812 LED。
- LED 周期性亮灭,每次"亮"时按调色板循环切换颜色(红/绿/蓝/黄/青/品红)。
- GPIO 引脚号、LED 数量、闪烁周期均可通过 `menuconfig` 配置。
- 通过串口日志打印当前 LED 状态与 RGB 值。

## 目录结构

```
.
├── CMakeLists.txt          # 顶层工程文件
├── main
│   ├── CMakeLists.txt      # main 组件构建脚本
│   ├── Kconfig.projbuild   # menuconfig 配置项(GPIO / LED 数量 / 周期)
│   ├── idf_component.yml   # 托管组件依赖(espressif/led_strip)
│   └── blink.c             # 主程序
└── .gitignore
```

## 环境要求

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32/get-started/) **v5.0 或更新版本**(`led_strip` 组件依赖)
- 一块带 WS2812 的开发板(如 ESP32-C3/S3 DevKit,板载 RGB LED 在 GPIO8),或外接 WS2812 的 ESP32 板

> `led_strip` 组件会在首次 `idf.py build` 时由组件管理器自动从
> [ESP Component Registry](https://components.espressif.com/components/espressif/led_strip)
> 下载,无需手动安装。

## 配置

```bash
idf.py menuconfig
```

在 **Blink Configuration** 菜单中可设置:

- **WS2812 data GPIO number**:WS2812 数据线连接的 GPIO,默认 `8`(ESP32-C3/S3 板载 RGB LED 常用 GPIO8)。
- **Number of WS2812 LEDs**:LED 数量,默认 `1`。
- **Blink period in milliseconds**:闪烁周期,默认 `1000` ms。

## 编译、烧录与监视

```bash
# 设置目标芯片(例如 esp32c3 / esp32s3 / esp32)
idf.py set-target esp32c3

# 编译(首次会自动拉取 led_strip 组件)
idf.py build

# 烧录并打开串口监视器(按实际串口替换 PORT)
idf.py -p PORT flash monitor
```

退出串口监视器:`Ctrl + ]`。

## 预期现象

板载 WS2812 以默认 1 秒为周期闪烁,每次点亮切换一种颜色,串口同时输出:

```
I (xxx) blink: LED ON  (R=32 G=0 B=0)
I (xxx) blink: LED OFF
I (xxx) blink: LED ON  (R=0 G=32 B=0)
I (xxx) blink: LED OFF
```
