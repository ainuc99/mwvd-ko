// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/**
 * VD抽象接口头文件
 *
 * Copyright (c) 2025 LU WEIFENG (wave.lu@gmail.com)
 * 版权所有 (c) 2025 陆伟峰
 *
 */

#ifndef __VD_H__
#define __VD_H__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/uio.h>
#include <linux/falloc.h>
#include <linux/ioprio.h>
#include <linux/blk-cgroup.h>
#include <linux/blk-mq.h>
#include <linux/spinlock.h>
#include <linux/version.h>

/* ITER_SOURCE/ITER_DEST 在 6.2 引入，更早内核使用 READ/WRITE */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 0)
#ifndef ITER_SOURCE
#define ITER_SOURCE WRITE
#endif
#ifndef ITER_DEST
#define ITER_DEST   READ
#endif
#endif

/* file_open_root: 5.10 (dentry, vfsmount, name, flags, mode) */
/*                5.11+ (path_ptr, name, flags, mode)            */
/* 注意：root_path 是 struct path * 指针 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0)
#define vd_file_open_root(root_path, path, flags, mode) \
  file_open_root((root_path)->dentry, (root_path)->mnt, path, flags, mode)
#else
#define vd_file_open_root(root_path, path, flags, mode) \
  file_open_root((root_path), path, flags, mode)
#endif

/* disk_openers: 6.0+ 返回 atomic_t，需用 atomic_read */
/*                5.11+ 的 gendisk->part0 是 struct block_device *，直接读
 *                part0->bd_openers，避免解引用可能为 NULL 的 vd->vd_device：
 *                5.15 下新建未绑定设备被 udev/blkid 打开→关闭时，
 *                mwvd_release() 读 vd->vd_device->bd_openers 空指针 → Oops
 *                （mov 0x20(%rax),%edx，RAX=0，CR2=0x20）。参考上游 loop 驱动。 */
/*                5.10 的 part0 仍是 struct hd_struct（无 bd_openers），
 *                保留 bdev 版但判空：未绑定设备 vd->vd_device==NULL 时按 0 处理。
 *                只有 mwvd_release 会在 UNBOUND 状态被调用（udev/blkid 探测），
 *                其余调用点（clr_fd/control_remove）均要求 state==BOUND，
 *                此时 vd_device 必非 NULL，判空分支不影响语义。 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
#define vd_disk_openers(disk, bdev)  atomic_read(&(disk)->part0->bd_openers)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
#define vd_disk_openers(disk, bdev)  ((disk)->part0->bd_openers)
#else
#define vd_disk_openers(disk, bdev)  ((bdev) ? (bdev)->bd_openers : 0)
#endif

#include "mwvd.h"

#include "vd_abi.h"
#include "mwvd_code.h"

/*
 * 闭源层调用入口：统一经函数表（g_mwvd_vdf）转发。
 *
 * 2.8.0 起只有一种来源：固件 `MWVD` 代码载荷（运行时加载）—— SPEC §6.9。
 * 模块不再链接内置闭源层 `vd.o`；载荷不可用（缺失/校验失败/架构或 ABI
 * 不匹配）时模块拒绝加载。调用点保持原样（vdf_xxx(...)），不感知来源。
 */
#define vdf_probe(...)                    g_mwvd_vdf->probe(__VA_ARGS__)
#define vdf_state_size(...)               g_mwvd_vdf->state_size(__VA_ARGS__)
#define vd_core_version()                 g_mwvd_vdf->core_version()
#define vd_core_version_code()            g_mwvd_vdf->core_version_code()
#define vdf_open(...)                     g_mwvd_vdf->open(__VA_ARGS__)
#define vdf_close(...)                    g_mwvd_vdf->close(__VA_ARGS__)
#define vdf_translate(...)                g_mwvd_vdf->translate(__VA_ARGS__)
#define vdf_flush(...)                    g_mwvd_vdf->flush(__VA_ARGS__)
#define vdf_flush_done(...)               g_mwvd_vdf->flush_done(__VA_ARGS__)
#define vdf_flush_reset(...)              g_mwvd_vdf->flush_reset(__VA_ARGS__)
#define vdf_flush_required(...)           g_mwvd_vdf->flush_required(__VA_ARGS__)
#define vdf_parse_efi(...)                g_mwvd_vdf->parse_efi(__VA_ARGS__)
#define vdf_parse_efi_blob_v2(...)        g_mwvd_vdf->parse_efi_blob_v2(__VA_ARGS__)
#define vd_acquire_config_blob(...)       g_mwvd_vdf->acquire_config_blob(__VA_ARGS__)
#define vd_acquire_config_blob_acpi(...)  g_mwvd_vdf->acquire_config_blob_acpi(__VA_ARGS__)
#define vdf_get_diag(...)                 g_mwvd_vdf->get_diag(__VA_ARGS__)

typedef enum VD_TYPE
{
  VD_TYPE_UNKNOWN = 0,
  VD_TYPE_RAW,
  VD_TYPE_ISO,
  VD_TYPE_VHD,
  VD_TYPE_VHDx,
  VD_TYPE_MAX,
} VD_TYPE;

typedef enum VD_FMT
{
  VD_FMT_DYNAMIC = 0,
  VD_FMT_FIXED,
  VD_FMT_DIFFERENCING,
  VD_FMT_MAX,
} VD_FMT;

/* Possible states of device */
enum mwvd_state
{
  MWVD_UNBOUND,
  MWVD_BOUND,
  MWVD_RUNDOWN,
  MWVD_DELETING,
};

typedef struct mwvd_device
{
  int vd_number;
  int vd_flags;
  char vd_file_name[MWVD_NAME_SIZE];
  char vd_fw_path[260];       /* 固件 v2 条目中的镜像路径（休眠标记用） */
  char vd_part_uuid[64];      /* 分区 GUID（休眠标记用；自动加载路径保存） */

  struct file *vd_backing_file;
  struct block_device *vd_device;

  gfp_t old_gfp_mask;

  spinlock_t vd_lock;
  int vd_state;
  spinlock_t vd_work_lock;
  struct workqueue_struct *workqueue;
  struct work_struct rootcg_work;
  struct work_struct replay_work;   /* 唤醒后重放 VHDX log（与 I/O 同 workqueue 串行） */
  struct list_head rootcg_cmd_list;
  struct list_head idle_worker_list;
  struct rb_root worker_tree;
  struct timer_list timer;

  struct request_queue *vd_queue;
  struct blk_mq_tag_set tag_set;
  struct gendisk *vd_disk;
  struct mutex vd_mutex;
  bool idr_visible;
  bool pm_replay_failed;      /* 设备级：唤醒时 replay/flush 失败置位，仅影响该设备 */

  uint16_t *Filename;                // Virtual Disk Image file name
  uint64_t FileSize;                 // Actual Image File Size
  VD_TYPE VDType;                  // Virtual Disk type
  VD_FMT VDFormat;                 // Fixed, Dynamic, Differencing
  void *vdf_state;
  struct mwvd_device *ParentVDisk; // Differencing Parent VDisk Struct
  struct path root_path;           // Mount context for auto-mounted filesystems
} VDISK;

/* mwvd_cmd 作为 blk-mq PDU 使用，嵌入在 request 预分配缓冲区中，无需 packed */
struct mwvd_cmd
{
  struct list_head list_entry;
  long ret;
  struct bio_vec *bvec;
  struct cgroup_subsys_state *blkcg_css;
  struct cgroup_subsys_state *memcg_css;
};

/* I/O path (implemented in vd_core.c) */

/* mwvd_monitor / mwvd_umonitor — 自动加载监控 */
int mwvd_monitor(void);
int mwvd_umonitor(void);
struct request;
int vd_handle_request(struct mwvd_device *vd, struct request *rq);

/* vd_core.c — 虚拟磁盘镜像打开/关闭（标准化函数调用） */
#define MWVD_PARENT_CHAIN_MAX  8   /* 差分父链最大层数（防深链耗尽内核栈） */
struct vdf_open_result;
void *vd_open_image(struct file *file, loff_t file_size,
                     int vd_number, const char *tag,
                     struct vdf_open_result *result);
int   vd_open_parent_image(struct mwvd_device *vd,
                            struct vdf_open_result *result, int depth);
void  vd_close_image(struct mwvd_device *vd);
/* 挂起/休眠（S3/S4）前落盘屏障：递归 flush 父链元数据 + fsync backing（不关闭镜像/不冻结队列）。 */
int   vd_flush_all(struct mwvd_device *vd);
/* 打开后立即应用待重放的 VHDX 日志（规范要求打开前重放）。可写镜像调用。
 * @return 0 成功；负值为错误码（供 pm_replay_failed 判定）。 */
int   vd_replay_pending_log(struct mwvd_device *vd);
/* 获取固件配置数据块（返回值用后须 memunmap）。 */
void *vd_get_config_blob(bool *mapped);

/* main.c */

int mw_mkdir_p(const char *path, umode_t mode);

int mwvd_clear_backing(unsigned int index);
int mwvd_configure_backing_path(unsigned int index, struct path *root_path,
                                const char *path, const char *part_uuid);

#endif