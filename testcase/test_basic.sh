#!/bin/bash
# SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
# Copyright (c) 2026 LU WEIFENG (wave.lu@gmail.com)
# 版权所有 (c) 2026 陆伟峰
#
# 基础功能测试：模块加载/卸载、RAW 设备读写
#
source "$(dirname "$0")/test_common.sh"

cd "$(dirname "$0")/.." || die "Cannot cd to project root"
TEST_DIR="/tmp/mwvd_test_basic"
mkdir -p "$TEST_DIR"

test_section "Module Lifecycle"

test_case "Load module"
load_module
test_ok

test_case "Module presence in lsmod"
lsmod | grep -q "^$MWVD_MODULE"
test_ok

test_case "Module version"
grep -q "$MWVD_MODULE_VERSION" /sys/module/${MWVD_MODULE}/version 2>/dev/null || \
    dmesg | grep -q "module $MWVD_MODULE_VERSION"
test_ok

test_section "RAW Device"

test_case "Create 64MB RAW image"
create_raw_img "$TEST_DIR/raw.img" 64M
test_ok

test_case "Create mwvd device from RAW image"
create_device "$TEST_DIR/raw.img" /dev/mwvd1
test_ok

test_case "Verify device size (64M)"
size=$(sudo blockdev --getsize64 /dev/mwvd1)
[ "$size" = "$((64 * 1024 * 1024))" ]
test_ok

test_case "Write and read back (single sector)"
dd if=/dev/urandom of="$TEST_DIR/w1.dat" bs=512 count=1 2>/dev/null
sudo dd if="$TEST_DIR/w1.dat" of=/dev/mwvd1 bs=512 seek=100 conv=notrunc 2>/dev/null
sudo dd if=/dev/mwvd1 of="$TEST_DIR/r1.dat" bs=512 skip=100 count=1 2>/dev/null
cmp "$TEST_DIR/w1.dat" "$TEST_DIR/r1.dat"
test_ok

test_case "Write and read back (1MB at end)"
dd if=/dev/urandom of="$TEST_DIR/w2.dat" bs=1M count=1 2>/dev/null
sudo dd if="$TEST_DIR/w2.dat" of=/dev/mwvd1 bs=1M seek=63 conv=notrunc 2>/dev/null
sudo dd if=/dev/mwvd1 of="$TEST_DIR/r2.dat" bs=1M skip=63 count=1 2>/dev/null
cmp "$TEST_DIR/w2.dat" "$TEST_DIR/r2.dat"
test_ok

test_case "Read zero from unwritten area"
sudo dd if=/dev/mwvd1 of="$TEST_DIR/z.dat" bs=512 skip=50000 count=4 2>/dev/null
hexdump -C "$TEST_DIR/z.dat" | grep -qv "[^00]"
test_ok

test_case "Flush device"
sudo blockdev --flushbufs /dev/mwvd1
test_ok

test_case "Detach device"
detach_device /dev/mwvd1
! [ -b /dev/mwvd1 ]
test_ok

test_section "Multiple Devices"

test_case "Create two RAW images and attach both"
create_raw_img "$TEST_DIR/raw2.img" 32M
create_raw_img "$TEST_DIR/raw3.img" 128M
create_device "$TEST_DIR/raw2.img" /dev/mwvd1
create_device "$TEST_DIR/raw3.img" /dev/mwvd1
test_ok

test_case "Independent I/O on both devices"
dd if=/dev/urandom of="$TEST_DIR/m1.dat" bs=1K count=4 2>/dev/null
dd if=/dev/urandom of="$TEST_DIR/m2.dat" bs=1K count=4 2>/dev/null
sudo dd if="$TEST_DIR/m1.dat" of=/dev/mwvd1 bs=1K seek=0 conv=notrunc 2>/dev/null
sudo dd if="$TEST_DIR/m2.dat" of=/dev/mwvd1 bs=1K seek=100 conv=notrunc 2>/dev/null
sudo dd if=/dev/mwvd1 of="$TEST_DIR/nm1.dat" bs=1K count=4 2>/dev/null
sudo dd if=/dev/mwvd1 of="$TEST_DIR/nm2.dat" bs=1K skip=100 count=4 2>/dev/null
cmp "$TEST_DIR/m1.dat" "$TEST_DIR/nm1.dat" && cmp "$TEST_DIR/m2.dat" "$TEST_DIR/nm2.dat"
test_ok

test_case "Detach all devices"
detach_device /dev/mwvd1
detach_device /dev/mwvd1
! [ -b /dev/mwvd1 ] && ! [ -b /dev/mwvd1 ]
test_ok

test_section "Cleanup"

test_case "Unload module"
unload_module
! lsmod | grep -q "^$MWVD_MODULE"
test_ok

rm -rf "$TEST_DIR"
summary
exit $?
