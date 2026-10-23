#!/bin/bash
# SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
# Copyright (c) 2026 LU WEIFENG (wave.lu@gmail.com)
# 版权所有 (c) 2026 陆伟峰
#
# MWVD 内核模块测试套件 — 公共函数
#

MWVD_MODULE="mwvd"
MWVD_MODULE_VERSION=$(sed -n 's/^#define MWVD_MODULE_VERSION "\(.*\)"/\1/p' "${MWVD_TEST_DIR}/../main.c")
MWVD_MAJOR=240
TEST_DIR="/tmp/mwvd_test"
MWCTL="mwctl"    # Global command (installed in PATH)
TIMEOUT=30          # Per-test timeout (seconds)

# Colors
PASS="\e[32mPASS\e[0m"
FAIL="\e[31mFAIL\e[0m"
SKIP="\e[33mSKIP\e[0m"
INFO="\e[36mINFO\e[0m"

total=0
passed=0
failed=0
skipped=0

die() {
    echo -e "  $FAIL: $*"
    exit 1
}

pass() {
    echo -e "  $PASS"
}

fail() {
    echo -e "  $FAIL: $*"
}

skip() {
    echo -e "  $SKIP: $*"
}

# === Module management ===

load_module() {
    if lsmod | grep -q "^$MWVD_MODULE"; then
        echo "Module already loaded"
        return 0
    fi
    sudo insmod ${MWVD_MODULE}.ko 2>/dev/null || \
        sudo modprobe ${MWVD_MODULE} 2>/dev/null || \
        die "Cannot load $MWVD_MODULE module"
    sleep 1
}

unload_module() {
    # Detach all devices first
    for dev in /dev/mwvd*; do
        [ -b "$dev" ] && sudo $MWCTL -d "$dev" 2>/dev/null
    done
    sudo rmmod $MWVD_MODULE 2>/dev/null
    sleep 0.5
}

reload_module() {
    unload_module
    load_module
}

# === Device management ===

device_ready() {
    local dev="$1"
    [ -b "$dev" ] && sudo blockdev --getsize64 "$dev" >/dev/null 2>&1
}

wait_device() {
    local dev="$1"
    local max_wait=10
    for i in $(seq 1 $max_wait); do
        device_ready "$dev" && return 0
        sleep 1
    done
    return 1
}

create_device() {
    local img="$1"
    local dev="${2:-/dev/mwvd1}"
    sudo $MWCTL -c "$dev" "$img" || die "Cannot create device $dev from $img"
    wait_device "$dev" || die "Device $dev not ready after creation"
}

detach_device() {
    local dev="$1"
    sudo $MWCTL -d "$dev" 2>/dev/null || true
}

# === Image creation ===

create_raw_img() {
    local path="$1"
    local size="${2:-64M}"
    dd if=/dev/zero of="$path" bs=1M count=0 seek=$(( ${size%M} )) 2>/dev/null
    # Write a signature
    echo -n "RAW" | dd of="$path" bs=1 count=3 conv=notrunc 2>/dev/null
}

# qemu-img capability detection
QEMU_CAN_VHDX_DIFF=false
QEMU_CAN_VHD_DIFF=false
detect_qemu_caps() {
    if command -v qemu-img &>/dev/null; then
        # Check VHDX differencing (backing file) support
        local td="/tmp/mwvd_qemu_test"
        mkdir -p "$td"
        qemu-img create -f vhdx "$td/p.vhdx" 1M &>/dev/null
        if qemu-img create -f vhdx -b "$td/p.vhdx" -F vhdx "$td/c.vhdx" &>/dev/null; then
            QEMU_CAN_VHDX_DIFF=true
        fi
        rm -f "$td/p.vhdx" "$td/c.vhdx"

        # Check VHD differencing support
        qemu-img create -f vpc "$td/p.vhd" 1M &>/dev/null
        if qemu-img create -f vpc -b "$td/p.vhd" -F vpc "$td/c.vhd" &>/dev/null; then
            QEMU_CAN_VHD_DIFF=true
        fi
        rm -f "$td/p.vhd" "$td/c.vhd" "$td" 2>/dev/null
        rmdir "$td" 2>/dev/null
    fi
}
detect_qemu_caps

create_vhdx_img() {
    local path="$1"
    local size="${2:-64M}"
    local fmt="${3:-dynamic}"  # dynamic, fixed, differencing
    local backing="${4:-}"

    if [ "$fmt" = "differencing" ]; then
        if ! $QEMU_CAN_VHDX_DIFF; then
            echo "  (SKIP: qemu-img does not support VHDX differencing)"
            return 1
        fi
        [ -n "$backing" ] || { echo "  (SKIP: no backing file specified)"; return 1; }
        qemu-img create -f vhdx -b "$backing" -F vhdx "$path" 2>/dev/null || \
            { echo "  (SKIP: qemu-img create differencing VHDX failed)"; return 1; }
    else
        local subfmt="$fmt"
        qemu-img create -f vhdx -o subformat="$subfmt" "$path" "${size}" 2>/dev/null || \
            die "qemu-img create VHDX failed"
    fi
}

create_vhd_img() {
    local path="$1"
    local size="${2:-64M}"
    local fmt="${3:-dynamic}"
    local backing="${4:-}"

    if [ "$fmt" = "differencing" ]; then
        if ! $QEMU_CAN_VHD_DIFF; then
            echo "  (SKIP: qemu-img does not support VHD differencing)"
            return 1
        fi
        [ -n "$backing" ] || { echo "  (SKIP: no backing file specified)"; return 1; }
        qemu-img create -f vpc -b "$backing" -F vpc "$path" 2>/dev/null || \
            { echo "  (SKIP: qemu-img create differencing VHD failed)"; return 1; }
    else
        qemu-img create -f vpc -o subformat="$fmt" "$path" "${size}" 2>/dev/null || \
            die "qemu-img create VHD failed"
    fi
}

# === Data verification ===

# Write a pattern and verify it reads back correctly
verify_rw() {
    local dev="$1"
    local offset="${2:-0}"
    local count="${3:-1024}"

    local tmp_in=$(mktemp)
    local tmp_out=$(mktemp)

    # Generate random data
    dd if=/dev/urandom of="$tmp_in" bs=512 count=$count 2>/dev/null

    # Write to device
    dd if="$tmp_in" of="$dev" bs=512 seek=$offset count=$count conv=notrunc 2>/dev/null || \
        { rm -f "$tmp_in" "$tmp_out"; return 1; }

    # Read back
    dd if="$dev" of="$tmp_out" bs=512 skip=$offset count=$count 2>/dev/null || \
        { rm -f "$tmp_in" "$tmp_out"; return 1; }

    # Compare
    cmp "$tmp_in" "$tmp_out"
    local rc=$?
    rm -f "$tmp_in" "$tmp_out"
    return $rc
}

# Write zeros and verify
verify_zero() {
    local dev="$1"
    local offset="${2:-0}"
    local count="${3:-1024}"

    dd if="$dev" bs=512 skip=$offset count=$count 2>/dev/null | hexdump -C | head -5 | grep -q "0000 0000"
    return $?
}

# === Test framework ===

test_section() {
    echo ""
    echo "===== $1 ====="
}

test_case() {
    local name="$1"
    echo -n "  TEST: $name ... "
    total=$((total + 1))
}

test_ok() {
    passed=$((passed + 1))
    echo -e " $PASS"
}

test_fail() {
    failed=$((failed + 1))
    echo -e " $FAIL"
    echo "    $1"
}

test_skip() {
    skipped=$((skipped + 1))
    echo -e " $SKIP"
    echo "    $1"
}

summary() {
    echo ""
    echo "===== Test Summary ====="
    echo "  Total:  $total"
    echo -e "  Passed: $PASS  $passed"
    [ $failed -gt 0 ] && echo -e "  Failed: $FAIL  $failed"
    [ $skipped -gt 0 ] && echo -e "  Skipped: $SKIP  $skipped"
    echo ""
    return $failed
}
