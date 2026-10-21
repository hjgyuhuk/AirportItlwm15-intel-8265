# 离线测试

宿主上跑的单元测试，不加载 kext、不碰硬件。

```sh
bash tools/run-tests.sh
```

## 两类测试

仓库里有 58 个测试源文件，分两类：

| | 数量 | 状态 |
|---|---|---|
| 只依赖仓库自身的 | **25** | ✅ 编译通过、全部通过 |
| 依赖 fork 作者本地的头文件 | **33** | ⏭️ 跳过 |

**★ 跳过的那 33 个需要一些类型/方法头文件（`iwm_data_rx_types.inc`、`sequoia_join_bsd_types.inc`、`arc4.h`、`host_crypto.h` 等）。这些文件**从来没有进过 git** —— 在 fork 作者的公开仓库里同样缺失，所以这些测试在那边也是编译不过的。★★**

**★ `tools/run-tests.sh` 会把每个跳过的测试和它缺的那个文件列出来，而不是让它们变成一片失败。★★**

## 覆盖的是什么

| 测试 | 覆盖 |
|---|---|
| `iwm_nvm_test` | NVM 段读取与解析（含 8000 家族的段 id 重标） |
| `iwm_firmware_trial_test` | 固件对话的各阶段 |
| `iwm_scan_trial_test` / `sequoia_native_scan_test` | 扫描请求构造、信道过滤、通知归一化 |
| `sequoia_native_scan_lifecycle_test` | 扫描生命周期：终态、缺失终态的安全网、覆盖度 |
| `sequoia_scan_plan_test` | 扫描计划的准备与拒绝理由 |
| `sequoia_scan_delivery_test` | 结果投递的顺序、重试、溢出、放弃 |
| `sequoia_scan_cache_test` | 扫描缓存的条目 |
| `sequoia_join_request_test` | 关联请求的构造与密钥清除 |
| `sequoia_leave_hal_test` | 离开流程 |
| `sequoia_trial_policy_test` | 版本/设备白名单策略 |
| `sequoia_wcl_test` / `sequoia_registration_test` | WCL 注册与接口生命周期 |
| `iwm_command_test` / `iwm_command_slots_test` | 命令槽位与传输 |
| `iwm_dma_test` / `iwm_nic_lease_test` | DMA 映射与网卡租约 |
| `iwm_phy_db_test` | 校准数据库解析 |
| `itl_scan_test` / `iwm_scan_frame_test` | 帧与扫描缓存的编解码 |
| `sequoia_packet_test` / `sequoia_pci_owner_test` / `sequoia_interface_lifecycle_test` | 数据包路径、设备归属、接口生命周期 |
| `taskq_test` | 任务队列 |

## 没覆盖什么

**★ 这些测试不替代实机验证。硬件相关的行为（固件对话、扫描、关联、换钥、睡眠唤醒）只能在真实硬件上确认，实测记录见 [`../docs/sequoia-8265-port.md`](../docs/sequoia-8265-port.md)。★★**
