# 0.1.0-beta.1 — first experimental Sequoia release

## 基础与目标 / Basis and scope

基于 [OpenIntelWireless/itlwm](https://github.com/OpenIntelWireless/itlwm) 的独立 AirportItlwm 分支，
继承 OpenBSD net80211、Intel IWM/IWX 和固件基础；共同上游基线为 `53c51c2cdd6e4b69beb91f310d74c53422b0f8bd`。
本仓库以整理后的源码快照开始，保留上游版权、GPL-2.0、各文件声明及独立 Intel 固件许可。
不是 OpenIntelWireless 官方版本，也不宣称达到其全部功能或硬件覆盖范围。

本 Sequoia 适配分支通过 **OpenAI Codex 辅助开发**，包括多轮源码分析、接口适配、修复、离线测试和发布整理；
维护者确定开发范围并配合完成所列实机验收。保留上游贡献归属，不代表 OpenAI 或 OpenIntelWireless 背书，
AI 辅助开发不等同于完整功能、安全审计或长期稳定性验证。

本版只面向 **Intel x86_64、AX210、macOS Sequoia 15.8 / build 24H23**。
适配原装 Sequoia 网络框架和 Skywalk/WCL，提供原生 Wi-Fi 服务，无需 HeliPort 或 OCLP 网络降级。
使用私有 ABI；驱动严格检查系统 build，其他 Sequoia 小版本及其他网卡不属于本版验证范围。

## 功能与变化 / Features

- 状态栏和系统设置 Wi-Fi 扫描列表，2.4/5 GHz WPA2-Personal/PSK/CCMP。
- WPA/WPA2、WPA2/WPA3 混合 AP 提供 WPA2-PSK/CCMP 时选择 WPA2；不表示支持 SAE 或 TKIP。
- Sequoia 接口注册、扫描生命周期、连接/断开、密钥重试、后台扫描、速率/基础流量诊断。
- 包含 r260 断开时保留已选扫描观察记录的修复，以及此前换钥重试和唤醒恢复实现。
- Release 沿用验证过的 `-O1`，移除调试符号，保留内核链接符号及运行诊断。

## 验证结果 / Validation

2026-09-30 在开发主机 AX210 / 24H23 上：

- 已从内置 OpenCore 启动；加载 `0.1.0b1` 和精确 Release UUID，唯一原生服务启动完成。
- 用户确认状态栏和系统设置列表正常，两个测试网络间切换成功，基本上网正常。
- 两次间隔约57秒的只读采样保持连接和有效端口；扫描51次/终态51次，无待处理扫描。
  接收失败、MIC、重放、解密及所查固件错误计数为0；这不是吞吐量或零丢包测试。
- 73个编译单元、链接、系统库检查及 root 离线 KXLD 验证通过。
  从发布源码归档独立重建，在同一工具链下二进制、Info.plist 和 UUID完全相同。
- 候选继承的 BSD/IWM/IWX 离线回归覆盖扫描、连接、密钥及恢复边界，包含 ASan/UBSan；
  硬件、固件和内核调度部分使用 mock，不能替代实机验收。GitHub Actions 尚未实际运行。

此前 r259 切网出现过候选缓存缺失、已连接项转圈及一次重新询问密码。
本版包含 r260 缓存修正，本次基本切网已确认正常；尚未完成相同多轮压力复测，不宣称根治所有 UI/重试现象。

## 限制 / Limitations

- 不支持 WPA3-only/SAE、强制 PMF、必须 TKIP 的网络、6 GHz、企业 802.1X、AWDL/AirDrop 或热点。
- PMF/TKIP 试验代码即使存在于源码中，也未作为可用能力启用。
- 睡眠唤醒、AP 失联恢复、持续双向流量、真实 AP PTK/GTK 换钥和长期稳定性仍待实机验收。
- 仅验证此系统 build 和适配器组合；不承诺其他设备、系统升级或跨编译器字节级可复现。

## 安装与资产 / Installation and assets

见 [README-SEQUOIA.md](README-SEQUOIA.md)：先备份自己的 EFI，保留 USB 恢复路径，
使用唯一的 `AirportItlwmSequoiaNative.kext` 及 `itlwm_native=1 itlwm_bsd_owner=skywalk`。
不分发开发主机 EFI、系统二进制/KC、机器身份或网络日志；不要运行中加载/卸载。
Release ZIP、完整源码 ZIP 和 `SHA256SUMS` 随版本提供。CFBundleVersion 为 Apple 格式 `0.1.0b1`。

**English:** Independent experimental fork of OpenIntelWireless/itlwm for native Sequoia networking.
Developed with assistance from OpenAI Codex, with maintainer-directed scope and hardware validation.
AX210 / x86_64 / 24H23 internal boot, both Wi-Fi lists, switching and basic internet use have been confirmed.
WPA2-Personal/CCMP is the supported security scope, including compatible transition networks selecting WPA2.
SAE/WPA3-only, mandatory PMF/TKIP, 6 GHz, enterprise authentication and AWDL are unsupported.
Sleep/wake, AP recovery, real rekeys, sustained traffic and long-term stability remain unverified.
This first beta is not an official upstream release or a claim of full upstream feature parity.
