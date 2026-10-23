#
# 云固件虚拟磁盘内核模块
# Makefile for mwvd (DKMS)
#
# 版权所有
# Copyright 2025 ~ 2026 陆伟峰(wave.lu@gmail.com)
#
# 支持：
#   1. DKMS 自动构建
#   2. 用户手动执行 "make" 构建
#   3. 自动检测内核编译器，若为 Clang 则自动启用 LLVM=1
#
# 闭源层 vd.b 按架构预编译（prebuild/<arch>/vd.*.b）
#
ifneq ($(KERNELRELEASE),)
# ================= 内核构建系统内部 =================
# ★ 2.8.0 起纯固件载荷模式：mwvd.ko 不再链接闭源层 vd.o，
#   格式后端只来自固件 ACPI `MWVD` 表的代码载荷（载荷不可用则拒载）。
#   prebuild/ 仅在生成固件侧 vd.b 时使用（见主 Makefile 的 prebuild 目标）。
obj-m := mwvd.o
mwvd-objs := main.o vd_core.o mwcfg_ext.o mwvd_code.o

# 源码树中 vd_abi.h 在 core/、mwcfg_v2.h 在 common/；DKMS 发布包已把两者
# 复制到根目录（见主 Makefile 的 release 目标），这里同时兼容两种布局。
ccflags-y += -I$(src)/core -I$(src)/common

# 与主 Makefile 保持一致：这些标志是 GPL 层跨内核编译的关键，
# 缺失会导致内核升级后 DKMS 重新构建失败（旧内核 gnu89 声明顺序 /
# GCC 12+ 栈保护 / 编译器版本差异警告等）。
ccflags-y += -std=gnu99
ccflags-y += -Wno-error=maybe-uninitialized
ccflags-y += -Wno-error=declaration-after-statement
ccflags-y += -fno-stack-protector

# ARCH 归一化：内核构建系统可能传递 x86_64/i386/aarch64，映射到 prebuild 目录 x86/i386/arm64
ARCH_NORM := $(ARCH)
ifeq ($(ARCH),x86_64)
    ARCH_NORM := x86
endif
ifeq ($(ARCH),i386)
    ARCH_NORM := i386
endif
ifeq ($(ARCH),aarch64)
    ARCH_NORM := arm64
endif
# 2.8.0 起本目录只用于 `make prebuild` 生成固件侧 vd.b 时的架构定位；
# mwvd.ko 本体不再需要 prebuild 下的任何文件。
PREBUILD_DIR := $(src)/prebuild/$(ARCH_NORM)

else
# ================= 外部调用（DKMS 或手动） =================
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

# ---------- 自动判断编译器 ----------
# 如果用户没有显式设置 LLVM，则自动检测内核是否使用 Clang
ifeq ($(origin LLVM),undefined)
    KERNEL_CONFIG := $(KDIR)/.config
    ifneq ($(wildcard $(KERNEL_CONFIG)),)
        CC_IS_CLANG := $(shell grep -c 'CONFIG_CC_IS_CLANG=y' $(KERNEL_CONFIG))
        ifeq ($(CC_IS_CLANG),1)
            LLVM := 1
        endif
    endif
endif
# 最终传递给内核构建系统的 LLVM 值（可能为空）
MAKE_OPTS := $(if $(LLVM),LLVM=$(LLVM))

.PHONY: all modules clean

all: modules

modules:
	@if [ ! -d "$(KDIR)" ]; then \
		echo "================================================"; \
		echo "  Linux Headers not found at $(KDIR)"; \
		echo "  Attempting automatic installation..."; \
		echo "================================================"; \
		KVER="$(shell uname -r)"; \
		if command -v apt-get >/dev/null 2>&1; then \
			echo "  Detected: Debian/Ubuntu"; \
			apt-get update -qq && \
			apt-get install -y linux-headers-$${KVER} || \
			apt-get install -y linux-headers-generic || \
			apt-get install -y linux-headers-$$(uname -m); \
		elif command -v dnf >/dev/null 2>&1; then \
			echo "  Detected: Fedora/RHEL (dnf)"; \
			dnf install -y "kernel-devel-$${KVER}" || \
			dnf install -y kernel-devel; \
		elif command -v yum >/dev/null 2>&1; then \
			echo "  Detected: RHEL/CentOS (yum)"; \
			yum install -y "kernel-devel-$${KVER}" || \
			yum install -y kernel-devel; \
		elif command -v zypper >/dev/null 2>&1; then \
			echo "  Detected: openSUSE/SLES"; \
			zypper install -y "kernel-devel-$${KVER}" || \
			zypper install -y kernel-devel; \
		elif command -v pacman >/dev/null 2>&1; then \
			echo "  Detected: Arch Linux"; \
			pacman -S --noconfirm linux-headers; \
		elif command -v apk >/dev/null 2>&1; then \
			echo "  Detected: Alpine Linux"; \
			apk add linux-headers; \
		else \
			echo "  Error: Unknown package manager."; \
			echo "  Please install linux-headers manually:"; \
			echo "    Debian/Ubuntu: apt-get install linux-headers-$$(uname -r)"; \
			echo "    Fedora:        dnf install kernel-devel"; \
			echo "    RHEL/CentOS:   yum install kernel-devel"; \
			echo "    Arch:          pacman -S linux-headers"; \
			echo "    openSUSE:      zypper install kernel-devel"; \
			echo "================================================"; \
			exit 1; \
		fi; \
		echo "================================================"; \
		echo "  Linux Headers installed. Proceeding with build."; \
		echo "================================================"; \
	fi
	$(MAKE) -C $(KDIR) M=$(PWD) $(MAKE_OPTS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) $(MAKE_OPTS) clean
	rm -f vd.o .vd.o.cmd
endif