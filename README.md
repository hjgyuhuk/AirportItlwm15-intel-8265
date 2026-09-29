# AirportItlwm-Sequoia

Native Intel Wi-Fi for macOS Sequoia, derived from [OpenIntelWireless/itlwm](https://github.com/OpenIntelWireless/itlwm).

**0.1.0-beta.1** targets Intel x86_64 macOS Sequoia **15.8 / build 24H23**, with AX210 as the tested adapter.
This is an independent fork. It is not an official OpenIntelWireless release.

See [Sequoia build, installation and limitations](README-SEQUOIA.md) and
[release notes](RELEASE_NOTES.md). Use the dedicated Python build entry point for Sequoia;
the inherited Xcode targets describe older macOS releases.

## 项目基础与当前状态 / Project status

本项目在 OpenIntelWireless/itlwm 的 AirportItlwm 路径上适配 Sequoia 原生网络框架，
继承其 OpenBSD net80211、Intel IWM/IWX 驱动和固件基础。开发分支的共同上游基线为
[`53c51c2`](https://github.com/OpenIntelWireless/itlwm/commit/53c51c2cdd6e4b69beb91f310d74c53422b0f8bd)
（2.4.0 开发线）；本仓库首个提交是整理后的源码快照，未包含本机开发历史。
主要改动覆盖 Sequoia 私有 ABI、Skywalk/WCL 接口注册、扫描结果、连接/断开、WPA2 密钥生命周期、
后台扫描及诊断。使用系统原装网络框架，无需 HeliPort 或 OCLP 网络框架降级。
Sonoma-ReAWDL 等项目仅作研究参考，本版没有实现 AWDL/AirDrop。

### 开发方式 / Development process

本 Sequoia 分支通过 **OpenAI Codex 辅助开发**，经历多轮源码分析、接口适配、缺陷修复、
离线测试和发布整理；维护者负责确定范围并配合实机启动、切网和上网验证。
这描述开发过程，不替代上游作者的贡献，也不代表 OpenAI 或 OpenIntelWireless 官方背书。
AI 辅助代码仍可能存在缺陷，验证范围及未完成项目如下。

This Sequoia fork was developed with assistance from OpenAI Codex. The maintainer directed scope
and performed the reported hardware checks. AI assistance is not a substitute for validation,
upstream attribution, or an endorsement by OpenAI or OpenIntelWireless.

| 项目 | 0.1.0-beta.1 状态 |
| --- | --- |
| 实机范围 | x86_64、Intel AX210、macOS 15.8 / 24H23；其他设备/系统版本未验收 |
| 已实测 | 从内置 OpenCore 启动、两处 Wi-Fi 列表、2.4/5 GHz 网络切换、基本上网 |
| 安全模式 | WPA2-Personal/CCMP；混合网络必须提供可用的 WPA2-PSK/CCMP |
| 仍待实机验收 | 睡眠唤醒、AP 失联恢复、持续双向收发、真实 PTK/GTK 换钥、长期稳定性 |
| 本版不支持 | WPA3-only/SAE、强制 PMF/TKIP、6 GHz、企业 802.1X、AWDL/AirDrop、热点 |

This is a first experimental beta, not a feature-complete or generally validated replacement for upstream AirportItlwm.
Internal OpenCore boot, both native Wi-Fi lists, switching between the tested networks and basic internet use
were confirmed on AX210 / 24H23. Offline regression tests and a reproducible local Release build do not replace
the reliability tests listed above. Other macOS builds are rejected by the driver's build check.

Download this fork from [Releases](https://github.com/zhangcheng142/AirportItlwm-Sequoia/releases).
Report fork-specific issues [here](https://github.com/zhangcheng142/AirportItlwm-Sequoia/issues), not to upstream.
Driver sources retain their GPL-2.0 and per-file notices; bundled Intel firmware retains its separate
[`iwlwifi-firmware-license`](iwlwifi-firmware-license). Upstream authors and credits remain below.

The upstream documentation below describes upstream itlwm/AirportItlwm, and does not expand this fork's tested scope.

---

# itlwm

**An Intel Wi-Fi Adapter Kernel Extension for macOS, based on the OpenBSD Project.**

## Documentation

We highly recommend exploring our documentation before using this Kernel Extension:

- [Intro](https://OpenIntelWireless.github.io/itlwm)
- [Compatibility](https://openintelwireless.github.io/itlwm/Compat)
- [FAQ](https://openintelwireless.github.io/itlwm/FAQ)

## Download

[![Download from https://github.com/OpenIntelWireless/itlwm/releases](https://img.shields.io/github/v/release/OpenIntelWireless/itlwm?label=Download)](https://github.com/OpenIntelWireless/itlwm/releases)

## Questions and Issues

Check out our [FAQ Page](https://openintelwireless.github.io/itlwm/FAQ) for more info.

If you have other questions or feedback, feel free to [![Join the chat at https://gitter.im/OpenIntelWireless/itlwm](https://badges.gitter.im/OpenIntelWireless/itlwm.svg)](https://gitter.im/OpenIntelWireless/itlwm?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge&utm_content=badge).

We only accept bug reports in GitHub Issues, before opening an issue, you're recommended to reconfirm it with us on [Gitter](https://gitter.im/OpenIntelWireless/itlwm); once it's confirmed, please use the provided issue template.

## Credits

- [Acidanthera](https://github.com/acidanthera) for [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK)
- [Apple](https://www.apple.com) for [macOS](https://www.apple.com/macos)
- [AppleIntelWiFi](https://github.com/AppleIntelWiFi) for [Black80211-Catalina](https://github.com/AppleIntelWiFi/Black80211-Catalina)
- [ErrorErrorError](https://github.com/ErrorErrorError) for UserClient bug fixes
- [Intel](https://www.intel.com) for [Wireless Adapter Firmwares](https://www.intel.com/content/www/us/en/support/articles/000005511/network-and-io/wireless.html) and [iwlwifi](https://wireless.wiki.kernel.org/en/users/drivers/iwlwifi)
- [Linux](https://www.kernel.org) for [iwlwifi](https://wireless.wiki.kernel.org/en/users/drivers/iwlwifi)
- [mercurysquad](https://github.com/mercurysquad) for [Voodoo80211](https://github.com/mercurysquad/Voodoo80211)
- [OpenBSD](https://openbsd.org) for [net80211, iwn, iwm, and iwx](https://github.com/openbsd/src)
- [pigworlds](https://github.com/OpenIntelWireless/itlwm/commits?author=pigworlds) for DVM devices support, MIRA bug fixes, and Tx aggregation for MVM Gen 1 devices
- [rpeshkov](https://github.com/rpeshkov) for [black80211](https://github.com/rpeshkov/black80211)
- [usr-sse2](https://github.com/usr-sse2) for implementing the usage of Apple RSN Supplicant and bug fixes
- [zxystd](https://github.com/zxystd) for developing [itlwm](https://github.com/OpenIntelWireless/itlwm)

## Acknowledgements

- [@penghubingzhou](https://github.com/startpenghubingzhou)
- [@Bat.bat](https://github.com/williambj1)
- [@iStarForever](https://github.com/XStar-Dev)
- [@stevezhengshiqi](https://github.com/stevezhengshiqi)
- [@DogAndPot](https://github.com/DogAndPot) for providing resources and help for system configuration
- [@Daliansky](https://github.com/Daliansky) for providing Wi-Fi cards
