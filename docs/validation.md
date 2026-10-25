# 三连接版本验证记录

日期：2026-10-25。固件v6已在当前COM4开发板完成烧录和哈希校验。提供WiFi、USB串口、加密BLE三种电脑连接方式，七个原有功能及抛硬币动画保留，菜单新增“设置”。

## 当前版本结果

| 项目 | 结果 |
| --- | --- |
| 开发环境 | VS Code使用的PlatformIO Core 6.2.0；Espressif32 7.0.1；Arduino ESP32 2.0.17 |
| 固件依赖 | TFT_eSPI 2.5.43、ArduinoJson 6.21.5、NimBLE-Arduino 2.3.6 |
| 固件构建与烧录 | SUCCESS；ESP32-C3 rev0.3；Flash使用1039504/1310720字节；静态RAM45724/327680字节 |
| 引脚 | 沿用用户本地配置：上13、下8、中4、左9、右5；屏幕SCK2、SDA3、RES10、DC6、CS7、BL11 |
| 设置页 | 上下循环选择WiFi/USB/BLE，中键确认，左键返回；SETTINGS_TEST PASS |
| 连接保存 | 选择USB后硬件复位，板端恢复USB；CONNECTION_PERSISTENCE_TEST PASS |
| USB实物双向通信 | COM4收到认证后的真实CPU、RAM、GPU和音量，板端pc=1；8种指令及成功回执全部通过 |
| BLE实物双向通信 | Windows实际发现并连接ESP32C3-Macropad，配对加密，认证后板端pc=1；真实状态及8种指令回执全部通过 |
| WiFi回归 | 新版接收程序通过原局域网接口向板端返回真实CPU、RAM、GPU和音量，板端pc=1 |
| 三连接共用执行 | 复制、粘贴、音量增加/减少/静音、播放暂停、下一首、上一首，复用同一电脑执行和去重接口 |
| 真实Ctrl+C | 新版接收程序向自己的测试窗口发送，HTTP200，窗口收到Ctrl+C，PASS；未读写用户剪贴板 |
| 时钟 | USB/BLE携带电脑毫秒时间用于校时；WiFi保留NTP及电脑校时；抓取的时钟界面与本机北京时间一致 |
| 番茄钟 | TIMER_TEST PASS；阶段转换、后台计时、暂停、重置、毫秒回绕通过 |
| 抛硬币 | COIN_TEST PASS；硬件随机采样、允许连续同面、中键单次触发、1.2秒动画、返回、后台完成及毫秒回绕通过 |
| 媒体映射 | MEDIA_TEST PASS；上键上一首、下键下一首、中键播放暂停、左键返回 |
| 中文显示 | 10×10字模共143个字符，硬币结果整数2倍放大；已查看设置页和九页总览 |
| 电脑端自动测试 | 48/48通过；包含分帧、认证、过期丢弃、跨连接事件去重、全部动作及原有功能 |
| Python依赖 | 工程.venv：pyserial 3.5、bleak 3.0.2；启动脚本自动选择该环境 |

## 实物测试方法与边界

运行接收程序的WiFi/本机接口，再独占当前串口运行：

~~~powershell
.venv\Scripts\python.exe pc/receiver.py --links wifi
.venv\Scripts\python.exe tools/check_device_links.py --port COM4 --mode usb
.venv\Scripts\python.exe tools/check_device_links.py --port COM4 --mode ble
~~~

USB/BLE检查使用实际开发板和生产连接代码，读取新版接收程序提供的真实硬件、时间、音量状态。测试端拦截8种桌面动作并返回成功回执，避免主动改变用户剪贴板、音量和播放器。它验证板端指令、物理链路、协议、认证及回执；真实Windows键盘输出另由独立测试窗口验证。特定目标应用仍取决于焦点、权限及其对系统媒体键的支持。

测试完成会恢复原先连接方式。串口诊断通过与机械按键相同的业务函数调用，不替代用户对5个机械方向接线的人工观察。

截图从实际TFT_eSprite缓冲区读取RGB565，保存到docs/previews。它能检查文字和布局，面板自身亮度、观察角度与色彩仍以实物为准。

硬件传感器不可读时返回null，板端显示“--”；当前本机CPU温度接口不可用，未声称验证CPU温度。NVIDIA GPU通过已安装的nvidia-smi读取；其他传感器可接入LibreHardwareMonitor本机Web Server。

有限随机采样不能证明随机性。抛硬币每次调用esp_random并读取最低位，正反各对应一半的32位输入，允许连续同面且不强制平衡计数。三连接版本保持射频初始化以维持硬件熵源，USB/BLE不依赖路由器。

## 已解决的连接问题

- 本次用户报告USB和蓝牙的电脑功能全部不可用：板子实际选中USB但pc=0，电脑接收程序没有运行。已安装依赖、更新启动脚本并在普通Windows环境启动新版接收程序。
- 蓝牙广播名称：原先在启用扫描响应前设置名称，名称与128位服务UUID超出单个广播包容量；已调整设置顺序，实物能识别ESP32C3-Macropad。
- Windows认证失败响应偶发WinError10053：拒绝请求前读取最多512字节的普通请求体，避免未读数据导致连接重置掩盖401。
- 串口被占用：接收程序USB线程、串口监视器、烧录和截图不能同时独占CH343。诊断时使用--links wifi或退出接收程序，正常使用默认启用三种连接。

切换采用连接代次隔离，旧队列指令、旧状态和旧回执不跨连接执行。丢失回执不会自动重发一次动作。共享令牌的HMAC握手不把原始令牌写入USB或BLE消息，BLE连接同时加密。

include/secrets.h、pc/receiver_config.json、.venv和.pio由.gitignore排除，文档不记录WiFi密码或API令牌。
