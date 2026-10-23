#!/bin/bash
# SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
# Copyright (c) 2026 LU WEIFENG (wave.lu@gmail.com)
# 版权所有 (c) 2026 陆伟峰
#
# 分区和文件系统测试：fdisk/mkfs/mount
#
source "$(dirname "$0")/test_common.sh"

cd "$(dirname "$0")/.." || die "Cannot cd to project root"
TEST_DIR="/tmp/mwvd_test_part"
mkdir -p "$TEST_DIR"
MNT_POINT="$TEST_DIR/mnt"

load_module

# ============================================================
test_section "Partition Table — RAW"
# ============================================================

test_case "Create 128MB RAW image"
create_raw_img "$TEST_DIR/raw.img" 128M
create_device "$TEST_DIR/raw.img" /dev/mwvd1
test_ok

test_case "Create MBR partition table with fdisk"
# One partition covering entire disk
echo -e "o\nn\np\n1\n\n\nw" | sudo fdisk /dev/mwvd1 >/dev/null 2>&1
sleep 1
sudo blockdev --rereadpt /dev/mwvd1 2>/dev/null || true
sleep 1
test_ok

test_case "Verify partition detected"
ls /dev/mwvd1p1 2>/dev/null
test_ok

test_case "Format partition as ext4"
sudo mkfs.ext4 -F /dev/mwvd1p1 2>/dev/null
test_ok

test_case "Mount and write file"
mkdir -p "$MNT_POINT"
sudo mount /dev/mwvd1p1 "$MNT_POINT" 2>/dev/null
echo "MWVD test file" | sudo tee "$MNT_POINT/test.txt" >/dev/null
sudo umount "$MNT_POINT"
test_ok

test_case "Re-mount and verify file content"
sudo mount /dev/mwvd1p1 "$MNT_POINT" 2>/dev/null
grep -q "MWVD test file" "$MNT_POINT/test.txt"
sudo umount "$MNT_POINT"
test_ok

detach_device /dev/mwvd1

# ============================================================
test_section "Partition Table — GPT"
# ============================================================

test_case "Create dynamic VHDX + GPT + ext4"
create_vhdx_img "$TEST_DIR/gpt.vhdx" 128M dynamic
create_device "$TEST_DIR/gpt.vhdx" /dev/mwvd1
test_ok

test_case "Create GPT partition table"
echo -e "g\nn\n1\n\n\nw" | sudo fdisk /dev/mwvd1 >/dev/null 2>&1
sleep 1
sudo blockdev --rereadpt /dev/mwvd1 2>/dev/null || true
sleep 1
test_ok

test_case "GPT: format and mount"
sudo mkfs.ext4 -F /dev/mwvd1p1 2>/dev/null
sudo mount /dev/mwvd1p1 "$MNT_POINT" 2>/dev/null
dd if=/dev/urandom of="$TEST_DIR/gpt_file.dat" bs=1M count=4 2>/dev/null
sudo cp "$TEST_DIR/gpt_file.dat" "$MNT_POINT/large.bin"
sync
sudo umount "$MNT_POINT"
test_ok

test_case "GPT: verify large file"
sudo mount /dev/mwvd1p1 "$MNT_POINT" 2>/dev/null
cmp "$TEST_DIR/gpt_file.dat" "$MNT_POINT/large.bin"
sudo umount "$MNT_POINT"
test_ok

detach_device /dev/mwvd1

# ============================================================
test_section "Differencing + Partition"
# ============================================================

skip_diff=false
if ! $QEMU_CAN_VHDX_DIFF && ! $QEMU_CAN_VHD_DIFF; then
    test_case "qemu-img: differencing not supported"
    test_skip "neither VHDX nor VHD differencing available"
    skip_diff=true
fi

# Use VHD format (more likely supported by some qemu-img versions)
DIFF_FMT=vhd
$QEMU_CAN_VHD_DIFF || DIFF_FMT=vhdx
$QEMU_CAN_VHDX_DIFF || $QEMU_CAN_VHD_DIFF || DIFF_FMT=""

if ! $skip_diff && [ -n "$DIFF_FMT" ]; then
    test_case "Create parent with ext4 filesystem ($DIFF_FMT)"
    if [ "$DIFF_FMT" = "vhd" ]; then
        create_vhd_img "$TEST_DIR/dp.$DIFF_FMT" 64M dynamic
    else
        create_vhdx_img "$TEST_DIR/dp.$DIFF_FMT" 64M dynamic
    fi
    test_ok

    test_case "Format parent and write data"
    create_device "$TEST_DIR/dp.$DIFF_FMT" /dev/mwvd1
    echo -e "g\nn\n1\n\n\nw" | sudo fdisk /dev/mwvd1 >/dev/null 2>&1
    sleep 1
    sudo blockdev --rereadpt /dev/mwvd1 2>/dev/null || true
    sleep 1
    sudo mkfs.ext4 -F /dev/mwvd1p1 2>/dev/null
    sudo mount /dev/mwvd1p1 "$MNT_POINT" 2>/dev/null
    echo "Parent data" | sudo tee "$MNT_POINT/parent.txt" >/dev/null
    sudo umount "$MNT_POINT"
    detach_device /dev/mwvd1
    test_ok

    test_case "Create child differencing ($DIFF_FMT)"
    if [ "$DIFF_FMT" = "vhd" ]; then
        create_vhd_img "$TEST_DIR/dc.$DIFF_FMT" 64M differencing "$TEST_DIR/dp.$DIFF_FMT"
    else
        create_vhdx_img "$TEST_DIR/dc.$DIFF_FMT" 64M differencing "$TEST_DIR/dp.$DIFF_FMT"
    fi
    test_ok

    test_case "Attach child, verify parent data visible"
    create_device "$TEST_DIR/dc.$DIFF_FMT" /dev/mwvd1
    sleep 1
    ls /dev/mwvd1p1 2>/dev/null
    sudo mount /dev/mwvd1p1 "$MNT_POINT" 2>/dev/null
    grep -q "Parent data" "$MNT_POINT/parent.txt"
    sudo umount "$MNT_POINT"
    test_ok

    test_case "Child overwrites file, parent unchanged"
    sudo mount /dev/mwvd1p1 "$MNT_POINT" 2>/dev/null
    echo "Child data" | sudo tee "$MNT_POINT/parent.txt" >/dev/null
    sudo umount "$MNT_POINT"
    detach_device /dev/mwvd1
    test_ok

    test_case "Re-attach child, verify child's version"
    create_device "$TEST_DIR/dc.$DIFF_FMT" /dev/mwvd1
    sudo mount /dev/mwvd1p1 "$MNT_POINT" 2>/dev/null
    grep -q "Child data" "$MNT_POINT/parent.txt"
    sudo umount "$MNT_POINT"
    detach_device /dev/mwvd1
    test_ok

    test_case "Re-attach parent, verify parent unchanged"
    create_device "$TEST_DIR/dp.$DIFF_FMT" /dev/mwvd1
    sudo mount /dev/mwvd1p1 "$MNT_POINT" 2>/dev/null
    grep -q "Parent data" "$MNT_POINT/parent.txt"
    sudo umount "$MNT_POINT"
    detach_device /dev/mwvd1
    test_ok
else
    for tc in "Create parent" "Format parent" "Create child" "Attach child" "Child overwrite" "Re-attach child" "Re-attach parent"; do
        test_case "$tc"
        test_skip "differencing not available"
    done
fi

# ============================================================
test_section "Cleanup"
# ============================================================
unload_module
rm -rf "$TEST_DIR"
summary
exit $?
