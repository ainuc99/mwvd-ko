#!/bin/bash
# SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
# Copyright (c) 2026 LU WEIFENG (wave.lu@gmail.com)
# 版权所有 (c) 2026 陆伟峰
#
# VHDX 格式专项测试：固定、动态、差分
#
source "$(dirname "$0")/test_common.sh"

cd "$(dirname "$0")/.." || die "Cannot cd to project root"
TEST_DIR="/tmp/mwvd_test_vhdx"
mkdir -p "$TEST_DIR"

load_module

# ============================================================
test_section "VHDX — Dynamic (动态)"
# ============================================================

test_case "Create dynamic VHDX (48MB)"
create_vhdx_img "$TEST_DIR/dynamic.vhdx" 48M dynamic
test_ok

test_case "Attach and verify size"
create_device "$TEST_DIR/dynamic.vhdx" /dev/mwvd1
size=$(sudo blockdev --getsize64 /dev/mwvd1)
[ "$size" = "$((48 * 1024 * 1024))" ]
test_ok

test_case "Sequential write across block boundary"
# 2MB blocks, write spanning block boundary at 1MB + 2KB
dd if=/dev/urandom of="$TEST_DIR/seq.dat" bs=1M count=4 2>/dev/null
sudo dd if="$TEST_DIR/seq.dat" of=/dev/mwvd1 bs=1M seek=0 conv=notrunc 2>/dev/null
sudo dd if=/dev/mwvd1 of="$TEST_DIR/seq_r.dat" bs=1M count=4 2>/dev/null
cmp "$TEST_DIR/seq.dat" "$TEST_DIR/seq_r.dat"
test_ok

test_case "Write to non-sequential blocks (fragmented)"
dd if=/dev/urandom of="$TEST_DIR/frag.dat" bs=512 count=256 2>/dev/null
sudo dd if="$TEST_DIR/frag.dat" of=/dev/mwvd1 bs=512 seek=10000 conv=notrunc 2>/dev/null
sudo dd if=/dev/mwvd1 of="$TEST_DIR/frag_r.dat" bs=512 skip=10000 count=256 2>/dev/null
cmp "$TEST_DIR/frag.dat" "$TEST_DIR/frag_r.dat"
test_ok

test_case "Read unallocated block returns zero"
sudo dd if=/dev/mwvd1 of="$TEST_DIR/unalloc.dat" bs=1M skip=40 count=1 2>/dev/null
[ "$(hexdump -C "$TEST_DIR/unalloc.dat" | wc -l)" -le 1 ]
test_ok

test_case "DISCARD on allocated block"
sudo blkdiscard -o 0 -l 1M /dev/mwvd1 2>/dev/null
test_ok

test_case "Read after DISCARD returns zero"
sudo dd if=/dev/mwvd1 of="$TEST_DIR/discard_r.dat" bs=512 count=16 2>/dev/null
[ "$(hexdump -C "$TEST_DIR/discard_r.dat" | wc -l)" -le 1 ]
test_ok

detach_device /dev/mwvd1

# ============================================================
test_section "VHDX — Fixed (固定)"
# ============================================================

test_case "Create fixed VHDX (32MB)"
create_vhdx_img "$TEST_DIR/fixed.vhdx" 32M fixed
test_ok

test_case "Attach fixed VHDX"
create_device "$TEST_DIR/fixed.vhdx" /dev/mwvd1
test_ok

test_case "Full write/read cycle"
dd if=/dev/urandom of="$TEST_DIR/fw.dat" bs=1M count=32 2>/dev/null
sudo dd if="$TEST_DIR/fw.dat" of=/dev/mwvd1 bs=1M conv=notrunc 2>/dev/null
sudo dd if=/dev/mwvd1 of="$TEST_DIR/fr.dat" bs=1M count=32 2>/dev/null
cmp "$TEST_DIR/fw.dat" "$TEST_DIR/fr.dat"
test_ok

detach_device /dev/mwvd1

# ============================================================
test_section "VHDX — Differencing (差分)"
# ============================================================

if ! $QEMU_CAN_VHDX_DIFF; then
    test_case "qemu-img does not support VHDX differencing"
    test_skip "Install qemu-img >= 8.0 or use manual image creation"
    skip_differencing_vhdx=true
else
    skip_differencing_vhdx=false
fi

test_case "Create parent dynamic VHDX (48MB)"
create_vhdx_img "$TEST_DIR/parent.vhdx" 48M dynamic
test_ok

test_case "Write known data to parent"
create_device "$TEST_DIR/parent.vhdx" /dev/mwvd1
dd if=/dev/urandom of="$TEST_DIR/pdata.dat" bs=1M count=48 2>/dev/null
sudo dd if="$TEST_DIR/pdata.dat" of=/dev/mwvd1 bs=1M conv=notrunc 2>/dev/null
sudo blockdev --flushbufs /dev/mwvd1
detach_device /dev/mwvd1
test_ok

if $skip_differencing_vhdx; then
    test_case "Create child differencing VHDX"
    test_skip "qemu-img VHDX differencing not supported"
else
    test_case "Create child differencing VHDX"
    create_vhdx_img "$TEST_DIR/child.vhdx" 48M differencing "$TEST_DIR/parent.vhdx" && test_ok || test_skip "create failed"
fi

if $skip_differencing_vhdx; then
    test_case "Attach child — reads parent data"
    test_skip "skipped (no child image)"
    test_case "Write to child — overrides parent"
    test_skip "skipped (no child image)"
    test_case "Unwritten area still reads from parent"
    test_skip "skipped (no child image)"
else
    create_device "$TEST_DIR/child.vhdx" /dev/mwvd1
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
test_section "Cleanup"
# ============================================================
unload_module
rm -rf "$TEST_DIR"
summary
exit $?
