# 硬件接线与资料依据

用户提供的 `Air101-Expansion-LCD.pdf`（2021-08-30，V1.0）确认了五向开关公共端接地、按下拉低，以及 LCD SPI 信号和三极管背光驱动。`CORE-ESP32-C3设计原理图_V1.2.pdf` 确认为 ESP32-C3 + CH343P 串口下载结构，BOOT 是 GPIO9，GPIO8 有上拉，GPIO12 / 13 连接板载 LED。

**扩展板的 J1～J32 是连接点位号，核心板连接器 J2 的 1～32 是另一套编号。不能把两个数字直接一一对应，也不能把它们当作 GPIO 编号。** 当前工程按用户提供的实际接线定义 GPIO；以下官方历史工程用于参考硬件结构和面板参数：

- [官方五向按键与屏幕硬件配置 device.lua](https://github.com/openLuat/LuatOS/blob/da8f98378ad52b25b60f3e7e84d34a84147e85b2/script/turnkey/lcd-0.96-lvgl/device.lua)，读取其 `esp32c3` 部分。
- [官方 ESP32-C3 LCD 示例 main.lua](https://github.com/openLuat/LuatOS/blob/da8f98378ad52b25b60f3e7e84d34a84147e85b2/demo/lcd/esp32c3/main.lua)，提供 ST7735S 160×80、新旧面板偏移与反色说明。
- [合宙扩展板资料入口](https://wiki.luatos.org/peripherals/lcd_air10x/hardware.html)。

上述官方代码版本为 `v0007.esp32c3.v1001`，提交 `da8f98378ad52b25b60f3e7e84d34a84147e85b2`。它是接线核对来源，并非本工程采用的固件框架。

## 五向开关

| 信号 | 扩展板原理图连接点 | ESP32-C3 GPIO | 本工程 |
| --- | --- | --- | --- |
| UPKEY | J4 | 13 | 当前本地方向映射：上键；菜单上翻、复制页 Ctrl+C |
| CENTER | J5 | 4 | 菜单进入；复制页 Ctrl+V |
| DWKEY | J6 | 8 | 当前本地方向映射：下键，启动配置脚 |
| LKEY | J3 | 9 | 返回菜单，与 BOOT 共用 |
| RKEY | J26 | 5 | 当前本地方向映射：右键；番茄钟重置 |
| COM | K1 公共端 | GND | 公共地 |

UI 升级前，用户本地 `board_config.h` 已调整为上13、下8、中4、左9、右5；本次按该当前配置实现交互。扩展板信号位号供参考，实际方向请以实物和 `board_config.h` 为准。

全部设置 `INPUT_PULLUP`，低电平表示按下。不要额外把公共端接到 3.3V。代码不把 GPIO12 / 13 当状态灯输出，以免与当前上键冲突。

GPIO8、GPIO9 参与芯片启动配置，开机 / 复位时释放所有方向键；需要进入下载模式时使用板载 BOOT 和 RESET。

## TFT 屏幕

| 扩展板信号 | 扩展板连接点 | ESP32-C3 GPIO |
| --- | --- | --- |
| LCD_SCK | J14 | 2 |
| LCD_SDA（SPI MOSI） | J13 | 3 |
| LCD_CS | J10 | 7 |
| LCD_DC | J11 | 6 |
| LCD_RES | J12 | 10 |
| LCD_BL | J9 | 11 |
| +3.3VA | 电源网络 | 3.3V |
| GND | 地网络 | GND |

`LCD_SDA` 是 SPI 数据输出，不是 I²C SDA。屏幕不需要 MISO，驱动中设为 `-1`。背光由 GPIO11 高电平点亮，默认 SPI 27MHz。

按用户指定的 [imshixin/esp32c3_lvgl_observer](https://github.com/imshixin/esp32c3_lvgl_observer) 再次核对：该仓库 `pio.zip` 中的 `TFT_eSPI/User_Setup.h` 定义 MOSI=3、SCLK=2、CS=7、DC=6、RST=10、MISO=-1，与上表一致。本工程同时对齐其 BGR 颜色顺序、27MHz 写入频率和 20MHz 读取频率，默认面板也使用相同的 `ST7735_REDTAB160x80`。引脚和屏幕参数在 `include/display_setup.h` 中。

该参考项目没有五向方向键 GPIO 定义，也没有显式定义背光 GPIO；按键和 GPIO11 背光使用当前本地配置。参考项目根目录的 `main.cpp` 把 GPIO12 / 13 用作 LED 输出；本工程 GPIO13 目前是上键输入，因此不复制这两路 LED 的输出初始化。

## ESP32-C3 屏幕初始化崩溃修复

在 COM4 实际抓取的旧固件日志中，`display.init()` 调用 TFT_eSPI 的 `SET_BUS_WRITE_MODE` 时发生 `Store access fault`，访问地址为 `0x10`。本机 SDK 的 `REG_SPI_BASE` 接收物理 SPI 编号 2，而 TFT_eSPI 2.5.43 传入 `SPI2_HOST` 枚举值 1，导致寄存器地址计算错误；对应的[上游问题记录](https://github.com/espressif/arduino-esp32/issues/10908)描述了同一情况。

`include/esp32c3_tft_compat.h` 针对这种 SDK 定义加入兼容映射，并用编译期断言检查 SPI2 基地址。它由 `include/tft_setup.h` 载入，不修改 PlatformIO 下载的框架或库文件。仅更换屏幕 GPIO 无法修复这次启动崩溃。

扩展板按标准方向叠插时应已完成连线；若使用杜邦线，根据上表连接 GPIO 和网络标签。不要依据两份图的 J 编号直接短接。实际板卡若为其他修订或改线版本，先断电，用通断档确认开关输出与对应 GPIO 的连通关系，再修改 `include/board_config.h` 和 `include/display_setup.h`。

## 面板差异

官方示例明确列出两种 0.96 英寸面板参数，因此工程提供一处切换：

| `LCD_PANEL_LEGACY` | TFT_eSPI 配置 | 横屏偏移 | 默认反色 |
| --- | --- | --- | --- |
| `0`（默认） | `ST7735_REDTAB160x80` | x=0, y=24 | 关 |
| `1` | `ST7735_GREENTAB160x80` | x=1, y=26 | 开 |

在 `include/display_setup.h` 修改 `LCD_PANEL_LEGACY` 后重新编译烧录。不依赖屏幕背面标签颜色识别版本；这里的 REDTAB / GREENTAB 是库的配置名称。若颜色单独相反，调整 `include/board_config.h` 的 `invertColors`。若整幅画面倒置，把 `rotation` 从 `1` 改为 `3`，两者均是 160×80 横屏。

## 下载接口

默认工程适配用户 PDF 的 CH343 转 UART 版本，使用 UART0 串口日志；合宙官方资料指出该版本应安装 CH343 正确驱动，见[开发板说明](https://wiki.luatos.org/chips/esp32c3/board.html)。

若实物实际上为另一种 USB 直连精简版，需在 `platformio.ini` 的 `build_flags` 加入 `-D ARDUINO_USB_CDC_ON_BOOT=1`，并按该版本确认下载驱动和端口；GPIO18 / 19 会用于 USB。这一变体未在本次硬件上验证。

ESP32-C3 内置 USB 外设是固定功能 Serial/JTAG，不能用普通 Arduino USB Keyboard 库直接变成通用 USB HID 键盘，见[乐鑫说明](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/get-started/establish-serial-connection.html)。本工程使用 WiFi + Windows 接收端实现快捷键。
