# nice!nano v2 蓝牙转 USB HID 接收器（实验版）

目标：公司电脑通过 USB 使用无线 ZMK 键盘；没带接收器时，键盘仍然可以切换蓝牙槽直连家里电脑或平板。

```text
右手 ──ZMK 分体蓝牙──> 左手主控 ──标准 BLE HID──> nice!nano v2 ──USB──> 公司电脑
```

这是对 ESP32 项目 HID passthrough 思路的 Zephyr/nRF52840 重实现，不是把 ESP-IDF 源码直接编译到 nice!nano，也不是原生 ZMK split dongle。键盘端无需修改固件、键位或左右手角色。接收器本身 USB 供电，不需要电池。

## 当前交付与验证状态

- 目标：原版 nice!nano v2，nRF52840，标准 UF2 bootloader；其他兼容板未验证。
- 固件：`firmware/nicenano-v2-ble-usb-wenwan.uf2`。
- 编译：Zephyr 3.5（ZMK 使用的 fork，提交固定在 `west.yml`），SDK 0.16.8；构建成功。
- 原生测试：带 ASan/UBSan 的描述符/报告打包测试，以及 20,000 组畸形输入检查通过。
- 从本次 wenwan ZMK 固件提取实际 176 字节 HID 描述符，确认键盘 ID 1（8 字节）、媒体键 ID 2（12 字节）、鼠标 ID 3（9 字节）能正确解析和加回 USB Report ID。
- **尚未连接真实接收器、键盘和电脑测试。编译成功不等于配对、重连和 USB 透传已实测通过。**

## 第一轮试用

1. 使用一块额外的 nice!nano v2 作为接收器，不要把这个固件刷到键盘左右手。
2. 双击接收器 RESET 进入 UF2 模式，把 `.uf2` 拷入其磁盘。没有自动刷机步骤。
   如果这块板子以前运行过 ZMK，应用 UF2 不会擦除旧蓝牙绑定；刷入后先按下文的 D0→GND 方法清除一次接收器绑定。
3. 在键盘上选一个空闲蓝牙槽，留给接收器。已绑定其他设备的槽应只清除该槽的绑定；不用刷 `settings_reset`。
4. 接收器上电后，首次配对窗口为 120 秒。当前固件只接受完整广播名为 **`wenwan`** 的设备，这是你当前键盘的名称。未配对时不需要在公司电脑的蓝牙界面操作。
5. 配对并读取 HID 描述符后，接收器才会首次枚举成 USB HID；此前电脑可能看不到 USB 键盘，这是预期行为。描述符和绑定会保存，之后上电可立即枚举并尝试重连。
6. v2 诊断指示灯：快闪为首次寻找设备；慢闪为寻找已绑定设备或初始化；每两秒双闪为 BLE HID 已就绪但 USB 未配置；常亮为 BLE 和 USB 均已就绪，收到 BLE 报告时短暂熄灭。配对窗口到期后需重新插拔接收器再试。
7. 回家后切换键盘到原来的电脑蓝牙槽即可。返回公司时切回接收器槽。

如果键盘名称不是 `wenwan`，在 `prj.conf` 添加 `CONFIG_BRIDGE_PEER_NAME="你的名称"` 后重新编译。名称必须与完整蓝牙广播名一致。空字符串允许配对发现的第一个 BLE HID，第一轮试用不建议这样配置。

## 清除接收器配对

接收器通电时，把 **D0（P0.08）与 GND** 通过按钮或导线连接，保持 3 秒后释放。它会清除接收器的绑定及描述符缓存并重启。这两个引脚仅作为可选配对按钮使用；不要将它们接到其他外设。

这不是短接 RESET。键盘上给接收器使用的那个蓝牙槽也可能需要 `BT_CLR` 后重新配对。其他电脑使用的槽不用清除。

## 实现范围

- 一个 BLE HID 设备、一个配对记录，开机扫描自动重连。
- 读取远端 Report Map，缓存到 NVS，以同一份报告描述符作为 USB HID；按 Report Reference 加回 USB Report ID。
- 转发普通键盘、NKRO、媒体键、鼠标输入（以设备提供的描述符为准）。
- 支持 USB Output Report 向 BLE Output Report 回传，例如远端确实提供的键盘灯报告。当前 wenwan 描述符没有 LED Output Report，接收器不会凭空增加它。
- BLE 断连、队列溢出等情况下，发送中立报告释放按键；异常流会断开重连。
- 单个 USB 输入/输出报告最多 64 字节（含 Report ID）、描述符最多 1536 字节、最多 16 个 Report 特征。
- 不支持 Feature Report 转发、多个 HID 服务合并、多个设备同时连接、WebUSB、网页 OTA、屏幕。
- **未实现独立 Boot Keyboard 接口和 NKRO→Boot 转换，不承诺 BIOS/UEFI 可用。**上游 ESP32 版的这项能力不能直接套用到本固件。
- 普通办公系统的 USB 兼容性、Caps Lock 灯（其他有此报告的键盘）、睡眠唤醒、固件更换后的描述符更新，都需要实机测试。
- USB VID/PID 使用 Zephyr 开发标识，不冒充原键盘品牌；正式分发应申请适当标识。

## 本机重新编译

使用已安装的依赖环境，不修改其他键盘项目或它们的 west 配置：

```sh
python3 build.py --workspace /Users/gofran/Documents/workspace/keyboard/zmk-local
```

脚本显式只加载 CMSIS、Nordic HAL、TinyCrypt、SEGGER、Picolibc，不引入 Tako 等其他键盘项目模块。生成 `firmware/` 内的 UF2、调试 ELF 和最终 `.config`；完整日志在 `build.log`。脚本只编译，不执行刷机。

独立安装依赖时，可通过本目录 `west.yml` 初始化 Zephyr workspace，安装 Zephyr Python requirements 与 Zephyr SDK 0.16.8，再在该 workspace 内执行：

```sh
west build -b nice_nano_v2_bridge /absolute/path/to/nicenano-ble-usb
```

使用现有 ZMK workspace 时应优先用 `build.py`，避免其他项目模块自动参与构建。

原生测试：

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -I src tests/test_hid_map.c src/hid_map.c -o /tmp/test_nicenano_hid_map
/tmp/test_nicenano_hid_map tests/fixtures/wenwan-report-map-0.bin tests/fixtures/wenwan-report-map-1.bin
```

## 实机验收

按顺序检查：首次配对 → 打字/组合键 → Miryoku 媒体键与鼠标方向/点击/滚动 → 按住键时关闭键盘能否释放 → 键盘休眠后重连 → 接收器拔插重连 → 切换到家里蓝牙槽再切回。

如果不能配对或没有输入，可通过 SWD+RTT 读取 `bridge` 日志；默认 USB 只有 HID，不额外暴露串口。提供失败步骤、电脑系统及日志后，可继续修正实机问题。

## 来源

- 设计参考：[mctechnology17/esp-usb-ble-hid，hid-passthrough-v1](https://github.com/mctechnology17/esp-usb-ble-hid/releases/tag/hid-passthrough-v1)。本版本不包含其 ESP-IDF/ESPP 实现。
- 平台：[Zephyr](https://github.com/zmkfirmware/zephyr/tree/dacab4875df72109b96cc8977547a0dc04875bcd)（Apache-2.0 等各模块原许可）。
- nice!nano v2 引脚和 UF2 分区布局参考：[ZMK nice_nano board](https://github.com/zmkfirmware/zmk/tree/v0.3/app/boards/arm/nice_nano)（MIT）。
- 测试描述符来自本次用户 wenwan 构建，两个 fixture 对应 ELF 内两份相同的 USB/BLE 描述符，不包含按键输入记录或蓝牙密钥。

接收器不会接管左右手汇总任务，因此不应据此预期主手续航达到副手水平。

## v3：兼容板与 USB 串口诊断

实机反馈修正为键盘实际上未连接；接收器是具体型号尚待确认的兼容板。不能再依据首版蓝灯常亮推断 BLE 已配对成功。

`python3 build.py --diagnostic` 生成 `firmware/nicenano-ble-usb-diagnostic-v3.uf2`：

- 使用内部 RC 低频时钟并启用校准，不依赖外部 32.768kHz 晶振；仍要求 nice!nano 兼容的 nRF52840、USB、UF2 分区和状态灯引脚。
- NVS 描述符缓存载入后立即启动 USB，未配对时使用 wenwan 的已验证描述符。蓝牙初始化或扫描失败也不应阻止 USB 枚举。
- USB 增加 CDC 串口用于日志，产品名为 `nice!nano HID Bridge v3 diagnostics`。只用于排障，正式使用版可不带串口。
- 日志每 5 秒打印 BLE/HID 就绪、USB 配置状态、BLE 报告数、USB 发送数、广告包数和目标匹配次数；还会打印连接、安全及 HID 初始化错误。
- Flash 不清除配对，无需再刷 reset。键盘应选择专用空闲/已清除的蓝牙槽并保持唤醒，广播名仍需为 `wenwan`。
- 新设备的报告描述符变化导致重启前，留出 1.5 秒等待 Bluetooth 的延迟密钥存储。

该版本尚未在实物上验证。当前目标是先确认 Mac 出现 USB HID 和 `/dev/cu.usbmodem*`，再通过日志判断为何没有连上键盘；不能宣称已修复蓝牙连接问题。

## v2 诊断版

首版实机反馈为 BLE 显示已连接但 Mac 无输入；本机 IOUSB 列表没有看到接收器，根因仍待确认。v2 增加 BLE 接收/USB 发送计数及状态日志，修正 USB configuration=0 的状态判断，并按实际 USB 供电声明设备。它不是已证实修复无输入问题的稳定版。

先确认双击 RESET 后是否有 UF2 磁盘，以排查线缆/端口。再刷 v2，观察双闪、常亮和按键闪动；Mac USB 列表及 RTT 计数可以继续区分枚举失败、无 BLE 报告与 USB 发送失败。已有绑定无需清除。

## v4：初始 BLE 连接参数对照

v3 实机确认 USB 枚举及 CDC 可用，USB=1、ERR=0。扫描匹配 wenwan 并报告连接结果 0，但紧接着 Security level 1/result 9、disconnect reason 62（0x3E），RX=0。发送计数仅含断连释放报告，不是打字成功证据。

v4 仅将初始连接改为 Zephyr BT_LE_CONN_PARAM_DEFAULT（latency=0），并在请求安全握手前等待 300ms。保留内部 RC 时钟与 USB 诊断。该变更是针对连接建立失败的待实测对照，不是已确认根因或已验证修复。`python3 build.py --diagnostic` 现在输出 diagnostic-v4。无需清除已有接收器绑定。

### v4 缺失密钥恢复

清除键盘槽位后的实机日志出现 BT_SECURITY_ERR_PIN_OR_KEY_MISSING（2）和 Security request -128；v4 遇到这个明确错误时，在断连后清除对应接收器端的旧绑定并重新扫描配对，保留 USB 描述符。不在普通连接失败或其他认证错误时自动删绑定。当前实机仍为 v3；v4 的恢复流程待刷入测试。

### NRF52nano V5 时钟对照

用户确认型号为 NRF52nano V5，过去可稳定运行 ZMK 蓝牙。v4 RC 实测仍在请求安全握手之前约 250ms 断开，reason=62，RX=0，因此缺失密钥并非唯一问题。新增 `--diagnostic --xtal`，仅把低频时钟切回外部晶振/50ppm（与已检查的 nice_nano_v2 ZMK 构建一致），保留其他 v4 行为，输出 v4-xtal.uf2。未获得该板原理图，不能据型号断言晶振配置；此固件是待实机验证的单变量对照。若 USB 仍正常但不再扫描，可能是该板不具备或无法启动外部低频晶振，应刷回 RC 版。

### v5 XTAL：GATT 阶段诊断

v4 XTAL 实机成功达到 Security level 2/result 0，表明比 RC 版本有明确进展；随后观察到 USB 串口重复断开并重新枚举，未出现 HID ready。当前还不能确定是否为 GATT 超时重启、描述符变化重启或其他故障。v5 保留 XTAL 配置，为 MTU、服务/特征/描述符发现、读取及 CCC 订阅增加阶段日志，并在 GATT 超时重启前等待 300ms 输出日志。尚待实机验证。构建命令仍为 `python3 build.py --diagnostic --xtal`，产物名更新为 v5-xtal。

### v6 XTAL：异常重启记录

v5 实机每轮均达到加密成功、ATT MTU=65、HID 服务 001d-002e，最后日志为特征发现；没有看到 HID ready。串口重新枚举现象重复出现。v6 增加硬件复位原因、保留 RAM 中的 fatal PC/LR/CFSR 记录、堆栈保护、断言和每个特征的日志。尚不能确定是异常、供电复位或超时，未宣称修复。UF2 编译通过，等待实机捕获。

### v7 XTAL：发现流程末尾完成条件

v6 实机打印六个 HID 特征声明 001d/001f/0021/0025/0029/002d 后重新枚举，未捕获到启动阶段的异常记录。v7 在特征 value_handle 达到服务 end_handle 时主动完成发现，在描述符 handle 达到范围末尾时同样完成，避免已到范围末尾后再探测一次。逻辑上完整保留范围内所有返回项；此改动能否消除重启仍待实机验证。

## 已验证可用基线：v7 XTAL

用户在 NRF52nano V5 接收器上刷入 v7 后，已通过 wenwan 无线输入本次消息。读取状态日志确认 BLE=1、USB=1、RX=329、TX=335、ERR=0，观察窗口内 uptime 45→55 秒连续，无重新枚举。说明普通打字的 BLE→USB 链路已实机跑通；媒体键、鼠标层、休眠唤醒、拔插重连和 Windows 尚未在本次确认。保留此 v7 XTAL 作为工作基线，暂不去除 CDC 或改变已验证配置。
