#!/bin/bash
# 采集 8000 家族探针的诊断信息。
#   用法: bash tools/collect-probe-diagnostics.sh [输出目录]
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$REPO/../probe-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"
echo "输出目录: $OUT"

echo "== 1. kext 是否加载 =="
kmutil showloaded 2>/dev/null | grep -iE "sequoia|itlwm" | tee "$OUT/01-kext-loaded.txt"

echo "== 2. 内核串 / OS 版本 =="
{ sysctl -n kern.version; sw_vers; } | tee "$OUT/02-system.txt"

echo "== 3. NVM trial 属性（第 1 级） =="
ioreg -r -c AirportItlwmSequoiaNvmTrial -l 2>/dev/null \
  | grep -oE '"Sequoia[A-Za-z]+" = [^,]*' | sort | tee "$OUT/03-nvm-trial.txt"

echo "== 4. firmware trial 属性（第 2 级） =="
ioreg -r -c AirportItlwmSequoiaFirmwareTrial -l 2>/dev/null \
  | grep -oE '"Sequoia[A-Za-z]+" = [^,]*' | sort | tee "$OUT/04-firmware-trial.txt"

echo "== 5. 主路径 provider 的阶段属性（第 3 级） =="
ioreg -r -c IOPCIEDeviceWrapper -l 2>/dev/null \
  | grep -oE '"ItlwmNative[A-Za-z]*" = [^,]*' | sort | tee "$OUT/05-native-stage.txt"

echo "== 5b. 电源管理（睡眠/唤醒）相关诊断 =="
# 唤醒路径失败时，enableAdapter 的返回码会发布在这里。
# 0 = 成功；其它值 = 哪一条守卫挡住了 radio 重启。
ioreg -r -c IOPCIEDeviceWrapper -l 2>/dev/null \
  | grep -oE '"(SequoiaNativePower[A-Za-z]*|ItlwmNativePower[A-Za-z]*|ItlwmNativeEnableRefused|ItlwmNativeBSDOwner|ItlwmNativeRadioTransition|ItlwmNativeState[A-Za-z]*)" = [^,]*' \
  | sort | tee "$OUT/05b-power-management.txt"

echo "== 6. NVRAM 面包屑 =="
nvram -p 2>/dev/null | grep -iE "itlwm" | tee "$OUT/06-nvram.txt"

echo "== 7. 本次启动的内核日志（驱动相关） =="
/usr/bin/log show --last 30m --style compact --predicate 'process == "kernel"' 2>/dev/null \
  | grep -iE "itlwm|AirportItlwm|Sequoia|IO80211Family|WCL|Skywalk|iwm_" \
  | tee "$OUT/07-kernel-log.txt" | tail -60

echo "== 8. 网络接口状态 =="
{ ifconfig -a; echo; networksetup -listallhardwareports; } 2>/dev/null \
  | tee "$OUT/08-network.txt" | grep -iE "^[a-z0-9]+:|Wi-Fi|Ethernet" | head -30

echo
echo "全部诊断已保存到: $OUT"
echo
echo "快速判读:"
echo "  第 1 级 (nvm)      看 03: SequoiaTrialPhase / SequoiaTrialNvmCheck / SequoiaTrialNvmResult"
echo "  第 2 级 (firmware) 看 04: SequoiaFirmwareStage"
echo "      nvm        → 固件/NVM 读取阶段"
echo "      calibration→ 校准数据阶段"
echo "      regular    → 常规固件阶段"
echo "      configuration → 天线/PHY DB/蓝牙配置阶段"
echo "      scan       → 被动扫描阶段"
echo "      complete   → 全部通过（8000 移植可行！）"
echo "  第 3 级 (main)     看 05: ItlwmNativeStage / ItlwmNativeLastStage / ItlwmNativeHal"
echo "  睡眠/唤醒          看 05b: SequoiaNativePowerOnError（0 = radio 成功重启）"
