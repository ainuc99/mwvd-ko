#!/bin/sh
# SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
# Copyright (c) 2026 LU WEIFENG (wave.lu@gmail.com)
# 版权所有 (c) 2026 陆伟峰
#
# MWVD 休眠自动配置脚本主机端测试
#
# 不加载内核模块、不修改 /etc，使用临时 sysfs/config 与 mock 命令验证：
#   - helper 的无声明 / usage 非 swap / 非法 UUID / 分区缺失分支
#   - initramfs-tools / dracut / mkinitcpio 适配器的配置生成与幂等性
#
# 用法：./test_hibernate_scripts.sh
#

set -eu

TEST_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$TEST_DIR/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

PASS=0
FAIL=0

ok() {
  PASS=$((PASS + 1))
  printf 'PASS: %s\n' "$*"
}

bad() {
  FAIL=$((FAIL + 1))
  printf 'FAIL: %s\n' "$*" >&2
}

assert_eq() {
  local want="$1" got="$2" name="$3"
  if [ "$want" = "$got" ]; then
    ok "$name"
  else
    bad "$name (want='$want' got='$got')"
  fi
}

assert_file_contains() {
  local file="$1" needle="$2" name="$3"
  if [ -f "$file" ] && grep -Fq "$needle" "$file"; then
    ok "$name"
  else
    bad "$name (missing '$needle' in $file)"
  fi
}

make_mocks() {
  mkdir -p "$TMP/bin" "$TMP/lib" "$TMP/etc"
  cp "$REPO_DIR/mwvd-distro.sh" "$TMP/lib/"
  cp "$REPO_DIR/mwvd-hibernate-setup" "$TMP/"

  cat > "$TMP/bin/blkid" <<'EOF'
#!/bin/sh
case "$*" in
  *TYPE*) printf '%s\n' "swap" ;;
  *UUID*) printf '%s\n' "7d8f3c2a-1b4e-4f6a-9c0d-123456789abc" ;;
esac
EOF
  cat > "$TMP/bin/blockdev" <<'EOF'
#!/bin/sh
printf '%s\n' "68719476736"
EOF
  cat > "$TMP/bin/update-initramfs" <<EOF
#!/bin/sh
printf '%s\n' "\$*" >> "$TMP/update-initramfs.calls"
EOF
  cat > "$TMP/bin/dracut" <<EOF
#!/bin/sh
if [ "\${1:-}" = "--help" ]; then
  printf '%s\n' "  --regenerate-all"
  exit 0
fi
printf '%s\n' "\$*" >> "$TMP/dracut.calls"
EOF
  cat > "$TMP/bin/mkinitcpio" <<EOF
#!/bin/sh
printf '%s\n' "\$*" >> "$TMP/mkinitcpio.calls"
EOF
  cat > "$TMP/bin/update-grub" <<EOF
#!/bin/sh
printf '%s\n' "\$*" >> "$TMP/update-grub.calls"
EOF
  chmod 0755 "$TMP/bin/"* "$TMP/mwvd-hibernate-setup"
}

run_helper() {
  local sysfs="$1" log="$2"
  set +e
  MWVD_LOG_TAG=test \
  MWVD_LIB_DIR="$TMP/lib" \
  MWVD_HIBERNATE_SYSFS="$TMP/$sysfs" \
  MWVD_HIBERNATE_CONFIG="$TMP/etc/hibernate.conf" \
  MWVD_HIBERNATE_DEFAULT="$TMP/etc/default" \
  MWVD_HIBERNATE_LOCK="$TMP/lock" \
  MWVD_HIBERNATE_SYSFS_WAIT_SECONDS=0 \
  MWVD_HIBERNATE_DEVICE_WAIT_SECONDS=0 \
  PATH="$TMP/bin:$PATH" \
    "$TMP/mwvd-hibernate-setup" apply >"$log" 2>&1
  local rc=$?
  return "$rc"
}

test_helper() {
  printf '%s\n' 'valid=0' > "$TMP/sysfs-none"
  printf '%s\n' 'valid=1' 'usage=2' 'resolved=0' \
    'part_uuid=5028fa50-0079-4c40-b240-abfaf28693ea' > "$TMP/sysfs-usage2"
  printf '%s\n' 'valid=1' 'usage=1' 'resolved=0' \
    'part_uuid=not-a-uuid' > "$TMP/sysfs-baduuid"
  printf '%s\n' 'valid=1' 'usage=1' 'resolved=0' \
    'part_uuid=5028fa50-0079-4c40-b240-abfaf28693ea' > "$TMP/sysfs-missing"

  local rc

  set +e
  run_helper sysfs-none "$TMP/none.log"
  rc=$?
  set -e
  assert_eq 0 "$rc" "helper: no declaration returns 0"

  set +e
  run_helper sysfs-usage2 "$TMP/usage2.log"
  rc=$?
  set -e
  assert_eq 10 "$rc" "helper: usage != 1 returns 10"

  set +e
  run_helper sysfs-baduuid "$TMP/baduuid.log"
  rc=$?
  set -e
  assert_eq 20 "$rc" "helper: invalid UUID returns 20"

  set +e
  run_helper sysfs-missing "$TMP/missing.log"
  rc=$?
  set -e
  assert_eq 20 "$rc" "helper: missing partition returns 20"
}

test_adapters() {
  local rc

  # initramfs-tools：生成 RESUME，并在变更时刷新一次 initramfs
  MWVD_LOG_TAG=test \
  MWVD_BACKUP_DIR="$TMP/backups" \
  MWVD_INITRAMFS_RESUME_CONF="$TMP/etc/resume.conf" \
  MWVD_INITRD_TYPE=initramfs-tools \
  MWVD_HIBERNATE_MANAGE_BOOT=0 \
  MWVD_PROC_CMDLINE=/dev/null \
  PATH="$TMP/bin:$PATH" \
    sh -c '. "$1"; mwvd_adapter_apply p s d' sh "$TMP/lib/mwvd-distro.sh" \
    >/dev/null 2>&1
  rc=$?
  assert_eq 0 "$rc" "adapter initramfs-tools: apply returns 0"
  assert_file_contains "$TMP/etc/resume.conf" "RESUME=UUID=s" \
    "adapter initramfs-tools: writes RESUME"
  if [ -e "$TMP/etc/resume.conf.mwvd.bak" ]; then
    bad "adapter initramfs-tools: leaves .bak in conf.d"
  else
    ok "adapter initramfs-tools: backup outside conf.d"
  fi

  # 幂等：第二次不重复刷新
  MWVD_LOG_TAG=test \
  MWVD_BACKUP_DIR="$TMP/backups" \
  MWVD_INITRAMFS_RESUME_CONF="$TMP/etc/resume.conf" \
  MWVD_INITRD_TYPE=initramfs-tools \
  MWVD_HIBERNATE_MANAGE_BOOT=0 \
  MWVD_PROC_CMDLINE=/dev/null \
  PATH="$TMP/bin:$PATH" \
    sh -c '. "$1"; mwvd_adapter_apply p s d' sh "$TMP/lib/mwvd-distro.sh" \
    >/dev/null 2>&1
  assert_eq 1 "$(wc -l < "$TMP/update-initramfs.calls" 2>/dev/null || printf '0')" \
    "adapter initramfs-tools: idempotent"

  # dracut
  MWVD_LOG_TAG=test \
  MWVD_DRACUT_CONF="$TMP/etc/dracut.conf" \
  MWVD_INITRD_TYPE=dracut \
  MWVD_HIBERNATE_MANAGE_BOOT=0 \
  MWVD_PROC_CMDLINE=/dev/null \
  PATH="$TMP/bin:$PATH" \
    sh -c '. "$1"; mwvd_adapter_apply p s d' sh "$TMP/lib/mwvd-distro.sh" \
    >/dev/null 2>&1
  assert_file_contains "$TMP/etc/dracut.conf" 'add_dracutmodules+=" resume "' \
    "adapter dracut: writes module config"

  # mkinitcpio：block 之后插入 resume
  printf '%s\n' 'MODULES=()' \
    'HOOKS=(base udev autodetect modconf block filesystems keyboard fsck)' \
    > "$TMP/etc/mkinitcpio.conf"
  MWVD_LOG_TAG=test \
  MWVD_MKINITCPIO_CONF="$TMP/etc/mkinitcpio.conf" \
  MWVD_INITRD_TYPE=mkinitcpio \
  MWVD_HIBERNATE_MANAGE_BOOT=0 \
  MWVD_PROC_CMDLINE=/dev/null \
  PATH="$TMP/bin:$PATH" \
    sh -c '. "$1"; mwvd_adapter_apply p s d' sh "$TMP/lib/mwvd-distro.sh" \
    >/dev/null 2>&1
  assert_file_contains "$TMP/etc/mkinitcpio.conf" "block resume filesystems" \
    "adapter mkinitcpio: inserts resume hook"
}

test_boot_cmdline() {
  local rc uuid="7d8f3c2a-1b4e-4f6a-9c0d-123456789abc"

  mkdir -p "$TMP/bls"
  printf '%s\n' 'title A' 'version 1' \
    'options root=UUID=root ro rhgb quiet' > "$TMP/bls/a.conf"
  printf '%s\n' 'title B' 'version 2' \
    'options root=UUID=root ro quiet resume=UUID=old console=ttyS0' \
    > "$TMP/bls/b.conf"
  printf '%s\n' 'GRUB_TIMEOUT=5' 'GRUB_CMDLINE_LINUX="rhgb quiet"' \
    > "$TMP/grub"
  printf '%s\n' 'root=UUID=root ro' > "$TMP/kernel-cmdline"
  printf '%s\n' 'BOOT_IMAGE=/vmlinuz ro' > "$TMP/proc-cmdline"

  run_cmdline_conf() {
    local bls_dir="${1:-$TMP/bls}" cmdline_file="${2:-/dev/null}"
    MWVD_LOG_TAG=test \
    MWVD_BLS_DIR="$bls_dir" \
    MWVD_GRUB_DEFAULT="$TMP/grub" \
    MWVD_KERNEL_CMDLINE_FILE="$TMP/kernel-cmdline" \
    MWVD_PROC_CMDLINE="$cmdline_file" \
    PATH="$TMP/bin:$PATH" \
      sh -c '. "$1"; mwvd_configure_resume_cmdline "$2"' \
      sh "$TMP/lib/mwvd-distro.sh" "$uuid" >/dev/null 2>&1
  }

  set +e
  run_cmdline_conf
  rc=$?
  set -e
  assert_eq 0 "$rc" "cmdline: configure returns 0"
  assert_file_contains "$TMP/bls/a.conf" "resume=UUID=$uuid" \
    "cmdline: appends resume to BLS entry without it"
  assert_file_contains "$TMP/bls/a.conf" "rhgb quiet" \
    "cmdline: keeps other BLS arguments"
  assert_file_contains "$TMP/bls/b.conf" "resume=UUID=$uuid" \
    "cmdline: replaces existing resume in BLS entry"
  if grep -Fq "resume=UUID=old" "$TMP/bls/b.conf"; then
    bad "cmdline: old resume left behind"
  else
    ok "cmdline: old resume removed"
  fi
  assert_file_contains "$TMP/bls/b.conf" "console=ttyS0" \
    "cmdline: keeps trailing BLS arguments"
  assert_file_contains "$TMP/grub" "resume=UUID=$uuid" \
    "cmdline: writes GRUB_CMDLINE_LINUX"
  assert_file_contains "$TMP/grub" "rhgb quiet" \
    "cmdline: keeps GRUB_CMDLINE_LINUX arguments"
  assert_file_contains "$TMP/kernel-cmdline" "resume=UUID=$uuid" \
    "cmdline: writes /etc/kernel/cmdline"

  # 幂等：内容不变时不重写（备份文件时间戳也不变）
  cp "$TMP/bls/a.conf" "$TMP/a.before"
  cp "$TMP/grub" "$TMP/grub.before"
  set +e
  run_cmdline_conf
  set -e
  if cmp -s "$TMP/bls/a.conf" "$TMP/a.before" &&
     cmp -s "$TMP/grub" "$TMP/grub.before"; then
    ok "cmdline: idempotent"
  else
    bad "cmdline: second run changed files"
  fi

  # 运行中的 cmdline 已经正确 ⇒ 直接返回 2，且不碰文件
  printf '%s\n' "BOOT_IMAGE=/vmlinuz ro resume=UUID=$uuid" > "$TMP/proc-ok"
  cp "$TMP/bls/a.conf" "$TMP/a.ok.before"
  set +e
  run_cmdline_conf "$TMP/bls" "$TMP/proc-ok"
  rc=$?
  set -e
  assert_eq 2 "$rc" "cmdline: returns 2 when already active"
  if cmp -s "$TMP/bls/a.conf" "$TMP/a.ok.before"; then
    ok "cmdline: no rewrite when already active"
  else
    bad "cmdline: rewrote BLS although already active"
  fi

  # 没有 BLS（Ubuntu/Arch grub 场景）⇒ 改 /etc/default/grub 并调用 update-grub
  rm -f "$TMP/update-grub.calls"
  set +e
  run_cmdline_conf "$TMP/no-such-bls" "$TMP/proc-cmdline"
  rc=$?
  set -e
  assert_eq 0 "$rc" "cmdline: grub-only path returns 0"
  if [ -f "$TMP/update-grub.calls" ]; then
    ok "cmdline: regenerates grub when BLS absent"
  else
    bad "cmdline: update-grub not invoked"
  fi
}

test_hibernate_mode() {
  local rc mode_conf power_disk

  mode_conf="$TMP/sleep.conf"
  power_disk="$TMP/power-disk"
  rm -f "$mode_conf"
  : > "$power_disk"

  run_sleep_mode() {
    MWVD_LOG_TAG=test \
    MWVD_SLEEP_CONF="$mode_conf" \
    MWVD_POWER_DISK="$power_disk" \
    MWVD_HIBERNATE_DISK_MODE="${1:-shutdown}" \
    PATH="$TMP/bin:$PATH" \
      sh -c '. "$1"; mwvd_configure_hibernate_mode' \
      sh "$TMP/lib/mwvd-distro.sh" >/dev/null 2>&1
  }

  # 1) 无 sleep.conf ⇒ 新建
  set +e
  run_sleep_mode
  rc=$?
  set -e
  assert_eq 0 "$rc" "sleep-mode: creates sleep.conf"
  assert_file_contains "$mode_conf" "[Sleep]" \
    "sleep-mode: adds [Sleep] section"
  assert_file_contains "$mode_conf" "HibernateMode=shutdown" \
    "sleep-mode: writes HibernateMode=shutdown"
  assert_eq shutdown "$(cat "$power_disk")" \
    "sleep-mode: sets /sys/power/disk immediately"

  # 2) 幂等
  set +e
  run_sleep_mode
  rc=$?
  set -e
  assert_eq 2 "$rc" "sleep-mode: idempotent"

  # 3) 已有 HibernateMode=platform ⇒ 替换，其它键与段保留
  printf '%s\n' '[Sleep]' 'HibernateMode=platform' 'SuspendMode=suspend' \
    '[Hibernate]' 'HibernateDelaySec=10min' > "$mode_conf"
  set +e
  run_sleep_mode
  rc=$?
  set -e
  assert_eq 0 "$rc" "sleep-mode: rewrites existing value"
  assert_file_contains "$mode_conf" "HibernateMode=shutdown" \
    "sleep-mode: value replaced"
  assert_file_contains "$mode_conf" "SuspendMode=suspend" \
    "sleep-mode: keeps other keys"
  assert_file_contains "$mode_conf" "HibernateDelaySec=10min" \
    "sleep-mode: keeps other sections"
  if grep -Fq "HibernateMode=platform" "$mode_conf"; then
    bad "sleep-mode: old value left behind"
  else
    ok "sleep-mode: old value removed"
  fi

  # 4) 有 [Sleep] 段但无 HibernateMode ⇒ 段内插入
  printf '%s\n' '[Sleep]' 'SuspendMode=suspend' '[Other]' 'X=1' > "$mode_conf"
  set +e
  run_sleep_mode
  rc=$?
  set -e
  assert_eq 0 "$rc" "sleep-mode: inserts into existing [Sleep]"
  if awk '/^\[Sleep\]/ {s=1} /^\[Other\]/ {s=0}
          /^HibernateMode=shutdown/ {if (s) f=1}
          END {exit !f}' "$mode_conf"; then
    ok "sleep-mode: inserted inside [Sleep] section"
  else
    bad "sleep-mode: inserted outside [Sleep] section"
  fi

  # 5) 无 [Sleep] 段 ⇒ 追加
  printf '%s\n' '[Other]' 'X=1' > "$mode_conf"
  set +e
  run_sleep_mode
  rc=$?
  set -e
  assert_eq 0 "$rc" "sleep-mode: appends section when absent"
  assert_file_contains "$mode_conf" "[Sleep]" "sleep-mode: appended [Sleep]"
  assert_file_contains "$mode_conf" "HibernateMode=shutdown" \
    "sleep-mode: appended value"

  # 6) mode=platform ⇒ 不管理（返回 2，文件不动）
  printf '%s\n' '[Sleep]' 'HibernateMode=platform' > "$mode_conf"
  cp "$mode_conf" "$TMP/sleep.platform.before"
  set +e
  run_sleep_mode platform
  rc=$?
  set -e
  assert_eq 2 "$rc" "sleep-mode: platform returns 2"
  if cmp -s "$mode_conf" "$TMP/sleep.platform.before"; then
    ok "sleep-mode: platform leaves file untouched"
  else
    bad "sleep-mode: platform modified file"
  fi
}

sh -n "$REPO_DIR/mwvd-hibernate-setup"
sh -n "$REPO_DIR/mwvd-distro.sh"
make_mocks
test_helper
test_adapters
test_boot_cmdline
test_hibernate_mode

echo "========================================"
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
