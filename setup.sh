#!/bin/bash
# SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
# 云固件内核模块安装脚本
# Copyright (c) 2026 LU WEIFENG (wave.lu@gmail.com)
# 版权所有 (c) 2026 陆伟峰
#
# 云固件内核模块基本依赖：
# ahci nvme usb-storage uas exfat ntfs3 mwvd
#
# 基本操作内容：
# 1. 检查是否以root权限运行
# 2. 设置initrd配置文件，包括initramfs-tools, dracut和mkinitcpio三种方案
#   - initramfs-tools方案：在/etc/initramfs-tools/modules文件中添加依赖模块
#   - dracut方案：在/etc/dracut.conf.d/目录下创建mwvd.conf配置文件，
#       使用force_drivers+=强制指定需要包含依赖模块
#   - mkinitcpio方案：在/etc/mkinitcpio.conf文件中添加依赖模块到MODULES数组中
#       在FILES数组中添加/etc/udev/rules.d/69-mwvd.rules文件，以确保udev规则在initrd中生效
# 3. 更新udev规则，增加mwvd*设备的事件处理
#   - 在/etc/udev/rules.d/目录下启用针对mwvd*设备的自定义69-mwvd.rules文件
#   原先修改的规则，由于udev规则文件更新后会被覆盖，所以改为使用自定义规则文件的方式来启用对mwvd*设备的支持
#   - 修改/lib/udev/rules.d/60-persistent-storage.rules文件，针对mwvd*设备启用默认规则
#   - 修改/lib/udev/rules.d/60-block.rules文件，针对mwvd*设备启用默认规则
# 4. 复制内核模块文件夹mwvd-2.0.0到/usr/src/下
# 5. 检查dkms是否安装，如果没有安装就执行安装操作
#   - Debian/Ubuntu系统：apt-get install dkms
#   - RHEL/CentOS系统：yum install dkms
#   - Arch Linux系统：pacman -S dkms
#   - openSUSE系统：zypper install dkms
#   - Fedora系统：dnf install dkms
#   - SUSE Linux Enterprise系统：zypper install dkms
# 6. 使用dkms安装内核模块，并更新initrd
#   - dkms add -m mwvd -v 2.0.0
#   - dkms install -m mwvd -v 2.0.0
#   - 更新initrd：
#       - Debian/Ubuntu系统：update-initramfs -u
#       - RHEL/CentOS系统：dracut -f
#       - Arch Linux系统：mkinitcpio -P
#       - openSUSE系统：dracut -f
#       - Fedora系统：dracut -f
#       - SUSE Linux Enterprise系统：dracut -f
# 注意事项：
# - 请确保内核版本与内核模块版本兼容，否则可能导致安装失败
# - 在执行安装操作前，建议备份重要数据，以防止意外情况发生
#
set -euo pipefail

# Check if the script is run as root
if [ "$EUID" -ne 0 ]; then
  echo "Please run as root"
  exit 1
fi

# 切到脚本所在目录：下面全部用相对路径（./mwvd.rules、cp -a ./* 等），
# 从别处调用（如 `sudo bash /root/mwvd-2.7.0/setup.sh`）会找不到文件。
cd "$(dirname "$0")"

MODULE_NAME="mwvd"
# 版本号从 main.c 唯一定义处提取（与 Makefile 同源，避免硬编码漂移：
# 曾固定为 2.0.0，导致 DKMS 目录/条目与实际模块版本不一致）
MODULE_VERSION="$(
  awk '/^#define MWVD_MODULE_VERSION /{gsub(/"/, "", $3); print $3; exit}' \
    "$(dirname "$0")/main.c" 2>/dev/null
)"
if [ -z "${MODULE_VERSION:-}" ]; then
  echo "Cannot determine mwvd version from main.c (MWVD_MODULE_VERSION)" >&2
  exit 1
fi
MODULE_SRC_DIR="${MODULE_NAME}-${MODULE_VERSION}"
USR_SRC_DIR="/usr/src/${MODULE_NAME}-${MODULE_VERSION}"
INITRAMFS_MODULES_FILE="/etc/initramfs-tools/modules"
DRACUT_CONF_DIR="/etc/dracut.conf.d"
DRACUT_CONF_FILE="${DRACUT_CONF_DIR}/${MODULE_NAME}.conf"
MKINITCPIO_CONF="/etc/mkinitcpio.conf"
MODULES_TO_ADD=(ahci nvme usb-storage uas exfat ntfs3 mwvd)
FILES_TO_ADD=("/etc/udev/rules.d/69-mwvd.rules")   # 需要添加的 udev 规则文件

# 休眠分区自动配置（udev + systemd + POSIX shell，无额外用户态二进制）
HIBERNATE_HELPER_SRC="./mwvd-hibernate-setup"
HIBERNATE_DISTRO_SRC="./mwvd-distro.sh"
HIBERNATE_RULE_SRC="./mwvd-hibernate.rules"
HIBERNATE_SERVICE_SRC="./mwvd-hibernate-setup.service"
HIBERNATE_DEFAULT_SRC="./mwvd-hibernate.default"

die() {
  echo "ERROR: $*" >&2
  exit 1
}

detect_platform() {
  local id id_like
  if [ -r /etc/os-release ]; then
    . /etc/os-release
    id="${ID:-unknown}"
    id_like="${ID_LIKE:-}"
  elif command -v lsb_release >/dev/null 2>&1; then
    id="$(lsb_release -si | tr '[:upper:]' '[:lower:]')"
    id_like=""
  else
    id="unknown"
    id_like=""
  fi
  echo "$id $id_like"
}

set_package_manager() {
  local distro="$1"
  case "$distro" in
    *ubuntu*|*debian*)
      PKG_INSTALL='DEBIAN_FRONTEND=noninteractive apt-get update && apt-get install -y dkms'
      ;;
    *rhel*|*centos*)
      PKG_INSTALL='yum install -y dkms'
      ;;
    *fedora*)
      PKG_INSTALL='dnf install -y dkms'
      ;;
    *arch*)
      PKG_INSTALL='pacman -S --noconfirm dkms'
      ;;
    *opensuse*|*suse*|*sles*)
      PKG_INSTALL='zypper install -y dkms'
      ;;
    *)
      if command -v apt-get >/dev/null 2>&1; then
        PKG_INSTALL='DEBIAN_FRONTEND=noninteractive apt-get update && apt-get install -y dkms'
      elif command -v dnf >/dev/null 2>&1; then
        PKG_INSTALL='dnf install -y dkms'
      elif command -v yum >/dev/null 2>&1; then
        PKG_INSTALL='yum install -y dkms'
      elif command -v pacman >/dev/null 2>&1; then
        PKG_INSTALL='pacman -S --noconfirm dkms'
      elif command -v zypper >/dev/null 2>&1; then
        PKG_INSTALL='zypper install -y dkms'
      else
        die "Unsupported distribution and no known package manager found"
      fi
      ;;
  esac
}

detect_initrd_system() {
  if command -v mkinitcpio >/dev/null 2>&1 || [ -f "$MKINITCPIO_CONF" ]; then
    echo "mkinitcpio"
  elif command -v dracut >/dev/null 2>&1 || [ -d "$DRACUT_CONF_DIR" ]; then
    echo "dracut"
  elif command -v update-initramfs >/dev/null 2>&1 || [ -f "$INITRAMFS_MODULES_FILE" ]; then
    echo "initramfs-tools"
  else
    echo "unknown"
  fi
}

set_initrd_handlers() {
  case "$1" in
    initramfs-tools)
      INITRD_COMMAND='update-initramfs -u'
      INITRD_TOOL='initramfs-tools'
      ;;
    dracut)
      INITRD_COMMAND='dracut -f'
      INITRD_TOOL='dracut'
      ;;
    mkinitcpio)
      INITRD_COMMAND='mkinitcpio -P'
      INITRD_TOOL='mkinitcpio'
      ;;
    *)
      die "Cannot determine initrd tool on this system"
      ;;
  esac
}

ensure_initramfs_modules() {
  if [ -f "$INITRAMFS_MODULES_FILE" ]; then
    for module in "${MODULES_TO_ADD[@]}"; do
      if ! grep -qE "^${module}$" "$INITRAMFS_MODULES_FILE"; then
        echo "$module" >> "$INITRAMFS_MODULES_FILE"
      fi
    done
    echo "Updated $INITRAMFS_MODULES_FILE"
  else
    echo "# mwvd dependencies" > "$INITRAMFS_MODULES_FILE"
    for module in "${MODULES_TO_ADD[@]}"; do
      echo "$module" >> "$INITRAMFS_MODULES_FILE"
    done
    echo "Created $INITRAMFS_MODULES_FILE"
  fi
}

ensure_dracut_conf() {
  mkdir -p "$DRACUT_CONF_DIR"

  printf 'force_drivers+=\" %s \"\n' "${MODULES_TO_ADD[*]}" > "$DRACUT_CONF_FILE"
  echo "Created or updated $DRACUT_CONF_FILE"
}

ensure_mkinitcpio_conf() {
  if [ -f "$MKINITCPIO_CONF" ]; then
    # ---------- 处理 MODULES ----------
    local current line body
    current="$(grep -E '^MODULES=' "$MKINITCPIO_CONF" | head -n1 || true)"
    if [ -n "$current" ]; then
      body="${current#MODULES=}"
      body="${body#(}"
      body="${body%)}"
      body="${body//\"/}"
      read -ra existing_modules <<< "$body"
      for module in "${MODULES_TO_ADD[@]}"; do
        if ! printf '%s\n' "${existing_modules[@]}" | grep -xq "$module"; then
          existing_modules+=("$module")
        fi
      done
      line="MODULES=(${existing_modules[*]})"
      sed -i "0,/^MODULES=/s|^MODULES=.*|$line|" "$MKINITCPIO_CONF"
      echo "Updated MODULES in $MKINITCPIO_CONF"
    else
      printf 'MODULES=(%s)\n' "${MODULES_TO_ADD[*]}" >> "$MKINITCPIO_CONF"
      echo "Appended MODULES to $MKINITCPIO_CONF"
    fi

    # ---------- 处理 FILES ----------
    local current_files line_files body_files
    current_files="$(grep -E '^FILES=' "$MKINITCPIO_CONF" | head -n1 || true)"
    if [ -n "$current_files" ]; then
      body_files="${current_files#FILES=}"
      body_files="${body_files#(}"
      body_files="${body_files%)}"
      body_files="${body_files//\"/}"
      read -ra existing_files <<< "$body_files"
      for file in "${FILES_TO_ADD[@]}"; do
        if ! printf '%s\n' "${existing_files[@]}" | grep -Fxq "$file"; then
          existing_files+=("$file")
        fi
      done
      line_files="FILES=(${existing_files[*]})"
      sed -i "0,/^FILES=/s|^FILES=.*|$line_files|" "$MKINITCPIO_CONF"
      echo "Updated FILES in $MKINITCPIO_CONF"
    else
      printf 'FILES=(%s)\n' "${FILES_TO_ADD[*]}" >> "$MKINITCPIO_CONF"
      echo "Appended FILES to $MKINITCPIO_CONF"
    fi

  else
    # 文件不存在时，同时写入 MODULES 和 FILES 两个数组
    printf 'MODULES=(%s)\n' "${MODULES_TO_ADD[*]}" > "$MKINITCPIO_CONF"
    printf 'FILES=(%s)\n' "${FILES_TO_ADD[*]}" >> "$MKINITCPIO_CONF"
    echo "Created $MKINITCPIO_CONF with MODULES and FILES"
  fi
}

ensure_udev_rules() {
  local src_file="./mwvd.rules"
  local target_dir="/etc/udev/rules.d"
  local target_file="${target_dir}/69-mwvd.rules"

  if [ ! -f "$src_file" ]; then
    echo "Warning: $src_file not found, skipping udev rule installation"
    return
  fi

  mkdir -p "$target_dir"
  if [ -f "$target_file" ]; then
    echo "$target_file already exists, skipping"
    return
  fi

  cp "$src_file" "$target_file"
  echo "Installed udev rule $target_file"
}

install_hibernate_support() {
  local missing=0
  local src

  for src in "$HIBERNATE_HELPER_SRC" "$HIBERNATE_DISTRO_SRC" \
             "$HIBERNATE_RULE_SRC" "$HIBERNATE_SERVICE_SRC" \
             "$HIBERNATE_DEFAULT_SRC"; do
    if [ ! -f "$src" ]; then
      echo "Warning: $src not found, skipping hibernate support installation"
      missing=1
    fi
  done
  [ "$missing" -eq 0 ] || return 0

  install -d -m 0755 /usr/lib/mwvd
  install -m 0755 "$HIBERNATE_HELPER_SRC" /usr/lib/mwvd/mwvd-hibernate-setup
  install -m 0644 "$HIBERNATE_DISTRO_SRC" /usr/lib/mwvd/mwvd-distro.sh
  install -d -m 0755 /usr/lib/udev/rules.d
  install -m 0644 "$HIBERNATE_RULE_SRC" /usr/lib/udev/rules.d/90-mwvd-hibernate.rules
  install -d -m 0755 /usr/lib/systemd/system
  install -m 0644 "$HIBERNATE_SERVICE_SRC" /usr/lib/systemd/system/mwvd-hibernate-setup.service
  install -d -m 0755 /etc/default
  if [ ! -f /etc/default/mwvd-hibernate ]; then
    install -m 0644 "$HIBERNATE_DEFAULT_SRC" /etc/default/mwvd-hibernate
  fi

  if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules >/dev/null 2>&1 || true
  fi
  if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >/dev/null 2>&1 || true
    systemctl enable mwvd-hibernate-setup.service >/dev/null 2>&1 || true
    if [ -e /dev/mwvd-control ]; then
      systemctl start mwvd-hibernate-setup.service >/dev/null 2>&1 || true
    fi
  fi

  echo "Installed hibernate auto-configuration (udev + systemd)"
}

copy_module_sources() {
  if [ -d "$MODULE_SRC_DIR" ]; then
    rm -rf "$USR_SRC_DIR"
    cp -a "$MODULE_SRC_DIR" "$USR_SRC_DIR"
    echo "Copied $MODULE_SRC_DIR to $USR_SRC_DIR"
  else
    mkdir -p "$USR_SRC_DIR"
    cp -a ./* "$USR_SRC_DIR/" 2>/dev/null || true
    cp -a ./.??* "$USR_SRC_DIR/" 2>/dev/null || true
    rm -rf "$USR_SRC_DIR/.git"
    echo "Copied repository sources to $USR_SRC_DIR"
  fi
}

install_dkms_package() {
  if command -v dkms >/dev/null 2>&1; then
    echo "dkms is already installed"
    return
  fi
  echo "Installing dkms..."
  sh -c "$PKG_INSTALL"
}

install_module_with_dkms() {
  # 确保 dkms.conf 中的版本号与 main.c 一致
  local dkms_conf="${USR_SRC_DIR}/dkms.conf"
  if [ -f "$dkms_conf" ]; then
    sed -i "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"${MODULE_VERSION}\"/" "$dkms_conf"
  fi

  # 移除旧版本（如果存在且与新版本不同）
  #
  # dkms status 的输出有两种格式，必须同时兼容：
  #   dkms 2.x（Ubuntu 20.04）：mwvd, 2.7.0, 5.4.0-xx-generic, x86_64: installed
  #   dkms 3.x（Fedora 44 等） ：mwvd/2.7.0, 7.0.4-200.fc44.x86_64, x86_64: installed
  # 只按 "mwvd," 匹配会漏掉 3.x，导致旧版本永不被清理。
  local old_versions
  old_versions=$(dkms status 2>/dev/null \
    | grep -E "^${MODULE_NAME}[/,]" \
    | grep -vE "^${MODULE_NAME}[/,] ?${MODULE_VERSION}(,|$)" || true)
  if [ -n "$old_versions" ]; then
    echo "Removing old ${MODULE_NAME} versions from DKMS..."
    echo "$old_versions" | while read -r line; do
      local old_ver
      old_ver=$(printf '%s\n' "$line" | \
        sed -E "s|^${MODULE_NAME}[/,] *([^,]+),.*|\1|" | tr -d ' ')
      [ -n "$old_ver" ] || continue
      echo "  Removing ${MODULE_NAME}/${old_ver}..."
      dkms remove -m "$MODULE_NAME" -v "$old_ver" --all 2>/dev/null || \
        echo "  (skip: ${MODULE_NAME}/${old_ver} may be in use)"
    done
  fi

  if dkms status 2>/dev/null | grep -qE "^${MODULE_NAME}[/,] ?${MODULE_VERSION}(,|$)"; then
    echo "${MODULE_NAME} ${MODULE_VERSION} already added to dkms, forcing rebuild/install"
    dkms install -m "$MODULE_NAME" -v "$MODULE_VERSION" --force
  else
    dkms add -m "$MODULE_NAME" -v "$MODULE_VERSION"
    dkms install -m "$MODULE_NAME" -v "$MODULE_VERSION"
  fi
}

update_initramfs_image() {
  if [ -n "${INITRD_COMMAND:-}" ]; then
    echo "Updating initrd with: $INITRD_COMMAND"
    cmd=${INITRD_COMMAND%% *}
    if command -v "$cmd" >/dev/null 2>&1; then
      $INITRD_COMMAND
    else
      echo "Warning: $cmd not found, skipping initrd update"
      return 0
    fi
  else
    die "No initrd update command configured for this system"
  fi
}

main() {
  local platform initrd_system
  platform="$(detect_platform)"
  set_package_manager "$platform"

  initrd_system="$(detect_initrd_system)"
  set_initrd_handlers "$initrd_system"

  case "$initrd_system" in
    initramfs-tools)
      ensure_initramfs_modules
      ;;
    dracut)
      ensure_dracut_conf
      ;;
    mkinitcpio)
      ensure_mkinitcpio_conf
      ;;
    *)
      die "Unsupported initrd system: $initrd_system"
      ;;
  esac

  ensure_udev_rules
  install_hibernate_support
  copy_module_sources
  install_dkms_package
  install_module_with_dkms
  update_initramfs_image

  echo "Installation completed."
}

main




