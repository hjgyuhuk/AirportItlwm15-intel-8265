# Intel 8265 在 macOS 15 Sequoia 上的原生 Wi-Fi

把 `zhangcheng142/AirportItlwm-Sequoia`（fork，起点 `f31a3ad`）适配到 **Intel 8265 / 8275（PCI `8086:24FD`，IWM 8000 家族，固件 36）**，在 macOS 15 上提供**系统原生** Wi-Fi —— 状态栏和系统设置里的 Wi-Fi 列表，而不是 HeliPort 之类的旁路。

基于 [OpenIntelWireless/itlwm](https://github.com/OpenIntelWireless/itlwm)，保留 GPL-2.0 许可及各源文件版权。非官方发行版。

## 现状

在 Hackintosh `MacBookPro15,4`（Intel i7-8550U）上，**macOS 15.8.1 (24H32)** 实测通过：

| | |
|---|---|
| 驱动加载 | ✅ `ItlwmNativeStage = "17_complete"` |
| 硬件启动 / 固件对话 / 校准数据回放 | ✅ 全部通过 |
| Wi-Fi 硬件端口 | ✅ `en2`，MAC `xx:xx:xx:xx:xx:xx` |
| 扫描 | ✅ 46 个信道，200+ beacon 交付，Wi-Fi 列表完整 |
| 关联 / 认证 | ✅ `AssocStatus = 0`，`AuthStatus = 0` |
| 上网 | ✅ 拿到 IP，默认路由走 en2，ping / DNS 0% 丢包 |

**★ 尚未验收**：睡眠唤醒、AP 失联恢复、持续双向流量、真实 PTK/GTK 换钥、长期稳定性。

**★ 硬限制**：驱动严格核对内核版本串（`Darwin Kernel Version 24.6.0: ... xnu-11417.140.69.712.69~1`），**差一个字符就静默不加载**。系统安全更新后必须重新编译。只支持 x86_64。

## 目录

```
AirportItlwm/             驱动主体
itlwm/ itl80211/ include/ HAL 与网络栈
scripts/sequoia/          构建脚本、SDK 锁定与补丁、发布前检查
tests/                    25 个离线测试
docs/                     完整移植记录与分析
tools/                    安装与诊断脚本
README-SEQUOIA.md         驱动级说明（构建、安装、范围）
MacKernelSDK/             构建用的 SDK（不入库，见 .gitignore）
build/                    构建产物（不入库）
```

## 快速开始

**★ 详细步骤见 [`README-SEQUOIA.md`](README-SEQUOIA.md)。★★**

```sh
# 构建（需要固定的 MacKernelSDK，见 README-SEQUOIA.md）
python3 scripts/sequoia/build.py --production      # 发布版：无诊断
python3 scripts/sequoia/build.py                   # 开发版：带诊断 + 探针

# 装到 OpenCore 的 ESP（先备份 EFI，确认有可启动的 USB）
bash tools/install-probe-kext.sh main

# 重启后采集全部诊断属性
bash tools/collect-probe-diagnostics.sh
```

**★ 必需的启动参数**（三个都不能少）：

```
itlwm_native=1 itlwm_bsd_owner=skywalk itlwm_assoc_scan=1
```

**★ `itlwm_assoc_scan=1` 容易被忽略：不加它，已连接时系统的扫描请求会被驱动拒掉，Wi-Fi 列表会只剩服务信道上的网络（表现为"找不到某个热点"）。★★**

## 两种构建

同一棵源码树，一个编译期开关 `SEQUOIA_DIAGNOSTICS` 区分：

| | 开发版 | 发布版 |
|---|---|---|
| 命令 | `build.py` | `build.py --production` |
| provider 诊断属性 | 几百个 | **无** |
| NVRAM 面包屑 / 阶段标记 / 日志 | 有 | **无** |
| 探针 personality（NVM / 固件对话） | 有 | **无** |
| 二进制里的调试字符串 | 有 | **无**（只剩 bundle id） |

**★ 发布版在构建前会跑 `scripts/sequoia/check-diagnostics.py`：被开关编译掉的代码块只允许发布属性和打日志；发消息、改网络栈状态、动电台一律构建失败。★★**

**★ 这条检查是被一次真实故障逼出来的：driver-available 信封是给无线控制层的消息，不是诊断；被开关吞掉之后，Wi-Fi 列表正常但永远关联不上。见 [`docs/sequoia-8265-port.md`](docs/sequoia-8265-port.md) §七·九·三。★★**

## 文档

| | |
|---|---|
| [**sequoia-8265-port.md**](docs/sequoia-8265-port.md) | **完整移植记录** —— 逐轮实测数据、能力位、47 个 commit 的作用、12 条关键知识、两次发布版故障的复盘 |
| [issue-1018-analysis.md](docs/issue-1018-analysis.md) | 上游 issue #1009 / #1018 的分析与本项目的结论 |
| [sequoia-wifi-guide.md](docs/sequoia-wifi-guide.md) | 面向普通用户：能不能用、怎么装、有什么坑 |
| [code-quality-review.md](docs/code-quality-review.md) | fork 的代码质量评估 |
| [wpa2-limitation.md](docs/wpa2-limitation.md) | 为什么只有 WPA2 |

## 离线测试

```sh
bash tools/run-tests.sh
```

**★ 25 个测试全绿；另有 33 个依赖 fork 作者本地的头文件（那些文件从未进过 git），脚本会列出并跳过。★★**
**★ 覆盖范围与每个测试负责什么见 [`tests/README.md`](tests/README.md)。★★**

**★ 这些测试不替代实机验证 —— 硬件相关行为（固件对话、扫描、关联、换钥、睡眠唤醒）只能在真实硬件上确认。★★**

## 许可与来源

- 上游：[OpenIntelWireless/itlwm](https://github.com/OpenIntelWireless/itlwm)
- fork：[zhangcheng142/AirportItlwm-Sequoia](https://github.com/zhangcheng142/AirportItlwm-Sequoia) @ `f31a3ad`
- SDK：[acidanthera/MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) @ `05094e5e88cec7caedbfb35e8449ed0db94bf95b` + `scripts/sequoia/MacKernelSDK-pexpert.patch`

驱动源码保留 GPL-2.0 及各文件许可声明（`LICENSE`）；捆绑的 Intel 固件另遵循 `iwlwifi-firmware-license`。
