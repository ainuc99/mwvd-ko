#!/bin/bash
# SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
# Copyright (c) 2026 LU WEIFENG (wave.lu@gmail.com)
# 版权所有 (c) 2026 陆伟峰
#
# 压力测试：并发 I/O、大量小写入、重复挂载卸载
#
source "$(dirname "$0")/test_common.sh"

cd "$(dirname "$0")/.." || die "Cannot cd to project root"
TEST_DIR="/tmp/mwvd_test_stress"
mkdir -p "$TEST_DIR"
MNT_POINT="$TEST_DIR/mnt"
mkdir -p "$MNT_POINT"

load_module

# ============================================================
test_section "Small Random Writes (4K granularity)"
# ============================================================

test_case "Create 256MB RAW image for stress"
create_raw_img "$TEST_DIR/stress.img" 256M
create_device "$TEST_DIR/stress.img" /dev/mwvd1
test_ok

test_case "500 random 4K writes across the device"
for i in $(seq 1 500); do
    sector=$((RANDOM % 524000))
    dd if=/dev/urandom of="$TEST_DIR/sw.dat" bs=4K count=1 2>/dev/null
    sudo dd if="$TEST_DIR/sw.dat" of=/dev/mwvd1 bs=4K seek=$sector conv=notrunc 2>/dev/null || {
        test_fail "Write failed at sector $sector"
        continue
    }
done
test_ok

test_case "Verify random reads succeed"
for i in $(seq 1 50); do
    sector=$((RANDOM % 524000))
    sudo dd if=/dev/mwvd1 of=/dev/null bs=4K skip=$sector count=1 2>/dev/null || {
        test_fail "Read failed at sector $sector"
        continue
    }
done
test_ok

detach_device /dev/mwvd1

# ============================================================
test_section "Concurrent Device Operations"
# ============================================================

test_case "Create 3 devices concurrently"
create_raw_img "$TEST_DIR/c1.img" 64M
create_raw_img "$TEST_DIR/c2.img" 64M
create_raw_img "$TEST_DIR/c3.img" 64M

create_device "$TEST_DIR/c1.img" /dev/mwvd1 &
create_device "$TEST_DIR/c2.img" /dev/mwvd1 &
create_device "$TEST_DIR/c3.img" /dev/mwvd2 &
wait
sleep 2
test_ok

test_case "Concurrent reads across 3 devices"
dd if=/dev/urandom of="$TEST_DIR/cc.dat" bs=1M count=10 2>/dev/null
sudo dd if="$TEST_DIR/cc.dat" of=/dev/mwvd1 bs=1M conv=notrunc 2>/dev/null &
sudo dd if="$TEST_DIR/cc.dat" of=/dev/mwvd1 bs=1M conv=notrunc 2>/dev/null &
sudo dd if="$TEST_DIR/cc.dat" of=/dev/mwvd2 bs=1M conv=notrunc 2>/dev/null &
wait

sudo dd if=/dev/mwvd1 of="$TEST_DIR/cc0.dat" bs=1M count=10 2>/dev/null &
sudo dd if=/dev/mwvd1 of="$TEST_DIR/cc1.dat" bs=1M count=10 2>/dev/null &
sudo dd if=/dev/mwvd2 of="$TEST_DIR/cc2.dat" bs=1M count=10 2>/dev/null &
wait
cmp "$TEST_DIR/cc.dat" "$TEST_DIR/cc0.dat" && cmp "$TEST_DIR/cc.dat" "$TEST_DIR/cc1.dat" && cmp "$TEST_DIR/cc.dat" "$TEST_DIR/cc2.dat"
test_ok

detach_device /dev/mwvd1
detach_device /dev/mwvd1
detach_device /dev/mwvd2

# ============================================================
test_section "Repeated Mount/Unmount"
# ============================================================

test_case "Create VHDX + ext4 for mount stress"
create_vhdx_img "$TEST_DIR/mount_test.vhdx" 128M dynamic
create_device "$TEST_DIR/mount_test.vhdx" /dev/mwvd1
echo -e "g\nn\n1\n\n\nw" | sudo fdisk /dev/mwvd1 >/dev/null 2>&1
sleep 1
sudo blockdev --rereadpt /dev/mwvd1 2>/dev/null || true
sleep 1
sudo mkfs.ext4 -F /dev/mwvd1p1 2>/dev/null
test_ok

test_case "20 mount/unmount cycles"
for i in $(seq 1 20); do
    sudo mount /dev/mwvd1p1 "$MNT_POINT" 2>/dev/null || {
        test_fail "Mount failed at cycle $i"
        continue
    }
    echo "cycle_$i" | sudo tee "$MNT_POINT/cycle.txt" >/dev/null
    sudo umount "$MNT_POINT" 2>/dev/null || {
        test_fail "Unmount failed at cycle $i"
        continue
    }
done
test_ok

test_case "Verify last cycle data"
sudo mount /dev/mwvd1p1 "$MNT_POINT" 2>/dev/null
grep -q "cycle_20" "$MNT_POINT/cycle.txt"
sudo umount "$MNT_POINT"
test_ok

detach_device /dev/mwvd1

# ============================================================
test_section "DISCARD / WRITE_ZEROES Stress"
# ============================================================

test_case "Create VHDX for discard stress"
create_vhdx_img "$TEST_DIR/discard_stress.vhdx" 256M dynamic
create_device "$TEST_DIR/discard_stress.vhdx" /dev/mwvd1

# Write data to entire device
dd if=/dev/urandom of="$TEST_DIR/ds_data.dat" bs=1M count=256 2>/dev/null
sudo dd if="$TEST_DIR/ds_data.dat" of=/dev/mwvd1 bs=1M conv=notrunc 2>/dev/null
test_ok

test_case "Blkdiscard entire device"
sudo blkdiscard /dev/mwvd1 2>/dev/null
test_ok

test_case "Verify all zeros after full discard"
sudo dd if=/dev/mwvd1 of="$TEST_DIR/ds_zero.dat" bs=1M count=256 2>/dev/null
[ "$(hexdump -C "$TEST_DIR/ds_zero.dat" 2>/dev/null | grep -v "0000 0000 0000 0000" | wc -l)" -le 2 ]
test_ok

detach_device /dev/mwvd1

# ============================================================
test_section "Module Reload Persistence"
# ============================================================

test_case "VHDX: write, unload, reload, verify"
create_vhdx_img "$TEST_DIR/persist.vhdx" 64M dynamic
create_device "$TEST_DIR/persist.vhdx" /dev/mwvd1
dd if=/dev/urandom of="$TEST_DIR/persist.dat" bs=1M count=64 2>/dev/null
sudo dd if="$TEST_DIR/persist.dat" of=/dev/mwvd1 bs=1M conv=notrunc 2>/dev/null
sudo blockdev --flushbufs /dev/mwvd1
detach_device /dev/mwvd1
unload_module
load_module
create_device "$TEST_DIR/persist.vhdx" /dev/mwvd1
sudo dd if=/dev/mwvd1 of="$TEST_DIR/persist_r.dat" bs=1M count=64 2>/dev/null
cmp "$TEST_DIR/persist.dat" "$TEST_DIR/persist_r.dat"
detach_device /dev/mwvd1
test_ok

# ============================================================
test_section "Cleanup"
# ============================================================
unload_module
rm -rf "$TEST_DIR"
summary
exit $?
