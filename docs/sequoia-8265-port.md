# Intel 8265 在 macOS 15 Sequoia 上的原生 Wi-Fi 移植记录

**目标**：让 `zhangcheng142/AirportItlwm-Sequoia`（fork `f31a3ad`，作者实测机型是 AX210）在 **Intel 8265 / 8275（PCI `8086:24FD`，IWM 8000 家族，固件 36）** 上提供原生 Wi-Fi（系统设置里的 Wi-Fi 列表，不是 HeliPort）。

**测试机**：`MacBookPro15,4`（Hackintosh，Intel i7-8550U，Dell 主板），macOS **15.8.1 (24H32)**，Darwin **24.6.0** `xnu-11417.140.69.712.69`，固件版本实测 **`36.ca7b901d.0`**。

**工作副本**：本仓库（分支 `feature/intel-8265-sequoia`，基于 fork 的 `f31a3ad`）。

**当前状态（2026-10-20）**：★ **完全跑通** ★ —— 扫描 → 关联 → 认证 → 拿到 IP → 上网，全部走 fork 的原生 Wi-Fi 路径。

| 阶段 | 状态 | 实测证据 |
|---|---|---|
| 驱动加载 | ✅ | `ItlwmNativeStage = "17_complete"` |
| NVM / 固件对话 / PHY DB 回放 | ✅ | 全部结果 0 |
| Wi-Fi 硬件端口 | ✅ | `Hardware Port: Wi-Fi` → `en2`（8265 的 MAC `xx:xx:xx:xx:xx:xx`） |
| **扫描** | ✅ | 22 个 beacon 全部解码 → 排队 → WCL 交付；`LastNotification = 0x0f`（UMAC 终态通知） |
| **关联 / 认证** | ✅ | `ItlwmNativeJoinConnected = 1`，`AssocStatus = 0`，`AuthStatus = 0` |
| **上网** | ✅ | `en2` 拿到 `192.168.3.x`，默认路由走 en2，ping / DNS 均通 |
| **Wi-Fi 列表完整** | ✅ | 104 次请求 → 104 次执行 → 104 次终态 → 104 次投递，**零拒绝**；`CacheNodesAtScanEnd = 86`（86 个 AP） |
| **发布版（无诊断）** | ✅ | 同样的扫描/关联/上网全部正常；符号表对比确认与开发版**功能差异为零** |

**尚未验证**：睡眠唤醒、AP 失联恢复、持续双向流量、真实 PTK/GTK 换钥、长期稳定性（fork 自述的未验收项）。

**★ 提速项（进行中）**：扫描后不停 radio，省掉关联前那次固件重启。开启耗时实测 **5.6 s → 目标 < 3.5 s**。**
**★ 出问题时的退路：boot-arg `itlwm_scan_keep_radio=0` 恢复「扫描后复位」的慢路径（无需重编译）。★★**

---

## 一、起点：三个被纠正的认知错误

| 初始认知 | 实际 | 证据 |
|---|---|---|
| 8265 的 PCI ID 是 `0x2725` | **`0x24FD`**（`0x2725` 是 AX200/AX210） | `ioreg` → `pci8086,24fd`，`model = <"Intel Wireless 8265">` |
| 8265 走 `ItlIwx`（AX HAL） | 走 **`ItlIwm`**（iwl-legacy HAL） | `itlwm/hal_iwm/mac80211.cpp:4518` → `PCI_PRODUCT_INTEL_WL_8265_1 0x24fd` |
| 只是"未实测" | 被 **device family 结构性排除** | 见第二节 |

---

## 二、device family：真正的分水岭

`ItlIwm` 把 7xxx/8xxx/9xxx 分三档（`hal_iwm/mac80211.cpp:4790-4900`）：

| family | 设备 | PCI ID | 固件 |
|---|---|---|---|
| **7000** | 3160 / 3165 / 3168 / 7260 / 7265 | `0x3160` `0x3165` `0x3168` `0x7260` `0x7265` | **17** |
| **8000** | 8260 / **8265** | `0x24F8` / **`0x24FD`** | **36** |
| 9000 | 9260 / 9560 / 9462 / 9461 | … | 46 |

**fork 的原生 scan/join 路径硬性要求 7000 家族**：

| 位置 | 判据 | 不满足时 | 本项目处置 |
|---|---|---|---|
| `hal_iwm/native_scan_sequoia.inc:18` | `sc_device_family == 9000` 才拒 | 拒绝 | 已放行 8000 |
| `hal_iwm/native_join_start_sequoia.inc:19` | `sc_device_family == 9000` 才拒 | `ENXIO` | 已放行 8000（`b1cd697`） |
| `hal_iwm/scan_trial_sequoia.inc:84` | 同上 | `ENOTSUP` | 已放行 8000（`b1cd697`） |

**★ 重要区别**：9000 家族（MQ RX）**必须**继续拒绝 —— 它的 station ABI 与 LMAC 不同（见第六节）。

---

## 三、探针阶梯（4 级）

作者在 fork 里留了两个诊断 personality，本项目把它们编进 kext 并扩展：

| 级别 | personality | 做什么 | 对应 boot-arg |
|---|---|---|---|
| 1 | `AirportItlwmSequoiaNvmTrial` | 硬件启动 + INIT 固件 + NVM 读取 | `itlwm-sequoia-nvm=1` |
| 2 | `AirportItlwmSequoiaFirmwareTrial` | 完整固件对话（校准 → 常规固件 → 天线/PHY DB/蓝牙 → 被动扫描） | `itlwm-sequoia-firmware=1` |
| 3 | 主路径 | `AirportItlwm` 真实 start（net80211 attach + WCL 接管） | 无诊断参数 |
| — | 对照模式 | 跑**驱动自己**的扫描请求（LMAC 或 UMAC） | `itlwm_scan_upstream=1` / `itlwm_scan_umac=1` |

工具：

```sh
tools/install-probe-kext.sh [nvm|firmware|main]   # 装 kext + 写 boot-arg
tools/collect-probe-diagnostics.sh                 # 重启后采集全部诊断属性
```

### 3.1 诊断通道（**这是本项目最关键的工程经验**）

**kext 的 `IOLog` / `kprintf` 进不了 unified log**（作者在源码里写明 *"no unified-log capture for kext IOLog"*）。真正能用的通道：

- **`IOPCIEDeviceWrapper` provider 的 IOKit 属性**：
  `ItlwmNativeStage` / `ItlwmNativeLastStage` / `ItlwmNativeHal` / `ItlwmNativeNote` / `ItlwmNativeEvents` …
- **NVRAM 面包屑** `itlwm-stage`
- 本项目新增的 trial 专用属性 `SequoiaFirmware*`

```bash
ioreg -r -c IOPCIEDeviceWrapper -l | grep -oE '"ItlwmNative[A-Za-z]*" = [^,]*'
```

**★ 教训：kernel 驱动调试必须自己埋诊断通道，且要把"失败发生在哪一步"编码成可读的数值（阶段码），而不是只留一个错误码。★★★**
**★ 本项目加的阶段码体系（`nativeScanFailStage`）：1=数量超限 / 2=信道校验失败 / 3=DFS或被动冲突 / 4=请求尺寸检查失败 / 5=分配失败 / 6=请求构造失败 / 0=成功。★★★**
**★ 另一个教训：诊断数组会**残留**历史值，必须同时发布一个"本次调用实际长度"，否则会被上一轮的值误导（本项目为此专门加了 `UsableCount`）。★★★**

---

## 四、逐级定位的结果

### 4.1 第 1 级：NVM —— 段 id 空间不同

| 项 | 值 |
|---|---|
| 命令通道 | ✅ `IWM_NVM_ACCESS_CMD`(0x88) 被固件接受，`AckResult=0`，状态 0，offset 回显正确 |
| 响应结构 | ✅ 8 字节头 + 数据，布局与 7000 家族一致 |
| 段 id | ❌ 请求段 N → 固件回 **`0x5A00+N`** |
| 段内容 | ❌ 直接请求 `0x5A00+N` 会拿到**另一个段**（内容为空） |

**⇒ 段 id 重标只是"应答"现象，请求必须用原始 id。**

（曾走过弯路：把请求也改成 `+0x5A00` → regulatory 段全零、46 个信道变 0。已由 `bb03cad` 纠正。）

**固件能力位实测**（`sc_enabled_capa` / `sc_ucode_api`）：

```
sc_enabled_capa  = 3f 13 df 9d 06 02 00 00 cf 7e 13 01 00 00 00 00
sc_ucode_api     = fb ff 9a dd cf 01 00 00 00 00 00 00 00 00 00
```

| 位 | 含义 | 8265 实测 | 影响 |
|---|---|---|---|
| `IWM_UCODE_TLV_CAPA_UMAC_SCAN` (2) | **支持 UMAC 扫描** | ✅ 置位 | upstream 据此选 **UMAC** 扫描请求；fork 只实现 LMAC → 曾被 `EOPNOTSUPP` 挡死 |
| `IWM_UCODE_TLV_CAPA_DQA_SUPPORT` (12) | 动态队列 | ✅ 置位 | 命令队列用 DQA |
| `IWM_UCODE_TLV_API_STA_TYPE` (30) | **支持 station type 分配** | ✅ 置位 | 决定 `ADD_STA` 用 `iwm_add_sta_cmd`（大）还是 `iwm_add_sta_cmd_v7`（小） |
| `IWM_UCODE_TLV_API_ADAPTIVE_DWELL` (32) | 自适应 dwell | ✅ 置位 | UMAC 请求用 v7 基准 |
| `IWM_UCODE_TLV_API_ADAPTIVE_DWELL_V2` (42) | | ❌ 未置位 | UMAC 请求不用 v8 基准 |
| `IWM_UCODE_TLV_API_SCAN_EXT_CHAN_VER` (58) | | ❌ 未置位 | UMAC 请求用 tail v1 |

**★ 8265 的能力位组合与 upstream 在这张卡上用的完全一致（v7 + tail v1）→ upstream 的 UMAC 路径在 8265 上是被验证过的（itlwm.kext 2.3.0 能上网）。★★★**

### 4.2 第 2 级：固件对话 —— 全部通过

```
NvmResult = 0   CalibrationResult = 0   RegularResult = 0   ConfigurationResult = 0
  AntennaResult = 0   ReplayResult = 0（PHY DB 回放）   PhyConfigResult = 0   BtResult = 0
```

**⇒ 固件 36 完整接受固件 17 的对话流程 + PHY DB 回放格式。**

途中被作者的保守检查挡了 3 次，全部是"能力位被当成了格式判据"（见第七节）。

### 4.3 第 2 级后半：扫描 —— trial 环境复位（**不是**格式问题）

| 请求来源 | 结果 |
|---|---|
| fork 的 `buildPassiveRequest`（LMAC） | 固件复位 |
| upstream 的 `iwm_lmac_scan`（LMAC） | 固件复位 |
| upstream 的 `iwm_umac_scan`（UMAC，命令 0x10D） | 固件复位 |

**★ 三种请求都一样复位 ⇒ 复位原因是 trial 环境缺前置状态**（trial 不 attach net80211，**主 STA 从未进过固件的 station table**），**不是请求格式问题**。

**★ 推论：LMAC 扫描请求需要 STA 已在固件里；主路径满足这个前提，所以必须在主路径上验证。★★★**

### 4.4 第 3 级：主路径 —— ★ 完整启动成功 ★

```
ItlwmNativeStage = "17_complete"          ← 启动完成
ItlwmNativeLastStage = "16a_bsd_registered"
ItlwmNativeInterfaceServicePublished = Yes
ItlwmNativeBSDOwner = "skywalk"
ItlwmNativeEvents = 542
```

**系统里出现 Wi-Fi 硬件端口**：

```
Hardware Port: Wi-Fi
Device: en2
Ethernet Address: xx:xx:xx:xx:xx:xx     ← 8265 的 MAC
en2: UP, RUNNING, MULTICAST
```

### 4.5 第 3 级后半：native scan 的 EINVAL 链（第 22–26 轮）

去掉 `UMAC_SCAN` 自拒后，native scan 往下走并报 `EINVAL(22)`。用阶段码逐条定位：

| 轮次 | `FailStage` | 结论 |
|---|---|---|
| 22 | 4 | `passiveRequestSize` 返回 0 |
| 23 | 4 | 11 个判据里 `count/capacity/MaxChannels`、`channel>196`、`txAnt&~7`、`bytes<=4088` 全过；剩 `validUnicast(source)` / `txAnt` / `rxAnt` 未验证 |
| 24 | 4 | `TxAnt=3` `RxAnt=3` `SourceValid=1` **全合法** ⇒ 排除 |
| 25 | 4 | **抓到一个真 bug**：`iwm_channel_id_to_papd` 返回 **`0xff`**（不是 `0xffff`），判据写错 ⇒ 144 从未被滤掉 |
| 26 | 0 | ✓ 144 滤掉后请求构造成功；`NativeResult=6`(ENXIO) 移到**提交**阶段 |

**★ `passiveRequestSize` 里的 `channel == 144` 是 7000 家族特有的排除**（注释：*"the 7000-series IWM PHY map excludes it there"*）。
**★ 但 8265 的 NVM inventory 里**有** 144（`iwm_nvm_channels_8000` 列了它），regulatory 段也给了它有效 flag → 请求里带了 144 → 整个请求被拒。★★★**

**★ 8265 还有 7000 家族没有的 68–96 信道** —— 驱动的硬编码 band 表（`iwm_is_valid_channel`）不认它们，导致它们的 PAPD/TXP 分组未知。**已改为数据驱动的 `iwm_channel_is_valid(sc, ch)`（查 NVM 信道表）**，而 144 的排除改成按家族限定（`PassivePlan::skipChannel144`，仅 7000 为真）。

### 4.6 提交阶段

请求构造成功后是 `ENXIO(6)`。`iwm_send_cmd` 的 ENXIO 有 4 个来源，已全部埋上阶段码（`nativeNvmDiagnostics.commandReject`）：

| 值 | 位置 | 含义 |
|---|---|---|
| 1 | `command_sequoia.inc:7` | workloop / gate / 命令缺失 |
| 2 | `command_sequoia.inc:44` | `!commandTransportReady` 或 `sc_flags & (SHUTDOWN\|HW_ERR)` |
| 3 | `command_sequoia.inc:155` | 命令槽位发布失败（硬件复位竞态） |
| 4 | `command_sequoia.inc:198` | token 被硬件复位作废（`consume` 失败） |

**★ 注意：该诊断原本只在 `isHardwareTrial()` 时记录，主路径拿不到 —— 已改为无条件记录（`65c28f8`）。这是本项目目前正在等待的下一轮数据。★★★**

---

## 四·七、突破：`IWM_SCAN_OFFLOAD_REQUEST_CMD` 是 7000 家族的东西

### 4.7.1 固件复位了（第 28 轮）

第 27 轮把请求构造阶段打通后，提交阶段给出了决定性的答案：

```
SubmitReject = 4      ← command_sequoia.inc:198
                         // A hardware reset invalidated this host token.
UsableCount  = 46
FailStage    = 0
```

`SubmitReject = 4` 只有一个含义：**固件收到这个命令后把电台复位了**，主机 token 因此作废。

### 4.7.2 原因：命令本身是 7000 家族的

对比 upstream 的两条扫描路径：

| | 命令 id | 请求布局 |
|---|---|---|
| upstream `iwm_lmac_scan` | `IWM_SCAN_OFFLOAD_REQUEST_CMD` (0x51) | `iwm_scan_req_lmac` |
| **fork 的 native scan** | **0x51（同一个）** | **自己写的 `buildNativeScanRequest`** |
| upstream `iwm_umac_scan` | `IWM_SCAN_REQ_UMAC` (0x10d) | `iwm_scan_req_umac` |

**★ 关键推理：upstream 在 8265 上走的是 UMAC（因为固件报了 `UMAC_SCAN` 能力位），所以 upstream 从来没在 8265 上发过 LMAC 请求 —— 那个请求在 8265 上是未被验证的路径。★**
**★ `IWM_SCAN_OFFLOAD_REQUEST_CMD` 配套的 `IWM_SCAN_OFFLOAD_COMPLETE`(0x6d) 是 7000 家族"scan offload"特性的通知，8000 家族（固件 36）不认这个命令 → 复位。★★**

### 4.7.3 改法：native 路径跟随固件的选择

新增 `itlwm/hal_iwm/native_scan_umac_sequoia.inc`：

| 项 | 取值 |
|---|---|
| 选路 | `isset(sc_enabled_capa, IWM_UCODE_TLV_CAPA_UMAC_SCAN)` → UMAC；否则保持 LMAC（7000 家族行为不变） |
| 布局 | base **v7**（固件报 `ADAPTIVE_DWELL`）+ tail **v1**（没报 `SCAN_EXT_CHAN_VER`），与 upstream 在 8265 上的组合一致 |
| 信道数组 | `sc_capa_n_scan_channels` = 52 项，`chanparam->count` = 实际信道数 |
| 命令 id | `iwm_cmd_id(IWM_SCAN_REQ_UMAC, IWM_LONG_GROUP, 0)` = **0x10d**（fork 的传输层本就支持宽头 group 命令） |
| 完成通知 | `IWM_SCAN_COMPLETE_UMAC`(0x0f) / `IWM_SCAN_ITERATION_COMPLETE_UMAC`(0xb5)，native 的 rx 现在认这两个 |
| 中止 | `IWM_SCAN_ABORT_UMAC` → 0x10e |

**★ 顺带发现并修掉一个"影子成员"bug：`nativeScanUmac` 同时声明在接口基类和派生类，派生类遮蔽了基类 → 属性读到的是没人写过的基类成员。★★★**

### 4.7.4 结果（第 29 轮）

```
NativeResult        = 0        ← 扫描成功
SubmitReject        = 0        ← 固件接受了
LastNotification    = 15 (0x0f)   ← IWM_SCAN_COMPLETE_UMAC ✓
LastNotificationBytes = 16       ← UMAC 通知头 16 字节 ✓
Notifications       = 46
CacheFrames         = 22
BeaconsDecoded      = 22
BeaconsQueued       = 22
BeaconsQueuedWithSSID = 18
BeaconsRejected     = 0
BeaconsFiltered     = 0
DeliveryResults     = 22
WclCompletionPosts  = 1
WclResultPosts      = 44
```

**★ 关联侧：`ItlwmNativeJoinConnected = 1`、`AssocStatus = 0`、`AuthStatus = 0`、`BsdState = 4`；`en2` 拿到 `192.168.3.x`，默认路由走 en2，ping 与 DNS 均通。★★**

**★ `PlanRejectCount = 40` / `PlanRejectReasons = 32`：bit 5 = `ScanPlanReason::Associated` —— 关联之后后续的 40 次非关联扫描被正常拒绝。★★**

## 四·八、回归与架构发现：通知归一化是跨代移植的契约

### 4.8.1 症状

把"扫描后不停 radio"（提速）去掉复位之后，开机 1 分钟 Wi-Fi 才起来、**列表空**、`trustd` 长时间高 CPU。

诊断数据：

```
WclCompletionPosts = 0      Terminals = 0
Requests = 146   但   WorkerStarts = 1   PlanRejectCount = 0
BeaconsDecoded = 10   DeliveryResults = 10    ← 扫描其实跑成了，beacon 也交付了
```

### 4.8.2 因果链

1. 扫描完成、beacon 交付，但**完成通知没有投递** → `WclCompletionPosts = 0`
2. `ScanDelivery::busy()` 永为真 → `NativeScan::begin()` 的 `delivery.busy()` 判据拒绝**之后全部 145 次请求**
3. 机场侧列表永远空 → airportd 反复重试
4. 定时器见 `delivery.busy()` 就每 **10 ms** 重排（`native_scan_sequoia.inc:689`），每次唤醒还调 `publishNativeScanDiagnostics()`（约 100 次 `setProperty`）→ **热自旋烧 CPU**
5. 接口反复 enable/disable → **trustd 反复重评估证书信任** → 高 CPU

### 4.8.3 根因：fork 的跨代移植契约是"每个 HAL 归一化自己家族的通知"

**★ AX210 的 HAL 就是这么做的（`itlwm/hal_iwx/ItlIwx.cpp:653`）：★**

```cpp
// AX 的 UMAC 通知 → 归一化成 LMAC 形状再交给上层
nativeScanNotifScratch[2] = bytes[6];              // UMAC status → LMAC status 槽
nativeScanSink.notification(ctx, 0x6d, scratch, 12);   // 0x6d = IWM_SCAN_OFFLOAD_COMPLETE
...
nativeScanNotifScratch[0] = bytes[4];              // 信道数
nativeScanNotifScratch[1] = bytes[5];              // 状态
nativeScanSink.notification(ctx, 0xe7, scratch, 12 + count*8);  // 0xe7 = IWM_SCAN_ITERATION_COMPLETE
```

**★ 而 IWM HAL 没有做归一化 —— 把原始的 `0x0f`/`0xb5` 直接往上送，上层只认 `0xe7`/`0x6d` → 终态分支根本不进。★★**

**★ 两族的结构布局完全相同**（`iwx_umac_scan_complete` == `iwm_umac_scan_complete`，status 都在 offset 6；`iwx_umac_scan_iter_complete_notif` == `iwm_umac_scan_iter_complete_notif`，count@4 / status@5 / results@16），所以归一化就是一次拷贝。

**★ ★★ 之前"能用"是个巧合：扫描后的 `iwm_stop_device()` → `sink.stopped()` → `hardwareStopped()` → `finish(DeviceStopped)` → **意外**产生了一个失败态 completion（状态码 `0xe3ff8100`），delivery 才能排空。★★**
**★ 一旦去掉那次复位，这条"意外的完成"就没了，真正的缺陷暴露出来。★★★**

**★ ★★★ 实测坐实（修复影子成员之后）：★★★**

```
ItlwmNativeScanRawTerminalCode   = 15  (0x0f = IWM_SCAN_COMPLETE_UMAC)          ← 固件原始
ItlwmNativeScanRawIterationCode  = 181 (0xb5 = IWM_SCAN_ITERATION_COMPLETE_UMAC) ← 固件原始
ItlwmNativeScanRawTerminalStatus = 1   (Success)
ItlwmNativeScanLastTerminalCode  = 109 (0x6d)   ← 归一化后，上层看到的
```

**★ ⇒ 固件对 UMAC 扫描请求回的确实是 UMAC 形状的通知，归一化一直在正常工作。★★**
**★ ⇒ 中途一度得出"固件回的是 LMAC 形状、归一化是死代码"的**相反结论** —— 那是影子成员 bug 让诊断值恒为 0 造成的假象（见 §6.8）。★★★**

### 4.8.4 两条必须守住的不变式

| 不变式 | 含义 | 违反后果 |
|---|---|---|
| **completion 必须永远存在** | 扫描无论怎么结束，都要留下一个可投递的 completion | `delivery.busy()` 永为真 → 后续请求全被拒 → 空列表 + 热自旋 |
| **槽位可复用绑定在"radio 被复位"上** | `drained` 只由 `terminal()`（固件自己报告结束）或 `hardwareStopped()`（radio 复位且 RX 排空）置位 | radio 不复位又没终态 → 槽位永远占着 → 同样死锁 |

**★ ⇒ "提速"（不停 radio）不是孤立的改动，它踩到了第二条不变式；必须同时补上归一化（让 `terminal()` 有机会置位 drained）和安全网。★★**

### 4.8.5 修复

| 改动 | 作用 |
|---|---|
| **IWM HAL 归一化通知**（照 AX 的做法） | 终态终于进得了"结束扫描"那段代码 → `terminal()` → `drained` → 槽位可复用 |
| **`Lifecycle::abandon()` / `ScanDelivery::abandon()`** | 安全网：扫描没报告结束就释放槽位并给出 TransportError，**绝不卡死** |
| **`SequoiaNativeScan::workerFinished()`** | 收尾时若还没有已知 outcome → `abandon()` |
| **定时器退避** | delivery 排不空时延迟 10→20→50→100→200 ms，排空后复位；不再 10 ms 热自旋 |
| **`itlwm_scan_keep_radio=0`** boot-arg | 现场退回"扫描后复位"的慢路径，不用重编译 |

### 4.8.7 「Wi-Fi 列表里找不到某个热点」：不是扫描能力问题，是"扫哪些信道"的问题

**★ 现象**：已连接时，系统设置的 Wi-Fi 列表里找不到某个 5 GHz 热点。★★

**★ 实测数据（同一时刻）：★★**

```
CacheNodesAtScanEnd = 86        ← 全信道扫描找到 86 个不同的 AP
BeaconsWithSSID     = 898       ← 累计交付 898 个带 SSID 的 beacon
UsableCount         = 6         ← 但最后一次成功的扫描只覆盖 6 个信道
ChannelList         = 80,84,88,92,96,100
PlanRejectCount     = 218       ← 218 次请求被拒
PlanRejectReasons   = 32        ← bit5 = ScanPlanReason::Associated
```

**★ 因果链：★★**

1. **airportd 每次扫描请求都指定信道列表** —— 有时全量（45 个），有时只要几个（服务信道附近）。
2. **已连接时，fork 默认拒绝所有扫描请求**（`itlwm_assoc_scan` 门没开 → 拒绝原因 `Associated`）→ 列表**冻结**在最后一次成功扫描的结果上。
3. 那次成功扫描恰好只覆盖 6 个信道 → 列表里只有这几个信道上的网络 → **热点在别的信道（UNII-3: 149/153/157/161）就不在列表里**。
4. **窄扫描会替换缓存** —— 真实 Mac 的驱动维护一个跨信道的扫描缓存，列表来自那个缓存；这里的 native 路径每次扫描都提交给 WCL，一次窄扫描就把 45 信道的成果覆盖掉。

**★ 解决：boot-arg `itlwm_assoc_scan=1`** —— 放行已连接时的扫描请求，fork 的健康门（`assocScanHealthy`：>= 3 个不同信道、>= 8 个排队 beacon、且有非 AP 信道的证据）保证不健康的窄扫描被 **withhold**（丢弃结果 + 强制失败 completion → WCL generation 被丢弃 → 缓存保留）。

**★ 这正是 fork r213 踩过的坑的防护：r213 无条件放行关联扫描，AP-only 的扫描替换了缓存里的邻居列表，把菜单清空了。★★★**

### 4.8.6 第一轮代码审查发现的两处缺陷

复查归一化代码时发现两个问题，都会让这一轮的修复失效：

| # | 缺陷 | 后果 | 修法 |
|---|---|---|---|
| **R1** | 信道数上限照抄了 AX 的 **40**（`ItlIwx` 的计划最多 40 个信道） | 8000 家族的 regulatory inventory 有 **46** 个信道 → 迭代通知被**整条拒绝** → 覆盖度永远不达成 → completion 变成失败态 | 上限改用 `ScanPlan::MaxChannels`（52），缓冲按同一上限定尺寸 |
| **R2** | 拷贝每信道结果时**只检查了目标缓冲，没检查源载荷长度** | 固件报一个比实际携带更多信道的 count → **越界读固件载荷** | 两个边界都查；尺寸全部取自通知结构体（`offsetof` / `sizeof`），不用魔数 |

**★ R2 是我引入的内存安全缺陷 —— AX 的对应实现是查了源边界的（`ItlIwx.cpp:668`），我漏了。★★**

**★ R1 的教训：「照抄另一个 HAL 的常量」比「照抄另一个 HAL 的逻辑」更危险 —— 逻辑是相对的，常量是绝对的。★★**
**★ 更普泛的教训：来自固件的计数永远不可信，凡是按它做的内存访问，源和目标两端的边界都要查。★★★**

两处都补了回归测试（46 信道必须被接受；声称 46 却只带 4 个结果的载荷必须被拒），并验证过把上限改回 40 时测试确实会失败。

## 五、为什么是 UMAC（已由实机证实）

**upstream itlwm 的选择逻辑用的是同一个能力位**（`hal_iwm/scan.cpp`）：

- 报 `IWM_UCODE_TLV_CAPA_UMAC_SCAN` → 用 **UMAC** 扫描请求
- 没报 → 用 **LMAC** 扫描请求

⇒ **8265 的固件偏好 UMAC 扫描请求。**

**★ 这条推断已被实机证实（第 4.7 节）★**
- 报能力位 → 发 UMAC 请求 → 固件接受（`SubmitReject = 0`）→ 22 个 beacon → 关联成功 → 上网
- 不发 UMAC（发 fork 自建的 LMAC 请求）→ 固件复位（`SubmitReject = 4`）

**★ 之前 trial 里"UMAC 请求也复位"的那次，原因是 trial 环境缺 STA，与请求格式无关。★★★**

---

## 六、关键知识清单（可复用）

### 6.1 「能力位」不等于「格式判据」

本项目在 fork 里撞了 **4 次**同一个模式的坑 —— 作者用"固件报了这个能力位"推断"必须用某种报文格式"：

| 能力位 | 作者的推断 | 8265 实测 |
|---|---|---|
| `IWM_UCODE_TLV_CAPA_UMAC_SCAN` | 报了 ⇒ 不能用 LMAC 请求 | ❌ 报的是"支持 UMAC"，不是"只能用 UMAC" |
| `IWM_UCODE_TLV_API_STA_TYPE` | 报了 ⇒ ADD_STA 格式不同 | ❌ 是"支持 station type 分配"，驱动据此选命令版本，policy 却写死 v7 |
| `fw->paging_mem_size` | 带 paging 镜像 ⇒ 不支持 | ❌ upstream 的成熟代码一直在处理 paging（itlwm.kext 就在用） |
| `IWM_UCODE_TLV_CAPA_DQA_SUPPORT` | DQA 队列 | ✅ 确实要用 DQA（这条是对的） |

**★ 教训：能力位表"支持什么"，不是"必须用什么"。拿它当格式门禁会误伤。★★★**

### 6.2 device family 是硬边界，不是软偏好

`sc_device_family` 在 IWM HAL 里分三档，各档的固件（NVM 布局、命令 ABI、station 格式）都不同。**跨档移植 = 跨固件代移植。** 7000↔8000 的差异是"固件 17 → 固件 36"。

### 6.3 段 id 重标

8000 固件把 NVM 段 id 的**应答**重标成 `0x5A00 + N`，但**请求**必须用原始 N；直接请求 `0x5A00+N` 会命中固件里另一个真实存在但内容为空的段。

### 6.4 哨兵值的宽度陷阱

```c
uint16_t iwm_channel_id_to_papd(uint16_t ch_id) {
    if (!iwm_is_valid_channel(ch_id)) return 0xff;   // 0x00FF，不是 0xFFFF
}
```

作者自己的比较用 `== 0xff` 所以没踩到；**照抄时写成 `== 0xffff` 就永远不成立**，导致过滤失效、144 一直留在请求里、每次扫描都 `EINVAL`。**本项目在这个坑上卡了三轮。**

**★ 教训：判"哨兵值"要用**语义**比较（`>= 组数`），不要写字面量。★★★**

### 6.5 kernel 驱动的调试通道

`IOLog`/`kprintf` 不进 unified log（macOS 11+）。可用：provider 的 IOKit 属性、NVRAM、corecapture。**必须自己埋，并给每个失败分支编码。**

### 6.6 命令 id 也是家族边界

`IWM_SCAN_OFFLOAD_REQUEST_CMD`(0x51) 配套 `IWM_SCAN_OFFLOAD_COMPLETE`(0x6d)，是 7000 家族"scan offload"特性的命令与通知。8000 家族的固件 36 不认这个命令，收到就复位。

**★ "命令 id 相同"不等于"命令在另一个固件代上还有效"。fork 自建的请求用了 upstream 的 LMAC 命令 id，但那个组合从未在 8265 上被验证过（upstream 在 8265 上走 UMAC）。★★**
**★ 教训：移植时不能只看"命令 id 一样"，要看"这条命令在这一代固件上有没有被用起来过"。★★★**

### 6.7 「每个 HAL 归一化自己家族的通知」是跨代移植的契约

机场侧只认一种形状的扫描通知（LMAC 形状）。**每个 HAL 必须把自家固件的原始通知归一化成那个形状** —— AX HAL 做了（`ItlIwx.cpp:653`），IWM HAL 原本没做。

**★ 教训：移植到一个新 HAL 时，先找"上层期望的形状"和"这个 HAL 实际产生的形状"，中间的转换层是最容易漏掉的一环。★★**

### 6.8 影子成员：一个在接口和一个在实现里同名的成员

C++ 里，派生类声明一个与基类同名的成员，会**遮蔽**基类的那个。两者是**两个不同的对象**：

```cpp
class ItlNativeHal { public: uint32_t nativeScanRawTerminal = 0; };   // 上层读这个
class ItlIwm : public ItlNativeHal { public: uint32_t nativeScanRawTerminal = 0; };  // 实现写这个
```

**★ 上层通过 `ItlNativeHal*` 读到的是基类那份，而实现写的是派生类那份 → 上层永远读到 0。★★**

**★ 本项目被这个咬了两次（`nativeScanUmac`、然后 `nativeScanRaw*`），而且第二次还造成了"固件回的是 LMAC 形状"这个**错误结论** —— 其实是诊断值根本没传上去。★★★**

**★ 修法与守卫：删掉派生类的重复声明，并在类的定义之后做编译期断言：★★**

```cpp
#define ITL_NATIVE_HAL_MEMBER_IS_NOT_SHADOWED(member) \
    static_assert(&ItlIwm::member == &ItlNativeHal::member, \
        #member " is declared in both the interface and this class")
```

**★ `&ItlIwm::member` 解析到实现声明的那份（没有就解析到继承的那份），与 `&ItlNativeHal::member` 比较地址即可判定是否遮蔽。★★**
**★ 教训：跨"接口 + 实现"传诊断值时，实现类里绝不能再声明同名成员；而且要有编译期守卫，因为这个 bug 的症状（值恒为 0）与"硬件没上报"完全一样，极易误判。★★★**

### 6.9 「关掉调试」的开关该放在哪

**★ 把开关做成包住调用点的宏，是最容易出事的位置：★★**

```cpp
#define ITLWM_FOO(x) do { } while (0)      // production：调用整个消失
```

**★ 它同时会丢掉**接收者**（`self->foo()` 变成 `foo()` → 在没有 `this` 的 lambda 里编译不过），也会把**功能调用**一起吞掉。★★★★**

**★ 正确做法：开关在函数**里面**，调用点保持原样：★★**
```cpp
void AirportItlwm::publishNativeScanDiagnostics()
{
#if SEQUOIA_DIAGNOSTICS
    ... 发布属性、打日志 ...
#endif
}
```

**★ 附带的好处：两种配置都必须能编译。**如果"诊断关"的配置能编而"诊断开"不能，说明其中一个的行为是未知的 —— 这本身就是个 bug 信号。★★★★**

### 6.10 「名字像诊断的函数」可能不是诊断

`announceNativeDriverAvailable()` 名字里没有 "Diagnostics"，但它**向 WCL 发消息** —— 而 WCL 等这个信封才工作。把它当诊断关掉，症状是"能列出来但连不上"。

**★ 规则：被"关掉调试"的开关编译掉的代码块，只允许**发布属性和打日志**。任何发消息、改状态、动硬件的调用都必须是构建错误。★★**
**★ 这条规则要有工具来守，不能靠人肉 —— 本项目因此加了 `scripts/sequoia/check-diagnostics.py`，在发布构建前跑。★★★**

### 6.11 「意外产生的 completion」会掩盖真正的缺陷

一个扫描没报告结束，却因为**别的原因**（radio 复位顺带触发的 `hardwareStopped()`）而产生了 completion —— 系统看起来是好的，缺陷被掩盖，直到有人去掉那个"顺带"的动作。

**★ 教训：任何"看起来能用但说不清为什么能用"的状态，都要去查它到底靠什么在撑着。★★★**
**★ 相应的防御：completion 必须由明确的路径产生，且"没有 completion"必须是可诊断、可恢复的状态，而不是静默死锁。★★★**

### 6.12 硬编码的信道 band 表是移植的隐形地雷

`iwm_is_valid_channel` 用硬编码频段表（1-14, 36-64, 100-140, 145-165），不认 8265 的 68-96 和 169-181。**改成从 NVM 的 regulatory 段取（`iwm_channel_is_valid(sc, ch)`）。**

---

## 七、commit 序列（41 个）

| 阶段 | commit | 内容 |
|---|---|---|
| **白名单** | `8076e45` | 放行 `0x24FD` |
| **family 门** | `b1cd697` | 拆 4 处 device family 硬门 |
| **探针阶梯** | `c5397e1` | 加入 NVM trial 一级 |
| | `eb2955a` | 记录固件对每个请求段的应答 id |
| | `76a7f51` | 段 id 映射（后证明方向反了） |
| | `f6fa972` | 记录拒绝原因与驱动状态 |
| | `b57ecdf` | 定位 regulatory 段里的非零 word |
| | `c39788c` | 暴露 regulatory channel flags 与 PHY SKU words |
| | `bb03cad` | **纠正**：接受段 id 重标，但不请求重标 |
| | `4dd493f` | 报告 TXP 组边界与逐信道分组 |
| | `7eafe38` | dump PAPD/TXP 组 payload |
| | `eba5ceb` | 接受 8000 家族的信道集合 |
| **固件对话** | `6f09847` / `4311c1e` | 放行带 CPU2 paging 镜像的常规固件 |
| | `b58ccd1` | 放行报 station type 能力的固件 |
| | `490be26` | 放行报 UMAC 扫描能力的固件 |
| | `21c88ff` | ADD_STA 的期望跟随驱动的能力位 |
| | `6e27de5` | 构建修正 |
| **probe 布局** | `e93ba99` | probe request 按固件期望的布局重排 |
| | `4fbdb15` | 速率集改成完整的 Rates IE |
| **对照模式** | `5a45947` | 跑驱动自己的扫描请求 |
| | `14a8643` | 跑 UMAC 扫描请求 |
| **native scan** | `46b1f76` | 不再因 UMAC 能力位拒绝适配器 |
| | `6936b3e` / `9acc2e3` | 报告请求构造在哪一步失败 |
| | `a32290e` | 报告尺寸检查用到的各个值 |
| | `50740eb` | 报告实际请求的信道与提交失败原因 |
| | `809b1ae` | 剔除无校准分组的信道 |
| | `9d0f0c2` | **修 0xff vs 0xffff 的宽度 bug** |
| | `65c28f8` | 保留 8000 家族的 68-96；拒绝原因无条件记录 |
| **UMAC** | `2c9e81d` | ★ **native 路径按固件的选择发 UMAC 请求**；认 UMAC 完成通知 |
| | `b4f8cdc` | 修掉 `nativeScanUmac` 的影子成员（接口与派生类重复声明） |
| **回归修复** | `4a14049` | 扫描后不停 radio（提速）；⚠️ 暴露了下面的缺陷 |
| | `1e9bbd2` | ★ **IWM HAL 归一化自己的通知**（照 AX HAL）；安全网 `abandon()`；定时器退避 |
| | `d17cb77` | 第一轮代码审查：修信道上限（40→52）与**拷贝源边界未查**；`abandon` 保留 cancel 语义 |
| | `5febdfb` | 第二轮代码审查：修**影子成员**（诊断值恒为 0）；加编译期守卫 |
| **发布版** | `a65d1a6` | 建立 production 分支：编译期开关关掉全部诊断，删掉探针 personality |
| | `c9172d3` | 保留 driver-available 信封（它是功能，不是诊断） |
| | `ac8a72c` | ★ **开关移进函数内部**（之前包住调用点 → 把调用一起吞了）；加"只经宏可达=违规"检查 |

---

## 七·九、两种构建：开发版与发布版

**★ 同一棵源码树，靠一个编译期开关 `SEQUOIA_DIAGNOSTICS` 区分：★★**

| 构建 | 命令 | 内容 |
|---|---|---|
| **开发版** | `python3 scripts/sequoia/build.py` | 带全部诊断（几百个属性、NVRAM 面包屑、阶段标记、日志）和三个探针 personality |
| **发布版** | `python3 scripts/sequoia/build.py --production` | 无任何诊断代码；探针 personality 不进 bundle |

**★ 早先两者是两个目录（`work/itlwm-sequoia-8265/` 与 `work/itlwm-sequoia-8265-production/`），现已合并为一棵树：分开会让两份源码漂移，而发布版的问题恰恰出在两份源码的差异上（见 §七·九·二、§七·九·三）。★★**

| 开关打开时 | 开关关闭时 |
|---|---|
| 诊断属性发布到 provider（435 处） | 一处也不发 |
| `IOLog` / `kprintf` 日志 | 全部丢弃（**参数也不求值**） |
| 阶段标记 `ITLWM_HAL_STAGE` / `ITLWM_NATIVE_STAGE` | 空宏 |
| NVRAM 面包屑 `itlwm-stage` | 不写 |
| 探针 personality（NVM / 固件对话 / probe） | 不编译、不在 Info.plist、源文件已删 |
| 用户可写的调试路由属性（`ItlwmNativeDebugScanRoute` 等） | 读取侧一并关闭 |

**★ 构建：★★**

```sh
# 开发版
python3 scripts/sequoia/build.py
# 发布版
python3 scripts/sequoia/build.py --production
```

**★ 产物对比（实测）：★★**

| | 开发版 | 发布版 |
|---|---|---|
| 二进制大小 | 15,893,976 | **15,700,736**（-193 KB） |
| `ItlwmNative*` 字符串 | 420 处 | **0 处** |
| `itlwm-native:` 字符串 | 22 处 | **0 处** |
| `SequoiaTrial*` 字符串 | 31 处 | **0 处** |
| Info.plist 的 personality | 5 个 | **2 个** |

**★ 回退：development 版的 kext 备份在仓库外的 `work/kext-backups/`，随时可换回去：★★**

```sh
BUILD_DIR=$PWD/build/Sequoia-0.1.0-beta.1-RELEASE bash tools/install-probe-kext.sh main
```

**★ 教训：发布版必须能**当场**退回可用状态 —— 所以安装脚本要能切换构建来源，而不是写死一个路径。★★**

### 七·九·二、发布版第一次就坏了：把功能代码当成了诊断

**★ 症状：Wi-Fi 列表正常，但连不上已知网络（"未能加入 Wi-Fi 网络"）。★★**

**★ 根因：`announceNativeDriverAvailable()` 不是诊断函数。★★**
**★ 它向 WCL 发布 `APPLE80211_M_DRIVER_AVAILABLE` 信封 —— 而 **WCL 要等这个信封才会开始扫描或关联**。★★**
**★ 我按名字把它当成了"announce + 可用"，和 `publishNative*Diagnostics` 一起门控掉了 → 发布版永远不发这个信封 → **能列出来，但永远关联不上**。★★★**

```cpp
fNetIf->postMessage(APPLE80211_M_DRIVER_AVAILABLE, &announcement, sizeof(announcement), true);
```

**★ 修法：信封必须留下，只有它发布的属性和日志被门控。★★**

**★ ★★ 更重要的是加了一条**构建期检查**（`scripts/sequoia/check-diagnostics.py`）：★★**
**★ 被开关编译掉的代码块**只允许**发布属性和打日志**；发消息、改网络栈状态、动电台 → **构建失败**。★★**

### 七·九·三、发布版连不上：开关放错了地方

**★ 上一节修了函数体（保留 `postMessage`），但**调用点**用的是宏 —— production 下宏是空的 → **函数从未被调用** → 信封还是没发 → 还是连不上。★★★**

```cpp
// 错的：开关包住调用点
ITLWM_ANNOUNCE_DRIVER_AVAILABLE(0);      // production → do { } while (0)  ← 没调用！

// 对的：开关在函数里面，调用点保持原样
announceNativeDriverAvailable(0);          // 无条件调用 ✓
```

**★ ★★★ 改对之后，**同一类 bug 整类消失**：不再有"接收者丢失"（`self->publishNativeScanDiagnostics()` 被我改成无接收者 → 在没有 `this` 的 lambda 里编译不过），不再有"调用被吞"。★★★★**

**★ 而且 production 分支在"诊断开"的配置下**编译不过**（调用点丢了接收者）—— 这件事本身就是个信号：**两个配置都必须能编译**，否则其中一个配置的行为是未知的。★★★★**

**★ 权威验证：把两种配置的 kext 的符号表对比 —— production 里缺的符号应该**只有诊断相关**的。实测符合。★★★**

**★ 教训一：名字会骗人。`publish*` / `capture*` / `*Diagnostics` 是诊断；`announce*` 不是。★★**
**★ 教训二：一个"看起来像调试"的东西被关掉时，症状是"某个功能不工作了"，而不是"启动失败"—— 而发布版没有诊断属性，这种症状最难查。★★**
**★ 教训三：把"关掉的东西"做成**构建期可检查**的，而不是靠人肉看。★★**
**★ 教训四：**开关要放在"做事的函数"里面，不要放在"调用点"外面** —— 后者会连调用本身一起吞掉，而且会丢掉接收者。★★★**

## 八、状态与后续

**已证实**

- macOS 版本完全命中（内核串逐字节匹配）
- kext 加载、符号链接、主路径完整启动（`17_complete`）
- NVM 读取、固件对话、PHY DB 回放全部通过
- **native scan：UMAC 请求被固件接受，22 个 beacon 全部解码并交付**
- **native join：关联与认证成功，`en2` 拿到 IP，默认路由走 en2，ping / DNS 均通**

**未证实 / 待办**（都是 fork 自述没测的项）

1. **睡眠唤醒** —— 唤醒后 radio 与 WCL 状态能否恢复
2. **AP 失联恢复** —— 信号中断后能否自动重扫重连
3. **持续双向流量** —— 大流量下的吞吐与丢包
4. **真实换钥** —— PTK/GTK rekey
5. **长期稳定性** —— 长时间运行的内存与句柄
6. **多网卡共存** —— 与其它 Wi-Fi/蓝牙设备的干扰

**★ 已解决**

- **Wi-Fi 列表只反映最后一次窄扫描** → `itlwm_assoc_scan=1`（§4.8.7）
- **扫描后不停 radio 导致的"意外 completion"** → 归一化 + 安全网（§4.8.3）
- **诊断值恒为 0** → 影子成员（§6.8）
6. **开机后 Wi-Fi 默认关闭**：需要手动打开 Wi-Fi 才会开始扫描。原因已查明（见 §4.8.7）：已连接时 airportd 的扫描请求被 `itlwm_assoc_scan` 门拒绝，列表冻结在最后一次成功扫描上。**加 `itlwm_assoc_scan=1` 后已解决**（104/104 请求零拒绝，列表完整）。

**风险**

- 内核串逐字节匹配 → **系统安全更新后驱动静默失效**
- fork 只验证过 AX210，8265 的一切结论都是本项目的实测
- 探针每轮一次重启，且改的是这台主力机的引导配置

---

## 九、相关文档

- [`issue-1018-analysis.md`](issue-1018-analysis.md) —— 上游 issue #1009/#1018 的分析、fork 的做法、本项目的结论
- [`code-quality-review.md`](code-quality-review.md) —— fork 代码质量评估
- [`wpa2-limitation.md`](wpa2-limitation.md) —— WPA2-only 的原因与影响
