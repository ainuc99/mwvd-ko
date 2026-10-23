#!/bin/bash
# SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
# Copyright (c) 2026 LU WEIFENG (wave.lu@gmail.com)
# 版权所有 (c) 2026 陆伟峰
#
# 内核版本兼容性测试工具
# 检测 mwvd 模块在不同内核版本间的编译兼容性
#
# 用法:
#   ./test_compat.sh                      # 自动检测本地可用内核源码
#   ./test_compat.sh /path/to/linux-6.6   # 指定内核源码目录
#   ./test_compat.sh --auto               # 扫描常见位置
#   ./test_compat.sh --list               # 列出可测试的内核
#
# 依赖:
#   - 内核源码树 (已编译过，至少已 make modules_prepare)
#

source "$(dirname "$0")/test_common.sh" 2>/dev/null || true

cd "$(dirname "$0")/.." || exit 1
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULT_DIR="/tmp/mwvd_compat"
rm -rf "$RESULT_DIR"
mkdir -p "$RESULT_DIR"

KERNEL_DIRS=()
VERBOSE=false
CLEANUP=true

# ============================================================
# 检测可用的内核源码
# ============================================================

find_kernel_sources() {
    local dirs=()

    # 当前运行内核
    local running="/lib/modules/$(uname -r)/build"
    [ -d "$running" ] && dirs+=("$running")

    # 常见内核源码位置
    for d in /home/*/linux-* $HOME/linux-* /usr/src/linux-* /opt/linux-*; do
        [ -d "$d" ] && dirs+=("$d")
    done

    # WSL2 内核常见位置
    for d in /home/*/WSL2-Linux-Kernel $HOME/WSL2-Linux-Kernel; do
        [ -d "$d" ] && dirs+=("$d")
    done

    # 去重
    printf '%s\n' "${dirs[@]}" | sort -u
}

detect_kernel_version() {
    local dir="$1"
    local ver=""

    # 从 Makefile 读取版本
    if [ -f "$dir/Makefile" ]; then
        ver=$(grep -m1 '^VERSION\s*=' "$dir/Makefile" 2>/dev/null | awk '{print $3}')
        local patch=$(grep -m1 '^PATCHLEVEL\s*=' "$dir/Makefile" 2>/dev/null | awk '{print $3}')
        local sub=$(grep -m1 '^SUBLEVEL\s*=' "$dir/Makefile" 2>/dev/null | awk '{print $3}')
        [ -n "$ver" ] && [ -n "$patch" ] && echo "${ver}.${patch}.${sub}" && return 0
    fi

    # fallback: 从目录名提取
    basename "$dir" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1
}

# ============================================================
# 编译测试
# ============================================================

test_compile() {
    local kdir="$1"
    local label="$2"
    local log="$RESULT_DIR/${label}.log"

    echo "  [${label}] Compiling..." >> "$log"

    # 测试 vd.o 编译
    local vd_ok=false
    local vdabi_ok=false
    local main_ok=false
    local vd_syms=""
    local vdabi_syms=""

    # 临时目录
    local tmp="$RESULT_DIR/${label}_build"
    mkdir -p "$tmp"

    # 复制源文件（排除 vd_abi 以测试纯 vd.o 兼容性）
    cp main.c mwvd.h vd.h core/vd_abi.h "$tmp/" 2>/dev/null
    cp vd_abi.c "$tmp/" 2>/dev/null

    # vd.o 使用本地源文件（vd.c + raw.c + vpc.c + vhdx.c + mon.c）
    # 在内核构建系统外部编译比较困难，我们直接使用内核构建系统
    
    # 创建临时 Makefile
    cat > "$tmp/Makefile" << 'EOF'
obj-m := mwvd_test.o
mwvd_test-objs := main.o vd.o vd_abi.o

# 禁用独立编译 vd.o 的规则，让内核构建系统自行编译
EOF

    # 尝试编译 vd_abi.o（轻量，快速验证）
    if make -C "$kdir" M="$tmp" modules 2>/dev/null >> "$log" 2>&1; then
        vdabi_ok=true
        # 提取符号
        vdabi_syms=$(nm "$tmp/vd_abi.o" 2>/dev/null | grep -i ' U ' | awk '{print $2}' | sort -u)
    else
        # 至少尝试编译 .o 文件
        make -C "$kdir" M="$tmp" "$tmp/vd_abi.o" 2>/dev/null >> "$log" 2>&1 && vdabi_ok=true
    fi

    # 清理
    make -C "$kdir" M="$tmp" clean 2>/dev/null >> "$log" 2>&1 || true
    rm -rf "$tmp"

    # 提取外部符号引用（从本地编译的 vd.o 或目标文件）
    if [ -f "vd.o" ]; then
        vd_syms=$(nm "vd.o" 2>/dev/null | grep -i ' U ' | awk '{print $2}' | sort -u)
    fi

    # 结果
    echo "$vd_ok:$vdabi_ok:${vd_syms//$'\n'/,}:${vdabi_syms//$'\n'/,}"
}

# ============================================================
# 输出报告
# ============================================================

print_report() {
    local results="$1"

    echo ""
    echo "============================================"
    echo "  内核兼容性测试报告"
    echo "============================================"
    echo ""

    printf "%-20s %-10s %-10s  %s\n" "Kernel" "vd_abi.o" "vd.o" "Note"
    printf "%-20s %-10s %-10s  %s\n" "------" "--------" "-----" "----"

    while IFS='|' read -r kver kdir vdabi vd syms_diff; do
        local vdabi_status="$([ "$vdabi" = "ok" ] && echo "✅" || echo "❌")"
        local vd_status="$([ "$vd" = "ok" ] && echo "✅" || echo "❌")"
        printf "%-20s %-10s %-10s  %s\n" "$kver" "$vdabi_status" "$vd_status" "$syms_diff"
    done <<< "$results"

    echo ""
    echo "详细日志: $RESULT_DIR/"
}

# ============================================================
# 主流程
# ============================================================

# 解析参数
if [ $# -eq 0 ]; then
    KERNEL_DIRS=($(find_kernel_sources))
elif [ "$1" = "--list" ]; then
    echo "Available kernel sources:"
    find_kernel_sources | while read d; do
        ver=$(detect_kernel_version "$d")
        echo "  ${ver:-unknown}  →  $d"
    done
    exit 0
elif [ "$1" = "--auto" ]; then
    KERNEL_DIRS=($(find_kernel_sources))
else
    KERNEL_DIRS=("$@")
fi

if [ ${#KERNEL_DIRS[@]} -eq 0 ]; then
    echo "No kernel sources found."
    echo "Usage: $0 /path/to/linux-source"
    exit 1
fi

echo "Found ${#KERNEL_DIRS[@]} kernel source(s):"
for d in "${KERNEL_DIRS[@]}"; do
    echo "  $(detect_kernel_version "$d")  →  $d"
done
echo ""

RESULTS=""

for kdir in "${KERNEL_DIRS[@]}"; do
    kver=$(detect_kernel_version "$kdir")
    label="${kver:-unknown}"

    echo -n "Testing kernel ${label}... "

    # 编译测试
    local tmp="$RESULT_DIR/${label}_build"
    mkdir -p "$tmp"
    cp main.c mwvd.h vd.h core/vd_abi.h vd_abi.c "$tmp/" 2>/dev/null

    # 创建 Makefile
    cat > "$tmp/Makefile" << 'MAKEEOF'
obj-m := mwvd_test.o
mwvd_test-objs := main.o vd_abi.o
MAKEEOF

    vdabi_ok=false
    main_ok=false
    vdabi_syms=""

    # 编译 vd_abi.o
    log="$RESULT_DIR/${label}.log"
    if make -C "$kdir" M="$tmp" "$tmp/vd_abi.o" >> "$log" 2>&1; then
        vdabi_ok=true
        vdabi_syms=$(nm "$tmp/vd_abi.o" 2>/dev/null | grep ' U ' | sort -u)
    fi

    # 编译 main.o
    if make -C "$kdir" M="$tmp" "$tmp/main.o" >> "$log" 2>&1; then
        main_ok=true
    fi

    make -C "$kdir" M="$tmp" clean >> "$log" 2>&1 || true
    rm -rf "$tmp"

    # 从本地 vd.o 提取外部符号
    vd_syms=""
    vd_sym_count=0
    if [ -f "vd.o" ]; then
        vd_syms=$(nm "vd.o" 2>/dev/null | grep ' U ' | sort -u)
        vd_sym_count=$(echo "$vd_syms" | wc -l)
    fi

    # 对比 vd_abi.o 和 vd.o 的符号交集
    common_syms=""
    if [ -n "$vd_syms" ] && [ -n "$vdabi_syms" ]; then
        common_syms=$(comm -12 <(echo "$vd_syms" | awk '{print $2}' | sort) \
                                 <(echo "$vdabi_syms" | awk '{print $2}' | sort))
    fi
    common_count=$(echo "$common_syms" | wc -l)

    # vd.o 中未通过 vd_abi 覆盖的符号数
    uncovered=$(comm -23 <(echo "$vd_syms" | awk '{print $2}' | sort) \
                        <(echo "$vdabi_syms" | awk '{print $2}' | sort) 2>/dev/null | wc -l)

    RESULTS="${RESULTS}${kver}|${kdir}|$([ "$vdabi_ok" = true ] && echo "ok" || echo "fail")|$([ "$main_ok" = true ] && echo "ok" || echo "fail")|${uncovered} syms uncovered"

    if [ "$vdabi_ok" = true ] && [ "$main_ok" = true ]; then
        echo "✅ vd_abi=OK main=OK (uncovered: ${uncovered} symbols)"
    else
        echo "❌ vd_abi=$([ "$vdabi_ok" = true ] && echo "OK" || echo "FAIL") main=$([ "$main_ok" = true ] && echo "OK" || echo "FAIL")"
    fi

    # 详细模式：列出未覆盖符号
    if [ "$VERBOSE" = true ] && [ "$uncovered" -gt 0 ]; then
        echo "  Uncovered symbols in vd.o:"
        comm -23 <(echo "$vd_syms" | awk '{print $2}' | sort) \
                 <(echo "$vdabi_syms" | awk '{print $2}' | sort) | head -20
    fi
done

# ============================================================
# 生成摘要
# ============================================================
echo ""
echo "============================================"
echo "  摘要"
echo "============================================"

# 表格
printf "%-16s %-10s %-10s  %s\n" "Kernel" "vd_abi.o" "main.o" "vd.o uncovered"
printf -- "----------------  ---------- ----------  -----------------\n"

while IFS='|' read -r kver kdir vdabi main uncovered; do
    printf "%-16s %-10s %-10s  %s\n" "$kver" "$vdabi" "$main" "$uncovered"
done <<< "$RESULTS"

echo ""
echo "说明:"
echo "  vd_abi.o = per-kernel ABI 层编译结果"
echo "  main.o   = per-kernel 驱动层编译结果"
echo "  vd.o uncovered = vd.o 引用但 vd_abi 未覆盖的内核符号数"
echo "  若 uncovered > 0 且符号名在不同内核版本间变化，vd.o 需要多版本预编译"
echo ""
echo "详细日志: $RESULT_DIR/"
