# nice!nano BLE → USB HID 接收器

让普通 ZMK 蓝牙键盘通过 USB 接收器连接电脑。电脑无需启用蓝牙；键盘保留原固件、Miryoku 键位和蓝牙多设备槽位，没有接收器时仍能直连其他蓝牙设备。

```text
wenwan 右手 → 左手主控 → 蓝牙 BLE → NRF52nano 接收器 → USB → 电脑
```

本项目使用 Zephyr 在 nRF52840 上实现 HID 透传，参考 ESP32 项目的设计思路；不是直接编译 ESP-IDF 代码，也不是原生 ZMK 分体 central 接收器。

## 当前可用版本

**v7 XTAL 是本次实机验证可打字的基线，请优先使用它。**

| 项目 | 已确认情况 |
|---|---|
| 接收器实物 | NRF52nano V5，nice!nano 兼容板 |
| 编译目标 | `nice_nano_v2_bridge`，nRF52840 |
| 时钟 | 外部低频晶振，50ppm 配置 |
| 键盘 | wenwan，ZMK v0.3，Miryoku |
| 测试电脑 | macOS |
| 实测结果 | 用户已通过接收器正常打字；日志 `BLE=1 USB=1 RX=329 TX=335 ERR=0`，观察窗口内未重启 |

固件：[`releases/v7/nicenano-ble-usb-diagnostic-v7-xtal.uf2`](releases/v7/nicenano-ble-usb-diagnostic-v7-xtal.uf2)。同目录包含最终构建配置和 SHA-256 校验值。调试 ELF 保留在本地，但不纳入 Git。

媒体键、鼠标层、睡眠唤醒、长时间运行、拔插重连及 Windows 兼容性尚未在本次实机验收。原版 nice!nano v2 和其他兼容板也不能仅凭编译成功视作已验证。

## 刷入和配对

1. 使用一块额外的 NRF52nano V5/nice!nano 兼容接收器板。**不要把接收器固件刷到键盘左右手。**
2. 双击接收器 RESET 进入 UF2 模式，把 v7 XTAL 的 `.uf2` 拷入磁盘。
3. 键盘选一个空闲或已清除的蓝牙槽，专门与接收器配对。此构建只接受完整广播名为 **`wenwan`** 的设备，首次配对窗口为上电后 120 秒。
4. 保持键盘唤醒，接收器自动扫描、加密连接并订阅 HID 报告。无需在电脑蓝牙设置里配对接收器。
5. 回家时切换键盘到原有电脑/平板的蓝牙槽；返回公司再切回接收器槽。键盘的直连蓝牙能力保持不变。

更新本项目固件通常不需要清配对。若键盘端清除了槽位，而接收器仍保留旧密钥，固件在明确收到“密钥缺失”错误后会删除对应接收器绑定并重新配对，不清其他身份或 USB 描述符缓存。

以前刷过其他 ZMK 固件的接收器可能带有旧绑定。原 ZMK `settings_reset` 可以清理，但清理后必须再刷回本项目接收器固件。不要为了电脑配对问题对键盘两半执行全量 settings reset。

可选清除按钮定义在 **D0/P0.08 → GND，保持 3 秒**。只有确认板子采用对应引脚布局时才能使用；它不是 RESET 引脚。

## 指示灯与诊断

固件只控制 P0.15 上的状态灯；兼容板另有的充电/电源灯不属于下面的定义。

| 状态灯 | 含义 |
|---|---|
| 快闪 | 首次扫描/初始化 |
| 慢闪 | 尝试恢复已有连接或初始化未完成 |
| 每两秒双闪 | BLE HID 已就绪，USB 尚未配置 |
| 常亮 | BLE HID 和 USB 均已就绪 |
| 常亮时短暂闪动 | 收到 BLE 输入报告 |

v7 保留 USB CDC 诊断串口，Mac 上通常显示为 `/dev/cu.usbmodem*`，波特率 115200，DTR 开启。日志包括扫描、连接、加密、GATT 阶段和计数，不输出实际按键内容。

其中 `RX` 是 BLE 输入报告数，`TX` 包含正常输入及断连时的释放报告，不能仅凭 TX 增长判断打字成功。`ERR` 为 USB 写入错误计数。USB 会在蓝牙连接前启动，便于排障。

## 本机重新编译

此仓库与现有 `zmk-local` 依赖环境放在同一级目录：

```text
Documents/workspace/keyboard/
├── nicenano-ble-usb/
└── zmk-local/
```

构建已验证基线：

```sh
python3 build.py --diagnostic --xtal
```

也可以显式指定依赖环境：

```sh
python3 build.py --diagnostic --xtal --workspace ../zmk-local
```

依赖为 Zephyr 3.5/ZMK fork（`west.yml` 固定提交）、Zephyr SDK 0.16.8，以及已有 workspace 内的 `.venv`、CMSIS、Nordic HAL、TinyCrypt、SEGGER 和 Picolibc。脚本显式限定模块，不混入其他键盘项目，也不修改它们的 west 配置。

输出位于 `firmware/`，日志位于 `build-diagnostic.log`；均默认不纳入 Git。脚本只编译，不自动刷机。日后新增经过验证的版本，可将对应 UF2、构建配置及校验值复制到新的 `releases/` 子目录。

`--diagnostic` 不加 `--xtal` 会生成内部 RC 时钟对照版；本次在 NRF52nano V5 上没有成功建立连接，不应替代当前基线。去掉诊断串口的普通构建尚未实机验证。

独立搭建 workspace 时，可通过本仓库 `west.yml` 获取固定版本依赖，安装对应 Zephyr Python requirements 与 SDK，再执行 `west build -b nice_nano_v2_bridge /absolute/path/to/nicenano-ble-usb`。要复现已验证的时钟和串口配置，仍应使用上述构建脚本与参数。

## 实现与限制

- 单个 BLE HID 设备、一个绑定记录；读取 Report Map，以同一描述符作为 USB HID，并根据 Report Reference 加回 USB Report ID。
- 支持普通键盘、NKRO、媒体键、鼠标输入的报告格式，以及 USB Output Report 回传；实际能力取决于远端描述符。本次 wenwan 的键盘/媒体/鼠标报告长度分别为 8/12/9 字节。
- 缓存描述符至 NVS。更换远端报告格式时保存新描述符并重启 USB 枚举；不接管键盘的左右手汇总任务，因此不能据此预期主手续航达到副手水平。
- 单个报告最多 64 字节（含 Report ID），描述符最多 1536 字节，最多 16 个 Report 特征。
- 不支持 Feature Report、多个 HID 服务合并、多设备并发、WebUSB、网页 OTA 或屏幕。
- 未实现 BIOS/UEFI Boot Keyboard 转换，不承诺 BIOS 使用。
- USB VID/PID 使用 Zephyr 开发标识；正式对外分发需要合适的 USB 标识。

## 测试

原生描述符与报告打包测试使用实际 wenwan 固件提取的 HID 描述符，覆盖报告 ID、长度、Push/Pop、越界和 20,000 组畸形输入：

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -I src tests/test_hid_map.c src/hid_map.c -o /tmp/test_nicenano_hid_map
/tmp/test_nicenano_hid_map tests/fixtures/wenwan-report-map-0.bin tests/fixtures/wenwan-report-map-1.bin
```

这些测试不能替代无线和 USB 实机验证。下一步验收可依次测试 Miryoku 鼠标/媒体键、按住键时断开是否释放、键盘休眠唤醒、接收器拔插，以及直连蓝牙槽来回切换。

## 来源与开发记录

- 设计参考：[ESP HID passthrough v1](https://github.com/mctechnology17/esp-usb-ble-hid/releases/tag/hid-passthrough-v1)。本实现不包含 ESP-IDF/ESPP 源码。
- 平台：[Zephyr fork](https://github.com/zmkfirmware/zephyr/tree/dacab4875df72109b96cc8977547a0dc04875bcd)。
- 引脚与 UF2 分区参考：[ZMK nice!nano board](https://github.com/zmkfirmware/zmk/tree/v0.3/app/boards/arm/nice_nano)。
- [调试过程记录](docs/bringup-history.md)保留早期诊断及尚未证实的假设，使用时以本 README 和 v7 基线为准。

项目代码采用 [MIT License](LICENSE)，Zephyr 与各依赖保留各自许可证。
