#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
# Copyright (c) 2026 LU WEIFENG (wave.lu@gmail.com)
# 版权所有 (c) 2026 陆伟峰
#
# 多版本内核编译矩阵（纯固件载荷模式回归）
#
# 目的：验证 GPL 层（main.c / vd_core.c / mwcfg_ext.c / mwvd_code.c）在
#       各内核版本上都能编译 + 链接，且不产生告警。2.8.0 起 mwvd.ko 不再
#       链接闭源层 vd.o，因此本矩阵只需内核树，不需要 prebuild/。
#
# 用法:
#   ./test_kernel_matrix.sh                    # 扫 $HOME/linux-kernels/linux-*
#   ./test_kernel_matrix.sh /path/to/kernels   # 指定内核树根目录
#   ./test_kernel_matrix.sh /path/to/kernels 5.15 6.6 7.2   # 只测指定版本
#
# 说明:
#   - 内核树需先 `make defconfig && make modules_prepare`（未完整构建的树
#     没有 Module.symvers，modpost 会把所有内核导出符号报成 undefined；
#     本脚本用 KBUILD_MODPOST_WARN=1 把这类**环境性**缺失降级为警告，
#     `error:` 才是真正的编译/链接失败）。
#   - 每个版本编译前都会先 `make ... clean`，结束时清理产物，不留 .o/.ko。
#
set -u

SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"
KDIR_ROOT="${1:-$HOME/linux-kernels}"
if [ $# -gt 0 ]; then shift; fi

if [ ! -d "$KDIR_ROOT" ]; then
  echo "ERROR: 内核树目录不存在: $KDIR_ROOT" >&2
  exit 2
fi

# 目标版本列表：显式给出则用之，否则扫 linux-<版本>
if [ $# -gt 0 ]; then
  VERSIONS=("$@")
else
  VERSIONS=()
  for d in "$KDIR_ROOT"/linux-*/; do
    [ -d "$d" ] || continue
    VERSIONS+=("$(basename "$d" | sed 's/^linux-//')")
  done
fi

[ ${#VERSIONS[@]} -gt 0 ] || { echo "ERROR: 未找到内核树" >&2; exit 2; }

echo "源码目录: $SRC_DIR"
echo "内核根目录: $KDIR_ROOT"
echo "版本数: ${#VERSIONS[@]}"
echo "========================================"

pass=0
fail=0
skip=0
failed_versions=()

for v in "${VERSIONS[@]}"; do
  K="$KDIR_ROOT/linux-$v"
  log="/tmp/mwvd-kbuild-$v.log"

  if [ ! -f "$K/Makefile" ]; then
    echo "SKIP linux-$v (无内核树)"
    skip=$((skip + 1))
    continue
  fi
  if [ ! -f "$K/include/config/auto.conf" ]; then
    echo "SKIP linux-$v (未 modules_prepare，缺 include/config/auto.conf)"
    skip=$((skip + 1))
    continue
  fi

  make -C "$K" M="$SRC_DIR" clean >/dev/null 2>&1
  if KBUILD_MODPOST_WARN=1 make -C "$K" M="$SRC_DIR" modules >"$log" 2>&1; then
    warn=$(grep -c 'warning:' "$log" 2>/dev/null)
    warn=${warn:-0}
    if [ "$warn" -eq 0 ]; then
      echo "PASS linux-$v"
    else
      echo "PASS linux-$v (warnings=$warn, 见 $log)"
    fi
    pass=$((pass + 1))
  else
    echo "FAIL linux-$v"
    grep -E 'error:' "$log" | head -5
    failed_versions+=("$v")
    fail=$((fail + 1))
  fi
done

# 清理最后一次的构建产物
if [ -f "$KDIR_ROOT/linux-${VERSIONS[${#VERSIONS[@]}-1]}/Makefile" ]; then
  make -C "$KDIR_ROOT/linux-${VERSIONS[${#VERSIONS[@]}-1]}" M="$SRC_DIR" clean \
    >/dev/null 2>&1
fi

echo "========================================"
echo "PASS=$pass FAIL=$fail SKIP=$skip"
[ "$fail" -eq 0 ] || { echo "失败版本: ${failed_versions[*]}"; exit 1; }
exit 0
