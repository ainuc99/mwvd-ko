#!/bin/bash
# SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
# Copyright (c) 2026 LU WEIFENG (wave.lu@gmail.com)
# 版权所有 (c) 2026 陆伟峰
#
# VPC/VHD 格式专项测试：固定、动态、差分
#
source "$(dirname "$0")/test_common.sh"

cd "$(dirname "$0")/.." || die "Cannot cd to project root"
TEST_DIR="/tmp/mwvd_test_vpc"
mkdir -p "$TEST_DIR"

load_module

# ============================================================
test_section "VHD — Dynamic (动态)"
# ============================================================

test_case "Create dynamic VHD (48MB)"
create_vhd_img "$TEST_DIR/dynamic.vhd" 48M dynamic
test_ok

test_case "Attach and verify size"
create_device "$TEST_DIR/dynamic.vhd" /dev/mwvd1
size=$(sudo blockdev --getsize64 /dev/mwvd1)
[ "$size" = "$((48 * 1024 * 1024))" ]
test_ok

test_case "Sequential write/read"
dd if=/dev/urandom of="$TEST_DIR/seq.dat" bs=1M count=4 2>/dev/null
sudo dd if="$TEST_DIR/seq.dat" of=/dev/mwvd1 bs=1M seek=0 conv=notrunc 2>/dev/null
sudo dd if=/dev/mwvd1 of="$TEST_DIR/seq_r.dat" bs=1M count=4 2>/dev/null
cmp "$TEST_DIR/seq.dat" "$TEST_DIR/seq_r.dat"
test_ok

test_case "Write at high sector number"
dd if=/dev/urandom of="$TEST_DIR/high.dat" bs=512 count=128 2>/dev/null
sudo dd if="$TEST_DIR/high.dat" of=/dev/mwvd1 bs=512 seek=90000 conv=notrunc 2>/dev/null
sudo dd if=/dev/mwvd1 of="$TEST_DIR/high_r.dat" bs=512 skip=90000 count=128 2>/dev/null
cmp "$TEST_DIR/high.dat" "$TEST_DIR/high_r.dat"
test_ok

test_case "DISCARD on allocated block"
sudo blkdiscard -o 1M -l 2M /dev/mwvd1 2>/dev/null
test_ok

test_case "Read after DISCARD returns zero"
sudo dd if=/dev/mwvd1 of="$TEST_DIR/discard_r.dat" bs=512 skip=$((2048)) count=16 2>/dev/null
[ "$(hexdump -C "$TEST_DIR/discard_r.dat" | grep -v "0000 0000 0000 0000" | wc -l)" -le 1 ]
test_ok

detach_device /dev/mwvd1

# ============================================================
test_section "VHD — Fixed (固定)"
# ============================================================

test_case "Create fixed VHD (32MB)"
create_vhd_img "$TEST_DIR/fixed.vhd" 32M fixed
test_ok

test_case "Attach fixed VHD"
create_device "$TEST_DIR/fixed.vhd" /dev/mwvd1
test_ok

test_case "Full write/read cycle"
dd if=/dev/urandom of="$TEST_DIR/fw.dat" bs=1M count=32 2>/dev/null
sudo dd if="$TEST_DIR/fw.dat" of=/dev/mwvd1 bs=1M conv=notrunc 2>/dev/null
sudo dd if=/dev/mwvd1 of="$TEST_DIR/fr.dat" bs=1M count=32 2>/dev/null
cmp "$TEST_DIR/fw.dat" "$TEST_DIR/fr.dat"
test_ok

detach_device /dev/mwvd1

# ============================================================
test_section "VHD — Differencing (差分)"
# ============================================================

skip_differencing_vhd=false
if ! $QEMU_CAN_VHD_DIFF; then
    test_case "qemu-img: VHD differencing not supported"
    test_skip "Install qemu-img >= 8.0"
    skip_differencing_vhd=true
fi

test_case "Create parent dynamic VHD (48MB)"
create_vhd_img "$TEST_DIR/parent.vhd" 48M dynamic
test_ok

test_case "Write known data to parent"
create_device "$TEST_DIR/parent.vhd" /dev/mwvd1
dd if=/dev/urandom of="$TEST_DIR/pdata.dat" bs=1M count=48 2>/dev/null
sudo dd if="$TEST_DIR/pdata.dat" of=/dev/mwvd1 bs=1M conv=notrunc 2>/dev/null
sudo blockdev --flushbufs /dev/mwvd1
detach_device /dev/mwvd1
test_ok

if $skip_differencing_vhd; then
    test_case "Create child differencing VHD"
    test_skip "qemu-img VHD differencing not supported"
    test_case "Attach child — reads parent data"
    test_skip "skipped (no child image)"
    test_case "Write to child — overrides parent"
    test_skip "skipped (no child image)"
    test_case "Unwritten area still reads from parent"
    test_skip "skipped (no child image)"
    test_case "Re-attach child (path resolution)"
    test_skip "skipped (no child image)"
else
    test_case "Create child differencing VHD"
    create_vhd_img "$TEST_DIR/child.vhd" 48M differencing "$TEST_DIR/parent.vhd" && test_ok || test_skip "create failed"

    if [ -f "$TEST_DIR/child.vhd" ]; then
        test_case "Attach child — reads parent data"
        create_device "$TEST_DIR/child.vhd" /dev/mwvd1
        sudo dd if=/dev/mwvd1 of="$TEST_DIR/cr.dat" bs=1M count=48 2>/dev/null
        cmp "$TEST_DIR/pdata.dat" "$TEST_DIR/cr.dat"
        test_ok

        test_case "Write to child — overrides parent"
        dd if=/dev/urandom of="$TEST_DIR/cw.dat" bs=4K count=1 2>/dev/null
        sudo dd if="$TEST_DIR/cw.dat" of=/dev/mwvd1 bs=4K seek=0 conv=notrunc 2>/dev/null
        sudo dd if=/dev/mwvd1 of="$TEST_DIR/cw_r.dat" bs=4K count=1 2>/dev/null
        cmp "$TEST_DIR/cw.dat" "$TEST_DIR/cw_r.dat"
        test_ok

        test_case "Unwritten area still reads from parent"
        sudo dd if=/dev/mwvd1 of="$TEST_DIR/cp_r.dat" bs=1M skip=1 count=47 2>/dev/null
        dd if="$TEST_DIR/pdata.dat" bs=1M skip=1 count=47 of="$TEST_DIR/cp_expected.dat" 2>/dev/null
        cmp "$TEST_DIR/cp_expected.dat" "$TEST_DIR/cp_r.dat"
        test_ok

        detach_device /dev/mwvd1
    fi

    # ============================================================
    test_section "VHD — Parent Locator (路径解析)"
    # ============================================================

    if [ -f "$TEST_DIR/child.vhd" ]; then
        test_case "Re-attach child (path resolution)"
        create_device "$TEST_DIR/child.vhd" /dev/mwvd1
        sudo dd if=/dev/mwvd1 of="$TEST_DIR/re.dat" bs=1M count=48 2>/dev/null
        cmp "$TEST_DIR/pdata.dat" "$TEST_DIR/re.dat"
        test_ok

        detach_device /dev/mwvd1
    else
        test_case "Re-attach child (path resolution)"
        test_skip "skipped (no child image)"
    fi
fi

# ============================================================
test_section "Cleanup"
# ============================================================
unload_module
rm -rf "$TEST_DIR"
summary
exit $?
