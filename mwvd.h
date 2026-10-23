// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/**
 * 云固件Linux内核模块MWVD
 * IOCTL 通讯协议头文件
 *
 * 仅保留实际使用的 ioctl 命令和数据结构。
 *
 * Copyright (c) 2025 LU WEIFENG (wave.lu@gmail.com)
 * 版权所有 (c) 2025 陆伟峰
 *
 */
#ifndef _UAPI_LINUX_MWVD_H
#define _UAPI_LINUX_MWVD_H

#define MWVD_NAME_SIZE 128

#define MWVDISK_MAJOR 240

/* 设备标志位 */
enum
{
  MWVD_FLAGS_READ_ONLY = 1,
  MWVD_FLAGS_AUTOCLEAR = 4,
  MWVD_FLAGS_PARTSCAN = 8,
};

#include <asm/posix_types.h>
#include <linux/types.h>

/*
 * 配置结构体（用于 MWVD_SET_FD ioctl 内核内部传递）。
 * 注意：用户态直接传 fd 整数作为 ioctl 参数，不使用此结构体。
 */
struct mwvd_config
{
  __u32 block_size;
  __u32 vd_flags;
  __u64 __reserved[7];
};

/* IOCTL commands */
#define MWVD_SET_FD  0x4C00
#define MWVD_CLR_FD  0x4C01
#define MWVD_GET_STATUS 0x4C02

/* 设备状态查询结果（MWVD_GET_STATUS，对 /dev/mwvdN ioctl） */
struct mwvd_status
{
  __u32 number;                   /* mwvdN 设备号 */
  __u32 state;                    /* 0=MWVD_UNBOUND 1=MWVD_BOUND 2=RUNDOWN 3=DELETING */
  __u32 vd_flags;                 /* MWVD_FLAGS_* */
  __u32 __pad;
  __u64 file_size;                /* 绑定镜像文件大小（未绑定为 0） */
  char file_name[MWVD_NAME_SIZE]; /* 绑定镜像路径（未绑定为空串） */
};

/* /dev/mwvd-control interface */
#define MWVD_CTL_ADD 0x4C80
#define MWVD_CTL_REMOVE 0x4C81
#define MWVD_CTL_GET_FREE 0x4C82

/*
 * 固件声明的休眠（S4）存储分区（v2.5 扩展记录 Type=0x0001；上游为 mw.config
 * 的 "hibernate <partuuid>"）。查询：对 /dev/mwvd-control ioctl
 * (MWVD_CTL_GET_HIBERNATE, &struct mwvd_hibernate)。
 *
 * 用途：swap/resume 部署（免手抄 UUID）与运行时校验；固件只声明意图，
 * 语义合法性（是否真为 swap、容量是否足够）由本端自判。
 */
#define MWVD_HIBERNATE_DEVPATH_SIZE 128

struct mwvd_hibernate
{
  __u32 valid;    /* 1 = 固件声明了休眠存储分区；0 = 未配置（记录缺失） */
  __u32 usage;    /* 0=未指定 1=Linux swap（统一 swap 分区） 2=其他 */
  __u32 resolved; /* 1 = device_path 已解析到具体块设备（分区当前可见） */
  __u32 __pad;
  char part_uuid[37];                         /* 36 字符小写 partuuid + NUL */
  char device_path[MWVD_HIBERNATE_DEVPATH_SIZE];
};

#define MWVD_CTL_GET_HIBERNATE 0x4C83
#endif /* _UAPI_LINUX_MWVD_H */
