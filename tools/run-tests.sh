#!/bin/bash
# 跑仓库里能跑的离线测试。
#
# 仓库里有两类测试：
#   * 25 个只依赖仓库自身的测试 —— 这些能编译、能跑
#   * 33 个依赖 fork 作者本地的类型/方法头文件（*.inc、arc4.h、host_crypto.h 等）。
#     那些文件从来没有进过 git，所以这些测试在 fork 的公开仓库里同样编译不过。
#     本脚本把它们列出来并跳过，而不是让它们变成一片失败。
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

INCLUDES=(
  -I include -I include/HAL -I itl80211/openbsd -I itl80211/linux -I itl80211
  -I itlwm/hal_iwm -I itlwm/hal_iwx -I itlwm/hal_iwn -I AirportItlwm -I tests
)
# The toolchain's own clang++ does not search the C++ library on its own;
# going through xcrun gives it the SDK.
COMPILER=(xcrun --sdk macosx clang++)
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

passed=0
failed=0
skipped=0
skipped_list=""

for source in tests/*_test.cpp; do
  name="$(basename "$source" .cpp)"
  if ! "${COMPILER[@]}" -std=gnu++14 "${INCLUDES[@]}" "$source" -o "$OUT/$name" 2>"$OUT/$name.log"; then
    missing="$(grep -m1 'file not found' "$OUT/$name.log" | grep -oE "'[A-Za-z0-9_.-]+'" | tr -d "'")"
    if [ -z "$missing" ]; then
      missing="$(grep -m1 -oE 'Undefined symbols|_test\.cpp:[0-9]+' "$OUT/$name.log" | head -1)"
    fi
    skipped=$((skipped + 1))
    skipped_list="$skipped_list  $name (缺 ${missing:-链接符号})\n"
    continue
  fi
  if "$OUT/$name" >"$OUT/$name.out" 2>&1; then
    passed=$((passed + 1))
    printf '  ✓ %s\n' "$name"
  else
    failed=$((failed + 1))
    printf '  ✗ %s\n' "$name"
    sed -n '1,3p' "$OUT/$name.out"
  fi
done

echo
echo "通过 ${passed} / 失败 ${failed} / 跳过 ${skipped}（跳过的是依赖 fork 作者本地头文件的测试）"
if [ "$skipped" -gt 0 ]; then
  echo
  echo "跳过的测试（依赖 fork 作者本地的头文件）："
  printf "$skipped_list"
fi
exit $((failed > 0))
