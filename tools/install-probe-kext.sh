#!/bin/bash
# 安装 8000 家族（Intel 8265）探针版 AirportItlwmSequoiaNative.kext 到 OpenCore。
#
# 用法:
#   1) 手动挂载 ESP:  sudo mkdir -p /Volumes/EFI && sudo mount_msdos -o rw /dev/disk0s1 /Volumes/EFI
#   2) bash tools/install-probe-kext.sh [nvm|firmware|main]
#
#   nvm      = 第 1 级（最安全）：只做硬件启动 + INIT 固件 + NVM 读取
#   firmware = 第 2 级：完整固件对话 + 校准 + 被动扫描
#   main     = 第 3 级：主路径（放宽 family 门后的完整 start）
#
# 环境变量:
#   BUILD_DIR = 要安装的构建目录（默认是开发树的构建，含诊断属性）
set -euo pipefail

MODE="${1:-nvm}"
ESP="/Volumes/EFI"
OC="$ESP/EFI/OC"
# The build to install. The development tree carries the diagnostics, the
# production tree does not.
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO/build/Sequoia-0.1.0-beta.1-RELEASE}"
KEXT_SRC="$BUILD_DIR/AirportItlwmSequoiaNative.kext"
WORK="${WORK:-$REPO/..}"

[ -d "$OC" ] || { echo "错误：$OC 不存在，请先挂载 ESP"; exit 1; }
[ -d "$KEXT_SRC" ] || { echo "错误：找不到编译产物 $KEXT_SRC"; exit 1; }

echo "==> 备份当前 config.plist"
cp -X "$OC/config.plist" "$OC/config.plist.bak"
[ -f "$WORK/config.plist.probe-nvm" ]      || cp -X "$OC/config.plist" "$WORK/config.plist.probe-nvm"
[ -f "$WORK/config.plist.probe-firmware" ] || cp -X "$OC/config.plist" "$WORK/config.plist.probe-firmware"
[ -f "$WORK/config.plist.probe-main" ]    || cp -X "$OC/config.plist" "$WORK/config.plist.probe-main"

echo "==> 安装 kext 到 $OC/Kexts/"
rm -rf "$OC/Kexts/AirportItlwmSequoiaNative.kext"
cp -R "$KEXT_SRC" "$OC/Kexts/AirportItlwmSequoiaNative.kext"

echo "==> ad-hoc 签名"
codesign --force --sign - --timestamp=none "$OC/Kexts/AirportItlwmSequoiaNative.kext"
codesign --verify --verbose=2 "$OC/Kexts/AirportItlwmSequoiaNative.kext" && echo "    签名 OK"

echo "==> 停用旧 itlwm.kext"
if [ -d "$OC/Kexts/itlwm.kext" ]; then
    rm -rf "$OC/Kexts/itlwm.kext.disabled"
    mv "$OC/Kexts/itlwm.kext" "$OC/Kexts/itlwm.kext.disabled"
    echo "    itlwm.kext -> itlwm.kext.disabled"
fi

echo "==> 写入 config.plist（模式：${MODE}）"
/usr/bin/python3 - "$OC/config.plist" "$MODE" <<'PY'
import plistlib, sys

path, mode = sys.argv[1], sys.argv[2]
with open(path, 'rb') as f:
    c = plistlib.load(f)

add = c['Kernel']['Add']
for e in add:
    if e.get('BundlePath') == 'itlwm.kext':
        e['Enabled'] = False

new = {
    'Arch': 'x86_64',
    'BundlePath': 'AirportItlwmSequoiaNative.kext',
    'Comment': 'V0.1.0-beta.1 | Sequoia native Wi-Fi PROBE (Intel 8265 / 0x24fd)',
    'Enabled': True,
    'ExecutablePath': 'Contents/MacOS/AirportItlwmSequoiaNative',
    'MaxKernel': '25.9.9',
    'MinKernel': '24.0.0',
    'PlistPath': 'Contents/Info.plist',
}
add = [e for e in add if e.get('BundlePath') != new['BundlePath']]
add.append(new)
c['Kernel']['Add'] = add

g = c['NVRAM']['Add']['7C436110-AB2A-4BBB-A880-FE41995C9F82']
# The arguments this script owns. Anything else the user put in the boot
# arguments belongs to them and is kept: overwriting the whole string would
# silently drop settings that took a reboot to find out about.
owned = ('darkwake=', 'ps2kbdonly=', '-wegnoegpu', 'itlwm_native=',
         'itlwm_bsd_owner=', 'itlwm-sequoia-')
kept = [arg for arg in str(g.get('boot-args', '')).split()
        if arg and not arg.startswith(owned)]
# The diagnostic boot argument switches the main personality off (the
# wrapper's probe() bails out on any itlwm-sequoia-* argument) and enables
# the matching trial personality instead.
extra = {'nvm': ['itlwm-sequoia-nvm=1'],
         'firmware': ['itlwm-sequoia-firmware=1'],
         'main': []}[mode]
base = ['darkwake=0', 'ps2kbdonly=1', '-wegnoegpu', 'itlwm_native=1',
        'itlwm_bsd_owner=skywalk']
g['boot-args'] = ' '.join(base + extra + kept)

with open(path, 'wb') as f:
    plistlib.dump(c, f, fmt=plistlib.FMT_XML, sort_keys=False)
print('    boot-args =', g['boot-args'])
PY

rm -f "$OC/._config.plist"
plutil -lint "$OC/config.plist"

echo "==> 校验 kext 符号链接"
/usr/bin/kmutil libraries -p "$OC/Kexts/AirportItlwmSequoiaNative.kext" --undef-symbols >/dev/null && echo "    kmutil OK"

echo
echo "完成。重启后运行:  bash tools/collect-probe-diagnostics.sh"
