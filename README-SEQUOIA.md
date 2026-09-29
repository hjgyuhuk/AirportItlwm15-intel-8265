# AirportItlwm-Sequoia 0.1.0-beta.1

基于 OpenIntelWireless/itlwm 的独立 Sequoia 原生 Wi-Fi 分支，保留原项目 GPL-2.0 许可及各源文件版权。
不是 OpenIntelWireless 官方发行版。当前硬件验收范围是 Intel AX210，x86_64，macOS build **24H23**。
本分支通过 OpenAI Codex 辅助进行分析、适配、修复及测试，由维护者配合实机验收；不代表官方背书或全面验证。
驱动严格检查内核版本；其他 macOS 小版本、Apple Silicon 和其他 Intel 无线设备不属于本版支持承诺。
发布为 Beta，表示已有可用功能，仍有未完成的可靠性验收。
内置 OpenCore 启动、状态栏/系统设置列表、两个测试网络之间切换及基本上网已由开发主机实测确认。
详情和验证边界见 [RELEASE_NOTES.md](RELEASE_NOTES.md)；不将一次基本使用验收等同于长期稳定性保证。

## 当前范围

- 原生状态栏和系统设置 Wi-Fi 列表、2.4/5 GHz、WPA2-Personal / PSK / CCMP。
- WPA/WPA2、WPA2/WPA3 混合 AP 提供 WPA2-PSK/CCMP 时选择 WPA2。
- 基础速率/流量诊断，切网、错误收尾及换钥重试实现。
- 不支持 SAE/WPA3-only、必须 PMF、必须 TKIP、6GHz、企业 802.1X、AWDL/AirDrop 或热点。
- 睡眠唤醒、AP失联恢复、持续双向流量及真实AP换钥尚未完成阶段验收。
- CFBundleVersion 使用 Apple 格式 `0.1.0b1`；公开版本为 `0.1.0-beta.1`。

## 独立构建

需要 macOS、Xcode Command Line Tools、Git、Python 3.9+。没有 Xcode GUI 工程迁移要求。
编译器沿用已验收开发链的 `-O1`，Release 使用 `-g0` 并剥离调试符号；保留符号表供内核链接及数值诊断。
Beta 保留运行诊断，不能把 Release 配置等同于所有功能已验证。

```sh
xcode-select --install  # 仅尚未安装 Command Line Tools 时
# 在本仓库根目录执行；SDK 已存在时先核对，勿覆盖已有修改。
git clone https://github.com/acidanthera/MacKernelSDK.git MacKernelSDK
git -C MacKernelSDK checkout 05094e5e88cec7caedbfb35e8449ed0db94bf95b
git -C MacKernelSDK apply ../scripts/sequoia/MacKernelSDK-pexpert.patch
/usr/bin/python3 scripts/sequoia/build.py
```

构建器逐文件核对 `sdk.lock.json`，不自动下载或修改 SDK。仅对现有 SDK 的 pexpert 字符串启动参数声明补丁有依赖。
源文件清单、SDK绑定、编译输出、SHA256 和 UUID 写入 `build/Sequoia-0.1.0-beta.1-RELEASE/`。
源码归档中不包含 SDK；按上述固定提交和补丁可重建，不需要原开发主机的个人配置、系统镜像或历史 artifacts。
输出 `AirportItlwmSequoiaNative.kext`，不是离线 KC，也不会安装或加载任何驱动。
在24H23上自动运行 `kmutil libraries`；其他构建主机只编译，不视为目标内核链接验证。

## OpenCore 安装

建议先从可恢复的 USB OpenCore 测试，确认可用后再迁移内置 EFI。
请使用自己的 EFI 配置；本项目不分发开发主机 EFI、序列号或整套 OpenCore。

1. 备份原始 EFI 和 config.plist，保留能启动的 USB。
2. 把 kext 复制到 `EFI/OC/Kexts/AirportItlwmSequoiaNative.kext`。
3. 在 `Kernel/Add` 添加唯一启用条目：BundlePath 为该名称，ExecutablePath 为
   `Contents/MacOS/AirportItlwmSequoiaNative`，PlistPath 为 `Contents/Info.plist`，Arch 为 `x86_64`。
4. 停用其他 itlwm/AirportItlwm，以及用于旧兼容路径的 IO80211FamilyLegacy/替换 IOSkywalkFamily。
   本分支面向原装 Sequoia 网络框架，不要求安装 OCLP 网络降级补丁。
5. 添加 `itlwm_native=1 itlwm_bsd_owner=skywalk`，保留你自己原有的其他启动参数。
   不要沿用开发用 `itlwm_*` 实验参数，尤其故意停在启动阶段的诊断开关。
6. 使用匹配 OpenCore 版本的 ocvalidate 检查配置，然后手动重启。

本版本使用私有 ABI，严格限于上述系统 build；安装前确认版本。不要尝试运行中 load/unload。
如果内置启动失败，从备用 USB 启动，恢复备份的 config.plist 和驱动目录。

## 源码与发行资产

发行包仅包括 kext、说明、许可与 SHA256；源码快照包含驱动、测试、构建脚本和固定 SDK 补丁。
驱动源码保留 GPL-2.0 及各文件许可声明；捆绑 Intel 固件另遵循 `iwlwifi-firmware-license`，发行包同时附带。
不会附带系统 Mach-O/KC、Apple 系统镜像、机器身份、网络凭据、日志或历史 EFI。
发布说明将区分实际机器测试和离线测试；未执行的测试不会标为通过。

## English summary

Independent GPL-2.0 fork of OpenIntelWireless/itlwm. This beta is limited to x86_64 Sequoia build 24H23
and tested on AX210. WPA2-Personal/CCMP and compatible transition networks are in scope.
WPA3-only/SAE, mandatory PMF/TKIP, 6 GHz, enterprise authentication and AWDL are out of scope.
Sleep, AP recovery, sustained traffic and real rekey acceptance remain pending.
Build with the pinned SDK and `scripts/sequoia/build.py`. Install through OpenCore with the two required boot arguments;
retain a bootable USB and your own EFI backup. No machine-specific EFI is distributed.
