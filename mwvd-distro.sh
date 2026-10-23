#!/bin/sh
# SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
# Copyright (c) 2026 LU WEIFENG (wave.lu@gmail.com)
# 版权所有 (c) 2026 陆伟峰
#
# 云固件 MWVD 休眠自动配置：发行版 / initramfs 适配库
#
# 本文件由 mwvd-hibernate-setup 通过 "." 加载，不单独执行。
# 只使用 POSIX shell 与发行版已有命令，不引入任何新的用户态二进制。
#
# 设计依据：design/HIBERNATE-UDEV-AUTOCONFIG-DESIGN.md §5.6
#
# 环境变量（测试用）：
#   MWVD_DISTRO_ID              指定发行版 ID（跳过 /etc/os-release）
#   MWVD_INITRD_TYPE            指定 initramfs 类型（跳过自动检测）
#   MWVD_OS_RELEASE             覆盖 /etc/os-release 路径
#   MWVD_INITRAMFS_RESUME_CONF  覆盖 initramfs-tools 配置文件路径
#   MWVD_DRACUT_CONF            覆盖 dracut 配置文件路径
#   MWVD_MKINITCPIO_CONF        覆盖 mkinitcpio.conf 路径
#   MWVD_PROC_CMDLINE           覆盖 /proc/cmdline 路径

: "${MWVD_LOG_TAG:=mwvd-hibernate}"

mwvd_log() {
  logger -t "$MWVD_LOG_TAG" "$*" 2>/dev/null || true
  printf '%s: %s\n' "$MWVD_LOG_TAG" "$*" >&2
}

mwvd_warn() {
  mwvd_log "WARN: $*"
}

#
# 输出发行版 ID（小写）。无法识别时输出 unknown。
#
mwvd_detect_distro() {
  if [ -n "${MWVD_DISTRO_ID:-}" ]; then
    printf '%s\n' "$MWVD_DISTRO_ID"
    return 0
  fi

  if [ -r "${MWVD_OS_RELEASE:-/etc/os-release}" ]; then
    (
      # shellcheck disable=SC1090
      . "${MWVD_OS_RELEASE:-/etc/os-release}"
      printf '%s\n' "${ID:-unknown}"
    )
    return 0
  fi

  if command -v lsb_release >/dev/null 2>&1; then
    lsb_release -si 2>/dev/null | tr '[:upper:]' '[:lower:]'
    return 0
  fi

  printf '%s\n' "unknown"
}

#
# 输出 initramfs 类型：initramfs-tools / dracut / mkinitcpio / unknown
#
mwvd_detect_initrd() {
  if [ -n "${MWVD_INITRD_TYPE:-}" ]; then
    printf '%s\n' "$MWVD_INITRD_TYPE"
    return 0
  fi

  local id
  id="$(mwvd_detect_distro)"

  case "$id" in
    debian|ubuntu|linuxmint|pop|neon|elementary|zorin|kali)
      if command -v update-initramfs >/dev/null 2>&1; then
        printf '%s\n' "initramfs-tools"
        return 0
      fi
      ;;
    fedora|rhel|centos|rocky|almalinux|ol|opensuse*|sles|sled)
      if command -v dracut >/dev/null 2>&1; then
        printf '%s\n' "dracut"
        return 0
      fi
      ;;
    arch|archarm|manjaro|endeavouros|garuda)
      if command -v mkinitcpio >/dev/null 2>&1; then
        printf '%s\n' "mkinitcpio"
        return 0
      fi
      ;;
  esac

  # 发行版识别失败或命令缺失时，按配置文件/命令回退探测
  if [ -f "${MWVD_MKINITCPIO_CONF:-/etc/mkinitcpio.conf}" ] &&
     command -v mkinitcpio >/dev/null 2>&1; then
    printf '%s\n' "mkinitcpio"
  elif [ -d "$(dirname "${MWVD_DRACUT_CONF:-/etc/dracut.conf.d/90-mwvd-resume.conf}")" ] &&
       command -v dracut >/dev/null 2>&1; then
    printf '%s\n' "dracut"
  elif command -v update-initramfs >/dev/null 2>&1; then
    printf '%s\n' "initramfs-tools"
  else
    printf '%s\n' "unknown"
  fi
}

#
# 检查内核命令行是否已带 resume=UUID=<swap_uuid>。
#
mwvd_cmdline_has_resume() {
  [ -r "${MWVD_PROC_CMDLINE:-/proc/cmdline}" ] || return 1
  tr ' ' '\n' < "${MWVD_PROC_CMDLINE:-/proc/cmdline}" |
    grep -Fqx "resume=UUID=$1"
}

#
# 在 BLS 条目（默认 /boot/loader/entries/*.conf）的 options 行上设置 resume=。
#
# 每条目：先删掉所有 resume=<...> token，再追加 <token>；其它参数原样保留。
# 返回：0 = 有文件被更新；2 = 全部已是目标内容；1 = 无可用条目或失败。
#
mwvd_bls_set_resume() {
  local token="$1"
  local dir="${MWVD_BLS_DIR:-/boot/loader/entries}"
  local f tmp rc found=0 changed=0 failed=0

  [ -d "$dir" ] || return 1

  for f in "$dir"/*.conf; do
    [ -f "$f" ] || continue
    grep -q '^options ' "$f" || continue
    found=1

    tmp="$(mktemp "${f}.mwvd.XXXXXX")" || {
      failed=1
      continue
    }
    if ! awk -v token="$token" '
        $1 == "options" {
          out = "options"
          for (i = 2; i <= NF; i++) {
            if ($i ~ /^resume=/) continue
            out = out " " $i
          }
          print out " " token
          next
        }
        { print }
      ' "$f" > "$tmp"; then
      rm -f "$tmp"
      failed=1
      continue
    fi

    mwvd_install_tmp "$f" "$tmp"
    rc=$?
    case "$rc" in
      0) changed=1 ;;
      2) ;;
      *) failed=1 ;;
    esac
  done

  [ "$found" = "1" ] || return 1
  [ "$changed" = "1" ] && return 0
  [ "$failed" = "1" ] && return 1
  return 2
}

#
# 在 /etc/default/grub 的 GRUB_CMDLINE_LINUX 上设置 resume=。
# 只改这一个变量（保留其它行与其它变量）。
# 返回：0 = 已更新；2 = 已是目标内容；1 = 文件缺失/格式不认识/失败。
#
mwvd_grub_default_set_resume() {
  local token="$1"
  local file="${MWVD_GRUB_DEFAULT:-/etc/default/grub}"
  local tmp

  [ -f "$file" ] || return 1
  grep -q '^GRUB_CMDLINE_LINUX="' "$file" || return 1

  tmp="$(mktemp "${file}.mwvd.XXXXXX")" || return 1
  if ! awk -v token="$token" '
      /^GRUB_CMDLINE_LINUX="/ {
        prefix = "GRUB_CMDLINE_LINUX=\""
        body = substr($0, length(prefix) + 1)
        sub(/"$/, "", body)
        n = split(body, a, " ")
        out = ""
        for (i = 1; i <= n; i++) {
          if (a[i] == "") continue
          if (a[i] ~ /^resume=/) continue
          out = out (out == "" ? "" : " ") a[i]
        }
        print prefix out (out == "" ? "" : " ") token "\""
        next
      }
      { print }
    ' "$file" > "$tmp"; then
    rm -f "$tmp"
    return 1
  fi

  mwvd_install_tmp "$file" "$tmp"
}

#
# 在 /etc/kernel/cmdline（systemd kernel-install 的模板）上设置 resume=。
# 文件通常只有一行；这里把整份内容当成一个参数列表处理。
# 返回：0 = 已更新；2 = 已是目标内容；1 = 文件缺失/失败。
#
mwvd_kernel_cmdline_set_resume() {
  local token="$1"
  local file="${MWVD_KERNEL_CMDLINE_FILE:-/etc/kernel/cmdline}"
  local tmp

  [ -f "$file" ] || return 1

  tmp="$(mktemp "${file}.mwvd.XXXXXX")" || return 1
  if ! awk -v token="$token" '
      BEGIN { out = "" }
      {
        for (i = 1; i <= NF; i++) {
          if ($i ~ /^resume=/) continue
          out = out (out == "" ? "" : " ") $i
        }
      }
      END { print out (out == "" ? "" : " ") token }
    ' "$file" > "$tmp"; then
    rm -f "$tmp"
    return 1
  fi

  mwvd_install_tmp "$file" "$tmp"
}

#
# 用发行版工具重新生成 grub 配置（仅在没有 BLS 条目时使用）。
#
mwvd_regenerate_grub() {
  if command -v update-grub >/dev/null 2>&1; then
    if update-grub >/dev/null 2>&1; then
      mwvd_log "grub config regenerated: update-grub"
      return 0
    fi
  fi
  if command -v grub2-mkconfig >/dev/null 2>&1 && [ -d /boot/grub2 ]; then
    if grub2-mkconfig -o /boot/grub2/grub.cfg >/dev/null 2>&1; then
      mwvd_log "grub config regenerated: grub2-mkconfig"
      return 0
    fi
  fi
  if command -v grub-mkconfig >/dev/null 2>&1 && [ -d /boot/grub ]; then
    if grub-mkconfig -o /boot/grub/grub.cfg >/dev/null 2>&1; then
      mwvd_log "grub config regenerated: grub-mkconfig"
      return 0
    fi
  fi
  mwvd_warn "no grub regeneration tool available (update-grub / grub2-mkconfig / grub-mkconfig)"
  return 1
}

#
# 把 resume=UUID=<swap_uuid> 写进引导器配置（三种适配器共用）。
#
# 覆盖三条落点，哪条存在改哪条：
#   1) BLS 条目            /boot/loader/entries/*.conf（Fedora grub2-BLS、Arch systemd-boot）
#   2) kernel-install 模板  /etc/kernel/cmdline（供后续新内核沿用）
#   3) grub 默认模板        /etc/default/grub 的 GRUB_CMDLINE_LINUX
# 没有 BLS 时再调用发行版工具重生成 grub.cfg 使其立即生效。
#
# 返回：0 = 已写入（或本就正确）；2 = 无需变更（运行中的 cmdline 已正确）；
#       1 = 无法自动配置（调用方应给出人工指引）。
#
mwvd_configure_resume_cmdline() {
  local swap_uuid="$1"
  local token="resume=UUID=$swap_uuid"
  local bls_dir="${MWVD_BLS_DIR:-/boot/loader/entries}"
  local grub_default="${MWVD_GRUB_DEFAULT:-/etc/default/grub}"
  local kernel_cmdline="${MWVD_KERNEL_CMDLINE_FILE:-/etc/kernel/cmdline}"
  local touched=0

  [ -n "$swap_uuid" ] || return 1

  # 运行中的 cmdline 已经正确 ⇒ 不重复改写
  if mwvd_cmdline_has_resume "$swap_uuid"; then
    return 2
  fi

  if [ -d "$bls_dir" ]; then
    mwvd_bls_set_resume "$token"
    case $? in
      0 | 2) touched=1 ;;
    esac
  fi

  if [ -f "$kernel_cmdline" ]; then
    mwvd_kernel_cmdline_set_resume "$token"
    case $? in
      0 | 2) touched=1 ;;
    esac
  fi

  if [ -f "$grub_default" ]; then
    mwvd_grub_default_set_resume "$token"
    case $? in
      0 | 2) touched=1 ;;
    esac
  fi

  if [ ! -d "$bls_dir" ]; then
    mwvd_regenerate_grub || true
  fi

  [ "$touched" = "1" ] && return 0
  return 1
}

#
# 把临时文件替换到目标位置：
#   返回 0 = 已更新，2 = 内容未变，1 = 失败
# 保留最近一份 <target>.mwvd.bak，权限 0644。
#
mwvd_install_tmp() {
  local target="$1" tmp="$2"
  local backup="${3:-${target}.mwvd.bak}"

  chmod 0644 "$tmp" || {
    rm -f "$tmp"
    return 1
  }

  if [ -f "$target" ] && cmp -s "$target" "$tmp"; then
    rm -f "$tmp"
    return 2
  fi

  if [ -f "$target" ]; then
    mkdir -p "$(dirname "$backup")" || {
      rm -f "$tmp"
      return 1
    }
    cp -f "$target" "$backup" || {
      rm -f "$tmp"
      return 1
    }
  fi

  mv -f "$tmp" "$target" || {
    rm -f "$tmp"
    return 1
  }
  return 0
}

#
# 原子写入目标文件：
#   返回 0 = 已更新，2 = 内容未变，1 = 失败
#
mwvd_write_file() {
  local target="$1" content="$2" backup="${3:-}"
  local dir tmp

  dir="$(dirname "$target")"
  mkdir -p "$dir" || return 1

  tmp="$(mktemp "${target}.mwvd.XXXXXX")" || return 1
  if ! printf '%s' "$content" > "$tmp"; then
    rm -f "$tmp"
    return 1
  fi

  if [ -n "$backup" ]; then
    mwvd_install_tmp "$target" "$tmp" "$backup"
  else
    mwvd_install_tmp "$target" "$tmp"
  fi
}

#
# 休眠模式：把 systemd `HibernateMode` 固定为 shutdown（默认）
#
# 为什么默认 shutdown：云固件环境下休眠/恢复由固件接管（MWCF 通道写 SLEEPING
# 标记，上电时固件定位镜像再交回内核），不需要内核走 ACPI S4 的 platform 钩子。
# Debian 13（6.12.63）实测：platform 模式会**静默回滚** —— `Creating image:`
# 之后直接 `ACPI: PM: Waking up from system sleep state S4` + `Restarting tasks`，
# swap 里没有任何镜像（无 S1SUSPEND），随后关机；改 shutdown 后 S4 正常。
#
# 落点：
#   1) systemd sleep.conf 的 [Sleep] HibernateMode=<mode>（systemd >= 254 生效）
#   2) /sys/power/disk 立即生效（当前启动即可验证）
#
# 返回：0 = 已更新；2 = 已是目标值/已关闭；1 = 写入失败
#
mwvd_configure_hibernate_mode() {
  local mode="${MWVD_HIBERNATE_DISK_MODE:-shutdown}"
  local conf="${MWVD_SLEEP_CONF:-/etc/systemd/sleep.conf}"
  local power_disk="${MWVD_POWER_DISK:-/sys/power/disk}"
  local new content rc

  case "$mode" in
    shutdown|reboot|suspend|test_resume) ;;
    platform)
      mwvd_log "hibernate mode: platform (systemd/default, not managed)"
      return 2
      ;;
    *)
      mwvd_warn "unknown MWVD_HIBERNATE_DISK_MODE=$mode; using shutdown"
      mode=shutdown
      ;;
  esac

  if [ -r "$conf" ]; then
    new="$(awk -v mode="$mode" '
      BEGIN { done = 0; in_sleep = 0; seen_sleep = 0 }
      /^[[:space:]]*\[/ {
        if (in_sleep && !done) { print "HibernateMode=" mode; done = 1 }
        in_sleep = ($0 ~ /^[[:space:]]*\[Sleep\][[:space:]]*$/)
        if (in_sleep) seen_sleep = 1
        print
        next
      }
      in_sleep && /^[[:space:]]*#?[[:space:]]*HibernateMode[[:space:]]*=/ {
        if (!done) { print "HibernateMode=" mode; done = 1 }
        next
      }
      { print }
      END {
        if (!done) {
          if (in_sleep) {
            print "HibernateMode=" mode
          } else if (!seen_sleep) {
            print "[Sleep]"
            print "HibernateMode=" mode
          }
        }
      }
    ' "$conf")" || {
      mwvd_warn "cannot rewrite $conf (awk failed)"
      return 1
    }
    # 命令替换会吃掉末尾换行，这里补回来，否则幂等比较（cmp）永远不相等
    new="$new
"
  else
    new="[Sleep]
HibernateMode=$mode
"
  fi

  mwvd_write_file "$conf" "$new"
  rc=$?
  if [ "$rc" -eq 1 ]; then
    mwvd_warn "cannot write $conf"
    return 1
  fi

  # 当前启动立即生效（不支持休眠的内核/容器里该文件可能只读，失败仅提示）
  if [ -w "$power_disk" ]; then
    if ! printf '%s\n' "$mode" > "$power_disk" 2>/dev/null; then
      mwvd_warn "cannot set $power_disk to $mode (takes effect after reboot)"
    fi
  fi

  if [ "$rc" -eq 0 ]; then
    mwvd_log "hibernate mode configured: HibernateMode=$mode"
  else
    mwvd_log "hibernate mode already HibernateMode=$mode; nothing to do"
  fi
  return "$rc"
}

mwvd_adapter_initramfs_tools() {
  local swap_uuid="$1"
  local conf content rc
  local stale_backup=0

  conf="${MWVD_INITRAMFS_RESUME_CONF:-/etc/initramfs-tools/conf.d/mwvd-resume.conf}"
  content="# Generated by mwvd-hibernate-setup; do not edit
RESUME=UUID=$swap_uuid
"

  # initramfs-tools 会 source conf.d 下的所有文件（不看扩展名）。
  # .mwvd.bak 放在 conf.d 会被当成配置文件，导致旧 RESUME 覆盖新值；
  # 因此备份必须放到 conf.d 之外，并清理旧版本遗留的 .bak。
  if [ -e "${conf}.mwvd.bak" ]; then
    rm -f "${conf}.mwvd.bak"
    stale_backup=1
  fi
  mwvd_write_file "$conf" "$content" \
    "${MWVD_BACKUP_DIR:-/var/backups}/mwvd/initramfs-tools-mwvd-resume.conf.mwvd.bak"
  rc=$?
  case "$rc" in
    2)
      # 清理旧 .bak 后即使配置文本未变，也要重建一次 initrd，
      # 否则旧 RESUME 仍可能被烘焙在已有 initramfs 中。
      if [ "$stale_backup" -eq 0 ]; then
        return 0
      fi
      ;;
    1)
      return 1
      ;;
  esac

  if command -v update-initramfs >/dev/null 2>&1; then
    mwvd_log "adapter=initramfs-tools action=update-initramfs"
    update-initramfs -u -k all || return 1
  else
    mwvd_warn "update-initramfs not found; run manually: update-initramfs -u -k all"
  fi
  return 0
}

mwvd_adapter_dracut() {
  local swap_uuid="$1"
  local conf content rc

  conf="${MWVD_DRACUT_CONF:-/etc/dracut.conf.d/90-mwvd-resume.conf}"
  content='# Generated by mwvd-hibernate-setup; do not edit
add_dracutmodules+=" resume "
'

  mwvd_write_file "$conf" "$content"
  rc=$?
  case "$rc" in
    2)
      return 0
      ;;
    1)
      return 1
      ;;
  esac

  if command -v dracut >/dev/null 2>&1; then
    mwvd_log "adapter=dracut action=rebuild"
    #
    # 默认只重建**当前运行内核**的 initramfs。
    #
    # 不用 --regenerate-all 的原因（Fedora 44 + dracut 108 实测）：
    #   1) 耗时：rescue + 每个内核一个 initramfs，实测约 10 分钟；本脚本由
    #      udev/systemd 触发，代价过高；
    #   2) 正确性：--regenerate-all 会为**没有 mwvd 模块的内核**也重建，而
    #      setup.sh 写入的 force_drivers 含 mwvd，dracut 会直接失败：
    #        dracut-install: Failed to find module 'mwvd'
    #        dracut[E]: FAILED: ... --kerneldir /usr/lib/modules/<旧内核> ...
    #      且整体退出码仍为 0 ⇒ 调用方察觉不到。
    # 需要覆盖全部内核时显式设置 MWVD_DRACUT_REGENERATE_ALL=1。
    #
    if [ "${MWVD_DRACUT_REGENERATE_ALL:-0}" = "1" ] &&
       dracut --help 2>&1 | grep -q -- '--regenerate-all'; then
      dracut -f --regenerate-all || return 1
    else
      dracut -f || return 1
    fi
  else
    mwvd_warn "dracut not found; run manually: dracut -f"
  fi
  return 0
}

mwvd_adapter_mkinitcpio() {
  local swap_uuid="$1"
  local conf tmp rc

  conf="${MWVD_MKINITCPIO_CONF:-/etc/mkinitcpio.conf}"
  if [ ! -f "$conf" ]; then
    mwvd_warn "mkinitcpio.conf not found: $conf"
    return 1
  fi

  tmp="$(mktemp "${conf}.mwvd.XXXXXX")" || return 1

  # 在 HOOKS 的 block 之后插入 resume；已有 resume 时保持原样。
  LC_ALL=C awk '
    BEGIN { seen = 0 }
    /^[[:space:]]*HOOKS\+?[[:space:]]*=/ && seen == 0 {
      line = $0
      seen = 1

      if (line ~ /(^|[^[:alnum:]_])resume([^[:alnum:]_]|$)/) {
        print line
        next
      }

      if (match(line, /(^|[[:space:](])block([[:space:])]|$)/)) {
        before = substr(line, 1, RSTART - 1)
        segment = substr(line, RSTART, RLENGTH)
        after = substr(line, RSTART + RLENGTH)
        sub(/block/, "block resume", segment)
        print before segment after
        next
      }

      if (match(line, /(^|[[:space:](])filesystems([[:space:])]|$)/)) {
        before = substr(line, 1, RSTART - 1)
        segment = substr(line, RSTART, RLENGTH)
        after = substr(line, RSTART + RLENGTH)
        sub(/filesystems/, "resume filesystems", segment)
        print before segment after
        next
      }

      pos = match(line, /\)[[:space:]]*$/)
      if (pos > 0) {
        print substr(line, 1, pos - 1) " resume" substr(line, pos)
      } else {
        print line " resume"
      }
      next
    }
    { print }
    END { if (!seen) exit 3 }
  ' "$conf" > "$tmp"
  rc=$?
  if [ "$rc" -ne 0 ]; then
    rm -f "$tmp"
    mwvd_warn "cannot update HOOKS in $conf (missing HOOKS line)"
    return 1
  fi

  mwvd_install_tmp "$conf" "$tmp"
  rc=$?
  case "$rc" in
    2)
      return 0
      ;;
    1)
      return 1
      ;;
  esac

  if command -v mkinitcpio >/dev/null 2>&1; then
    mwvd_log "adapter=mkinitcpio action=mkinitcpio -P"
    mkinitcpio -P || return 1
  else
    mwvd_warn "mkinitcpio not found; run manually: mkinitcpio -P"
  fi
  return 0
}

mwvd_adapter_manual() {
  local swap_uuid="$1"

  mwvd_warn "no supported initramfs detected; configure resume manually"
  mwvd_warn "  kernel cmdline: resume=UUID=$swap_uuid"
  mwvd_warn "  then rebuild your initramfs with the distribution tool"
  return 0
}

#
# 适配器入口：adapter_apply <part_uuid> <swap_uuid> <device>
#
# 顺序很重要：
#   1) 先写引导器 cmdline（resume=UUID=...）——UKI 类引导（Arch 的
#      EFI/Linux/*.efi、systemd-boot）会把 cmdline **内嵌**进镜像，必须在重建
#      之前写好，否则重建出来的镜像仍带旧 cmdline（实测命中）；
#   2) 再按 initramfs 类型写配置并重建（内容未变化时跳过昂贵的重建）；
#   3) cmdline 三道落点都写不进去时才告警要求人工添加。
#
mwvd_adapter_apply() {
  local part_uuid="$1" swap_uuid="$2" device="$3"
  local initrd rc cmdline_rc

  initrd="$(mwvd_detect_initrd)"
  mwvd_log "adapter=$initrd part_uuid=$part_uuid device=$device"

  # ---- 1) 引导器 cmdline（必须在 initramfs/UKI 重建之前）----
  cmdline_rc=2
  if [ "${MWVD_HIBERNATE_MANAGE_BOOT:-1}" = "1" ]; then
    mwvd_configure_resume_cmdline "$swap_uuid"
    cmdline_rc=$?
    case "$cmdline_rc" in
      0) mwvd_log "bootloader configured: resume=UUID=$swap_uuid" ;;
      2) mwvd_log "bootloader already has resume=UUID=$swap_uuid; nothing to do" ;;
      *) mwvd_warn "cannot configure bootloader automatically; add resume=UUID=$swap_uuid to the kernel cmdline manually" ;;
    esac
  else
    mwvd_log "bootloader cmdline management disabled by MWVD_HIBERNATE_MANAGE_BOOT"
  fi

  # ---- 1b) 休眠模式（云固件下用 shutdown，不用 ACPI platform）----
  if [ "${MWVD_HIBERNATE_DISK_MODE:-shutdown}" != "platform" ] &&
     [ "${MWVD_HIBERNATE_MANAGE_SLEEP_MODE:-1}" = "1" ]; then
    # 0 = 已更新，2 = 已是目标值（日志里已说明），只有 1 = 写入失败才告警
    mwvd_configure_hibernate_mode
    if [ $? -eq 1 ]; then
      mwvd_warn "cannot configure hibernate mode; set HibernateMode=shutdown in /etc/systemd/sleep.conf manually"
    fi
  else
    mwvd_log "hibernate sleep-mode management disabled (MWVD_HIBERNATE_DISK_MODE=${MWVD_HIBERNATE_DISK_MODE:-shutdown})"
  fi

  # ---- 2) initramfs 配置 + 重建 ----
  case "$initrd" in
    initramfs-tools)
      mwvd_adapter_initramfs_tools "$swap_uuid"
      rc=$?
      ;;
    dracut)
      mwvd_adapter_dracut "$swap_uuid"
      rc=$?
      ;;
    mkinitcpio)
      mwvd_adapter_mkinitcpio "$swap_uuid"
      rc=$?
      ;;
    *)
      mwvd_adapter_manual "$swap_uuid"
      rc=$?
      ;;
  esac

  [ "$rc" -eq 0 ] || return "$rc"

  return 0
}
