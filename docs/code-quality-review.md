# fork 代码质量评估

对象：`zhangcheng142/AirportItlwm-Sequoia` @ `f31a3ad`（0.1.0-beta.1）
方法：源码静态审查 + 与 upstream `53c51c2` 全量 diff。**未实际编译、未在真机验证**（无 Sequoia 实机）。

---

## 一句话结论

**工程素养明显高于普通个人项目，但存在一类结构性隐患：大量逆向结论没有沉淀为可验证的知识。**

作者自己说的「AI 辅助开发」在代码里看得很清楚——**注释质量极高，结构意识很强，但知识沉淀不足**。

---

## 二、做得好的地方

### 2.1 测试覆盖：超出预期

`tests/` 下 **58 个测试文件**（对比 upstream：0 个）。

覆盖范围包括：

- 连接状态机（`sequoia_join_state_test`、`sequoia_join_hal_test`、`sequoia_join_worker_test`）
- 扫描生命周期（`sequoia_scan_controller_test`、`sequoia_pmf_test`）
- 密钥/加密（`sequoia_crypto_test`、`sequoia_join_security_test`）
- DMA 与固件（`iwm_dma_test`、`iwm_nvm_test`、`iwm_pnvm_test`）
- 边界与失败路径（`iwm_fault_test`、`sequoia_registration_test`、`sequoia_pci_owner_test`）

测试写法也讲究。例如 `tests/sequoia_join_security_test.cpp:14-16` 的注释说明了 mock 边界的设计原则：

```cpp
// Only the kernel/hardware boundary is mocked. Enums, macros, WPA/RSN
// layouts and functions above/below are captured from the production sources.
```

只 mock 硬件边界，保留真实的数据结构和函数——**这是对的 mock 策略**，比 mock 一切有价值得多。

### 2.2 构建可复现：做了版本锁定

`scripts/sequoia/sdk.lock.json` 锁定 SDK；`release.json` 锁定：

```json
"required_build": "24H23",
"required_kernel_uuid": "B7B3E8AD-9CA6-382C-BD56-8D52139342A4",
"tested_adapter": "AX210",
"pci_match": "0x31668086 0x27258086"
```

RELEASE_NOTES 声明「从发布源码归档独立重建，同一工具链下二进制、Info.plist 和 UUID 完全相同」。**声明了可复现性并给出验证手段**，这对内核代码很少见。

CI（`.github/workflows/main.yml`）也固定了 SDK commit：`05094e5e88cec7caedbfb35e8449ed0db94bf95b`。

### 2.3 注释密度高，且解释「为什么」

`AirportItlwmV2.cpp` 有 151 条行内注释。新代码的注释普遍在解释**决策依据**而非复述代码。例：

`AirportItlwmV2.cpp:1351-1355`
```cpp
// 24G830 BCM resetInterfacesComplete emits message 49 with uint32=1.
// SSM translates this into FW_LOADED, which loads the permanent MAC,
// capabilities and channels. Driver-available alone does not do this.
// Send only after real firmware enable succeeds. Queue a copied event;
// never synchronously enter WCL while holding the HAL command gate.
```

「为什么不能同步进入 WCL」——这是真实的踩坑记录，不是废话。

### 2.4 诊断能力做得好

连接失败时区分 6 个拒绝阶段（`native_join_sequoia.inc:15-16`）：

```cpp
// 0=radio, 1=busy, 2=HAL, 3=capture, 4=policy, 5=timer, 6=threadcall.
```

而且把诊断结果**暴露成 kext property**（`publishNativeJoinDiagnostics()`）。内核调试困难，作者显然吃够了苦。

### 2.5 用宏做版本隔离，没有破坏 upstream

所有改动都用 `__IO80211_TARGET >= __MAC_15_0` 包起来。旧的 Sonoma / Monterey 路径完整保留（`IO80211ControllerV2.h` 的 `#else` 分支齐全）。

**这一点做得很克制**，没有为了自己的需求砍掉旧路径。

### 2.6 类布局有静态断言兜底

```c
static_assert(sizeof(IO80211Controller) == 0x128, "Sequoia Controller size");
```

内核代码里 ABI 布局靠 `filler` 填充本身就是脆弱设计，这里至少加了断言。

---

## 三、问题：核心隐患

### 3.1 逆向结论没有沉淀为可验证的知识（最严重）

这是我认为最值得警惕的一点。看 `include/Airport/SequoiaWCL.h:8-46` 的注释：

```cpp
// Experimental wire layout recovered from 24G830, not an Apple public API.
...
// That mapping is now settled against the family itself, and it is the reverse
// of what this file said. ...
// the r162/r163 boots show which one this driver actually needs: with 0xc9 the
// FSM logged SCAN_MANAGER_EVENT_SCAN_COMPLETE 21 times and ... SCAN_MANAGER_EVENT_SCAN_RESULT
// not once, no WCL result handler ever ran ...
```

这段话包含几个信息：

1. **文件开头写「recovered from 24G830」，结论却在讲 r162/r163 的实验**——版本在迭代，注释没同步
2. **「it is the reverse of what this file said」**——明确承认这个文件以前的结论是**错的**
3. 大量 r149/r150/r159/r162/r263 的实验编号散落在注释里

问题不是注释长，而是：**这些逆向出来的 ABI 结论（消息号、报文布局、状态机转移）只存在于注释里，没有任何形式化描述、测试断言或文档支撑。**

后果很具体：

- 注释里混着多个版本号的实验记录，**没人能判断当前代码对应的是哪一轮结论**
- 如果不读这一大段注释，就不知道 `kScanResultMessage = 0xce` 这个魔法数字是怎么来的
- 文件开头说「recovered from 24G830」但正文讲 r162/r163，**说明注释本身已经不同步了**

**这类逆向知识本应是整个项目最值钱的资产**，但它躺在注释里 = 没有知识管理。一旦作者停止维护，这些结论基本无法复用。

### 3.2 `.inc` 文件模式：可疑的设计选择

新增 29 个 `.inc` 文件，通过 `#include` 引入 `.cpp`：

```
AirportItlwm/AirportItlwmV2.cpp:6 个 .inc
itlwm/hal_iwx/ItlIwx.cpp:1 个
AirportItlwm/AirportItlwmSkywalkInterface.cpp:1 个
```

`AirportItlwm/native_lifecycle_sequoia.inc` 有 **1597 行**——单文件比 `AirportItlwmV2.cpp`（1777 行）只小一点。

为什么用 `.inc` 而不是正常拆分？ripwire 索引显示这些文件**未被计入主符号图**。可能原因：

- 为了让测试能 `#include` 生产代码（`sequoia_join_bsd_functions.inc` 就是这个用法）
- 避免 xcodeproj 逐个添加文件
- AI 生成时常用的文件切分策略

无论原因，**把 1597 行代码藏进 `.inc` 后缀里**，对 IDE 索引、代码跳转、review 工具都是负担。可维护性上是负分。

### 3.3 超大文件

| 文件 | 行数 |
|---|---|
| `itlwm/hal_iwx/ItlIwx.cpp` | **14662** |
| `itlwm/hal_iwx/if_iwxreg.h` | 8107 |
| `itlwm/hal_iwn/ItlIwn.cpp` | 7497 |

`ItlIwx.cpp` 一万四千多行。但要注意：**这是 upstream 继承来的**，不是 fork 造成的。upstream 同样有超大文件。所以这不算 fork 的责任，只是 fork 没有改善它。

### 3.4 CI 几乎不跑测试

`.github/workflows/main.yml` 只有 build 步骤，**没有跑 `tests/` 里的 58 个测试**。

而且触发方式是 `workflow_dispatch`——**只能手动触发**，不是 push 自动跑。

RELEASE_NOTES 也承认：「GitHub Actions 尚未实际运行」。

所以 58 个测试目前是**手工执行**的，没有 CI 保障。这削弱了「测试覆盖好」的实际价值。

### 3.5 TODO 数量：比 upstream 还少

```
upstream: 123
fork:     119
```

这本身是好信号（没有新增技术债标记）。但结合「AI 辅助开发」的背景，另一种可能是**部分未完成的工作没有被标注**——README 里那句「PMF/TKIP 试验代码即使存在于源码中，也未作为可用能力启用」听起来像是有意保留的未完成代码。

无法判定，需要作者本人说明。

### 3.6 文档与代码不一致

已在 7.4 节记录：README 说实测 15.8 / 24H23，`release.json` 写 `24H23`，但 `SequoiaTrialPolicy.h` 写死 `24.6.0`（对应 15.6）。

**三处不一致**，说明发布流程缺少交叉校验。

---

## 四、如果让我评估

### 作为「一次性个人项目」：**合格偏好**

- 有测试（58 个）
- 有构建锁定和可复现性声明
- 注释质量高
- 有诊断设计
- 克制地保留了旧路径

这个完整度在 GitHub 个人项目里属于上游水平（pun intended）。

### 作为「可长期维护的驱动」：**不够**

三个硬伤：

1. **逆向知识没沉淀** —— 最核心的资产躺在注释里，格式混乱、版本自相矛盾
2. **测试不进 CI** —— 58 个测试没有自动化保障
3. **单点维护** —— 作者一个人（加 AI），随时可能停更

### 作为「给 8265 用」：需要额外谨慎

**你的 8265 完全没有实测过。** 作者测的是 AX210。

而且**上游原版连 8265 都不认**：8265 的 PCI ID 是 `0x24FD`（`ItlIwx` 走的是 AX200/AX210 的 `0x2725`），既不在 `SequoiaTrialPolicy.h:17` 的运行时白名单里，也不在 `release.json` 的 `pci_match` 里 —— 后者会在构建时覆盖 Info.plist 的完整 ID 列表，连源码里本来有的 `0x24fd8086` 都被抹掉。

代码质量再好，也不能替代硬件验证。不过有一点值得说：fork 的 Sequoia 原生实现（`itlwm/hal_iwm/native_*.inc`、`include/HAL/Iwm*.h`、`AirportItlwm/SequoiaFirmwareTrial.cpp` 直接 `new ItlIwm`）**是围绕 Intel 的 IWM HAL 写的**，而不是 AX210 的 `ItlIwx` —— 这一点此前的判断是对的。

**但 2026-10-20 实机验证后发现更关键的事实**：那套 IWM 原生代码**内部按 device family 分支，且只认 7000 家族**（`native_scan_sequoia.inc`、`native_join_start_sequoia.inc` → `sc->sc_device_family != IWM_DEVICE_FAMILY_7000` 就拒绝）。**8265 属于 8000 家族**，因此被结构性排除 —— 不是"没测过"，是"根本走不到"。

这暴露了一个更深的问题：**fork 自己的支持边界是自相矛盾的**。出厂白名单 `{0x3166, 0x2725}` 两个都是 AX 系列（走 `hal_iwx`，作者实测）；而 `hal_iwm` 那套原生路径是为 7000 家族写的，那些 ID（`0x3165` / `0x7265` / `0x3168` / `0x3160`）**一个都不在白名单里**。也就是说 7000 家族路径在出厂版本里够不着，属于死代码 —— 作者没有在 release notes 里说明这一点。

> 这也意味着：**只改白名单**（本文之前建议的方向）**是不够的**，即使对 7000 家族卡也要额外放行对应的 PCI ID；而对 8265 则完全无效。这属于"逆向结论没有沉淀"的典型后果 —— 家族边界写死在代码里，却没有在任何文档中说明。

**★ 2026-10-20 探针实验后的再评估：上面的"完全无效"结论过重。★**

拆掉那几处 family 硬门之后，**fork 的完整主路径在 8265 上跑通了**（`17_complete`，系统出现 Wi-Fi 硬件端口），固件对话（NVM / 校准 / 常规固件 / **PHY DB 回放** / 蓝牙）**全部通过**。

也就是说 —— **"逆向工作量"被高估了**：

| 原判断 | 实测 |
|---|---|
| 需要逆向移植 WCL 命令传输层到 8000 家族 | ❌ 槽位式命令传输层在 8000 家族上原生可用 |
| 固件 36 可能不接受固件 17 的对话格式 | ❌ 完整接受，连 PHY DB 回放格式都不用改 |
| 剩下一堆未知 | 只剩 native 扫描要不要改走 UMAC 这一个决定性问题 |

**★ 而那"最后一个决定性问题"的答案，是本项目最值钱的一条结论：★**

**fork 自建的扫描请求用的是 `IWM_SCAN_OFFLOAD_REQUEST_CMD`(0x51) —— 7000 家族"scan offload"特性的命令。8000 家族固件 36 不认这个命令，收到就复位。**
**upstream 在 8265 上走的是 `IWM_SCAN_REQ_UMAC`(0x10d)，所以那个组合从未在 8265 上被验证过 —— 这不是"格式没对上"，是"命令在这一代固件上不存在"。**

**⇒ 判断移植难度的正确方法是：别看"代码写了什么"，看"**这条代码路径在目标硬件上有没有被跑通过**"。★★**

**★ 这也再次印证了本文的核心判断：作者在 IWM HAL 上的逆向功底是扎实的**（`native_*.inc` 那一套是围绕 IWM 写的，不是 AX210 的 `ItlIwx`）。**问题不在逆向能力，在知识沉淀** —— 家族边界、能力位的语义、格式选择的依据，全在注释里，没有一篇文档说明。适配时踩的 4 个坑（能力位 ≠ 格式判据），全是这个问题的直接后果。

**★ 最终结果（2026-10-20）：扫描 → 关联 → 认证 → 拿到 IP → 上网，全链路实测通过。★★**

完整移植记录（含踩过的坑清单、能力位实测值、55 个 commit 的作用）见 [`sequoia-8265-port.md`](sequoia-8265-port.md)。

---

## 五、如果要用，建议

1. **先跑版本检查**（见 `sequoia-wifi-guide.md` 第三节）。版本不对直接放弃，不用折腾。
2. **备份 EFI + 准备 USB 恢复盘**。改坏开不了机是最坏的结果。
3. **装之前先看诊断属性的默认值**。连接失败时 kext 会把拒绝阶段写进 property，能快速判断是卡在哪一步，比盲试快。
4. **预期管理**：把它当实验品，不是成品。作者自己都叫它 "first experimental beta"。
5. **kernel 调试别指望 `IOLog`** —— 它进不了 unified log。作者在源码里写明 *"no unified-log capture for kext IOLog"*。真正能用的是 provider 的 IOKit 属性（`ItlwmNativeStage` / `ItlwmNativeLastStage` / …）和 NVRAM 面包屑 `itlwm-stage`。**失败分支一定要自己编码成阶段码**，只留一个错误码根本查不动。

---

## 附：审查方法与局限

**已做**：源码静态审查、与 upstream 全量 diff、符号图分析、测试与构建脚本检查、关键路径验证（`postMessage` 符号机制、WPA3 缺失、8265 白名单链路 —— 该项结论当时有误，已在 `issue-1018-analysis.md` §7 更正）。

**未做**：
- **未编译**（无对应 SDK 环境）
- **未运行测试**（需 macOS Sequoia 环境）
- **未真机验证**（无 Sequoia 机器）

所以本文只评价**代码本身的可读性与工程素养**，不对其功能正确性背书。功能是否正确，只有实机能回答。
