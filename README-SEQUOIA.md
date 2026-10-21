# Intel 8265 / 8275 在 macOS 15 Sequoia 上的原生 Wi-Fi

把 `zhangcheng142/AirportItlwm-Sequoia`（fork，起点 commit `f31a3ad`）适配到 **Intel 8265 / 8275（PCI `8086:24FD`，IWM 8000 家族，固件 36）**，在 macOS 15 上提供系统原生 Wi-Fi（状态栏 / 系统设置里的 Wi-Fi 列表），而不是靠 HeliPort 之类的旁路。

基于 OpenIntelWireless/itlwm，保留 GPL-2.0 许可及各源文件版权。不是官方发行版。

## 状态

在 Hackintosh `MacBookPro15,4`（Intel i7-8550U）上，**macOS 15.8.1 (24H32)** 实测：

| | |
|---|---|
| 驱动加载 | ✅ `ItlwmNativeStage = "17_complete"` |
| 硬件启动 / 固件对话 / 校准数据回放 | ✅ 全部通过 |
| Wi-Fi 硬件端口 | ✅ `en2`，MAC `xx:xx:xx:xx:xx:xx` |
| 扫描 | ✅ 46 个信道，200+ beacon 交付 |
| 关联 / 认证 | ✅ `AssocStatus = 0`，`AuthStatus = 0` |
| 上网 | ✅ 拿到 IP，默认路由走 en2 |

**★ 完整移植记录（含每一轮实测数据、能力位、47 个 commit 的作用、踩过的坑清单）见 [`docs/sequoia-8265-port.md`](docs/sequoia-8265-port.md)。★★**

**尚未验收**：睡眠唤醒、AP 失联恢复、持续双向流量、真实 PTK/GTK 换钥、长期稳定性。

**限制**：驱动严格核对内核版本串（`Darwin Kernel Version 24.6.0: ... xnu-11417.140.69.712.69~1`），差一个字符就静默不加载。**系统安全更新后必须重新编译。**只支持 x86_64。

## 两种构建

同一份源码，一个编译期开关 `SEQUOIA_DIAGNOSTICS` 区分：

```sh
# 开发版：带全部诊断（几百个 provider 属性、NVRAM 面包屑、阶段标记、日志）
#        和三个探针 personality（NVM / 固件对话 / probe）
/usr/bin/python3 scripts/sequoia/build.py

# 发布版：诊断全部关掉，探针 personality 不进 bundle
/usr/bin/python3 scripts/sequoia/build.py --production
```

**★ 发布版在 `--production` 之前会跑 `scripts/sequoia/check-diagnostics.py`：被开关编译掉的代码块只允许发布属性和打日志，发消息 / 改网络栈状态 / 动电台一律构建失败。★★**

输出 `AirportItlwmSequoiaNative.kext`（不是离线 KC，不安装也不加载任何驱动）。构建器逐文件核对 `sdk.lock.json`，不下载也不修改 SDK。

## 构建

需要 macOS、Xcode Command Line Tools、Git、Python 3.9+。

```sh
xcode-select --install          # 仅尚未安装 Command Line Tools 时
# SDK 已存在时先核对，勿覆盖已有修改
git clone https://github.com/acidanthera/MacKernelSDK.git MacKernelSDK
git -C MacKernelSDK checkout 05094e5e88cec7caedbfb35e8449ed0db94bf95b
git -C MacKernelSDK apply ../scripts/sequoia/MacKernelSDK-pexpert.patch
/usr/bin/python3 scripts/sequoia/build.py
```

源文件清单、SDK 绑定、编译输出、SHA256 和 UUID 写入 `build/Sequoia-0.1.0-beta.1-RELEASE/`。在 24H23 上自动跑 `kmutil libraries`；其他构建主机只编译。

**★ 离线测试（宿主可跑）：★**

```sh
bash tools/run-tests.sh
```

**★ 25 个通过；另有 33 个依赖 fork 作者本地的头文件（从未进过 git），脚本会列出并跳过。详见 [`tests/README.md`](tests/README.md)。★★**

## OpenCore 安装

**★ 先备份 EFI、确认有可启动的 USB OpenCore。★★**

1. 把 kext 复制到 `EFI/OC/Kexts/AirportItlwmSequoiaNative.kext`
2. 在 `Kernel/Add` 添加唯一启用条目：`BundlePath` 为该名称，`ExecutablePath` 为 `Contents/MacOS/AirportItlwmSequoiaNative`，`PlistPath` 为 `Contents/Info.plist`，`Arch` 为 `x86_64`
3. 停用其它 itlwm / AirportItlwm，以及旧兼容路径用的 `IO80211FamilyLegacy` / 替换的 `IOSkywalkFamily`
4. **★ 添加启动参数（三个都要）：★★**
   ```
   itlwm_native=1 itlwm_bsd_owner=skywalk itlwm_assoc_scan=1
   ```
   **★ `itlwm_assoc_scan=1` 不能省：不加它，已连接时系统的扫描请求会被驱动拒掉，Wi-Fi 列表会只剩服务信道上的网络。★★★**
5. 用匹配 OpenCore 版本的 `ocvalidate` 检查配置，然后手动重启

不要在系统运行中 load/unload。内置启动失败就从备用 USB 启动，恢复备份。

**★ 诊断与安装脚本在仓库的 `tools/`：★**

```sh
bash tools/install-probe-kext.sh [nvm|firmware|main]   # 装 kext 并写启动参数
bash tools/collect-probe-diagnostics.sh                 # 重启后采集全部诊断属性
```

**★ `BUILD_DIR=` 环境变量可切换安装哪个构建（开发版 / 发布版）。★★**

## 内核版本检查

**★ 驱动按固定偏移读 IO80211Family 的私有数据（接口 ivars 的 `+0x8` → `+0x120` → `+0xb08` 链），也调用它的私有方法 —— 别的 build 上符号照样能加载，但布局变了就可能让内核读到不是指针的东西。★★**

**★ 所以默认要求内核串完全相同：★★**

```
Darwin Kernel Version 24.6.0: Sun Aug 23 20:30:56 PDT 2026; root:xnu-11417.140.69.712.69~1/RELEASE_X86_64
```

**★ macOS 安全更新会换 build → 驱动静默不加载。可以逐级放宽：★★**

| 启动参数 | 放行范围 | 风险 |
|---|---|---|
| 不加（默认） | 完全相同的内核串 | 无 |
| `itlwm_kernel_check=minor` | 同一 Darwin 小版本（24.6）的任何 build | ★ 安全更新只换 build、不动私有布局 —— **更新后还能用的正解** ★ |
| `itlwm_kernel_check=major` | 任何 Darwin 24.x | 跨小版本，布局可能已变 |
| `itlwm_kernel_check=off` | 不查版本 | ★ 跨代，布局几乎肯定不同 |

**★ 生效的级别发布成 `ItlwmNativeKernelMatchLevel`（0/1/2/3）。无论哪一级，设备白名单与 `itlwm_native=1` 都照查。★★**

## 范围

- 支持：2.4 / 5 GHz、WPA2-Personal / PSK / CCMP；WPA/WPA2 与 WPA2/WPA3 混合 AP 提供 WPA2-PSK/CCMP 时选 WPA2
- 不支持：SAE / WPA3-only、必须 PMF、必须 TKIP、6 GHz、企业 802.1X、AWDL / AirDrop、热点
- 关联方式：`itlwm_bsd_owner=skywalk`，BSD 层交给 Skywalk

## 文档

| | |
|---|---|
| [`docs/sequoia-8265-port.md`](docs/sequoia-8265-port.md) | **完整移植记录**：逐轮实测、能力位、47 个 commit、12 条关键知识 |
| [`docs/issue-1018-analysis.md`](docs/issue-1018-analysis.md) | 上游 issue #1009/#1018 的分析与本项目的结论 |
| [`docs/sequoia-wifi-guide.md`](docs/sequoia-wifi-guide.md) | 面向普通用户：能不能用、怎么装、有什么坑 |
| [`docs/code-quality-review.md`](docs/code-quality-review.md) | fork 的代码质量评估 |
| [`docs/wpa2-limitation.md`](docs/wpa2-limitation.md) | 为什么只有 WPA2 |

**★ fork 作者对 0.1.0-beta.1（AX210）的原始发布说明与 README 已从本树删除；它们在 git 历史的 `f31a3ad` 里。★★**

## 许可与来源

驱动源码保留 GPL-2.0 及各文件许可声明（`LICENSE`）；捆绑的 Intel 固件另遵循 `iwlwifi-firmware-license`。

- 上游：`OpenIntelWireless/itlwm`
- fork：`zhangcheng142/AirportItlwm-Sequoia` @ `f31a3ad`
- SDK：`acidanthera/MacKernelSDK` @ `05094e5e88cec7caedbfb35e8449ed0db94bf95b` + `scripts/sequoia/MacKernelSDK-pexpert.patch`
