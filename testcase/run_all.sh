#!/bin/bash
# SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
# Copyright (c) 2026 LU WEIFENG (wave.lu@gmail.com)
# 版权所有 (c) 2026 陆伟峰
#
# MWVD 内核模块完整测试运行器
#
# 用法:
#   ./run_all.sh              # 运行所有测试
#   ./run_all.sh basic        # 仅运行基础测试
#   ./run_all.sh vhdx vpc     # 运行 VHDX 和 VPC 测试
#   ./run_all.sh -s           # 包含压力测试(较慢)
#
# 注意: 需要 root 权限运行, 且内核模块已编译
#

cd "$(dirname "$0")" || { echo "Cannot cd to testcase dir"; exit 1; }

# 检查编译状态
if [ ! -f "../mwvd.ko" ]; then
    echo "ERROR: mwvd.ko not found. Please run 'make' first."
    exit 1
fi

# 检查 mwctl 工具
if ! command -v mwctl &>/dev/null; then
    echo "ERROR: mwctl not found in PATH."
    echo "  Install with: sudo cp mwctl /usr/local/bin/"
    exit 1
fi

# Check prerequisites
PREREQ_FAIL=0
for cmd in dd fdisk mkfs.ext4 mount umount blkdiscard hexdump; do
    if ! command -v $cmd &>/dev/null; then
        echo "WARNING: $cmd not found. Some tests may be skipped."
    fi
done

# qemu-img is optional but needed for VHDX/VHD creation
if ! command -v qemu-img &>/dev/null; then
    echo "WARNING: qemu-img not found. VHDX/VHD creation tests will fail."
    echo "  Install with: sudo apt install qemu-utils"
fi

if [ "$EUID" -ne 0 ] && [ "$(id -u)" -ne 0 ]; then
    echo "NOTE: Some tests need root. Run with sudo."
fi

# Determine which tests to run
RUN_ALL=false
RUN_STRESS=false
INCLUDE_TESTS=()

if [ $# -eq 0 ]; then
    RUN_ALL=true
else
    for arg in "$@"; do
        case "$arg" in
            -s|--stress) RUN_STRESS=true ;;
            -a|--all) RUN_ALL=true ;;
            basic|vhdx|vpc|partition|stress) INCLUDE_TESTS+=("$arg") ;;
            *) echo "Unknown test: $arg (options: basic, vhdx, vpc, partition, stress, -s)" ;;
        esac
    done
    [ ${#INCLUDE_TESTS[@]} -eq 0 ] && RUN_ALL=true
fi

should_run() {
    local name="$1"
    $RUN_ALL && return 0
    for t in "${INCLUDE_TESTS[@]}"; do
        [ "$t" = "$name" ] && return 0
    done
    return 1
}

results=()
start_time=$(date +%s)

echo "========================================"
echo "  MWVD Kernel Module Test Suite"
echo "  Date: $(date)"
echo "  Module: $(modinfo ../mwvd.ko 2>/dev/null | grep version | head -1)"
echo "========================================"
echo ""
echo "Tests will run from: $(pwd)"
echo ""

run_test() {
    local script="$1"
    local name="$2"
    echo "----------------------------------------"
    echo " Running: $name"
    echo "----------------------------------------"
    bash "$script"
    local rc=$?
    results+=("$name:$rc")
    echo ""
    return $rc
}

# === Basic Tests ===
should_run "basic" && run_test "./test_basic.sh" "Basic Operations"
should_run "vhdx" && run_test "./test_vhdx.sh" "VHDX Format"
should_run "vpc" && run_test "./test_vpc.sh" "VHD/VPC Format"
should_run "partition" && run_test "./test_partition.sh" "Partition & Filesystem"

# === Stress Tests (slow, only if requested) ===
if $RUN_STRESS; then
    should_run "stress" && run_test "./test_stress.sh" "Stress Tests"
fi

# === Summary ===
echo "========================================"
echo "  Overall Results"
echo "========================================"
overall_pass=0
overall_fail=0
for r in "${results[@]}"; do
    name="${r%%:*}"
    rc="${r##*:}"
    if [ "$rc" -eq 0 ]; then
        echo -e "  [PASS] $name"
        overall_pass=$((overall_pass + 1))
    else
        echo -e "  [FAIL] $name (exit=$rc)"
        overall_fail=$((overall_fail + 1))
    fi
done

end_time=$(date +%s)
duration=$((end_time - start_time))

echo ""
echo "  Passed: $overall_pass  Failed: $overall_fail"
echo "  Duration: ${duration}s"
echo "========================================"

exit $overall_fail
