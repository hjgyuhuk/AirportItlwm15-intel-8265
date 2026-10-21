# itlwm #1018 — macOS Sequoia (15.x) 支持分析

分析对象：[itlwm#1018](https://github.com/OpenIntelWireless/itlwm/issues/1018)（`macos sequoia support (15.x)`）
关联主 issue：[itlwm#1009](https://github.com/OpenIntelWireless/itlwm/issues/1009)（`Please add AirportItlwm support for macOS 15 Sequoia`）
参考源码：`OpenIntelWireless/itlwm` @ `53c51c2`（v2.4.0），检索用；探索期的临时副本已删除，git 历史里可取回

---

## 1. Issue 结论

| 项 | 结论 |
|---|---|
| #1018 状态 | 重复 issue，社区已在第 11 条评论标记 `Duplicate of #1009` |
| 官方支持 | **无**。upstream 未合并任何 Sequoia 支持代码，#1009 至今 open |
| 唯一"可用"路径 | OCLP root patch 恢复旧版 `IO80211FamilyLegacy.kext`，属于 hack，非真修复 |
| 本地自行编译 | **失败**，`Code=31`，无法 bind 缺失符号 |

---

## 2. 失败根因（已在源码中定位并验证）

报错符号：

```
_ZN17IO80211Controller11postMessageEjPvmjS0_
```

### 2.1 符号反解

实测用 clang 编译验证反解结果（`EjPvmjS0_` 对应 5 参数签名）：

```
IO80211Controller::postMessage(unsigned int, void*, unsigned long, unsigned int, void*)
```

该签名在 itlwm 源码中只有一处声明：

- `include/Airport/IO80211ControllerV2.h:252`
  ```cpp
  virtual void postMessage(UInt,void *,unsigned long,UInt,void *);
  ```

### 2.2 为什么 V2 头文件被编译进来

`include/Airport/Apple80211.h:15-28` 按宏切换两套 IO80211 头：

```c
#ifdef IO80211FAMILY_V2
#include "IO80211ControllerV2.h"   // ← 走这条
...
#else
#include "IO80211Controller.h"      // ← 旧路径，无此符号
#endif
```

`IO80211FAMILY_V2` 在 `itlwm.xcodeproj/project.pbxproj` 中对较新 target（Sonoma 等）开启。

### 2.3 符号为什么会成为未定义引用

这是关键点：**不是代码里直接调用导致的**，而是 **vtable 残留**。

- `AirportItlwm/AirportItlwmV2.hpp:42`
  ```cpp
  class AirportItlwm : public IO80211Controller {
  ```
- `AirportItlwm/AirportItlwmV2.cpp` 中所有 `postMessage` 调用都打在 `fNetIf`（`IO80211SkywalkInterface*`）或 `interface` 上，**没有任何一处直接调用 controller 的 `postMessage`**。

已实测确认：子类继承了未定义的虚函数时，编译器会为其生成 vtable，vtable 里带上该未定义符号，从而产生 undefined reference——即使代码从不调用它。

```cpp
struct A { virtual void missing(unsigned,void*,unsigned long,unsigned int,void*); };
struct B : A { B(){} };   // 仅构造即触发 vtable 发射
// nm -u → A::missing(unsigned int, void*, unsigned long, unsigned int, void*)
```

itlwm 侧对应场景：`AirportItlwm` 被 `OSDeclareDefaultStructors` 实例化，vtable 必然发射。

**所以：修掉这一个符号，只是把错误推到下一个符号。**

---

## 3. Sequoia 到底改了什么

`#1029` 里 crangel660 提供了 Sequoia 上 `IO80211Family` 的符号表，`postMessage` 并非消失，而是**签名变了**：

```
IO80211Controller::postMessage(IO80211SkywalkInterface*, unsigned int, void*, unsigned long, bool)
IO80211Controller::postMessageSync(IO80211SkywalkInterface*, unsigned int, void*, unsigned long, bool)
IO80211SkywalkInterface::postMessage(unsigned int, void*, unsigned long, bool)
IO80211InterfacePostMessage(char const*, unsigned int, void*, unsigned long, IO80211Controller*)
```

对比 itlwm 期望的：

```
IO80211Controller::postMessage(unsigned int, void*, unsigned long, unsigned int, void*)
```

差异：`IO80211Controller` 版本**新增了首参 `IO80211SkywalkInterface*`**，且尾部 `(UInt, void*)` 消失。5 参数旧签名不再导出 → `kextutil` bind 阶段 `Code=31`。

### 更深层的问题（zxystd 的说明）

`#983` / `#1029` 中 zxystd 明确指出，真正的障碍不是符号：

1. Apple 把 Wi-Fi 驱动主体移到了**用户态**，内核侧 IOCTL 通过 `IO80211APIUserClient` 转发。
2. Apple **不为第三方驱动提供**链接所需的方法，没有公开的用户态通信接口。
3. 社区观察到 Apple 用 `IO80211NoneProtocol` 做了个空壳类承接 IOCTL，新栈疑似引入 `WCL`（WiFi Core Layer）前缀组件。

**结论：Sequoia 支持 = 重写用户态通信层（IOCTL/IOUC 通路），不是补几个符号。** zxystd 提到可能的路子是用 Lilu hook 掉 ioctl 方法整体替换（该思路未见实现）。

---

## 4. 社区现有方案对比

| 方案 | 原生 Wi-Fi | 需 root patch | SIP | 备注 |
|---|---|---|---|---|
| **A. OCLP root patch**（fake BRCM ID + patch `IO80211FamilyLegacy.kext`） | 是 | 是 | 需降到 `03080000`，`csrutil status` 显示 `Kext Signing: disabled`、`Filesystem Protections: disabled` | sughero 方案；每次系统更新会撤销全部 root patch，需重新走流程并清 NVRAM 修蓝牙；会破坏增量更新 |
| **B. itlwm.kext + HeliPort** | 否 | 否 | 完整保留（`0000000`） | 现成可用，代价是没有原生 Wi-Fi 菜单/隔空投屏等 |
| **C. 社区 fork `zhangcheng142/AirportItlwm-Sequoia`** | 是 | 未知 | 未知 | 评论称在 15.8 可用；未在 upstream 合并 |
| **D. upstream PR #1054**（serejaishkin, `Update AirportItlwm for Sequoia`） | — | — | — | open，+316/-369 改 20 文件；截至 2026-09 未合并，维护者未表态是否真解决 |

方案 A 的 OCLP 配置可参考 `5T33Z0/OCLP4Hackintosh` 的 `AirportItlwm_Sequoia.plist`（需 `IO80211FamilyLegacy.kext`、`AirportItlwm_Sequoia.kext`，`MinKernel 24.0.0`）。

蓝牙不依赖 root patch，只需按 macOS 版本放对 kext（`IntelBluetoothFirmware` + `BlueToolFixup` + `IntelBTPatcher`），必要时加 NVRAM：

```
bluetoothExternalDongleFailed       00
bluetoothInternalControllerInfo     00000000 00000000 00000000 0000
```

---

## 5. 若要自行推进：验证与实施顺序

### 5.1 先建立符号基线（不需要真机就能做的部分）

把当前构建产物与目标系统符号表对比，一次性列出**全部**缺失符号，而不是逐个撞错：

```bash
# 1. 构建 AirportItlwm（选对应 target）
xcodebuild -project itlwm.xcodeproj -target AirportItlwm-Sonoma -configuration Release

# 2. 列出 kext 的未定义 C++ 符号
nm -u <kext>/Contents/MacOS/AirportItlwm | c++filt | rg 'IO80211|Apple80211'

# 3. 在 Sequoia 实机 / 解出的 KDK 上导出系统侧符号
nm -gU /System/Library/Extensions/IO80211FamilyLegacy.kext/Contents/MacOS/IO80211FamilyLegacy \
  | c++filt | rg 'IO80211'

# 4. 做差集，得到真正需要适配的清单
comm -23 <(步2排序去重) <(步3排序去重)
```

`kextutil -n -t <kext>`（`-n` 只做符号/依赖校验，不加载）可在目标系统上一次性拿到全部 bind 错误。

### 5.2 修复顺序建议

1. **只补符号跑通加载**——在 `AirportItlwmV2.cpp` 里实现
   ```cpp
   void AirportItlwm::postMessage(UInt, void *, unsigned long, UInt, void *) { /* TODO */ }
   ```
   可消除该条 undefined reference，但预计立刻暴露下一个缺失符号。仅用于确认符号清单，不要误以为接近完成。

2. **按 Sequoia 新签名改头文件**——把 `IO80211ControllerV2.h` 的 `postMessage` 对齐成 `(IO80211SkywalkInterface*, UInt, void*, unsigned long, bool)`，并同步核对 vtable 布局。注意 `IO80211ControllerV2.h:256` 用 `uint8_t filler[0x128]` 填充类大小，**类布局一旦变化，对象大小随之变化**，与之相关的 `static_assert` 和成员偏移都要复核。

3. **处理 IOCTL/IOUC 通路**——这是真正的工作量所在（见第 3 节），符号对齐只解决"能加载"。

### 5.3 必须核对的连带项

改动 `IO80211Controller` / `IO80211SkywalkInterface` 布局会连带影响：

- `AirportItlwm/Info.plist` 与 `AirportItlwm-Sonoma-Info.plist` 的 `OSBundleLibraries` 版本键
  （Sonoma 版声明 `com.apple.iokit.IO80211Family = 1.5.0`、`com.apple.iokit.IOSkywalkFamily = 1.0`、`com.apple.driver.corecapture = 1.0.0`）
- `IOPCMatch` 中的 PCI ID 列表（`AirportItlwm/Info.plist`）
- `itlwm.xcodeproj` 中 `__IO80211_TARGET` 的取值（现最高 `__MAC_14_4`，没有 Sequoia 对应档）

---

## 6. 实证：`zhangcheng142/AirportItlwm-Sequoia` 怎么解的

参考源码：fork `zhangcheng142/AirportItlwm-Sequoia` @ `f31a3ad`（0.1.0-beta.1）

### 6.1 改动规模

与 upstream `53c51c2` 全量 tree diff：**80+ 文件不同**，新增 30+ 个 `*_sequoia.inc` / `.inc` 片段、`scripts/sequoia/`（Python 构建入口）、`tests/`（17 个离线测试）。

关键文件 diff 行数：`AirportItlwmV2.cpp` 923 行、`IO80211SkywalkInterface.h` 123 行、`IO80211ControllerV2.h` 122 行。

**这不是补丁，是一次真实的框架适配。**

### 6.2 我预测的符号问题，他们的解法比我想的更彻底

第 2 节我判断「修掉一个符号只会暴露下一个」。他们确实撞上了，但**解法不是逐个补**，而是釜底抽薪：

```c
// include/Airport/IO80211ControllerV2.h  (新)
#if __IO80211_TARGET >= __MAC_15_0
    // 关键：改成非虚函数，且首参改为 IO80211SkywalkInterface*
    IOReturn postMessage(IO80211SkywalkInterface *, UInt, void *, unsigned long, bool);
private:
    void postMessageSync(IO80211SkywalkInterface *, UInt, void *, unsigned long, bool);
#else
    virtual void postMessage(UInt,void *,unsigned long,UInt,void *);   // 旧路径保留
#endif
```

**`virtual` 关键字被去掉了。** 这直接消灭了 vtable 残留引用——不虚就不会进 vtable，不需要外部符号，bind 阶段自然不再报错。同时签名对齐了 Sequoia 真实 ABI（首参 `IO80211SkywalkInterface*`），正好印证第 3 节 crangel660 的符号表。

源码注释写明用途：*"This audited, nonvirtual posting API must be callable by the native result adapter."*

### 6.3 类布局：印证了我在 5.2 提的担忧

```c
#if __IO80211_TARGET >= __MAC_15_0
    uint8_t filler[0x128 - sizeof(IOEthernetController)];   // 改
#else
    uint8_t filler[0x128];
#endif
};
static_assert(sizeof(IO80211Controller) == 0x128, "Sequoia Controller size");
```

`filler` 从固定 `0x128` 改成减去基类大小，并加 `static_assert` 锁死对象尺寸。**Sequoia 上 `IO80211Controller` 的成员顺序和数量确实变了**——不只是签名变，vtable 布局也变（新增 `debugStateInit`、`CreatePostOffice`、`getPostOffice`、`getActionFramePoolCapacity` 等一批方法，`setHardwareAddress` 被条件编译移出）。

`IO80211SkywalkInterface` 同样有签名变化：`prepareBSDInterface` 返回类型 `bool`→`IOReturn`、`inputPacket` 多一参、`logTxCompletionPacket` 多一参、`createPeerManager`→新增 `createPeer`。

### 6.4 构建体系

- 新增 `__MAC_15_0`（`= 150000`）到 `itlwm/PrivateSPI.pch`，upstream 最高只到 `__MAC_14_4`
- 新增 `scripts/sequoia/build.py` 作为专用入口，含 `MacKernelSDK-pexpert.patch`、`sdk.lock.json`（SDK 版本锁定）、`release.json`
- README 明确说：*the inherited Xcode targets describe older macOS releases* —— Sequoia 构建**不走 xcodebuild target**
- `AirportItlwm/Info.plist` 的 `OSBundleLibraries` 仍是 `com.apple.iokit.IO80211Family = 1200.12.2b1`（与 upstream 一致）

### 6.5 能力边界（README / RELEASE_NOTES 自述）

只验证 **Intel x86_64 + AX210 + Sequoia 15.8 / build 24H23**，严格校验系统 build，其他版本直接拒绝加载。

不支持：WPA3-only/SAE、强制 PMF、TKIP、6 GHz、企业 802.1X、AWDL/AirDrop、热点。
待实机验收：睡眠唤醒、AP 失联恢复、持续双向流量、真实 PTK/GTK 换钥、长期稳定性。

安装需要 boot-arg：`itlwm_native=1 itlwm_bsd_owner=skywalk`。

开发方式：README 与 RELEASE_NOTES 都明确声明**由 OpenAI Codex 辅助开发**，作者本人未背书完整功能与稳定性。

### 6.6 与我此前结论的偏差（更正）

我在第 3、6 节说「重写用户态通信层，符号对齐只解决能加载」，这个判断**过于悲观**。实际情况：

- 用户态 IOCTL 通路确实重写了（`native_join_*`、`native_scan_*`、`native_key_*`、`native_leave_*` 等 `.inc`），但**没有走 Lilu**，而是在自己的 kext 内实现了一套 native adapter
- 走通了，因为它只支持 AX210 单一适配器 —— 覆盖面窄反而让「逐个适配私有 ABI」变得可行。换多型号就是另一回事

**结论修正：Sequoia 支持是可做的，但代价是绑定单一硬件 + 单一系统 build，且放弃大量特性。**

---

## 7. 硬件适配性评估：Intel 8265 / 8275

### 7.1 结论（已按实机结果再次更正）

**原结论有误，已更正两次：**

1. 8265 的 PCI ID 是 `0x24FD`，不是 `0x2725`（那是 AX200/AX210）；原样安装会被**静默拒绝**。
2. **2026-10-20 实机验证后更正**：改白名单放行 8265 之后，kext 仍然**无法工作**。根因不是"未实测"，而是 **IWM HAL 的 Sequoia 原生 scan/join 硬性要求 `sc_device_family == IWM_DEVICE_FAMILY_7000`**，而 8265 属于 **8000 家族**。这是一道**结构性硬门**，不是配置开关 —— 见 7.3。
3. **2026-10-20 探针实验后再次更正**：拆掉 family 硬门后，**fork 的完整主路径在 8265 上启动成功**（`ItlwmNativeStage = "17_complete"`，系统出现 Wi-Fi 硬件端口，BSD 层交给 skywalk），固件对话（NVM / 校准 / 常规固件 / PHY DB 回放 / 蓝牙）**全部通过**。剩下的只有 native 扫描这一个环节。**"结构性硬门"的说法只对"未改造的 fork"成立。**
4. **2026-10-20 收尾更正**：native 扫描改走固件要求的 UMAC 请求后，**扫描 → 关联 → 认证 → 上网全链路跑通**。真正的分水岭不是"family 硬门"，而是**扫描请求的命令 id 与格式属于家族边界**（`IWM_SCAN_OFFLOAD_REQUEST_CMD` 是 7000 家族的 scan offload 命令，8000 家族收到就复位）。

**最终判断（2026-10-20 实机后）：**
- **macOS 版本**：✅ 完全适配（内核串逐字节命中，kext 加载成功，Apple 的 IO80211Family 正常接受控制器）
- **Intel 8265**：✅ **已适配成功**（2026-10-20 实测：扫描 22 个 beacon、关联认证成功、`en2` 拿到 IP、上网通）。原判断"需要逆向移植 7000→8000 家族的 WCL 命令传输层"**过重** —— 实测发现 **WCL 传输层在 8000 家族上原生可用**，要改的只是 4 处 device family 硬门 + native 扫描改走 UMAC + 两处格式细节（段 id 重标、信道 144 的家族归属）。详见 [`sequoia-8265-port.md`](sequoia-8265-port.md)。
- **若换成 7000 家族卡（7265 / 3165 / 3168 / 3160）**：✅ 极有可能可用，只需扩白名单（该路径本就是为它们写的，但出厂白名单里没有它们 —— 见 7.3 的"死代码"分析）
- **若换成 AX210 / AX211**：✅ 作者实测路径，风险最低

### 7.2 8265 在 fork 中的位置：驱动链路完整，改造后已跑通主路径

> ⚠️ **更正记录**：本文早期版本把 8265 的 PCI ID 写成 `0x2725`（那是 AX200/AX210 的 ID），并断言 8265 走 `ItlIwx`（AX 系列 HAL）。**两处都错**。
> 8265 走的是 `ItlIwm`（7xxx/8xxx iwl-legacy 系列 HAL）。实测本机 `ioreg`：`pci8086,24fd` → `model = <"Intel Wireless 8265">`。

**驱动链路（已核实全部打通）**

| 检查项 | 位置 | 结果 |
|---|---|---|
| PCI ID | `ioreg` → `pci8086,24fd`（device `0x24FD`，vendor `0x8086`） | ✅ |
| HAL 设备表 | `itlwm/hal_iwm/mac80211.cpp:4518` → `PCI_PRODUCT_INTEL_WL_8265_1 0x24fd`；`:4549` 在 `iwm_devices[]` | ✅ |
| HAL 实例化 | `AirportItlwm/IOPCIEDeviceWrapper.cpp:95-98` → `ItlIwm::iwm_match()` 命中则 `new ItlIwm` | ✅ |
| 固件 | `itlwm/firmware/iwm-8265-36` 已随包分发 | ✅ |
| Info.plist 源码 | `AirportItlwm/Info.plist` `IOPCIMatch` **含 `0x24fd8086`** | ✅（但见下方闸门 2） |
| **device family** | `mac80211.cpp:4858` → `sc_device_family = IWM_DEVICE_FAMILY_8000` | ❌ **原生路径只认 7000** |

**卡住的两道门**

| 闸门 | 位置 | 上游值 | 对 8265 的后果 |
|---|---|---|---|
| 1. 运行时白名单 | `include/HAL/SequoiaTrialPolicy.h:17` | `device == 0x3166 \|\| device == 0x2725` | ❌ `0x24FD` 不在其中 → `IOPCIEDeviceWrapper.cpp:89` 直接 `return nullptr`，**静默不加载** |
| 2. 构建时白名单 | `scripts/sequoia/release.json:14` `pci_match` | `"0x31668086 0x27258086"` | ❌ `scripts/sequoia/build.py:65` 用它**覆盖** Info.plist 的 `IOPCIMatch` → 源码里本来有的 `0x24fd8086` 被抹掉 |
| **3. device family 硬门** | `itlwm/hal_iwm/native_scan_sequoia.inc:18`、`native_join_start_sequoia.inc:19` | 拒绝 9000 家族（MQ RX） | ✅ **已改为「拒绝 9000、放行 8000」** —— 8265 主路径已跑通 |

闸门 2 很容易被忽略：`AirportItlwm/Info.plist` 里的完整 ID 列表（含 `0x24fd8086`）**不参与** Sequoia 构建，只有 `release.json` 的两三个 ID 生效。

**★ 三道闸门现已全部处理（见第 9 节）：闸门 1、2 靠扩白名单，闸门 3 靠逐处放行 8000 家族（保留 9000 的拒绝）。★★**

**意外的好消息：fork 的 Sequoia 原生实现本来就是围绕 `ItlIwm` 写的**

| 证据 | 位置 |
|---|---|
| `ItlIwm` 实现了原生 HAL 接口 | `itlwm/hal_iwm/ItlIwm.hpp:50`（`public ItlNativeHal`）、`:77`（`getNativeHal() override { return this; }`） |
| `ItlIwx`（AX210）同样实现了 | `itlwm/hal_iwx/ItlIwx.hpp:150,172` |
| `ItlIwn`（4965 等老卡）**没有**实现 | 无 `ItlNativeHal` 继承 |
| IWM 专属原生代码 | `itlwm/hal_iwm/native_join_candidate/security/start/state_sequoia.inc`、`native_key_sequoia.inc`、`native_leave_sequoia.inc`、`native_scan_cache_sequoia.inc`、`native_watchdog_sequoia.inc`、`dma_sequoia.inc`、`nvm_sequoia.inc`、`phy_db_sequoia.inc`、`scan_request_sequoia.inc`、`firmware_trial/policy_sequoia.inc` |
| IWM 专属头文件 | `include/HAL/IwmCommandId.h`、`IwmCommandSlots.h`、`IwmFirmwareTrial.h`、`IwmNicLease.h`、`IwmNvmDiagnostics.h`、`IwmOperationOwner.h`、`IwmScanFrame.h`、`IwmScanNotification.h`、`IwmScanTrial.h` |
| 诊断模块直接用 `ItlIwm` | `AirportItlwm/SequoiaFirmwareTrial.cpp:28,179`、`AirportItlwm/SequoiaNvmTrial.cpp:27,98` |
| 离线测试 | `tests/` 下 10 个 `iwm_*_test.cpp` |

即：**被挡的是闸门，不是能力** —— 这句话对 7000 家族成立，对 8265 不成立。原生层通过 `ItlNativeHal` 虚接口调度（`AirportItlwm/native_join_sequoia.inc:236,260`、`native_lifecycle_sequoia.inc:460`），与具体 HAL 无关；**但 HAL 内部按 device family 分支**，见 7.3。

### 7.3 真正的硬门：IWM 的 device family（2026-10-20 实测后追加）

`ItlIwm` 把 7xxx/8xxx/9xxx 系列按 `sc_device_family` 分为三档（`itlwm/hal_iwm/mac80211.cpp:4790-4900`）：

| family | 设备 | PCI ID |
|---|---|---|
| **7000** | 3160 / 3165 / 3168 / 7260 / 7265 | `0x3160` `0x3165` `0x3168` `0x7260` `0x7265` |
| **8000** | 8260 / **8265** | `0x24F8` / **`0x24FD`** |
| 9000 | 9260 / 9560 / 9462 / 9461 | … |

**fork 的 IWM 原生 scan/join 只认 7000 家族**：

| 文件 | 判断 | 不满足时 | 本项目处置 |
|---|---|---|---|
| `itlwm/hal_iwm/native_scan_sequoia.inc` | `sc->sc_device_family != IWM_DEVICE_FAMILY_7000` | 扫描返回 `EOPNOTSUPP` | 放行 8000（`46b1f76`） |
| `itlwm/hal_iwm/native_join_start_sequoia.inc` | 同上 | 关联返回 `ENXIO` | 放行 8000（`b1cd697`） |
| `itlwm/hal_iwm/scan_trial_sequoia.inc` | 同上 | 扫描试验拒绝 | 放行 8000（`b1cd697`） |
| `itlwm/hal_iwm/firmware_trial_sequoia.inc` | 同上 | 固件试验拒绝 | 放行 8000（`b1cd697`） |

**★ 9000 家族（MQ RX）必须继续拒绝** —— 它的 station ABI 与 LMAC 不同，跨代移植等于重写。★★★**

更深的依赖：fork 用新的**槽位式命令传输层**（`itlwm/hal_iwm/command_sequoia.inc`，`commandSlots.publish/acknowledge`）替换了 upstream 的完成队列 + `iwm_nic_lock()` 机制，而 **NIC 锁只在 7000 家族生效**：

```cpp
// command_sequoia.inc
if (sc->sc_device_family == IWM_DEVICE_FAMILY_7000 && !ring.queued && !iwm_nic_lock(sc)) { error = EBUSY; ... }
if (--ring.queued == 0 && sc->sc_device_family == IWM_DEVICE_FAMILY_7000) iwm_nic_unlock(sc);
```

8000 家族在 upstream 里靠另一套 `sc_nic_locks` / `tx.cpp:177-179` 处理。

> ⚠️ **更正（2026-10-20 探针实验后）**：本文早期版本据此断言"要把整套 WCL 命令传输层在 8000 家族上重新对齐 —— 这是逆向工作量，不是改配置"。
> **实测证明这个判断过重**：槽位式命令传输层在 8000 家族上**原生可用** —— 固件对话（NVM / 校准 / 常规固件 / PHY DB 回放 / 蓝牙）**全部通过**，主路径也完整启动到 `17_complete`。
> **实际要改的只有**：4 处 device family 硬门、native 扫描改走 UMAC、段 id 重标、信道 144 的家族归属、两处构建细节。

#### 一个反直觉的推论：7000 家族路径在出厂版本里是"够不着的死代码"

出厂白名单 = `{0x3166, 0x2725}`，两个都是 **AX 系列**（`hal_iwx`）→ 作者实测路径。
而 `hal_iwm` 那套原生路径是为 **7000 家族**（7265/3165…）写的，**这些 ID 一个都不在白名单里**。

也就是说：**出厂的 `AirportItlwm-Sequoia` 实际上对 7000 家族卡也不开箱可用**，只是需要把白名单扩到 `0x3165` / `0x7265` / `0x3168` / `0x3160`。

→ **如果换一张 Intel 7265 或 3165（7000 家族），这个 fork 反而是最有希望开箱可用的组合**（只需扩白名单 + `pci_match`）。

### 7.4 真正的门槛：系统版本必须精确匹配

`IOPCIEDeviceWrapper.cpp:86-89` 在匹配 PCI 之前先过这道闸：

```cpp
if (!PE_parse_boot_argn("itlwm_native", &nativeOptIn, sizeof(nativeOptIn)) ||
    !SequoiaTrial::allowed(nativeOptIn, version_major, version_minor, version,
        device->configRead16(kIOPCIConfigVendorID), device->configRead16(kIOPCIConfigDeviceID)))
    return nullptr;      // ← 不匹配直接放弃加载
```

`SequoiaTrial::allowed` 的判据（`SequoiaTrialPolicy.h:15-17`）：

```cpp
return mode == 1 && major == 24 && minor == 6 && kernelVersion &&
       strcmp(kernelVersion, kKernelVersion) == 0 && vendor == 0x8086 &&
       (device == 0x3166 || device == 0x2725);   // ← 上游原样；本地改为加上 0x24FD
```

五项全中才允许加载：

1. `mode == 1` —— boot-arg `itlwm_native=1`
2. `major == 24` —— Darwin 24.x（即 macOS 15.x）
3. `minor == 6` —— **Darwin 24.6**
4. `strcmp(kernelVersion, kKernelVersion) == 0` —— **内核版本字符串逐字节相等**
5. `vendor == 0x8086` 且 `device` 在白名单内

第 4 项是最严的。要求完全等于：

```
Darwin Kernel Version 24.6.0: Sun Aug 23 20:30:56 PDT 2026; root:xnu-11417.140.69.712.69~1/RELEASE_X86_64
```

这是**内核 build 号 + 编译时间戳的精确匹配**。只要你的系统有任意一次更新（哪怕是同一个小版本的安全更新），build 号就会变，`strcmp` 立刻失败 → 驱动静默不加载。

这与 README 自述的「严格检查系统 build，其他 Sequoia 小版本不属于验证范围」一致。

**关于 `release.json` 的 `required_build: "24H23"`**：它**不是运行时闸门**。它在源码里只出现于 `scripts/sequoia/build.py:143`：

```python
host_build = subprocess.check_output(["sw_vers", "-buildVersion"], text=True).strip()
if host_build == config["required_build"]:
    run(["/usr/bin/kmutil", "libraries", "-p", str(bundle), "--undef-symbols"], "libraries")
```

即：**只决定构建时要不要跑一次 `kmutil libraries` 符号校验**，与 kext 运行时能否加载无关。（`build.py:60` 把它注入为 `SequoiaRequiredBuild` 宏，但该宏在源码中从未被引用，属死代码。）

### 7.5 对你的判断流程

先确认系统版本，再决定是否投入：

```bash
sw_vers                                   # 看 macOS 版本号
uname -v                                  # 应为 Darwin Kernel Version 24.6.0: ... xnu-11417.140.69.712.69~1
sysctl -n kern.version                     # 与 kext 内核里读到的 `version` 是同一个串
```

对照 `SequoiaTrialPolicy.h:8-10` 的目标串：

| `uname -v` 结果 | 判断 |
|---|---|
| xnu 版本与 `11417.140.69.712.69` **完全一致** | ✅ 可以走原生路线 |
| `24.6.0` 但 xnu build 不同 | ❌ 白名单拒绝，需改 `kKernelVersion` 常量重编译 |
| 非 `24.6.0`（如 `24.6.x` 其他、`24.4`、`24.7`） | ❌ 拒绝 |

**本机实测（2026-10-20，`MacBookPro15,4` / Hackintosh，Intel 8265）**：

| 项 | 实测值 | gate 要求 | 结果 |
|---|---|---|---|
| `sw_vers` | 15.8.1 (24H32) | — | — |
| `uname -v` / `kern.version` | `Darwin Kernel Version 24.6.0: Sun Aug 23 20:30:56 PDT 2026; root:xnu-11417.140.69.712.69~1/RELEASE_X86_64` | 逐字节相同 | ✅ **通过** |
| `version_major` / `version_minor` | 24 / 6 | 24 / 6 | ✅ |
| PCI | `pci8086,24fd` | vendor `0x8086` | ✅ |
| device | `0x24FD` | 白名单 | ❌ **上游拒绝 → 本地已改** |

所以就**系统版本**而言这台机器是完全命中的（这是 8265 用户里极少见的情况）；唯一的阻塞就是 7.3 的几处 family 硬门。

### 7.6 若不匹配，三条路

1. **改 `SequoiaTrialPolicy.h` + `release.json` 重编译**——本仓库已完成（见第 9 节）。
   ⚠️ 但这**只是放开闸门，不保证 8265 真能工作**。该 fork 只在 AX210 + 特定 build 上验证过；8265 属于「代码路径覆盖但未实测」。
2. **退回 Sonoma + upstream AirportItlwm**——8265 是老卡，Sonoma 支持成熟稳定（upstream `IOPCMatch` 同样含 `0x24fd8086`）。
3. **OCLP root patch**——通用解，但牺牲 SIP。

### 7.7 一个额外提醒

8265 是 2015 年的老卡，macOS 15 已不再原生支持。Hackintosh 用它属于常规操作，但如果这是你主力笔记本，建议评估：**万一驱动在睡眠唤醒后失效，你是否有 USB 恢复手段**。RELEASE_NOTES 明确把「睡眠唤醒」列为未验收项。

### 7.8 实机探针结论：固件对话与主路径已打通（2026-10-20，见第 10 节）

用 fork 自带的 `AirportItlwmSequoiaNvmTrial` / `AirportItlwmSequoiaFirmwareTrial` 诊断 personality 在 8265 上实测，得到：

| 结论 | 证据 |
|---|---|
| **命令传输层完全可用** | 固件接受 `IWM_NVM_ACCESS_CMD`(0x88)、`AckResult=0`、`AckFlags=0`、状态 0、offset 回显正确 |
| **响应结构布局没变** | 8 字节头 + N 字节数据 = payload 72（8+64），与 7000 家族一致 |
| **固件代际确认** | `SequoiaTrialFirmware = "36.ca7b901d.0"` → **固件 36**（7000 家族是固件 17） |
| **段 id 空间不同（唯一卡点）** | 请求段 → 应答 id：`1→0x5A01` `3→0x5A03` `4→0x5A04` `5→0x5A05` `10→0x5A0A` `12→0x5A0C` → **固定基址 `0x5A00 + 段号`**，6/6 命中 |
| **8000 的 NVM 内容可解析** | 读到 5472 字节、6 个段（掩码 5178 = 段 1/3/4/5/10/12），**解析出 46 个信道** |

| **段 id 重标只是应答现象** | 请求必须用**原始 id**；直接请求 `0x5A00+N` 会命中另一个内容为空的段（`bb03cad` 纠正） |
| **固件对话全部通过** | NVM / 校准 / 常规固件 / 天线 / **PHY DB 回放** / 蓝牙 —— 结果全 0（`490be26` 之后） |
| **主路径完整启动** | `ItlwmNativeStage = "17_complete"`，系统出现 Wi-Fi 硬件端口（`en2` / 8265 的 MAC），BSD 交给 skywalk |
| **native scan 请求已能构造并提交** | 第 26 轮 `FailStage = 0`（构造成功）；提交阶段 ENXIO 的具体原因已埋阶段码待测 |

**含义**：NVM 层只需一张段 id 偏移表（已实现为 `itlwm/hal_iwm/nvm_sequoia.inc` 的 `iwm_nvm_answered_section()`）。**难度评估要大幅下调** —— **station ABI 也是现成的**（固件 36 完整接受固件 17 的对话格式 + PHY DB 回放），剩下的只有 **native 扫描是否要改走 UMAC** 这一个问题。

**★ 完整逐轮记录、能力位实测值、47 个 commit 的作用、关键知识清单 → [`sequoia-8265-port.md`](sequoia-8265-port.md)。★★★**

---

## 8. 初步结论与建议（实机验证前，已被第 10 节取代）

> ⚠️ 本节是**实机验证之前**的判断，其中"8265 属代码路径覆盖、未实测硬件"这一条**已被证伪** —— 实机证明 8265 所在家族被结构性排除（见 7.3、第 10 节）。保留原文以记录推理演进。

- **短期**：留在 Sonoma，或切到 `itlwm.kext + HeliPort`。方案 A 的 root patch 反复被系统更新打断，长期维护成本高。
- **针对 8265 / 8275**：先跑 `uname -v`，与 `SequoiaTrialPolicy.h:8-10` 的目标串逐字节比对（见 7.4）。
  - 匹配 → 可试 fork，但须知 8265 属**代码路径覆盖、未实测**硬件；且上游白名单**不放行** `0x24FD`，必须先改代码（见第 9 节）　← ❌ **此判断错误**
  - 不匹配 → 优先退回 **Sonoma + upstream**，8265 在上游支持成熟，风险最低
- **装任何第三方 kext 前**：备份 EFI，确认有 USB 恢复路径，不要在运行中加载/卸载（RELEASE_NOTES 明确警告）。
- **8265 的完整移植记录**（逐级探针结果、device family 分水岭、UMAC 结论、commit 序列）：[`sequoia-8265-port.md`](sequoia-8265-port.md)。
- **PR #1054** 仍 open，未合并，暂不作为依赖。
- **前置条件**：任何原生方案都**必须有 Sequoia 实机**。KDK / ramdisk 符号只能做静态分析，替代不了切网、唤醒、换钥这些真实验收。

---

## 9. 本地适配执行记录（2026-10-20）

工作副本：本仓库（分支 `feature/intel-8265-sequoia`），基于 fork 的 `f31a3ad`。

### 9.1 改动（3 个文件）

| 文件 | 改动 |
|---|---|
| `include/HAL/SequoiaTrialPolicy.h:17` | `(device == 0x3166 \|\| device == 0x2725)` → 追加 `\|\| device == 0x24FD`，并加注释说明这是未经上游验证的 8265 试开 |
| `scripts/sequoia/release.json:14` | `"pci_match": "0x31668086 0x27258086"` → 追加 `" 0x24fd8086"`（`build.py:65` 会用它覆盖 Info.plist 的 `IOPCIMatch`） |
| `tests/sequoia_trial_policy_test.cpp` | 增加 `assert(allowed(..., 0x24fd))` 与反向 `assert(!allowed(..., 0x24f3))` |

`required_build` 保持 `"24H23"` 未动（它只影响构建时是否跑 `kmutil libraries`，不是运行时闸门）。

### 9.2 构建

```sh
git clone https://github.com/acidanthera/MacKernelSDK.git MacKernelSDK
git -C MacKernelSDK checkout 05094e5e88cec7caedbfb35e8449ed0db94bf95b
git -C MacKernelSDK apply ../scripts/sequoia/MacKernelSDK-pexpert.patch
/usr/bin/python3 scripts/sequoia/build.py
```

SDK 完整性由 `sdk.lock.json` 把关：1227 个文件的 sha256 全部匹配（`build.py:100`）。

### 9.3 产物与验证

```
build/Sequoia-0.1.0-beta.1-RELEASE/AirportItlwmSequoiaNative.kext
  UUID          1F54ADB8-6043-3EC8-ABA4-A2DFA5B17E42
  binary SHA256 c1b563be3155deb96e31679bfe8579548c9aa30b519dbdefaaa57c9595f4cc04
  Info.plist    1e182ecb13663d8ddc408d8639715833d36925206aca124f1f9a4f487a1cb5b1
  compiled      73 个编译单元
  status        built-not-boot-validated
```

| 验证项 | 结果 |
|---|---|
| `kmutil libraries -p <kext> --undef-symbols` | exit 0（980 个未定义符号中 978 个解析到 `BootKernelExtensions.kc`） |
| 旧 `postMessage` 残留 | ✅ 已消除。剩下的全是 Sequoia 新签名：`IO80211Controller::postMessage(IO80211SkywalkInterface*, unsigned int, void*, unsigned long, bool)`、`IO80211InfraInterface::postMessage(unsigned int, void*, unsigned long, bool)`、`IO80211SkywalkInterface::postMessageIOUC(char const*, ...)` |
| 生成的 `Info.plist` | ✅ `IOPCIMatch = "0x31668086 0x27258086 0x24fd8086"`；两个 personality 的 `CFBundleIdentifier` 均为 `com.zxystd.AirportItlwm.SequoiaNative` |
| `tests/sequoia_trial_policy_test.cpp` | ✅ 单独编译运行通过 |
| `OSBundleLibraries` 可满足性 | ✅ 系统侧 IO80211Family `1200.13.1` ≥ 1.5.0；IOSkywalkFamily `1.0` ≥ 1.0；corecapture `1.0.4` ≥ 1.0.0 |
| `PE_parse_boot_arg_str` 导出 | ✅ `/System/Library/Kernels/kernel` 中 `T _PE_parse_boot_arg_str`（SDK 那个 pexpert patch 补的是缺失声明，符号真实存在） |

**两个未解析符号的说明**：`_RESERVEDIONetworkController6Ev` / `7Ev` 未出现在 `kmutil` 的解析列表里。原因是钉住的 MacKernelSDK 私有头 `IONetworkController.h` 比系统实际的新 —— 现代公开 SDK 里 vtable slot 6/7 已被改成实名方法（`allocatePacketNoWait` / `setHardwareAssists`），而旧私有头把它们写成 `OSMetaClassDeclareReservedUnused`，于是派生类 vtable 引用了 `_RESERVEDIONetworkController6/7`。**vtable 槽位序号本身是对齐的**，且这是上游构建同样具有的性质 —— 作者用同一份 SDK 在同一 OS 小版本（Darwin 24.6.0 / IONetworkingFamily 3.4）上启动成功并跑通原生 Wi-Fi，证明 kxld 能容忍这两个引用。

### 9.4 已完成的安装与实机验证（2026-10-20）

**安装**：EFI 全量备份到 `/Volumes/USB/EFI-backup-20261020-142839/`（190 个文件逐个比对）；config.plist 改 3 处（停用旧 `itlwm.kext`、新增 `AirportItlwmSequoiaNative.kext` 条目、boot-arg 加 `itlwm_native=1 itlwm_bsd_owner=skywalk`）；kext 已 ad-hoc 签名（`codesign --verify` exit 0）。

**实机启动结果：kext 加载成功，但 Wi-Fi 不工作。**

```
kmutil showloaded | grep SequoiaNative
  92  0 0xffffff80042d4000 0xf8d000 0xf8d000  com.zxystd.AirportItlwm.SequoiaNative (0.1.0b1)
      1F54ADB8-6043-3EC8-ABA4-A2DFA5B17E42
```

内核日志（`/usr/bin/log show`，2026-10-20 14:42:56）：

```
14:42:56.438 (IO80211Family)  AirportItlwm::init IO80211_kexts-1485.17   ← 控制器对象创建 ✅
14:42:56.439 (corecapture)    Created CCLogPipe (Owner: com.zxystd.AirportItlwm) ← initCCLogs ✅
14:42:56.441 (IO80211Family)  controller common start                    ← super::start() 进入
14:42:56.447 (IO80211Family)  IO80211Controller::start, disabling IOCTL logic
14:42:56.449 (IO80211Family)  {WiFiTimeSync} controller common start done ← super::start() 成功 ✅
14:42:57.237 (IO80211Family)  IO80211Controller::stop[845] ::stop(...)    ← 788 ms 后失败回滚 ❌
```

- boot-args 正确：`darkwake=0 ps2kbdonly=1 -wegnoegpu itlwm_native=1 itlwm_bsd_owner=skywalk`
- 失败点在 `startNative()` 的 **stage 40 → 170**（super::start 之后）
- `itlwm-native: stage …` 一条都没有 —— 作者在 `native_lifecycle_sequoia.inc` 里写明了原因：**"no unified-log capture for kext IOLog"**，kext 的 IOLog 进不了 unified log。真正的诊断在 provider 属性（`ItlwmNativeStage` / `ItlwmNativeLastStage` / `ItlwmNativeHal` / `ItlwmNativeNote`）和 NVRAM 面包屑（`itlwm-stage`）里。
- 回滚已完成：config.plist 恢复原版，旧 `itlwm.kext` 2.3.0 + HeliPort 恢复正常。

**根因判定**：不是"未实测"，而是 7.4 的 device family 硬门 —— 8265 是 8000 家族，IWM 原生 scan/join 只接受 7000 家族。**即使把 attach 阶段修好，扫描与关联仍会被 `EOPNOTSUPP` / `ENXIO` 拒绝。**

### 9.5 探针阶梯的进展

fork 自带的两个诊断 personality 被编进 kext，构成 4 级探针阶梯（`nvm` → `firmware` → `main`），每轮给出确定结论。
**★ 关键里程碑：第 3 级（主路径）在 8265 上完整启动成功（`ItlwmNativeStage = "17_complete"`），系统出现 Wi-Fi 硬件端口。★★**

逐轮数据、能力位实测值、47 个 commit 的作用、关键知识清单见 **[`sequoia-8265-port.md`](sequoia-8265-port.md)**。

### 9.6 尚未验证的部分

- **native 扫描提交给固件后的最终结果**：请求已能构造并提交，提交阶段的 ENXIO 具体原因（4 个候选已埋阶段码）待下一轮确认。
- **native 扫描是否必须改走 UMAC**：upstream 用同一个能力位选路（报 `UMAC_SCAN` 就用 UMAC 请求），而 8265 恰好报了该位 → 推断必须 UMAC；fork 只实现 LMAC。**这是最后一步。**
- **关联 / AUTH**：`native_join_*` 路径（STA_TYPE 分叉已验证可用）。
- fork 自述未验收项仍然成立：睡眠唤醒、AP 失联恢复、持续双向流量、真实 PTK/GTK 换钥、长期稳定性。

## 10. 结论与建议（实机验证后更新）

| 你的网卡 | 能否用这个 fork 跑 macOS 15 原生 Wi-Fi | 做法 |
|---|---|---|
| **Intel 8265 / 8275（`0x24FD`，8000 家族）** | ✅ **可用**（扫描 → 关联 → 认证 → 拿到 IP → 上网，全链路实测通过；Wi-Fi 列表完整需加 `itlwm_assoc_scan=1`） | 完整移植记录见 **[`sequoia-8265-port.md`](sequoia-8265-port.md)** |
| **Intel 7265 / 3165 / 3168 / 3160（7000 家族）** | ✅ **最有希望** | 把 `SequoiaTrialPolicy.h` 和 `release.json` 的白名单扩到 `0x3165` / `0x7265` / `0x3168` / `0x3160` |
| **Intel AX210 / AX211（`0x2725` / `0x3166`）** | ✅ **作者实测，风险最低** | 只扩 `pci_match` 即可（本来就在白名单里） |
| 任意卡 + **itlwm.kext 2.3.0 + HeliPort** | ✅ **确定可用**（现状） | 无原生菜单、无隔空投屏 |

其它结论：

- **macOS 版本这一关你这台机器完全通过**（内核串逐字节命中 `xnu-11417.140.69.712.69`），这是绝大多数用户拿不到的运气。但**下次安全更新后 build 号会变 → 驱动静默失效**。
- **留在 macOS 14 + 上游 AirportItlwm**：8265 原生支持，最省心。
- **OCLP root patch**：通用但牺牲 SIP，且每次系统更新要重来。
- **PR #1054** 仍 open，未合并，不能依赖。
- 装任何第三方 kext 前：备份 EFI，确认有 USB 恢复路径，不要在运行中加载/卸载。

---

## 11. 8000 家族探针实验（2026-10-20）

用 fork 自带的两个诊断 personality 做阶梯实验，把 8265 的实际行为逐层测出来。

**完整记录见 [`sequoia-8265-port.md`](sequoia-8265-port.md)**（含逐轮数据、能力位实测值、47 个 commit 的作用、关键知识清单）。此处只列结论。

| 级别 | 结论 |
|---|---|
| 1 · NVM | 段 id 被固件**重标**成 `0x5A00+N`；命令通道与响应布局均正常。**请求必须用原始 id** |
| 2 · 固件对话 | NVM / 校准 / 常规固件 / 天线 / **PHY DB 回放** / 蓝牙 —— **全部通过**，固件 36 完整接受固件 17 的对话格式 |
| 2 · 扫描 | LMAC / upstream-LMAC / upstream-UMAC **三种请求都复位** ⇒ 复位是 trial **缺 STA**，不是格式问题 |
| 3 · 主路径 | ★ **`17_complete`**，系统出现 Wi-Fi 硬件端口（`en2` / 8265 的 MAC），BSD 层交给 skywalk |
| 3 · native scan | LMAC 请求 → **固件复位**（`SubmitReject = 4`）→ 改发 UMAC 请求 → ✅ **22 个 beacon 全部交付** |
| 3 · native join | ✅ **关联 + 认证成功**，`en2` 拿到 IP，默认路由走 en2，ping / DNS 通 |

**★ 结论：fork 的原生路径在 8265 上完全可用。★★**
**★ 关键发现：native scan 必须走 UMAC（`IWM_SCAN_REQ_UMAC`, 0x10d）。fork 自建的 LMAC 请求用的是 7000 家族的 scan offload 命令（0x51），8000 家族固件 36 收到就复位。★★**

### 11.1 途中撞到的 4 个「能力位 ≠ 格式判据」的坑

作者用"固件报了这个能力位"推断"必须用某种报文格式"，在 8265 上误伤了 4 次：

| 能力位 | 作者的推断 | 实际 |
|---|---|---|
| `IWM_UCODE_TLV_CAPA_UMAC_SCAN` | 报了 ⇒ 不能用 LMAC 请求 | 报的是"支持 UMAC"，不是"只能用 UMAC" |
| `IWM_UCODE_TLV_API_STA_TYPE` | 报了 ⇒ ADD_STA 格式不同 | 是"支持 station type 分配"，驱动据此选命令版本，policy 却写死 v7 |
| `fw->paging_mem_size` | 带 paging 镜像 ⇒ 不支持 | upstream 的成熟代码一直在处理 paging |
| `IWM_UCODE_TLV_CAPA_DQA_SUPPORT` | DQA 队列 | ✅ 这条是对的 |

### 11.2 两个自己踩的坑（记录下来给后来人）

- **哨兵值宽度**：`iwm_channel_id_to_papd()` 无分组时返回 **`0xff`**（不是 `0xffff`）。照抄时写成 `== 0xffff` → 过滤永远不生效 → 信道 144 一直留在请求里 → 每次扫描 `EINVAL`。**判哨兵要用语义比较（`>= 组数`）。**
- **硬编码 band 表**：`iwm_is_valid_channel` 不认 8265 特有的 68–96 信道。**改成从 NVM 的 regulatory 段取。**

### 11.3 诊断通道（kernel 驱动调试的关键）

`IOLog` / `kprintf` **进不了 unified log**（作者在源码里写明 *"no unified-log capture for kext IOLog"*）。可用通道：

- `IOPCIEDeviceWrapper` provider 的 IOKit 属性（`ItlwmNativeStage` / `ItlwmNativeLastStage` / `ItlwmNativeHal` / …）
- NVRAM 面包屑 `itlwm-stage`
- 本项目新增的 `SequoiaFirmware*` 属性

**★ 必须给每个失败分支编码（本项目用 `nativeScanFailStage` 1–6），只留一个错误码是查不动的。★★★**

## 参考

参考源码目录：
- upstream `OpenIntelWireless/itlwm` @ `53c51c2`（v2.4.0），diff 基线
- fork `zhangcheng142/AirportItlwm-Sequoia` @ `f31a3ad`（0.1.0-beta.1），本仓库的起点
- 本仓库 — 本地适配副本（分支 `feature/intel-8265-sequoia`），见第 9 节

外部链接：
- <https://github.com/OpenIntelWireless/itlwm/issues/1018>
- <https://github.com/OpenIntelWireless/itlwm/issues/1009>
- <https://github.com/OpenIntelWireless/itlwm/issues/1029>（含 Sequoia 符号表）
- <https://github.com/OpenIntelWireless/itlwm/issues/983>
- <https://github.com/OpenIntelWireless/itlwm/pull/1054>
- <https://github.com/zhangcheng142/AirportItlwm-Sequoia>
- <https://github.com/5T33Z0/OCLP4Hackintosh/blob/main/plist/AirportItlwm_Sequoia.plist>
