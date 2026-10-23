// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/**
 * 云固件Linux内核模块
 *
 * 支持从内核直接读取镜像文件(RAW,VHD,VHDx格式)
 *
 * Copyright (c) 2025-2026 LU WEIFENG (wave.lu@gmail.com)
 * 版权所有 (c) 2025-2026 陆伟峰
 *
 *
 */
#include <linux/moduleparam.h>
#include <linux/kthread.h>
#include <linux/memcontrol.h>
#include <linux/pagemap.h>
#include <linux/compat.h>
#include <linux/string.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/blkdev.h>
#include <linux/device.h>
#include <linux/version.h>
#include <linux/miscdevice.h>
#include <linux/init.h>
#include <linux/statfs.h>
#include <linux/sched/mm.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/suspend.h>
#include <linux/efi.h>
#include <linux/acpi.h>
#include <linux/crc32.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 19, 0)
#include <linux/fs_types.h>
#endif

#include "vd.h"
#include "mwcfg_ext.h"

/*
 * 版本号语义：主版本.次版本.修订号。
 * 次版本号表示功能调整，修订号表示 bug 修复。
 */
#define MWVD_MODULE_VERSION "2.8.3"

/* 云固件虚拟磁盘控制器的 ACPI `_HID`（通讯 v2.8 §6.8，整串精确匹配） */
#define MWVD_ACPI_DEVICE_HID "MWVD0001"

#if IS_ENABLED(CONFIG_ACPI)
static bool mwvd_acpi_device_present(void)
{
  /* 精确匹配 _HID；_STA 缺省/0x0F 时返回 true。 */
  return acpi_dev_present(MWVD_ACPI_DEVICE_HID, NULL, -1);
}
#else
static bool mwvd_acpi_device_present(void)
{
  return false;
}
#endif

/* blk_mode_t 在 6.5 才引入，更早内核使用 fmode_t */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0)
#define blk_mode_t fmode_t
#endif

/* 5.15-6.0: bdev_discard_granularity/bdev_nonrot 等尚未引入 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 0, 0)
#define bdev_discard_granularity(b)  (bdev_get_queue(b)->limits.discard_granularity)
#define bdev_nonrot(b)               (!blk_queue_nonrot(bdev_get_queue(b)))
/* 上游 5.15-6.0 未引入 GD_SUPPRESS_PART_SCAN，回退到 GD_NEED_PART_SCAN；
 * 发行版（如 Ubuntu 5.15.0-139）已 backport GD_SUPPRESS_PART_SCAN（genhd.h），
 * 必须 #ifndef 保护，否则 -Wredefined 警告；clear/set 统一走 GD_SUPPRESS_PART_SCAN。 */
#ifndef GD_SUPPRESS_PART_SCAN
#define GD_SUPPRESS_PART_SCAN        GD_NEED_PART_SCAN
#endif
#define clear_gd_suppress(d)         clear_bit(GD_SUPPRESS_PART_SCAN, &(d)->state)
#define set_gd_suppress(d)           set_bit(GD_SUPPRESS_PART_SCAN, &(d)->state)
#else
#define clear_gd_suppress(d)         clear_bit(GD_SUPPRESS_PART_SCAN, &(d)->state)
#define set_gd_suppress(d)           set_bit(GD_SUPPRESS_PART_SCAN, &(d)->state)
#endif

/* 7.1+: bdev_nonrot() 已移除，改用 bdev_rot()（语义相反） */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 1, 0)
#define bdev_nonrot(b)               (!bdev_rot(b))
#endif

/* 5.x: bio_blkcg_css() 尚未引入（6.0+ 才提供），用 bio_blkcg() 等价替代。
 * 6.6 实现: bio_blkcg_css(bio) = &bio->bi_blkg->blkcg->css
 * 5.15 实现: bio_blkcg(bio)     = bio->bi_blkg->blkcg
 * 两者语义一致，仅 API 命名不同。 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 0, 0)
#define bio_blkcg_css(b)             (&bio_blkcg(b)->css)
#endif

/* 6.5 之前的 API 兼容 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0)
#define BLK_OPEN_EXCL    FMODE_EXCL
#define BLK_OPEN_WRITE   FMODE_WRITE
#define timer_shutdown_sync(t)  del_timer_sync(t)
/* bd_prepare_to_claim: 6.5+ 有 3 个参数 */
/* 5.10: 3 参数 (bdev, whole, holder); 5.15-6.4: 2 参数 (bdev, holder) */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
/* 5.10: whole device 同 bdev 本身（mwvd 设备非分区） */
#define bd_prepare_to_claim(b, f, h)  bd_prepare_to_claim(b, b, h)
#define bd_abort_claiming(b, h)       bd_abort_claiming(b, b, h)
#else
#define bd_prepare_to_claim(b, f, h)  bd_prepare_to_claim(b, f)
#endif
/* blkdev_get_by_dev: 6.5+ 有 4 个参数，之前 3 个 */
#define blkdev_get_by_dev(d, m, h, p) blkdev_get_by_dev(d, m, h)
/* blkdev_put: 6.5+ 用 (bdev, holder), 5.15-6.4 用 (bdev, mode) */
#define blkdev_put_holder(b, h)  blkdev_put(b, 0)
#endif

#define MWVD_IDLE_WORKER_TIMEOUT (60 * HZ)
#define MWVD_DEFAULT_HW_Q_DEPTH 128

static DEFINE_IDR(mwvd_index_idr);
static DEFINE_MUTEX(mwvd_ctl_mutex);
static DEFINE_MUTEX(mwvd_validate_mutex);

/*
 * 系统级 PM 状态标记：挂起/休眠（S3/S4）期间置位，恢复/失败回滚后清除。
 * PM 挂起/休眠是系统级操作（所有 vd 同时挂起/恢复），用全局变量而非 per-device 字段。
 * 仅作状态记录/调试用途，不用于拒绝 I/O——挂起期 workqueue 已带 WQ_FREEZABLE 冻结，
 * 无 I/O 会执行；且 S4 恢复跳回快照时该标记被捕获且无 POST 事件清除，用作 I/O 拒绝
 * 会导致恢复后 rootfs 不可访问（致命缺陷）。I/O 路径用 READ_ONCE 读取；挂起期
 * workqueue 已冻结无并发写，恢复瞬间竞争由 READ_ONCE/WRITE_ONCE 保证一致。
 */
static bool mwvd_pm_suspended;

/**
 * mwvd_global_lock_killable() - take locks for safe mwvd_validate_file() test
 *
 * @vd: struct mwvd_device
 * @global: true if @vd is about to bind another "struct mwvd_device", false otherwise
 *
 * Returns 0 on success, -EINTR otherwise.
 *
 * Since mwvd_validate_file() traverses on other "struct mwvd_device" if
 * is_mwvd_device() is true, we need a global lock for serializing concurrent
 * mwvd_configure()/mwvd_change_fd()/__mwvd_clr_fd() calls.
 */
static int mwvd_global_lock_killable(struct mwvd_device *vd, bool global)
{
  int err;

  if (global)
  {
    err = mutex_lock_killable(&mwvd_validate_mutex);
    if (err)
      return err;
  }
  err = mutex_lock_killable(&vd->vd_mutex);
  if (err && global)
    mutex_unlock(&mwvd_validate_mutex);
  return err;
}

/**
 * mwvd_global_unlock() - release locks taken by mwvd_global_lock_killable()
 *
 * @vd: struct mwvd_device
 * @global: true if @vd was about to bind another "struct mwvd_device", false otherwise
 */
static void mwvd_global_unlock(struct mwvd_device *vd, bool global)
{
  mutex_unlock(&vd->vd_mutex);
  if (global)
    mutex_unlock(&mwvd_validate_mutex);
}

static int max_part = 8;
static int part_shift;

/**
 * mwvd_set_size() - sets device size and notifies userspace
 * @vd: struct mwvd_device to set the size for
 * @size: new sector size of the mwvd device
 *
 * Callers must validate that the size passed into this function fits into
 * a sector_t, eg using mwvd_validate_size()
 */
static void mwvd_set_size(struct mwvd_device *vd, sector_t size)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
  if (!set_capacity_and_notify(vd->vd_disk, size))
#else
  if (!set_capacity_revalidate_and_notify(vd->vd_disk, size, false))
#endif
    kobject_uevent(&disk_to_dev(vd->vd_disk)->kobj, KOBJ_CHANGE);
}

static void mwvd_reread_partitions(struct mwvd_device *vd)
{
  int rc;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
  mutex_lock(&vd->vd_disk->open_mutex);
  rc = bdev_disk_changed(vd->vd_disk, false);
  mutex_unlock(&vd->vd_disk->open_mutex);
#else
  /* bdev_disk_changed 在 5.10 需要 struct block_device * */
  mutex_lock(&vd->vd_device->bd_mutex);
  rc = bdev_disk_changed(vd->vd_device, false);
  mutex_unlock(&vd->vd_device->bd_mutex);
#endif
  if (rc)
    pr_warn("%s: partition scan of mwvd%d (%s) failed (rc=%d)\n",
            __func__, vd->vd_number, vd->vd_file_name, rc);
}

static inline int is_mwvd_device(struct file *file)
{
  struct inode *i = file->f_mapping->host;

  return i && S_ISBLK(i->i_mode) && imajor(i) == MWVDISK_MAJOR;
}

static int mwvd_validate_file(struct file *file, struct block_device *bdev)
{
  struct inode *inode = file->f_mapping->host;
  struct file *f = file;

  /* Avoid recursion */
  while (is_mwvd_device(f))
  {
    struct mwvd_device *l;

    lockdep_assert_held(&mwvd_validate_mutex);
    if (f->f_mapping->host->i_rdev == bdev->bd_dev)
      return -EBADF;

    l = I_BDEV(f->f_mapping->host)->bd_disk->private_data;
    if (l->vd_state != MWVD_BOUND)
      return -EINVAL;
    /* Order wrt setting vd->vd_backing_file in mwvd_configure(). */
    rmb();
    f = l->vd_backing_file;
  }
  if (!S_ISREG(inode->i_mode) && !S_ISBLK(inode->i_mode))
    return -EINVAL;
  return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
static void mwvd_assign_backing_file(struct mwvd_device *vd, struct file *file)
{
  vd->vd_backing_file = file;
  vd->old_gfp_mask = mapping_gfp_mask(file->f_mapping);
  mapping_set_gfp_mask(file->f_mapping,
                       vd->old_gfp_mask & ~(__GFP_IO | __GFP_FS));
}
#endif

static int mwvd_check_backing_file(struct file *file)
{
  if (!file->f_op->read_iter)
    return -EINVAL;

  if ((file->f_mode & FMODE_WRITE) && !file->f_op->write_iter)
    return -EINVAL;

  return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
static void mwvd_get_discard_config(struct mwvd_device *vd,
                                    u32 *granularity, u32 *max_discard_sectors)
{
  struct file *file = vd->vd_backing_file;
  struct inode *inode = file->f_mapping->host;
  struct kstatfs sbuf;

  /*
   * If the backing device is a block device, mirror its zeroing
   * capability. Set the discard sectors to the block device's zeroing
   * capabilities because mwvd discards result in blkdev_issue_zeroout(),
   * not blkdev_issue_discard(). This maintains consistent behavior with
   * file-backed mwvd devices: discarded regions read back as zero.
   */
  if (S_ISBLK(inode->i_mode))
  {
    struct block_device *bdev = I_BDEV(inode);

    *max_discard_sectors = bdev_write_zeroes_sectors(bdev);
    *granularity = bdev_discard_granularity(bdev);

    /*
     * We use punch hole to reclaim the free space used by the
     * image a.k.a. discard.
     */
  }
  else if (file->f_op->fallocate && !vfs_statfs(&file->f_path, &sbuf))
  {
    *max_discard_sectors = UINT_MAX >> 9;
    *granularity = sbuf.f_bsize;
  }
}
#else
static void mwvd_config_discard(struct mwvd_device *vd)
{
  struct file *file = vd->vd_backing_file;
  struct inode *inode = file->f_mapping->host;
  struct request_queue *q = vd->vd_queue;
  u32 granularity, max_discard_sectors;

  /*
   * If the backing device is a block device, mirror its zeroing
   * capability. Set the discard sectors to the block device's zeroing
   * capabilities because mwvd discards result in blkdev_issue_zeroout(),
   * not blkdev_issue_discard(). This maintains consistent behavior with
   * file-backed mwvd devices: discarded regions read back as zero.
   */
  if (S_ISBLK(inode->i_mode))
  {
    struct request_queue *backingq = bdev_get_queue(I_BDEV(inode));

    max_discard_sectors = backingq->limits.max_write_zeroes_sectors;
    granularity = bdev_discard_granularity(I_BDEV(inode)) ?: queue_physical_block_size(backingq);

    /*
     * We use punch hole to reclaim the free space used by the
     * image a.k.a. discard.
     */
  }
  else if (!file->f_op->fallocate)
  {
    max_discard_sectors = 0;
    granularity = 0;
  }
  else
  {
    struct kstatfs sbuf;

    max_discard_sectors = UINT_MAX >> 9;
    if (!vfs_statfs(&file->f_path, &sbuf))
      granularity = sbuf.f_bsize;
    else
      max_discard_sectors = 0;
  }

  if (max_discard_sectors)
  {
    q->limits.discard_granularity = granularity;
    blk_queue_max_discard_sectors(q, max_discard_sectors);
    blk_queue_max_write_zeroes_sectors(q, max_discard_sectors);
  }
  else
  {
    q->limits.discard_granularity = 0;
    blk_queue_max_discard_sectors(q, 0);
    blk_queue_max_write_zeroes_sectors(q, 0);
  }
}
#endif

struct mwvd_worker
{
  struct rb_node rb_node;
  struct work_struct work;
  struct list_head cmd_list;
  struct list_head idle_list;
  struct mwvd_device *vd;
  struct cgroup_subsys_state *blkcg_css;
  unsigned long last_ran_at;
};

static void mwvd_workfn(struct work_struct *work);

#ifdef CONFIG_BLK_CGROUP
static inline int mwvd_queue_on_root_worker(struct cgroup_subsys_state *css)
{
  return !css || css == blkcg_root_css;
}
#else
static inline int mwvd_queue_on_root_worker(struct cgroup_subsys_state *css)
{
  return !css;
}
#endif

static void mwvd_queue_work(struct mwvd_device *vd, struct mwvd_cmd *cmd)
{
  struct rb_node **node, *parent = NULL;
  struct mwvd_worker *cur_worker, *worker = NULL;
  struct work_struct *work;
  struct list_head *cmd_list;

  spin_lock_irq(&vd->vd_work_lock);

  if (mwvd_queue_on_root_worker(cmd->blkcg_css))
    goto queue_work;

  node = &vd->worker_tree.rb_node;

  while (*node)
  {
    parent = *node;
    cur_worker = container_of(*node, struct mwvd_worker, rb_node);
    if (cur_worker->blkcg_css == cmd->blkcg_css)
    {
      worker = cur_worker;
      break;
    }
    else if ((long)cur_worker->blkcg_css < (long)cmd->blkcg_css)
    {
      node = &(*node)->rb_left;
    }
    else
    {
      node = &(*node)->rb_right;
    }
  }
  if (worker)
    goto queue_work;

  worker = kzalloc(sizeof(struct mwvd_worker), GFP_NOWAIT | __GFP_NOWARN);
  /*
   * In the event we cannot allocate a worker, just queue on the
   * rootcg worker and issue the I/O as the rootcg
   */
  if (!worker)
  {
    cmd->blkcg_css = NULL;
    if (cmd->memcg_css)
      css_put(cmd->memcg_css);
    cmd->memcg_css = NULL;
    goto queue_work;
  }

  worker->blkcg_css = cmd->blkcg_css;
  css_get(worker->blkcg_css);
  INIT_WORK(&worker->work, mwvd_workfn);
  INIT_LIST_HEAD(&worker->cmd_list);
  INIT_LIST_HEAD(&worker->idle_list);
  worker->vd = vd;
  rb_link_node(&worker->rb_node, parent, node);
  rb_insert_color(&worker->rb_node, &vd->worker_tree);
queue_work:
  if (worker)
  {
    /*
     * We need to remove from the idle list here while
     * holding the lock so that the idle timer doesn't
     * free the worker
     */
    if (!list_empty(&worker->idle_list))
      list_del_init(&worker->idle_list);
    work = &worker->work;
    cmd_list = &worker->cmd_list;
  }
  else
  {
    work = &vd->rootcg_work;
    cmd_list = &vd->rootcg_cmd_list;
  }
  list_add_tail(&cmd->list_entry, cmd_list);
  queue_work(vd->workqueue, work);
  spin_unlock_irq(&vd->vd_work_lock);
}

static void mwvd_set_timer(struct mwvd_device *vd)
{
  timer_reduce(&vd->timer, jiffies + MWVD_IDLE_WORKER_TIMEOUT);
}

static void mwvd_free_idle_workers(struct mwvd_device *vd, bool delete_all)
{
  struct mwvd_worker *pos, *worker;

  spin_lock_irq(&vd->vd_work_lock);
  list_for_each_entry_safe(worker, pos, &vd->idle_worker_list,
                           idle_list)
  {
    if (!delete_all &&
        time_is_after_jiffies(worker->last_ran_at +
                              MWVD_IDLE_WORKER_TIMEOUT))
      break;
    list_del(&worker->idle_list);
    rb_erase(&worker->rb_node, &vd->worker_tree);
    css_put(worker->blkcg_css);
    kfree(worker);
  }
  if (!list_empty(&vd->idle_worker_list))
    mwvd_set_timer(vd);
  spin_unlock_irq(&vd->vd_work_lock);
}

static void mwvd_free_idle_workers_timer(struct timer_list *timer)
{
  struct mwvd_device *vd = container_of(timer, struct mwvd_device, timer);

  return mwvd_free_idle_workers(vd, false);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 9, 0)
static void mwvd_update_rotational(struct mwvd_device *vd)
{
  struct file *file = vd->vd_backing_file;
  struct inode *file_inode = file->f_mapping->host;
  struct block_device *file_bdev = file_inode->i_sb->s_bdev;
  struct request_queue *q = vd->vd_queue;
  bool nonrot = true;

  /* not all filesystems (e.g. tmpfs) have a sb->s_bdev */
  if (file_bdev)
    nonrot = bdev_nonrot(file_bdev);

  if (nonrot)
    blk_queue_flag_set(QUEUE_FLAG_NONROT, q);
  else
    blk_queue_flag_clear(QUEUE_FLAG_NONROT, q);
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
static void mwvd_update_limits(struct mwvd_device *vd, struct queue_limits *lim,
                               unsigned int bsize)
{
  struct file *file = vd->vd_backing_file;
  struct inode *inode = file->f_mapping->host;
  struct block_device *backing_bdev = NULL;
  u32 granularity = 0, max_discard_sectors = 0;

  if (S_ISBLK(inode->i_mode))
    backing_bdev = I_BDEV(inode);
  else if (inode->i_sb->s_bdev)
    backing_bdev = inode->i_sb->s_bdev;

  if (!bsize)
    bsize = SECTOR_SIZE;

  mwvd_get_discard_config(vd, &granularity, &max_discard_sectors);

  lim->logical_block_size = bsize;
  lim->physical_block_size = bsize;
  lim->io_min = bsize;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
  lim->features &= ~(BLK_FEAT_WRITE_CACHE | BLK_FEAT_ROTATIONAL);
  if (file->f_op->fsync && !(vd->vd_flags & MWVD_FLAGS_READ_ONLY))
    lim->features |= BLK_FEAT_WRITE_CACHE;
  if (backing_bdev && !bdev_nonrot(backing_bdev))
    lim->features |= BLK_FEAT_ROTATIONAL;
#endif
  lim->max_hw_discard_sectors = max_discard_sectors;
  lim->max_write_zeroes_sectors = max_discard_sectors;
  if (max_discard_sectors)
    lim->discard_granularity = granularity;
  else
    lim->discard_granularity = 0;
}
#endif

/* 仅用作 bd_prepare_to_claim / bd_abort_claiming 的持有者标识 */
static int mwvd_claim_holder(void) { return 0; }

/* ================================================================== */

static loff_t mwvd_get_filesize(struct mwvd_device *vd)
{
  return i_size_read(vd->vd_backing_file->f_mapping->host);
}

static int mwvd_configure(struct mwvd_device *vd, blk_mode_t mode,
                                 struct block_device *bdev, struct file *file,
                                 const struct mwvd_config *config)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
  struct queue_limits lim;
#else
  struct inode *inode;
  struct address_space *mapping;
  unsigned short bsize;
#endif
  int error;
  sector_t ssize = 0;
  bool partscan;
  bool is_mwvd_dev;

  if (!file)
    return -EBADF;

  error = mwvd_check_backing_file(file);
  if (error)
    goto out_putf;

  is_mwvd_dev = is_mwvd_device(file);

  /* This is safe, since we have a reference from open(). */
  __module_get(THIS_MODULE);

  if (!(mode & BLK_OPEN_EXCL))
  {
    error = bd_prepare_to_claim(bdev, mwvd_claim_holder, NULL);
    if (error)
      goto out_putf;
  }

  error = mwvd_global_lock_killable(vd, is_mwvd_dev);
  if (error)
    goto out_bdev;

  error = -EBUSY;
  if (vd->vd_state != MWVD_UNBOUND)
    goto out_unlock;

  error = mwvd_validate_file(file, bdev);
  if (error)
    goto out_unlock;

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 9, 0)
  mapping = file->f_mapping;
  inode = mapping->host;

  if (config->block_size)
  {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
    error = blk_validate_block_size(config->block_size);
#else
    /* blk_validate_block_size 在 6.0 前不存在 */
    if (config->block_size < 512 || config->block_size > PAGE_SIZE ||
        !is_power_of_2(config->block_size))
      error = -EINVAL;
#endif
    if (error)
      goto out_unlock;
  }
#endif

  /* 如果 vd_file_name 为空（例如通过 MWVD_SET_FD ioctl 传入 fd），
   * 从 struct file 获取文件路径以支持差分镜像父文件解析。 */
  if (!vd->vd_file_name[0]) {
    char *path_buf = kmalloc(PATH_MAX, GFP_KERNEL);
    if (path_buf) {
      char *p = d_path(&file->f_path, path_buf, PATH_MAX);
      if (!IS_ERR(p))
        strscpy(vd->vd_file_name, p, MWVD_NAME_SIZE);
      kfree(path_buf);
    }
  }
  vd->vd_flags = config->vd_flags;

  if (!(file->f_mode & FMODE_WRITE) || !(mode & BLK_OPEN_WRITE) ||
      !file->f_op->write_iter)
    vd->vd_flags |= MWVD_FLAGS_READ_ONLY;

  if (!vd->workqueue)
  {
    vd->workqueue = alloc_workqueue("mwvd%d",
                                    WQ_UNBOUND | WQ_FREEZABLE,
                                    1, vd->vd_number);
    if (!vd->workqueue)
    {
      error = -ENOMEM;
      goto out_unlock;
    }
  }

  /* suppress uevents while reconfiguring the device */
  dev_set_uevent_suppress(disk_to_dev(vd->vd_disk), 1);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
  disk_force_media_change(vd->vd_disk
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
                          , 0
#endif
  );
#endif
  set_disk_ro(vd->vd_disk, (vd->vd_flags & MWVD_FLAGS_READ_ONLY) != 0);

  vd->vd_device = bdev;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
  mwvd_assign_backing_file(vd, file);

  lim = queue_limits_start_update(vd->vd_queue);
  mwvd_update_limits(vd, &lim, config->block_size);
  /* No need to freeze the queue as the device isn't bound yet. */
  error = queue_limits_commit_update(vd->vd_queue, &lim);
  if (error)
    goto out_unlock;

  /* 6.9-6.10: features field not available, use old API for write cache/rotational */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
  if (!(vd->vd_flags & MWVD_FLAGS_READ_ONLY) && file->f_op->fsync)
    blk_queue_write_cache(vd->vd_queue, true, false);
  {
    struct block_device *rot_bdev = NULL;
    struct inode *rot_inode = file->f_mapping->host;
    if (S_ISBLK(rot_inode->i_mode))
      rot_bdev = I_BDEV(rot_inode);
    else if (rot_inode->i_sb->s_bdev)
      rot_bdev = rot_inode->i_sb->s_bdev;
    if (rot_bdev && !bdev_nonrot(rot_bdev))
      blk_queue_flag_clear(QUEUE_FLAG_NONROT, vd->vd_queue);
  }
#endif

  /*
   * Flush backing file page cache before binding.
   */
  vfs_fsync(file, 0);
#else
  vd->vd_backing_file = file;
  vd->VDType = VD_TYPE_UNKNOWN;
  vd->old_gfp_mask = mapping_gfp_mask(mapping);
  mapping_set_gfp_mask(mapping, vd->old_gfp_mask & ~(__GFP_IO | __GFP_FS));

  if (!(vd->vd_flags & MWVD_FLAGS_READ_ONLY) && file->f_op->fsync)
    blk_queue_write_cache(vd->vd_queue, true, false);

  if (config->block_size)
    bsize = config->block_size;
  else
    bsize = 512;

  blk_queue_logical_block_size(vd->vd_queue, bsize);
  blk_queue_physical_block_size(vd->vd_queue, bsize);
  blk_queue_io_min(vd->vd_queue, bsize);

  mwvd_config_discard(vd);
  mwvd_update_rotational(vd);
#endif

  /* 记录镜像文件大小 */
  vd->FileSize = mwvd_get_filesize(vd);
  vd->VDType = VD_TYPE_UNKNOWN;

  /* 打开并初始化镜像 */
  pr_debug("mwvd%d: opening image (size=%lld)\n", vd->vd_number, (long long)vd->FileSize);
  {
    struct vdf_open_result vdf_res;

    memset(&vdf_res, 0, sizeof(vdf_res));
    vd->vdf_state = vd_open_image(vd->vd_backing_file, vd->FileSize,
                                         vd->vd_number, "child", &vdf_res);
    if (!vd->vdf_state) {
      /* 镜像无法打开：拒绝绑定，避免 0 容量空盘占用设备号
       * （此前该设备号被绑定为 0 容量后无法再次挂载）。 */
      pr_err("mwvd%d: failed to open image file (size=%lld)\n",
             vd->vd_number, (long long)vd->FileSize);
      error = -EINVAL;
      goto out_unlock;
    }
    if (vd->vdf_state) {
      ssize = vdf_res.total_sectors;

      pr_info("mwvd%d: image opened: sectors=%lld sector_size=%d format=%d parent=%s\n",
              vd->vd_number, (long long)vdf_res.total_sectors,
              vdf_res.sector_size, vdf_res.format,
              vdf_res.parent_name[0] ? vdf_res.parent_name : "(none)");

      /*
       * 根据 VHDX 实际逻辑扇区大小同步块设备队列。
       * VHDX 可能使用 4096 字节扇区，而块设备默认设为 512。
       * 不匹配会导致扇区→块的映射偏移 8 倍，深层数据读取到错误位置。
       * 若用户显式指定了 config->block_size，则优先使用用户设置。
       */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
      {
        unsigned int cur_lbs = queue_logical_block_size(vd->vd_queue);
        pr_debug("mwvd%d: sector_size=%d queue logical_block_size=%u %s\n",
                 vd->vd_number, vdf_res.sector_size, cur_lbs,
                 (!config->block_size && vdf_res.sector_size > 0 &&
                  (unsigned int)vdf_res.sector_size != cur_lbs) ?
                 "-> NEED SYNC" : "-> OK");
      }
      if (!config->block_size && vdf_res.sector_size > 0 &&
          (unsigned int)vdf_res.sector_size !=
          queue_logical_block_size(vd->vd_queue)) {
        struct queue_limits lim;
        lim = queue_limits_start_update(vd->vd_queue);
        lim.logical_block_size = vdf_res.sector_size;
        lim.physical_block_size = vdf_res.sector_size;
        lim.io_min = vdf_res.sector_size;
        queue_limits_commit_update(vd->vd_queue, &lim);
        pr_info("mwvd%d: logical block size synced to sector size %d\n",
                vd->vd_number, vdf_res.sector_size);
      }
#else
      {
        unsigned int cur_lbs = queue_logical_block_size(vd->vd_queue);
        pr_debug("mwvd%d: sector_size=%d queue logical_block_size=%u %s\n",
                 vd->vd_number, vdf_res.sector_size, cur_lbs,
                 (!config->block_size && vdf_res.sector_size > 0 &&
                  vdf_res.sector_size != cur_lbs) ?
                 "-> NEED SYNC" : "-> OK");
      }
      if (!config->block_size && vdf_res.sector_size > 0 &&
          vdf_res.sector_size != queue_logical_block_size(vd->vd_queue)) {
        blk_queue_logical_block_size(vd->vd_queue, vdf_res.sector_size);
        blk_queue_physical_block_size(vd->vd_queue, vdf_res.sector_size);
        blk_queue_io_min(vd->vd_queue, vdf_res.sector_size);
        pr_info("mwvd%d: logical block size synced to sector size %d\n",
                vd->vd_number, vdf_res.sector_size);
      }
#endif

      /* 差分盘：如有父镜像则递归打开父链 */
      if (vdf_res.parent_name[0]) {
        int perr = vd_open_parent_image(vd, &vdf_res, 0);
        if (perr < 0) {
          pr_err("mwvd%d: failed to open parent image: %d\n",
                 vd->vd_number, perr);
          vd_close_image(vd);
          error = perr;
          goto out_unlock;
        }
      }

      /* 打开后立即应用待重放的 VHDX 日志（规范要求打开前重放） */
      vd_replay_pending_log(vd);
    }
  }

  /* ssize 已在打开镜像时设置 */
  /* 先计算 partscan 再打印：此前 pr_info 在 vd_flags|=PARTSCAN 之前执行，
   * partscan 永远显示 0（即使实际已启用分区扫描，max_part=8 → part_shift=4），
   * 误导调试判断。 */
  if (part_shift)
    vd->vd_flags |= MWVD_FLAGS_PARTSCAN;
  partscan = vd->vd_flags & MWVD_FLAGS_PARTSCAN;
  pr_info("mwvd%d: setting capacity to %lld sectors (%lld bytes), block_size=%u, partscan=%d\n",
          vd->vd_number, (long long)ssize, (long long)ssize * 512,
          queue_logical_block_size(vd->vd_queue),
          partscan ? 1 : 0);
  mwvd_set_size(vd, ssize);

  /* Order wrt reading vd_state in mwvd_validate_file(). */
  wmb();

  vd->vd_state = MWVD_BOUND;
  if (partscan)
    clear_gd_suppress(vd->vd_disk);

  dev_set_uevent_suppress(disk_to_dev(vd->vd_disk), 0);
  kobject_uevent(&disk_to_dev(vd->vd_disk)->kobj, KOBJ_CHANGE);

  mwvd_global_unlock(vd, is_mwvd_dev);
  if (partscan)
    mwvd_reread_partitions(vd);

  if (!(mode & BLK_OPEN_EXCL))
    bd_abort_claiming(bdev, mwvd_claim_holder);

  return 0;

out_unlock:
  mwvd_global_unlock(vd, is_mwvd_dev);
out_bdev:
  if (!(mode & BLK_OPEN_EXCL))
    bd_abort_claiming(bdev, mwvd_claim_holder);
out_putf:
  fput(file);
  /* This is safe: open() is still holding a reference. */
  module_put(THIS_MODULE);
  return error;
}

/* mwvd_claim_holder — see declaration above */

/**
 * mwvd_device_get - 根据索引获取 mwvd_device
 * @index: 设备编号（如 0 表示 mwvd0）
 * @bdev: 输出参数，返回打开的 block_device（需要调用 mwvd_device_put 释放）
 *
 * 返回 mwvd_device 指针，失败返回 ERR_PTR。
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
static struct mwvd_device *mwvd_device_get(unsigned int index, struct file **bdev_file)
{
  struct block_device *bdev;

  *bdev_file = bdev_file_open_by_dev(MKDEV(MWVDISK_MAJOR, index << part_shift),
                                      FMODE_READ | FMODE_WRITE, NULL, NULL);
  if (IS_ERR(*bdev_file))
    return ERR_CAST(*bdev_file);

  bdev = file_bdev(*bdev_file);
  return bdev->bd_disk->private_data;
}

static void mwvd_device_put(struct file *bdev_file)
{
  fput(bdev_file);
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
/* 6.8: blkdev_get_by_dev/blkdev_put 移除，引入 bdev_open_by_dev/bdev_release */
static struct mwvd_device *mwvd_device_get(unsigned int index, struct bdev_handle **handle)
{
  *handle = bdev_open_by_dev(MKDEV(MWVDISK_MAJOR, index << part_shift),
                              FMODE_READ | FMODE_WRITE, NULL, NULL);
  if (IS_ERR(*handle))
    return ERR_CAST(*handle);

  return (*handle)->bdev->bd_disk->private_data;
}

static void mwvd_device_put(struct bdev_handle *handle)
{
  bdev_release(handle);
}
#else
static struct mwvd_device *mwvd_device_get(unsigned int index, struct block_device **bdev)
{
  *bdev = blkdev_get_by_dev(MKDEV(MWVDISK_MAJOR, index << part_shift),
                              FMODE_READ | FMODE_WRITE, NULL, NULL);
  if (IS_ERR(*bdev))
    return ERR_CAST(*bdev);

  return (*bdev)->bd_disk->private_data;
}

static void mwvd_device_put(struct block_device *bdev)
{
  blkdev_put(bdev
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0)
             , 0
#else
             , NULL
#endif
  );
}
#endif

/**
 * mwvd_configure_backing_path - 在自动挂载上下文中配置 mwvd 设备
 * @index: 设备编号（如 0 表示 mwvd0）
 * @root_path: 挂载上下文路径
 * @path: 镜像文件在挂载内的相对路径
 * @part_uuid: 分区 GUID（休眠标记用；可 NULL）
 */
int mwvd_configure_backing_path(unsigned int index, struct path *root_path,
                                 const char *path, const char *part_uuid)
{
  struct mwvd_device *vd;
  struct file *file;
  struct mwvd_config config;
  int err;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
  struct file *bdev_file = NULL;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
  struct bdev_handle *bdev_handle = NULL;
#else
  struct block_device *bdev = NULL;
#endif

  if (!path || !*path)
    return -EINVAL;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
  vd = mwvd_device_get(index, &bdev_file);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
  vd = mwvd_device_get(index, &bdev_handle);
#else
  vd = mwvd_device_get(index, &bdev);
#endif
  if (IS_ERR(vd))
  {
    pr_err("mwvd%d: get block device failed: %ld\n", index, PTR_ERR(vd));
    return PTR_ERR(vd);
  }

  /*
   * 保存固件 v2 条目里的镜像路径（不是 d_path 得到的宿主挂载路径）。
   * BootUI 的休眠唤醒按 part_uuid + vdisk_path 精确匹配菜单条目；
   * Linux 侧路径分隔符是 '/'，写标记时再统一转换为 '\' 绝对路径。
   */
  strscpy(vd->vd_fw_path, path, sizeof(vd->vd_fw_path));

  file = vd_file_open_root(root_path, path, O_RDWR | O_LARGEFILE, 0);
  if (IS_ERR(file)) {
    err = PTR_ERR(file);
    if (err == -EACCES || err == -EROFS)
      file = vd_file_open_root(root_path, path, O_RDONLY | O_LARGEFILE, 0);
  }
  if (IS_ERR(file))
  {
    err = PTR_ERR(file);
    /* 区分诊断：-ENOENT(-2)=文件不存在；-ENXIO(-6)=inode 无 open 方法
     * （NTFS reparse point/符号链接/特殊文件）；-EISDIR(-21)=路径指向目录 */
    pr_err("mwvd%d: open image \"%s\" failed: %d (%s)\n",
           index, path, err,
           err == -ENOENT ? "no such file" :
           err == -ENXIO  ? "inode has no open method (special/reparse file)" :
           err == -EISDIR ? "is a directory" : "unknown");
    goto out_put_dev;
  }

  /* 必须先保存 root_path 到 vd 设备，供 vd_open（在 mwvd_configure 内调用）打开父镜像时使用 */
  if (root_path && root_path->mnt) {
    vd->root_path = *root_path;
    path_get(&vd->root_path);
  }

  /* 保存分区 GUID（休眠标记定位镜像用） */
  if (part_uuid && *part_uuid)
    strscpy(vd->vd_part_uuid, part_uuid, sizeof(vd->vd_part_uuid));

  memset(&config, 0, sizeof(config));
  config.vd_flags = MWVD_FLAGS_PARTSCAN;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
  err = mwvd_configure(vd, file->f_mode, file_bdev(bdev_file),
                               file, &config);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
  err = mwvd_configure(vd, file->f_mode, bdev_handle->bdev,
                               file, &config);
#else
  err = mwvd_configure(vd, file->f_mode, bdev, file, &config);
#endif
  if (err)
  {
    pr_err("mwvd%d: configure from \"%s\" failed: %d\n", index, path, err);
    goto out_put_file;
  }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
  mwvd_device_put(bdev_file);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
  mwvd_device_put(bdev_handle);
#else
  mwvd_device_put(bdev);
#endif
  return 0;

out_put_file:
  /* mwvd_configure 失败时清理已保存的 root_path */
  path_put(&vd->root_path);
  memset(&vd->root_path, 0, sizeof(vd->root_path));
  fput(file);
out_put_dev:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
  mwvd_device_put(bdev_file);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
  mwvd_device_put(bdev_handle);
#else
  mwvd_device_put(bdev);
#endif
  return err;
}

static void __mwvd_clr_fd(struct mwvd_device *vd, bool release)
{
  struct file *filp;
  struct block_device *bdev;
  gfp_t gfp = vd->old_gfp_mask;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
  struct queue_limits lim;
#else
  if (test_bit(QUEUE_FLAG_WC, &vd->vd_queue->queue_flags))
    blk_queue_write_cache(vd->vd_queue, false, false);

  /*
   * Freeze the request queue when unbinding on a live file descriptor and
   * thus an open device.  When called from ->release we are guaranteed
   * that there is no I/O in progress already.
   */
  if (!release)
    blk_mq_freeze_queue(vd->vd_queue);
#endif

  spin_lock_irq(&vd->vd_lock);
  filp = vd->vd_backing_file;
  spin_unlock_irq(&vd->vd_lock);

  /*
   * 清除虚拟磁盘相关数据结构（标准化函数调用，参考 v1.3.1 的 vd_close 模式）。
   */
  vd_close_image(vd);

  spin_lock_irq(&vd->vd_lock);
  vd->vd_backing_file = NULL;
  vd->VDType = VD_TYPE_UNKNOWN;
  spin_unlock_irq(&vd->vd_lock);

  /* 保存 bdev 副本后再清空字段：5.15(<6.0) 走 invalidate_bdev(bdev)，
   * 若先置 NULL 再取 vd->vd_device 会变成 invalidate_bdev(NULL) → 内核 Oops，
   * 进程被杀死、设备残留（对齐 loop __loop_clr_fd 的写法）。 */
  bdev = vd->vd_device;
  vd->vd_device = NULL;
  memset(vd->vd_file_name, 0, MWVD_NAME_SIZE);
  memset(vd->vd_fw_path, 0, sizeof(vd->vd_fw_path));
  memset(vd->vd_part_uuid, 0, sizeof(vd->vd_part_uuid)); /* 解绑清分区 GUID，防残留 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
  /*
   * Reset the block size to the default.
   *
   * No queue freezing needed because this is called from the final
   * ->release call only, so there can't be any outstanding I/O.
   */
  lim = queue_limits_start_update(vd->vd_queue);
  lim.logical_block_size = SECTOR_SIZE;
  lim.physical_block_size = SECTOR_SIZE;
  lim.io_min = SECTOR_SIZE;
  queue_limits_commit_update(vd->vd_queue, &lim);
#else
  blk_queue_logical_block_size(vd->vd_queue, 512);
  blk_queue_physical_block_size(vd->vd_queue, 512);
  blk_queue_io_min(vd->vd_queue, 512);
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
  invalidate_disk(vd->vd_disk);
#else
  if (bdev)
    invalidate_bdev(bdev);
#endif
  /* let user-space know about this change */
  kobject_uevent(&disk_to_dev(vd->vd_disk)->kobj, KOBJ_CHANGE);
  mapping_set_gfp_mask(filp->f_mapping, gfp);
  /* This is safe: open() is still holding a reference. */
  module_put(THIS_MODULE);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 9, 0)
  if (!release)
    blk_mq_unfreeze_queue(vd->vd_queue);
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
  disk_force_media_change(vd->vd_disk
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
                          , 0
#endif
  );
#endif

  /* 清除容量：detach 后 backing 已释放，容量必须归零再重扫分区。
   * 若保留旧容量，bdev_disk_changed 重扫会尝试读完整分区表（sector 0/24），
   * 而 backing 已被清理 → 全部 EIO（"unable to read partition table /
   * I/O error on dev mwvdN"），旧分区也不随 detach 移除。
   * 容量归零后重扫只移除旧分区、不再读 backing（对齐 loop __loop_clr_fd
   * 的 set_capacity(0)）。 */
  set_capacity(vd->vd_disk, 0);

  if (vd->vd_flags & MWVD_FLAGS_PARTSCAN)
  {
    int err;

    /*
     * open_mutex has been held already in release path, so don't
     * acquire it if this function is called in such case.
     *
     * If the reread partition isn't from release path, lo_refcnt
     * must be at least one and it can only become zero when the
     * current holder is released.
     */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
    err = bdev_disk_changed(vd->vd_disk, false);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
    if (!release)
      mutex_lock(&vd->vd_disk->open_mutex);
    err = bdev_disk_changed(vd->vd_disk, false);
    if (!release)
      mutex_unlock(&vd->vd_disk->open_mutex);
#else
    /* 5.10: vd->vd_device 上方已置 NULL，须用本函数开头保存的 bdev 副本
     * （= vd->vd_device），否则 mutex_lock(NULL->bd_mutex) 立即 Oops。 */
    if (!release)
      mutex_lock(&bdev->bd_mutex);
    err = bdev_disk_changed(bdev, false);
    if (!release)
      mutex_unlock(&bdev->bd_mutex);
#endif
    if (err)
      pr_warn("%s: partition scan of mwvd%d failed (rc=%d)\n",
              __func__, vd->vd_number, err);
    /* Device is gone, no point in returning error */
  }

  /*
   * vd->vd_state is set to MWVD_UNBOUND here after above partscan has
   * finished. There cannot be anybody else entering __mwvd_clr_fd() as
   * MWVD_RUNDOWN state protects us from all the other places trying to
   * change the 'vd' device.
   */
  vd->vd_flags = 0;
  if (!part_shift)
    set_gd_suppress(vd->vd_disk);
  mutex_lock(&vd->vd_mutex);
  vd->vd_state = MWVD_UNBOUND;
  mutex_unlock(&vd->vd_mutex);

  /* 清理自动挂载上下文 */
  path_put(&vd->root_path);
  memset(&vd->root_path, 0, sizeof(vd->root_path));

  /*
   * Need not hold vd_mutex to fput backing file. Calling fput holding
   * vd_mutex triggers a circular lock dependency possibility warning as
   * fput can take open_mutex which is usually taken before vd_mutex.
   */
  if (filp->f_op->fsync)
    vfs_fsync(filp, 0);
  fput(filp);
}

static int mwvd_clr_fd(struct mwvd_device *vd)
{
  int err;

  /*
   * Since lo_ioctl() is called without locks held, it is possible that
   * mwvd_configure()/mwvd_change_fd() and mwvd_clr_fd() run in parallel.
   *
   * Therefore, use global lock when setting MWVD_RUNDOWN state in order to
   * make sure that mwvd_validate_file() will fail if the "struct file"
   * which mwvd_configure()/mwvd_change_fd() found via fget() was this
   * mwvd device.
   */
  err = mwvd_global_lock_killable(vd, true);
  if (err)
    return err;
  if (vd->vd_state != MWVD_BOUND)
  {
    mwvd_global_unlock(vd, true);
    return -ENXIO;
  }
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
  /*
   * If we've explicitly asked to tear down the mwvd device,
   * and it has an elevated reference count, set it for auto-teardown when
   * the last reference goes away. This stops $!~#$@ udev from
   * preventing teardown because it decided that it needs to run blkid on
   * the loopback device whenever they appear. xfstests is notorious for
   * failing tests because blkid via udev races with a losetup
   * <dev>/do something like mkfs/losetup -d <dev> causing the losetup -d
   * command to fail with EBUSY.
   */
  if (vd_disk_openers(vd->vd_disk, vd->vd_device) > 1)
  {
    vd->vd_flags |= MWVD_FLAGS_AUTOCLEAR;
    mwvd_global_unlock(vd, true);
    return 0;
  }
  vd->vd_state = MWVD_RUNDOWN;
  mwvd_global_unlock(vd, true);

  __mwvd_clr_fd(vd, false);
#else
  /*
   * Mark the device for removing the backing device on last close.
   * If we are the only opener, also switch the state to roundown here to
   * prevent new openers from coming in.
   */

  vd->vd_flags |= MWVD_FLAGS_AUTOCLEAR;
  if (vd_disk_openers(vd->vd_disk, vd->vd_device) == 1)
    vd->vd_state = MWVD_RUNDOWN;
  mwvd_global_unlock(vd, true);
#endif

  return 0;
}

/**
 * mwvd_clear_backing - 清除指定 mwvd 设备的 backing file
 * @index: 设备编号（如 0 表示 mwvd0）
 *
 * 供 mwvd_monitor 模块在卸载时调用。
 */
int mwvd_clear_backing(unsigned int index)
{
  struct mwvd_device *vd;
  int err;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
  struct file *bdev_file = NULL;

  vd = mwvd_device_get(index, &bdev_file);
  if (IS_ERR(vd))
    return PTR_ERR(vd);

  err = mwvd_clr_fd(vd);
  mwvd_device_put(bdev_file);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
  struct bdev_handle *bdev_handle = NULL;

  vd = mwvd_device_get(index, &bdev_handle);
  if (IS_ERR(vd))
    return PTR_ERR(vd);

  err = mwvd_clr_fd(vd);
  mwvd_device_put(bdev_handle);
#else
  struct block_device *bdev = NULL;

  vd = mwvd_device_get(index, &bdev);
  if (IS_ERR(vd))
    return PTR_ERR(vd);

  err = mwvd_clr_fd(vd);
  mwvd_device_put(bdev);
#endif
  return err;
}

static int mwvd_ioctl(struct block_device *bdev, blk_mode_t mode,
                      unsigned int cmd, unsigned long arg)
{
  struct mwvd_device *vd = bdev->bd_disk->private_data;
  int err;

  switch (cmd)
  {
  case MWVD_SET_FD:
  {
    struct file *file = fget(arg);
    struct mwvd_config config;

    if (!file)
      return -EBADF;
    memset(&config, 0, sizeof(config));
    return mwvd_configure(vd, mode, bdev, file, &config);
  }
  case MWVD_CLR_FD:
    return mwvd_clr_fd(vd);
  case MWVD_GET_STATUS:
  {
    struct mwvd_status __user *ust = (struct mwvd_status __user *)arg;
    struct mwvd_status st;

    if (!ust)
      return -EINVAL;
    memset(&st, 0, sizeof(st));
    st.number = vd->vd_number;
    st.state = vd->vd_state;
    st.vd_flags = vd->vd_flags;
    if (vd->vd_state == MWVD_BOUND)
      st.file_size = vd->FileSize;
    strscpy(st.file_name, vd->vd_file_name, sizeof(st.file_name));
    if (copy_to_user(ust, &st, sizeof(st)))
      return -EFAULT;
    return 0;
  }
  default:
    err = -ENOTSUPP;
    break;
  }

  return err;
}

/**
 * 打开虚拟磁盘
 *
 *
 *
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0)
static int mwvd_open(struct block_device *bdev, fmode_t mode)
{
  struct mwvd_device *vd = bdev->bd_disk->private_data;
#else
static int mwvd_open(struct gendisk *disk, blk_mode_t mode)
{
  struct mwvd_device *vd = disk->private_data;
#endif
  int err;

  err = mutex_lock_killable(&vd->vd_mutex);
  if (err)
    return err;

  if (vd->vd_state == MWVD_DELETING || vd->vd_state == MWVD_RUNDOWN)
    err = -ENXIO;
  mutex_unlock(&vd->vd_mutex);
  return err;
}

/**
 * 关闭虚拟磁盘
 *
 *
 *
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0)
static void mwvd_release(struct gendisk *disk, fmode_t mode)
#else
static void mwvd_release(struct gendisk *disk)
#endif
{
  struct mwvd_device *vd = disk->private_data;

  if (vd_disk_openers(disk, vd->vd_device) > 0)
    return;

  mutex_lock(&vd->vd_mutex);
  if (vd->vd_state == MWVD_BOUND && (vd->vd_flags & MWVD_FLAGS_AUTOCLEAR))
    vd->vd_state = MWVD_RUNDOWN;
  mutex_unlock(&vd->vd_mutex);

  if (vd->vd_state == MWVD_RUNDOWN)
    __mwvd_clr_fd(vd, true);

  return;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
/**
 * 清理虚拟磁盘关联队列、锁
 *
 *
 */
static void mwvd_free_disk(struct gendisk *disk)
{
  struct mwvd_device *vd = disk->private_data;

  if (vd->workqueue)
    destroy_workqueue(vd->workqueue);
  mwvd_free_idle_workers(vd, true);
  timer_shutdown_sync(&vd->timer);
  mutex_destroy(&vd->vd_mutex);
  kfree(vd);
}
#endif

/*
 * struct block_device_operations 各内核版本的情况：
 *   - WSL2 6.6.x：有 .ioctl
 *   - 上游 7.0.x：有 .ioctl（6.3~6.x 期间的 .locked_ioctl 已还原）
 *   因此统一使用 .ioctl + .compat_ioctl，跨版本最安全。
 */
static const struct block_device_operations vd_fops = {
    .owner = THIS_MODULE,
    .open = mwvd_open,
    .release = mwvd_release,
    .ioctl = mwvd_ioctl,
    .compat_ioctl = mwvd_ioctl,
  #if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
  .free_disk = mwvd_free_disk,
#endif
};

/*
 * And now the modules code and kernel interface.
 */

/*
 * If max_loop is specified, create that many devices upfront.
 * This also becomes a hard limit. If max_loop is not specified,
 * the default isn't a hard limit (as before commit 85c50197716c
 * changed the default value from 0 for max_loop=0 reasons), just
 * create CONFIG_BLK_DEV_MWVD_MIN_COUNT loop devices at module
 * init time. Loop devices can be requested on-demand with the
 * /dev/loop-control interface, or be instantiated by accessing
 * a 'dead' device node.
 */
// static int max_loop = CONFIG_BLK_DEV_MWVD_MIN_COUNT;
/* 按需生成：默认不预创建设备，镜像到来时动态创建 mwvdN，解绑时动态注销。
 * 可通过 max_mwvd 模块参数指定预创建数量（或作为初始设备数）。 */
static int max_loop = 0;

#ifdef CONFIG_BLOCK_LEGACY_AUTOLOAD
static bool max_mwvd_specified;

static int max_mwvd_param_set_int(const char *val,
                                  const struct kernel_param *kp)
{
  int ret;

  ret = param_set_int(val, kp);
  if (ret < 0)
    return ret;

  max_mwvd_specified = true;
  return 0;
}

static const struct kernel_param_ops max_mwvd_param_ops = {
    .set = max_mwvd_param_set_int,
    .get = param_get_int,
};

module_param_cb(max_mwvd, &max_mwvd_param_ops, &max_loop, 0444);
MODULE_PARM_DESC(max_mwvd, "Maximum number of mwvd devices");
#else
module_param_named(max_mwvd, max_loop, int, 0444);
MODULE_PARM_DESC(max_mwvd, "Initial number of mwvd devices");
#endif

module_param(max_part, int, 0444);
MODULE_PARM_DESC(max_part, "Maximum number of partitions per mwvd device");

static int hw_queue_depth = MWVD_DEFAULT_HW_Q_DEPTH;

static int mwvd_set_hw_queue_depth(const char *s, const struct kernel_param *p)
{
  int qd, ret;

  ret = kstrtoint(s, 0, &qd);
  if (ret < 0)
    return ret;
  if (qd < 1)
    return -EINVAL;
  hw_queue_depth = qd;
  return 0;
}

static const struct kernel_param_ops mwvd_hw_qdepth_param_ops = {
    .set = mwvd_set_hw_queue_depth,
    .get = param_get_int,
};

device_param_cb(hw_queue_depth, &mwvd_hw_qdepth_param_ops, &hw_queue_depth, 0444);
MODULE_PARM_DESC(hw_queue_depth, "Queue depth for each hardware queue. Default: " __stringify(MWVD_DEFAULT_HW_Q_DEPTH));

static bool no_autoload;
module_param(no_autoload, bool, 0444);
MODULE_PARM_DESC(no_autoload, "Disable automatic EFI-based image loading");

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("WeiFeng Lu (wave.lu@gmail.com)");
MODULE_VERSION(MWVD_MODULE_VERSION);
MODULE_DESCRIPTION("Multiware virtual disk linux kernel module");
MODULE_ALIAS_BLOCKDEV_MAJOR(MWVDISK_MAJOR);
MODULE_SOFTDEP("pre: ntfs3 exfat");
// MODULE_DEPENDENCY("ntfs3");
// MODULE_DEPENDENCY("exfat");

static void mwvd_complete_rq(struct request *rq)
{
  struct mwvd_cmd *cmd = blk_mq_rq_to_pdu(rq);
  blk_status_t ret = BLK_STS_OK;

  if (cmd->ret < 0)
    ret = errno_to_blk_status(cmd->ret);
  blk_mq_end_request(rq, ret);
}

static blk_status_t mwvd_queue_rq(struct blk_mq_hw_ctx *hctx,
                                  const struct blk_mq_queue_data *bd)
{
  struct request *rq = bd->rq;
  struct mwvd_cmd *cmd = blk_mq_rq_to_pdu(rq);
  struct mwvd_device *vd = rq->q->queuedata;

  blk_mq_start_request(rq);

  if (vd->vd_state != MWVD_BOUND)
    return BLK_STS_IOERR;

  /* 安全气囊：workqueue 未创建时不应有 I/O，若意外到达则拒绝 */
  if (unlikely(!vd->workqueue))
    return BLK_STS_IOERR;

  /* always use the first bio's css */
  cmd->blkcg_css = NULL;
  cmd->memcg_css = NULL;
#ifdef CONFIG_BLK_CGROUP
  if (rq->bio)
  {
    cmd->blkcg_css = bio_blkcg_css(rq->bio);
#ifdef CONFIG_MEMCG
    if (cmd->blkcg_css)
    {
      cmd->memcg_css =
          cgroup_get_e_css(cmd->blkcg_css->cgroup,
                           &memory_cgrp_subsys);
    }
#endif
  }
#endif
  mwvd_queue_work(vd, cmd);

  return BLK_STS_OK;
}

/**
 * 虚拟磁盘命令处理入口
 *
 *
 */
static void mwvd_handle_cmd(struct mwvd_cmd *cmd)
{
  struct cgroup_subsys_state *cmd_blkcg_css = cmd->blkcg_css;
#ifdef CONFIG_MEMCG
  struct cgroup_subsys_state *cmd_memcg_css = cmd->memcg_css;
#endif
  struct request *rq = blk_mq_rq_from_pdu(cmd);
  const bool write = op_is_write(req_op(rq));
  struct mwvd_device *vd = rq->q->queuedata;
  int ret = 0;
#ifdef CONFIG_MEMCG
  struct mem_cgroup *old_memcg = NULL;
#endif

  if (write && (vd->vd_flags & MWVD_FLAGS_READ_ONLY))
  {
    ret = -EIO;
    goto failed;
  }

  /*
   * 设备级唤醒失败：驱动内先重试 replay（对齐 NVMe/SCSI「尽力恢复，永久失败才上抛」），
   * 成功清标记放行，重试失败返回 -EIO 由上层决定重试/放弃。
   * 注：不在此检查 mwvd_pm_suspended——挂起期 workqueue 已带 WQ_FREEZABLE 冻结，
   * 本函数根本不会在挂起期执行；且 S4 恢复跳回快照时该标记被快照捕获为 true 且无
   * POST 事件清除，若用作 I/O 拒绝会导致恢复后 rootfs 不可访问（致命缺陷）。
   * mwvd_pm_suspended 仅作系统级状态记录/调试用途。
   */
  if (READ_ONCE(vd->pm_replay_failed))
  {
    if (vd_replay_pending_log(vd) == 0)
      WRITE_ONCE(vd->pm_replay_failed, false);
    else
    {
      ret = -EIO;
      goto failed;
    }
  }

  if (cmd_blkcg_css)
    kthread_associate_blkcg(cmd_blkcg_css);
#ifdef CONFIG_MEMCG
  if (cmd_memcg_css)
    old_memcg = set_active_memcg(
        mem_cgroup_from_css(cmd_memcg_css));
#endif

  /*
   * vd_handle_request() may complete the request synchronously.
   * Do not touch 'cmd' after it returns.
   */
  ret = vd_handle_request(vd, rq);

  if (cmd_blkcg_css)
    kthread_associate_blkcg(NULL);

#ifdef CONFIG_MEMCG
  if (cmd_memcg_css)
  {
    set_active_memcg(old_memcg);
    css_put(cmd_memcg_css);
  }
#endif
failed:
  if (ret == -EOPNOTSUPP)
    cmd->ret = ret;
  else
    cmd->ret = ret ? -EIO : 0;
  if (likely(!blk_should_fake_timeout(rq->q)))
    blk_mq_complete_request(rq);
}

static void mwvd_process_work(struct mwvd_worker *worker,
                              struct list_head *cmd_list, struct mwvd_device *vd)
{
  int orig_flags = current->flags;
  struct mwvd_cmd *cmd;

  current->flags |= PF_LOCAL_THROTTLE | PF_MEMALLOC_NOIO;
  spin_lock_irq(&vd->vd_work_lock);
  while (!list_empty(cmd_list))
  {
    cmd = container_of(
        cmd_list->next, struct mwvd_cmd, list_entry);
    list_del(cmd_list->next);
    spin_unlock_irq(&vd->vd_work_lock);

    mwvd_handle_cmd(cmd);
    cond_resched();

    spin_lock_irq(&vd->vd_work_lock);
  }

  /*
   * We only add to the idle list if there are no pending cmds
   * *and* the worker will not run again which ensures that it
   * is safe to free any worker on the idle list
   */
  if (worker && !work_pending(&worker->work))
  {
    worker->last_ran_at = jiffies;
    list_add_tail(&worker->idle_list, &vd->idle_worker_list);
    mwvd_set_timer(vd);
  }
  spin_unlock_irq(&vd->vd_work_lock);
  current->flags = orig_flags;
}

static void mwvd_workfn(struct work_struct *work)
{
  struct mwvd_worker *worker =
      container_of(work, struct mwvd_worker, work);
  mwvd_process_work(worker, &worker->cmd_list, worker->vd);
}

static void mwvd_rootcg_workfn(struct work_struct *work)
{
  struct mwvd_device *vd =
      container_of(work, struct mwvd_device, rootcg_work);
  mwvd_process_work(NULL, &vd->rootcg_cmd_list, vd);
}

/*
 * 唤醒后重放 VHDX log（workqueue 串行执行，与 I/O 同 worker 互斥）。
 * POST 回调不直接调用 vd_replay_pending_log（PM 线程与 workqueue 并发会竞争
 * vdf_state），改为排入该 vd 的 workqueue，保证与正常 I/O 串行。
 * per-device 容错：失败仅告警 + 置设备级标记，不影响其余设备。
 */
static void mwvd_replay_workfn(struct work_struct *work)
{
  struct mwvd_device *vd =
      container_of(work, struct mwvd_device, replay_work);

  if (vd_replay_pending_log(vd) < 0) {
    pr_warn("mwvd%d: replay on resume failed\n", vd->vd_number);
    WRITE_ONCE(vd->pm_replay_failed, true);
  } else {
    WRITE_ONCE(vd->pm_replay_failed, false);
  }
}

static const struct blk_mq_ops mwvd_mq_ops = {
    .queue_rq = mwvd_queue_rq,
    .complete = mwvd_complete_rq,
};

static int mwvd_control_add(int i)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
  struct queue_limits lim = {
      /*
       * Random number picked from the historic block max_sectors cap.
       */
      .max_hw_sectors = 2560u,
  };
#endif
  struct mwvd_device *vd;
  struct gendisk *disk;
  int err;

  err = -ENOMEM;
  vd = kzalloc(sizeof(*vd), GFP_KERNEL);
  if (!vd)
    goto out;
  vd->worker_tree = RB_ROOT;
  INIT_LIST_HEAD(&vd->idle_worker_list);
  timer_setup(&vd->timer, mwvd_free_idle_workers_timer, TIMER_DEFERRABLE);
  vd->vd_state = MWVD_UNBOUND;

  err = mutex_lock_killable(&mwvd_ctl_mutex);
  if (err)
    goto out_free_dev;

  /* allocate id, if @id >= 0, we're requesting that specific id */
  if (i >= 0)
  {
    err = idr_alloc(&mwvd_index_idr, vd, i, i + 1, GFP_KERNEL);
    if (err == -ENOSPC)
      err = -EEXIST;
  }
  else
  {
    err = idr_alloc(&mwvd_index_idr, vd, 0, 0, GFP_KERNEL);
  }
  mutex_unlock(&mwvd_ctl_mutex);
  if (err < 0)
    goto out_free_dev;
  i = err;

  vd->tag_set.ops = &mwvd_mq_ops;
  vd->tag_set.nr_hw_queues = 1;
  vd->tag_set.queue_depth = hw_queue_depth;
  vd->tag_set.numa_node = NUMA_NO_NODE;
  vd->tag_set.cmd_size = sizeof(struct mwvd_cmd);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
  vd->tag_set.flags = BLK_MQ_F_STACKING | BLK_MQ_F_NO_SCHED_BY_DEFAULT;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
  vd->tag_set.flags = BLK_MQ_F_SHOULD_MERGE | BLK_MQ_F_STACKING |
                      BLK_MQ_F_NO_SCHED_BY_DEFAULT;
#else
  vd->tag_set.flags = BLK_MQ_F_SHOULD_MERGE | BLK_MQ_F_STACKING;
#endif
  vd->tag_set.driver_data = vd;

  err = blk_mq_alloc_tag_set(&vd->tag_set);
  if (err)
    goto out_free_idr;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
  disk = vd->vd_disk = blk_mq_alloc_disk(&vd->tag_set, &lim, vd);
#else
  disk = vd->vd_disk = blk_mq_alloc_disk(&vd->tag_set, vd);
#endif
#else /* < 5.15 */
  vd->vd_queue = blk_mq_init_queue(&vd->tag_set);
  if (IS_ERR(vd->vd_queue)) {
    err = PTR_ERR(vd->vd_queue);
    goto out_cleanup_tags;
  }
  disk = vd->vd_disk = alloc_disk(1 << part_shift);
  if (!disk) {
    err = -ENOMEM;
    goto out_cleanup_queue;
  }
  disk->queue = vd->vd_queue;
#endif
  if (IS_ERR(disk))
  {
    err = PTR_ERR(disk);
    goto out_cleanup_tags;
  }
  vd->vd_queue = vd->vd_disk->queue;

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 9, 0)
  blk_queue_max_hw_sectors(vd->vd_queue, BLK_SAFE_MAX_SECTORS);
#endif

  /*
   * Disable partition scanning by default. The in-kernel partition
   * scanning can be requested individually per-device during its
   * setup. Userspace can always add and remove partitions from all
   * devices. The needed partition minors are allocated from the
   * extended minor space, the main loop device numbers will continue
   * to match the loop minors, regardless of the number of partitions
   * used.
   *
   * If max_part is given, partition scanning is globally enabled for
   * all loop devices. The minors for the main loop devices will be
   * multiples of max_part.
   *
   * Note: Global-for-all-devices, set-only-at-init, read-only module
   * parameteters like 'max_loop' and 'max_part' make things needlessly
   * complicated, are too static, inflexible and may surprise
   * userspace tools. Parameters like this in general should be avoided.
   */
  if (!part_shift)
    set_bit(GD_SUPPRESS_PART_SCAN, &disk->state);
  mutex_init(&vd->vd_mutex);
  vd->vd_number = i;
  spin_lock_init(&vd->vd_lock);
  spin_lock_init(&vd->vd_work_lock);
  INIT_WORK(&vd->rootcg_work, mwvd_rootcg_workfn);
  INIT_WORK(&vd->replay_work, mwvd_replay_workfn);
  INIT_LIST_HEAD(&vd->rootcg_cmd_list);
  disk->major = MWVDISK_MAJOR;
  disk->first_minor = i << part_shift;
  disk->minors = 1 << part_shift;
  disk->fops = &vd_fops;
  disk->private_data = vd;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
  disk->queue = vd->vd_queue;
#endif
  /* 虚拟盘非可移动介质，不需要内核主动轮询事件；
   * 所需的 uevent（KOBJ_CHANGE）已在 mwvd_set_size / __mwvd_clr_fd 中显式发送。
   * 设置 DISK_EVENT_MEDIA_CHANGE 会在 add_disk 时触发异常的 workqueue 路径。
   */
  disk->events = 0;
  disk->event_flags = 0;
  sprintf(disk->disk_name, "mwvd%d", i);
  /* Make this mwvd device reachable from pathname. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
  err = add_disk(disk);
  if (err)
    goto out_cleanup_disk;
#else
  add_disk(disk);
#endif

  /* Show this mwvd device. */
  mutex_lock(&mwvd_ctl_mutex);
  vd->idr_visible = true;
  mutex_unlock(&mwvd_ctl_mutex);

  return i;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
out_cleanup_disk:
  put_disk(disk);
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
out_cleanup_queue:
  blk_cleanup_queue(vd->vd_queue);
#endif
out_cleanup_tags:
  blk_mq_free_tag_set(&vd->tag_set);
out_free_idr:
  mutex_lock(&mwvd_ctl_mutex);
  idr_remove(&mwvd_index_idr, i);
  mutex_unlock(&mwvd_ctl_mutex);
out_free_dev:
  kfree(vd);
out:
  return err;
}

static void mwvd_remove(struct mwvd_device *vd)
{
  /* Make this mwvd device unreachable from pathname. */
  del_gendisk(vd->vd_disk);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(6, 0, 0)
  /*
   * 5.14~5.19 对齐 loop_remove：释放 tag_set 之前必须先 blk_cleanup_queue。
   * 5.15 上缺此步会导致 UAF Oops：put_disk → disk_release → blk_put_queue →
   * blk_mq_release 会访问已被 blk_mq_free_tag_set 释放的 hctx->tags，
   * 表现为 mwctl -d 打印 "detached" 后进程被 Killed（设备残留系统中）。
   * 6.0+ 的 del_gendisk/queue 生命周期已重构（且 vd_fops 注册了 .free_disk），
   * 无需（也没有）blk_cleanup_queue，保持与 6.x loop_remove 一致。
   */
  blk_cleanup_queue(vd->vd_queue);
#endif
  blk_mq_free_tag_set(&vd->tag_set);

  mutex_lock(&mwvd_ctl_mutex);
  idr_remove(&mwvd_index_idr, vd->vd_number);
  mutex_unlock(&mwvd_ctl_mutex);

  put_disk(vd->vd_disk);
}

static int mwvd_control_remove(int idx)
{
  struct mwvd_device *vd;
  int ret;

  if (idx < 0)
  {
    pr_warn_once("deleting an unspecified mwvd device is not supported.\n");
    return -EINVAL;
  }

  /* Hide this mwvd device for serialization. */
  ret = mutex_lock_killable(&mwvd_ctl_mutex);
  if (ret)
    return ret;
  vd = idr_find(&mwvd_index_idr, idx);
  if (!vd || !vd->idr_visible)
    ret = -ENODEV;
  else
    vd->idr_visible = false;
  mutex_unlock(&mwvd_ctl_mutex);
  if (ret)
    return ret;

  /* Check whether this mwvd device can be removed. */
  ret = mutex_lock_killable(&vd->vd_mutex);
  if (ret)
    goto mark_visible;
  if (vd->vd_state != MWVD_UNBOUND || vd_disk_openers(vd->vd_disk, vd->vd_device) > 0)
  {
    mutex_unlock(&vd->vd_mutex);
    ret = -EBUSY;
    goto mark_visible;
  }
  /* Mark this mwvd device as no more bound, but not quite unbound yet */
  vd->vd_state = MWVD_DELETING;
  mutex_unlock(&vd->vd_mutex);

  mwvd_remove(vd);
  return 0;

mark_visible:
  /* Show this mwvd device again. */
  mutex_lock(&mwvd_ctl_mutex);
  vd->idr_visible = true;
  mutex_unlock(&mwvd_ctl_mutex);
  return ret;
}

static int mwvd_control_get_free(int idx)
{
  struct mwvd_device *vd;
  int id, ret;

  ret = mutex_lock_killable(&mwvd_ctl_mutex);
  if (ret)
    return ret;
  idr_for_each_entry(&mwvd_index_idr, vd, id)
  {
    /* Hitting a race results in creating a new mwvd device which is harmless. */
    if (vd->idr_visible && data_race(vd->vd_state) == MWVD_UNBOUND)
      goto found;
  }
  mutex_unlock(&mwvd_ctl_mutex);
  return mwvd_control_add(-1);
found:
  mutex_unlock(&mwvd_ctl_mutex);
  return id;
}

/*
 * 固件声明的休眠（S4）存储分区（v2.5 扩展记录 Type=0x0001；上游为 mw.config
 * "hibernate <partuuid>"）。模块加载时从启动条目解析一次，供用户态查询。
 * 固件只声明意图；是否真为 swap、容量是否足够等语义判断由部署工具负责。
 */
static struct mwcfg_hibernate g_hibernate_part;
static bool g_hibernate_part_valid;

/* 前置声明：定义于本文件后部（mwvd_mount_partition 之前） */
static int mw_resolve_device_by_uuid(const char *uuid, char *dev_path, int len);

/**
 * mwvd_control_get_hibernate - 查询固件声明的休眠（S4）存储分区
 * @parm: 用户态 struct mwvd_hibernate 指针
 *
 * 数据来源：固件 v2 配置块的启动条目扩展记录（规范 COMMUNICATION_V2_SPEC §6.7）。
 */
static long mwvd_control_get_hibernate(unsigned long parm)
{
  struct mwvd_hibernate hib;
  char dev_path[MWVD_HIBERNATE_DEVPATH_SIZE];

  if (parm == 0)
    return -EINVAL;

  memset(&hib, 0, sizeof(hib));

  if (g_hibernate_part_valid)
  {
    hib.valid = 1;
    hib.usage = g_hibernate_part.usage;
    memcpy(hib.part_uuid, g_hibernate_part.part_uuid, sizeof(hib.part_uuid));

    if (mw_resolve_device_by_uuid(hib.part_uuid, dev_path, sizeof(dev_path)) == 0)
    {
      hib.resolved = 1;
      strscpy(hib.device_path, dev_path, sizeof(hib.device_path));
    }
  }

  if (copy_to_user((void __user *)parm, &hib, sizeof(hib)))
    return -EFAULT;

  return 0;
}

/**
 * hibernate_show - 只读 sysfs 查询接口（供 udev 脚本使用）
 *
 * 输出 key=value 文本，避免部署脚本依赖 ioctl 或额外用户态二进制。
 * 输出格式见 design/HIBERNATE-UDEV-AUTOCONFIG-DESIGN.md §5.1。
 */
static ssize_t hibernate_show(struct device *dev,
                              struct device_attribute *attr, char *buf)
{
  char dev_path[MWVD_HIBERNATE_DEVPATH_SIZE];
  bool resolved = false;

  if (!g_hibernate_part_valid)
    return sysfs_emit(buf, "valid=0\n");

  if (mw_resolve_device_by_uuid(g_hibernate_part.part_uuid, dev_path,
                                sizeof(dev_path)) == 0)
    resolved = true;

  if (resolved)
    return sysfs_emit(buf,
                      "valid=1\nusage=%u\nresolved=1\npart_uuid=%s\ndevice_path=%s\n",
                      (unsigned int)g_hibernate_part.usage,
                      g_hibernate_part.part_uuid, dev_path);

  return sysfs_emit(buf, "valid=1\nusage=%u\nresolved=0\npart_uuid=%s\n",
                    (unsigned int)g_hibernate_part.usage,
                    g_hibernate_part.part_uuid);
}

static DEVICE_ATTR_RO(hibernate);

static long mwvd_control_ioctl(struct file *file, unsigned int cmd,
                               unsigned long parm)
{
  switch (cmd)
  {
  case MWVD_CTL_ADD:
    return mwvd_control_add(parm);
  case MWVD_CTL_REMOVE:
    return mwvd_control_remove(parm);
  case MWVD_CTL_GET_FREE:
    return mwvd_control_get_free(parm);
  case MWVD_CTL_GET_HIBERNATE:
    return mwvd_control_get_hibernate(parm);
  default:
    return -ENOSYS;
  }
}

static const struct file_operations mwvd_ctl_fops = {
    .open = nonseekable_open,
    .unlocked_ioctl = mwvd_control_ioctl,
    .compat_ioctl = mwvd_control_ioctl,
    .owner = THIS_MODULE,
    .llseek = noop_llseek,
};

static struct miscdevice mwvd_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "mwvd-control",
    .fops = &mwvd_ctl_fops,
};

MODULE_ALIAS_MISCDEV(MISC_DYNAMIC_MINOR);
MODULE_ALIAS("devname:mwvd-control");

/**
 * mw_mkdir_p - 递归创建目录（类似用户空间 mkdir -p）
 * @path: 以 '/' 开头的绝对路径字符串（如 "/run/initramfs/vds"）
 *
 * 返回 0 表示成功，否则返回负的错误码。
 * 已存在的目录不会导致错误（忽略 -EEXIST）。
 * 使用 inode_lock 保护查找和创建操作，避免触发 lookup_one_len 的警告。
 */
int mw_mkdir_p(const char *path, umode_t mode)
{
  struct path root_path, cur_path;
  char *tmp, *start, *next;
  struct dentry *dentry;
  int ret = 0;
  bool root_consumed = false; /* root_path 的引用是否已由 cur_path 释放 */

  if (!path || path[0] != '/')
    return -EINVAL;

  ret = kern_path("/", LOOKUP_DIRECTORY, &root_path);
  if (ret)
    return ret;
  cur_path = root_path;

  tmp = kstrdup(path, GFP_KERNEL);
  if (!tmp)
  {
    path_put(&root_path);
    return -ENOMEM;
  }

  start = tmp;
  while (*start == '/')
    start++;

  next = start;
  while (next && *next)
  {
    char *slash = strchr(next, '/');
    if (slash)
      *slash = '\0';

    /* 对当前父目录加锁，lookup_one_len 和 vfs_mkdir 要求锁 */
    inode_lock(d_inode(cur_path.dentry));

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
    // 6.16+：使用新的 lookup_noperm 或 lookup_one
    dentry = lookup_noperm(&QSTR(next), cur_path.dentry);
#else
    // 6.6-6.15：继续使用 lookup_one_len
    dentry = lookup_one_len(next, cur_path.dentry, strlen(next));
#endif

    // dentry = lookup_one_len(next, cur_path.dentry, strlen(next));
    if (IS_ERR(dentry))
    {
      inode_unlock(d_inode(cur_path.dentry));
      ret = PTR_ERR(dentry);
      break;
    }

    if (dentry->d_inode)
    {
      /* 目录或文件已存在 */
      if (S_ISDIR(dentry->d_inode->i_mode))
      {
        /* 已是目录，进入下一级 */
        inode_unlock(d_inode(cur_path.dentry));
        path_put(&cur_path);
        root_consumed = true;
        cur_path.dentry = dentry;
        cur_path.mnt = mntget(root_path.mnt);
      }
      else
      {
        inode_unlock(d_inode(cur_path.dentry));
        dput(dentry);
        ret = -ENOTDIR;
        break;
      }
    }
    else
    {
      /* 创建目录 — vfs_mkdir 签名跨版本变化 */
      /* < 5.12:  int vfs_mkdir(struct inode *, struct dentry *, umode_t)          */
      /* 5.12-6.1: int vfs_mkdir(struct user_namespace *, struct inode *, dentry *, umode_t) */
      /* 6.2-6.18: int vfs_mkdir(struct mnt_idmap *, struct inode *, dentry *, umode_t)      */
      /* 6.15+:     struct dentry *vfs_mkdir(mnt_idmap *, inode *, dentry *, umode_t)        */
      /*            — 注意：返回的 dentry 即传入的 dentry，引用仍归调用者，不应 dput！       */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
      {
        struct dentry *new_dentry = vfs_mkdir(mnt_idmap(cur_path.mnt),
                                              d_inode(cur_path.dentry),
                                              dentry, mode, NULL);
        if (IS_ERR(new_dentry)) {
          ret = PTR_ERR(new_dentry);
          /* vfs_mkdir 失败时 dentry 未被消耗，调用者负责 dput */
        } else {
          ret = 0;
          /* 成功：new_dentry == dentry，引用保留给下方 cur_path 使用 */
        }
      }
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
      {
        struct dentry *new_dentry = vfs_mkdir(mnt_idmap(cur_path.mnt),
                                              d_inode(cur_path.dentry),
                                              dentry, mode);
        if (IS_ERR(new_dentry)) {
          ret = PTR_ERR(new_dentry);
          /* vfs_mkdir 失败时 dentry 未被消耗，调用者负责 dput */
        } else {
          ret = 0;
          /* 成功：new_dentry == dentry，引用保留给下方 cur_path 使用 */
        }
      }
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
      ret = vfs_mkdir(mnt_idmap(cur_path.mnt),
                      d_inode(cur_path.dentry), dentry, mode);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
      ret = vfs_mkdir(&init_user_ns,
                      d_inode(cur_path.dentry), dentry, mode);
#else
      ret = vfs_mkdir(d_inode(cur_path.dentry), dentry, mode);
#endif

      if (ret && ret != -EEXIST)
      {
        inode_unlock(d_inode(cur_path.dentry));
        dput(dentry);
        break;
      }

      /* 创建成功或已存在，进入新目录 */
      inode_unlock(d_inode(cur_path.dentry));
      path_put(&cur_path);
      root_consumed = true;
      cur_path.dentry = dentry;
      cur_path.mnt = mntget(root_path.mnt);
      ret = 0; /* 清除 -EEXIST */
    }

    if (slash)
      next = slash + 1;
    else
      next = NULL;
  }

  /* 释放最后的 cur_path */
  path_put(&cur_path);
  kfree(tmp);
  if (!root_consumed)
    path_put(&root_path);
  return ret;
}

/* ==================================================================
 * 镜像文件监控 — 自动加载文件系统和镜像文件
 * (原 mon.c 内容合并)
 * ================================================================== */

#define VD_MOUNT_POINT "/vdbase"

/* 内核模块支持的虚拟设备数量上限（镜像文件超过此数则错误打印、不挂载超出部分） */
#define MWVD_MAX_VD 4

// 记录自动加载信息和回调上下文（每个虚拟设备一个）
struct vd_mon_ctx
{
  struct vdf_efi_result info;
  int index;                        // 设备编号（mwvd%d）
  struct file_system_type *fstype;
  struct vfsmount *vd_mnt;
  struct dentry *vd_root;
  struct path root_path;
  atomic_t mount_done;
  struct delayed_work mount_work;
};

static struct vd_mon_ctx g_vdmon_ctx[MWVD_MAX_VD];

/*
 * 分区挂载共享表：同一分区（同 part_uuid）只挂载一次，多个镜像条目复用，
 * 避免重复挂载分区导致后续镜像无法访问；不同分区（不同 part_uuid）各自独立挂载。
 * refs 为该分区当前被引用的条目数，归零时真正卸载分区。
 */
struct vd_mon_mount
{
  char part_uuid[64];
  struct file_system_type *fstype;
  struct vfsmount *mnt;
  struct dentry *root;
  int refs;
};

static struct vd_mon_mount g_vdmon_mounts[MWVD_MAX_VD];

/* 保护 g_vdmon_mounts 共享挂载表的并发访问（检查/登记/释放） */
static DEFINE_MUTEX(mwvd_mon_mount_lock);

/* 串行化 monitor 按需加载（get_free + configure），避免并发条目取到同一设备 */
static DEFINE_MUTEX(mwvd_mon_load_lock);

static void __maybe_unused mw_convert_backslashes(char *path)
{
  if (!path)
    return;
  while (*path)
  {
    if (*path == '\\')
      *path = '/';
    path++;
  }
}

/**
 * mw_resolve_device_by_uuid - 根据分区UUID查找设备路径
 * @uuid: 分区UUID
 * @dev_path: 存储设备路径的缓冲区
 * @len: 缓冲区长度
 *
 * 返回 0 表示成功，否则返回负的错误码。
 */
static int mw_resolve_device_by_uuid(const char *uuid, char *dev_path, int len)
{
  char sysfs_path[64];
  struct path path;
  int ret;

  if (!uuid || !*uuid)
    return -EINVAL;

  ret = snprintf(sysfs_path, sizeof(sysfs_path), "/dev/disk/by-partuuid/%s", uuid);
  if (ret < 0 || ret >= sizeof(sysfs_path))
    return -ENAMETOOLONG;

  ret = kern_path(sysfs_path, LOOKUP_FOLLOW, &path);
  if (ret)
    return ret;

  path_put(&path);
  strscpy(dev_path, sysfs_path, len);
  return 0;
}

/**
 * mount镜像分区
 *
 */
static int mwvd_mount_partition(struct vd_mon_ctx *ctx, const char *dev_path)
{
  struct vd_mon_mount *slot = NULL;
  int i, ret = 0;

  if (!dev_path || !*dev_path || !*ctx->info.fs_type)
    return -EINVAL;

  /* 加锁保护共享挂载表：确保同分区（同 part_uuid）只挂载一次 */
  mutex_lock(&mwvd_mon_mount_lock);

  /* 同分区（同 part_uuid）已挂载：复用挂载上下文，不重复挂载分区 */
  for (i = 0; i < MWVD_MAX_VD; i++) {
    if (g_vdmon_mounts[i].mnt &&
        strcmp(g_vdmon_mounts[i].part_uuid, ctx->info.part_uuid) == 0) {
      slot = &g_vdmon_mounts[i];
      goto reuse;
    }
  }

  /* 申请一个空闲共享挂载槽 */
  for (i = 0; i < MWVD_MAX_VD; i++) {
    if (!g_vdmon_mounts[i].mnt) {
      slot = &g_vdmon_mounts[i];
      break;
    }
  }
  if (!slot) {
    pr_err("vd_mon[%d]: partition mount slot exhausted\n", ctx->index);
    ret = -ENOMEM;
    goto out;
  }

  ctx->fstype = get_fs_type(ctx->info.fs_type);
  if (!ctx->fstype)
  {
    pr_err("vd_mon[%d]: filesystem type \"%s\" not supported, cannot load image\n",
           ctx->index, ctx->info.fs_type);
    ret = -ENODEV;
    goto out;
  }

  ctx->vd_mnt = vfs_kern_mount(ctx->fstype, 0, dev_path, NULL);
  if (IS_ERR(ctx->vd_mnt))
  {
    long mnt_err = PTR_ERR(ctx->vd_mnt);
    /* vfs_kern_mount 失败：释放 get_fs_type 获得的文件系统模块引用，
     * 避免 fstype 引用计数泄漏（与 umount_partition 的 module_put 配对）。 */
    module_put(ctx->fstype->owner);
    ctx->fstype = NULL;
    pr_err("vd_mon[%d]: mount partition %s as %s failed: %ld\n",
           ctx->index, dev_path, ctx->info.fs_type, mnt_err);
    ret = (int)mnt_err;
    goto out;
  }

  ctx->vd_root = ctx->vd_mnt->mnt_root;
  dget(ctx->vd_root); // 增加引用，后续使用完需要 dput

  ctx->root_path.mnt = mntget(ctx->vd_mnt);
  ctx->root_path.dentry = dget(ctx->vd_root);

  /* 登记共享挂载（独立引用：module/mnt/dentry 各一份） */
  strscpy(slot->part_uuid, ctx->info.part_uuid, sizeof(slot->part_uuid));
  slot->fstype = get_fs_type(ctx->info.fs_type);
  if (!slot->fstype) {
    /* 防御：理论不可达（ctx->fstype 刚成功）；回退清理 */
    module_put(ctx->fstype->owner);
    ctx->fstype = NULL;
    dput(ctx->vd_root);
    ctx->vd_root = NULL;
    kern_unmount(ctx->vd_mnt);
    ctx->vd_mnt = NULL;
    path_put(&ctx->root_path);
    memset(&ctx->root_path, 0, sizeof(ctx->root_path));
    ret = -ENODEV;
    goto out;
  }
  slot->mnt  = mntget(ctx->vd_mnt);
  slot->root = dget(ctx->vd_root);
  slot->refs = 1;

  pr_info("vd_mon[%d]: mounted partition %s type=%s\n",
          ctx->index, dev_path, ctx->info.fs_type);
  goto out;

reuse:
  /* 同一分区不重复挂载：复用已挂载分区的挂载上下文（各自独立引用） */
  ctx->fstype = get_fs_type(ctx->info.fs_type);
  if (!ctx->fstype) {
    pr_err("vd_mon[%d]: filesystem type \"%s\" unavailable on reuse\n",
           ctx->index, ctx->info.fs_type);
    ret = -ENODEV;
    goto out;
  }
  ctx->vd_mnt            = mntget(slot->mnt);
  ctx->vd_root           = dget(slot->root);
  ctx->root_path.mnt     = mntget(slot->mnt);
  ctx->root_path.dentry  = dget(slot->root);
  slot->refs++;

  pr_info("vd_mon[%d]: reused mount for partition %s (refs=%d)\n",
          ctx->index, slot->part_uuid, slot->refs);

out:
  mutex_unlock(&mwvd_mon_mount_lock);
  return ret;
}

/**
 * umount镜像分区
 *
 */
static int mwvd_umount_partition(struct vd_mon_ctx *ctx)
{
  struct vd_mon_mount *slot = NULL;
  int i;

  if (!ctx->vd_mnt)
    return -EINVAL;

  mutex_lock(&mwvd_mon_mount_lock);

  /* 定位所属共享挂载槽（同 part_uuid） */
  for (i = 0; i < MWVD_MAX_VD; i++) {
    if (g_vdmon_mounts[i].mnt &&
        strcmp(g_vdmon_mounts[i].part_uuid, ctx->info.part_uuid) == 0) {
      slot = &g_vdmon_mounts[i];
      break;
    }
  }

  /* 释放本条目持有的引用 */
  dput(ctx->vd_root);
  ctx->vd_root = NULL;
  kern_unmount(ctx->vd_mnt);
  ctx->vd_mnt = NULL;
  path_put(&ctx->root_path);
  memset(&ctx->root_path, 0, sizeof(ctx->root_path));
  if (ctx->fstype) {
    module_put(ctx->fstype->owner);
    ctx->fstype = NULL;
  }

  /* 共享槽引用计数减一；归零时真正卸载分区 */
  if (slot) {
    slot->refs--;
    if (slot->refs <= 0) {
      kern_unmount(slot->mnt);
      slot->mnt = NULL;
      dput(slot->root);
      slot->root = NULL;
      module_put(slot->fstype->owner);
      slot->fstype = NULL;
      slot->part_uuid[0] = '\0';
      pr_info("vd_mon[%d]: unmounted partition (last user)\n", ctx->index);
    } else {
      pr_info("vd_mon[%d]: released partition mount (refs=%d)\n",
              ctx->index, slot->refs);
    }
  }

  mutex_unlock(&mwvd_mon_mount_lock);
  return 0;
}

// 设备出现时的回调处理（index: mwvd%d）
static int mwvd_load_on_device_ready(struct vd_mon_ctx *ctx, const char *device_path)
{
  int index = (int)(ctx - g_vdmon_ctx);  /* 条目标识（数组下标），仅用于日志 */
  int dev_idx, ret;

  ret = mwvd_mount_partition(ctx, device_path);
  if (ret)
  {
    pr_err("vd_mon[%d]: mount_partition failed: %d\n", index, ret);
    return ret;
  }

  /*
   * 按需生成设备：获取空闲 mwvd 设备，无空闲则自动创建（mwvdN）。
   * 设备号动态分配，解绑时由 mwvd_unload_auto() 注销设备。
   * mwvd_mon_load_lock 串行化「取设备 + 绑定」，避免并发条目取到同一设备。
   */
  mutex_lock(&mwvd_mon_load_lock);
  dev_idx = mwvd_control_get_free(-1);
  if (dev_idx < 0)
  {
    mutex_unlock(&mwvd_mon_load_lock);
    pr_err("vd_mon[%d]: no free mwvd device available (%d)\n", index, dev_idx);
    return dev_idx;
  }
  ctx->index = dev_idx;
  pr_info("vd_mon[%d]: configuring mwvd%d backing file %s\n", index, dev_idx, ctx->info.vdisk_path);

  ret = mwvd_configure_backing_path(dev_idx, &ctx->root_path, ctx->info.vdisk_path,
                                    ctx->info.part_uuid);
  mutex_unlock(&mwvd_mon_load_lock);
  if (ret)
  {
    pr_err("vd_mon[%d]: failed to configure mwvd%d from %s (%d)\n", index, dev_idx, ctx->info.vdisk_path, ret);
    mwvd_umount_partition(ctx);
    return ret;
  }

  atomic_set(&ctx->mount_done, 1);
  pr_info("vd_mon[%d]: loaded image %s as mwvd%d\n", index, ctx->info.vdisk_path, dev_idx);
  return 0;
}

/**
 * mwvd_unload_auto - 卸载监控加载的虚拟磁盘（index: mwvd%d）
 *
 */
static int mwvd_unload_auto(int index)
{
  struct vd_mon_ctx *ctx = &g_vdmon_ctx[index];
  int ret;

  if (!atomic_read(&ctx->mount_done))
    return 0;

  if (ctx->index < 0)
    return 0;

  ret = mwvd_clear_backing(ctx->index);
  if (ret)
    pr_warn("vd_mon[%d]: clear backing failed (%d)\n", index, ret);
  else
  {
    /* 注销镜像的同时注销设备（按需释放设备号）；若设备仍有打开者则保留 */
    ret = mwvd_control_remove(ctx->index);
    if (ret)
      pr_warn("vd_mon[%d]: remove device mwvd%d failed (%d, in use?)\n",
              index, ctx->index, ret);
    ctx->index = -1;
  }

  return ret;
}

/* 轮询工作：每 100ms 检查一次设备节点 */
static void mwvd_try_mount(struct work_struct *work)
{
  struct vd_mon_ctx *ctx;
  char devpath[128];
  struct path path;
  int index;

  ctx = container_of(work, struct vd_mon_ctx, mount_work.work);
  index = (int)(ctx - g_vdmon_ctx);   /* 条目标识（数组下标），仅用于日志 */

  if (atomic_read(&ctx->mount_done))
    return;

  snprintf(devpath, sizeof(devpath), "/dev/disk/by-partuuid/%s", ctx->info.part_uuid);
  if (kern_path(devpath, LOOKUP_FOLLOW, &path) != 0)
  {
    /* 设备未出现，100ms 后再次检查 */
    schedule_delayed_work(&ctx->mount_work, msecs_to_jiffies(100));
    return;
  }
  path_put(&path);

  if (mwvd_load_on_device_ready(ctx, devpath) != 0)
  {
    /* 文件系统无法加载（如 ntfs 挂载失败）：该分区上不存在镜像文件，
     * 无需继续轮询等待，报错退出该条目的挂载流程。 */
    atomic_set(&ctx->mount_done, 0);
    pr_err("vd_mon[%d]: filesystem/image load failed for %s, giving up "
           "(no further retry)\n", index, devpath);
    return;
  }
  pr_info("vd_mon[%d]: async mount complete\n", index);
}

/**
 * 校验分区 GUID 字符串：
 *   - GPT：36 字符 UUID（位置 8/13/18/23 为 '-'）；
 *   - MBR：8 位磁盘签名，或 8 位签名 + '-' + 2 位分区号（如 be1afdfa-01）。
 * 仅用于自动挂载前的配置有效性检查，避免无效 UUID 进入无限轮询。
 */
static bool mwvd_is_hex_n(const char *s, int n)
{
  int i;

  for (i = 0; i < n; i++)
  {
    if (!((s[i] >= '0' && s[i] <= '9') ||
          (s[i] >= 'a' && s[i] <= 'f') ||
          (s[i] >= 'A' && s[i] <= 'F')))
      return false;
  }
  return true;
}

static bool mwvd_part_uuid_valid(const char *uuid)
{
  size_t len;

  if (uuid == NULL)
    return false;

  len = strlen(uuid);

  /* GPT：8-4-4-4-12 */
  if (len == 36 &&
      uuid[8] == '-' && uuid[13] == '-' && uuid[18] == '-' && uuid[23] == '-' &&
      mwvd_is_hex_n(uuid, 8) && mwvd_is_hex_n(uuid + 9, 4) &&
      mwvd_is_hex_n(uuid + 14, 4) && mwvd_is_hex_n(uuid + 19, 4) &&
      mwvd_is_hex_n(uuid + 24, 12))
    return true;

  /* MBR：8 位磁盘签名，或 8 位签名 + 2 位分区号 */
  if (len == 8 && mwvd_is_hex_n(uuid, 8))
    return true;
  if (len == 11 && uuid[8] == '-' && mwvd_is_hex_n(uuid, 8) &&
      mwvd_is_hex_n(uuid + 9, 2))
    return true;

  return false;
}

/**
 * 发起镜像监控
 *
 * 读取固件配置数据块，解析其中的镜像条目，按需动态创建 mwvd 设备并挂载
 * （来一个镜像生成一个设备；解绑时注销设备）。条目数超过 MWVD_MAX_VD 时，
 * 错误打印并仅挂载前 MWVD_MAX_VD 个。
 */
int mwvd_monitor(void)
{
  /* 栈上 entries[4]（≈368B×4）会触发 -Wframe-larger-than=1024 警告；
   * mwvd_monitor 仅在模块加载时调用一次（mwvd_init），用静态数组消除。 */
  static struct vdf_efi_result entries[MWVD_MAX_VD];
  bool blob_mapped = false;
  void *blob;
  int n, total = 0, i, ret;
  int hib_ret;

  memset(g_vdmon_ctx, 0, sizeof(g_vdmon_ctx));
  memset(g_vdmon_mounts, 0, sizeof(g_vdmon_mounts));

  /*
   * v2.8 §6.8 消费端门控：未发现固件发布的 ACPI 设备 MWVD0001 时，
   * 不自动创建/绑定任何虚拟磁盘；驱动本体保持加载，控制节点仍可用于
   * 诊断。此检查必须放在任何 schedule_delayed_work() 之前，避免无设备
   * 时后台空转或反复重试导致系统卡顿。
   */
  if (!mwvd_acpi_device_present())
  {
    pr_warn("vd_mon: ACPI device %s not found; skip vdisk auto-mount "
            "(driver stays loaded)\n", MWVD_ACPI_DEVICE_HID);
    return 0;
  }

  /* 读取固件配置数据块 */
  blob = vd_get_config_blob(&blob_mapped);
  if (!blob)
  {
    pr_warn("vd_mon: no v2 config block found (firmware did not publish config)\n");
    return 0;
  }

  n = vdf_parse_efi_blob_v2(blob, entries, MWVD_MAX_VD, &total);

  /*
   * 扩展记录：固件声明的休眠存储分区（v2.5 §6.7）——须在解除映射前读取。
   * 记录缺失（未配置）/ 损坏（安全放弃）都不影响镜像条目解析结果。
   */
  hib_ret = mwcfg_ext_get_hibernate(blob, &g_hibernate_part);
  g_hibernate_part_valid = (hib_ret == MWCFG_EXT_FOUND);

  if (blob_mapped)
    memunmap(blob);

  if (g_hibernate_part_valid)
    pr_info("vd_mon: firmware hibernate partition: %s (usage=%u)\n",
            g_hibernate_part.part_uuid, (unsigned int)g_hibernate_part.usage);
  else if (hib_ret == MWCFG_EXT_EBROKEN)
    pr_warn("vd_mon: hibernate partition record broken; ignored\n");
  else if (hib_ret < 0)
    pr_warn("vd_mon: hibernate partition record unreadable (%d); ignored\n", hib_ret);
  else
    pr_info("vd_mon: firmware declared no hibernate partition (mw.config hibernate unset)\n");

  if (n < 0)
  {
    pr_err("vd_mon: v2 config block found but invalid/corrupted, parse failed\n");
    return -EINVAL;
  }

  if (n == 0)
  {
    pr_warn("vd_mon: v2 config block found but no enabled image entries\n");
    return 0;
  }

  /* 镜像文件数超过内核模块支持的虚拟设备数：错误打印，不挂载超出部分 */
  if (total > MWVD_MAX_VD)
    pr_err("vd_mon: found %d image(s) but module supports at most %d virtual devices; "
           "mounting only the first %d, ignoring %d image(s)\n",
           total, MWVD_MAX_VD, MWVD_MAX_VD, total - MWVD_MAX_VD);

  ret = mw_mkdir_p(VD_MOUNT_POINT, 0755);
  if (ret)
  {
    pr_err("vd_mon: mkdir_p failed: %d\n", ret);
    return ret;
  }

  pr_info("vd_mon: created mount point %s\n", VD_MOUNT_POINT);

  for (i = 0; i < n; i++)
  {
    struct vd_mon_ctx *ctx = &g_vdmon_ctx[i];
    char device_path[64];

    /*
     * 无效条目（空/占位 part_uuid、空 fs/vdisk）不能进入延迟轮询：
     * 否则设备永远不会出现，delayed work 会每 100ms 无限重试。
     */
    if (!mwvd_part_uuid_valid(entries[i].part_uuid) ||
        entries[i].fs_type[0] == '\0' || entries[i].vdisk_path[0] == '\0')
    {
      pr_warn("vd_mon[%d]: invalid entry (part_uuid='%s' fs='%s' vdisk='%s'); "
              "skip without retry\n", i, entries[i].part_uuid,
              entries[i].fs_type, entries[i].vdisk_path);
      continue;
    }

    ctx->index = -1;  /* 尚未分配设备号；加载时由 mwvd_load_on_device_ready 动态分配 */
    ctx->info = entries[i];
    INIT_DELAYED_WORK(&ctx->mount_work, mwvd_try_mount);

    pr_info("vd_mon[%d]: partguid=%s fs=%s vdisk=%s\n",
            i, ctx->info.part_uuid, ctx->info.fs_type, ctx->info.vdisk_path);

    /* 设备已就绪，直接处理（不再调度延迟工作，避免竞态） */
    if (mw_resolve_device_by_uuid(ctx->info.part_uuid, device_path, sizeof(device_path)) == 0)
    {
      atomic_set(&ctx->mount_done, 1);
      pr_info("vd_mon[%d]: device %s already exists, triggering autoload\n", i, device_path);
      mwvd_load_on_device_ready(ctx, device_path);
      continue;
    }

    /* 设备未就绪，注册延迟轮询 */
    atomic_set(&ctx->mount_done, 0);
    schedule_delayed_work(&ctx->mount_work, 0);
    pr_info("vd_mon[%d]: waiting for device UUID=%s to appear (non-blocking)\n",
            i, ctx->info.part_uuid);
  }

  return 0;
}

/**
 * 取消监控
 *
 * 检查是否已挂载，若已挂载则卸载分区；取消任何未完成的挂载尝试；清理上下文状态。
 * 如果还存在没有结束的定时任务，那么取消定时任务。
 *
 */
int mwvd_umonitor(void)
{
  int i;

  for (i = 0; i < MWVD_MAX_VD; i++)
  {
    struct vd_mon_ctx *ctx = &g_vdmon_ctx[i];

    if (delayed_work_pending(&ctx->mount_work))
    {
      pr_info("vd_mon[%d]: canceling pending mount work\n", i);
      cancel_delayed_work_sync(&ctx->mount_work);
    }

    if (ctx->vd_mnt)
    {
      /* 已挂载分区：统一释放挂载与 root_path（含共享槽引用） */
      mwvd_umount_partition(ctx);
    }

    if (atomic_read(&ctx->mount_done))
    {
      /* 清除 mwvd 设备的 backing file */
      mwvd_unload_auto(i);
      atomic_set(&ctx->mount_done, 0);
    }
  }
  pr_info("vd_mon: monitoring stopped and resources cleaned up\n");

  return 0;
}

/*
 * 休眠标记（EFI 变量 / NVRAM）
 * 变量名：MwHibernateState；GUID：e7c9f0a5-4a61-4c85-92e6-5bf813423b90
 * 载荷：分区 part_uuid + 镜像文件全路径（与 v2 契约识别镜像方式一致，固件端据此定位
 *   休眠镜像并验证可用）；固定布局，禁止 sizeof 跨端计算偏移（对齐 COMMUNICATION_V2_SPEC §1.4）。
 * 内核模块先行定义（OS 端权威），固件侧 BootUI 实现时使用同一 GUID 与布局。
 * 休眠发生前由 MWVD 写入；恢复成功后由 MWVD 清除（S4 专用，S3 不写）。
 */
#define MW_HIBERNATE_VAR_NAME L"MwHibernateState"
#define MW_HIBERNATE_VAR_ATTR \
  (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | \
   EFI_VARIABLE_RUNTIME_ACCESS)

#define MW_HIBERNATE_SIGNATURE "MWHIBRN0"
#define MW_HIBERNATE_MARK_VERSION 1
#define MW_HIBERNATE_STATE_IDLE 0
#define MW_HIBERNATE_STATE_SLEEPING 1

/* 固定载荷尺寸（HIBERNATE_SPEC.md §5.3）：禁止 sizeof 跨端计算偏移 */
#define MW_HIBERNATE_MARK_SIZE  (8 + 4 + 37 + 260 + 1 + 3 + 8 + 4)  /* = 325 */

static efi_guid_t mw_hibernate_guid = EFI_GUID(0xe7c9f0a5, 0x4a61, 0x4c85,
    0x92, 0xe6, 0x5b, 0xf8, 0x13, 0x42, 0x3b, 0x90);

/* 休眠标记载荷（固定布局，各端从本定义复制，禁止修改） */
/* part_uuid[37]/vdisk_path[260] 与固件-OS 通讯契约镜像条目载荷中的分区 GUID 与
 * 镜像路径字段长度/格式对齐，固件侧 BootUI 照此实现。 */
/*
 * 布局必须为 1 字节对齐（__packed）：C 默认对齐下 u64 timestamp 会 8 字节对齐，
 * 在 timestamp 前插入 7 字节 padding（offset 313→320），使 sizeof=336、
 * offsetof(crc32)=328，与固件端 325/321 漂移 → 固件 GetVariable(325) 报
 * EFI_BUFFER_TOO_SMALL，休眠唤醒永远不触发（见 HIBERNATE_SPEC.md §5.3/§5.4）。
 */
struct __packed mw_hibernate_mark {
  u8   signature[8];        /* "MWHIBRN0" */
  u32  version;             /* 1 */
  char part_uuid[37];       /* 分区 GUID（36 字符小写 ASCII + NULL；MBR "xxxx-xx"） */
  char vdisk_path[260];     /* 镜像文件完整路径 */
  u8   state;               /* 0=IDLE, 1=SLEEPING */
  u8   reserved[3];
  u64  timestamp;           /* 休眠时刻（秒） */
  u32  crc32;               /* 载荷校验 */
};
/* 尺寸 = 8 + 4 + 37 + 260 + 1 + 3 + 8 + 4 = 325 */
/* 编译期断言：布局漂移（非 1 字节对齐 / 字段增减）必须在此处暴露（规范 §5.3 要求） */
_Static_assert(sizeof(struct mw_hibernate_mark) == MW_HIBERNATE_MARK_SIZE,
               "mw_hibernate_mark layout drift");
_Static_assert(offsetof(struct mw_hibernate_mark, crc32) == 321,
               "mw_hibernate_mark crc32 offset drift");

/*
 * EFI 状态码 → errno 转换（模块自实现）。
 *
 * 内核的 efi_status_to_err() 在 5.10/5.15 等旧内核中仅定义并声明
 * （drivers/firmware/efi/efi.c + include/linux/efi.h），但并未
 * EXPORT_SYMBOL_GPL——直到 6.0 才导出。模块直接调用该符号时，
 * 编译期因有 extern 声明可通过（check-one 只编译 .o），但链接/加载期
 * 报 Unknown symbol (err -2)，模块无法加载。故此处自实现等价映射
 * （映射表与内核 efi.c 完全一致），跨版本行为统一且不依赖内核导出符号。
 */
static int mwvd_efi_status_to_err(efi_status_t status)
{
  switch (status) {
  case EFI_SUCCESS:
    return 0;
  case EFI_INVALID_PARAMETER:
    return -EINVAL;
  case EFI_OUT_OF_RESOURCES:
    return -ENOSPC;
  case EFI_DEVICE_ERROR:
    return -EIO;
  case EFI_WRITE_PROTECTED:
    return -EROFS;
  case EFI_SECURITY_VIOLATION:
    return -EACCES;
  case EFI_NOT_FOUND:
    return -ENOENT;
  case EFI_ABORTED:
    return -EINTR;
  default:
    return -EINVAL;
  }
}

/*
 * 固件条目路径 → BootUI 休眠标记路径。
 * BootUI 菜单条目使用反斜杠绝对路径（如 \Ubuntu-20.04\System.vhdx）；
 * Linux 侧 v2 条目可能保存为 /Ubuntu-20.04/System.vhdx，故统一折叠分隔符。
 */
static void mwvd_fw_path_to_mark(const char *src, char *dst, size_t dst_size)
{
  size_t i;
  size_t j = 0;

  if (dst_size == 0)
    return;
  dst[0] = '\0';
  if (src == NULL || src[0] == '\0')
    return;

  dst[j++] = '\\';
  for (i = 0; src[i] != '\0' && (j + 1) < dst_size; i++)
  {
    char c = src[i];
    if (c == '/' || c == '\\')
    {
      if (j > 0 && dst[j - 1] == '\\')
        continue;
      c = '\\';
    }
    dst[j++] = c;
  }
  dst[j] = '\0';
}

/*
 * 写休眠标记：S4 休眠前（PM_HIBERNATION_PREPARE）调用。
 * 遍历已绑定的虚拟盘，取第一个镜像作为休眠系统标识（rootfs 系统盘）。
 * UEFI 下 RT 服务几乎必然可用；失败时仅告警、继续休眠（降级为失去默认唤醒体验，
 * 用户手动选原系统仍可恢复——休眠本身不依赖标记）。
 */
static void mwvd_hibernate_mark_write(void)
{
  struct mwvd_device *vd;
  struct mw_hibernate_mark mark;
  efi_status_t st;
  int id;
  u32 crc;

  /*
   * 取第一个已绑定且有镜像路径的 vd 作为休眠系统标识。
   * 约定：云固件 v2 blob 条目顺序保证主盘/系统盘（rootfs 所在）在前，
   * 故 idr 中第一个 vd 即系统盘。idr_for_each_entry 宏每次迭代自动取下一个条目，
   * 循环内无需手动置 NULL。
   */
  /*
   * 锁内完成 idr 遍历 + 字段拷贝（防设备被并发解绑释放后悬垂引用）；
   * 锁外再调 efi.set_variable（避免持锁调用可能阻塞的 Runtime 服务）。
   */
  mutex_lock(&mwvd_ctl_mutex);
  vd = NULL;
  idr_for_each_entry(&mwvd_index_idr, vd, id) {
    if (vd && vd->vd_backing_file && vd->vd_part_uuid[0])
      break;
  }
  if (!vd) {
    mutex_unlock(&mwvd_ctl_mutex);
    pr_warn("mwvd: no bound vdisk for hibernate mark\n");
    return;
  }
  /*
   * 没有固件条目路径 ⇒ 写出的 vdisk_path 一定匹配不上 BootUI 菜单条目，
   * 唤醒时固件只会报 image unavailable 并清掉睡眠状态。此时宁可不写标记：
   * 固件按正常流程启动，内核仍可凭 initramfs 的 resume= 恢复。
   */
  if (!vd->vd_fw_path[0]) {
    mutex_unlock(&mwvd_ctl_mutex);
    pr_warn("mwvd: no firmware image path, skip hibernate mark\n");
    return;
  }

  memset(&mark, 0, sizeof(mark));
  memcpy(mark.signature, MW_HIBERNATE_SIGNATURE, 8);
  mark.version = MW_HIBERNATE_MARK_VERSION;
  /* vd_part_uuid 为 [64]（vd_abi.h），载荷为 [37]：截取前 36 字符 + NULL */
  memcpy(mark.part_uuid, vd->vd_part_uuid, sizeof(mark.part_uuid) - 1);
  mwvd_fw_path_to_mark(vd->vd_fw_path, mark.vdisk_path, sizeof(mark.vdisk_path));
  mark.state = MW_HIBERNATE_STATE_SLEEPING;
  mark.timestamp = ktime_get_real_seconds();
  mutex_unlock(&mwvd_ctl_mutex);

  /*
   * CRC-32 覆盖除 crc32 字段外全部载荷。
   * 必须与固件端 gBS->CalculateCrc32 一致：标准 IEEE CRC-32（多项式 0xEDB88320，
   * 初始 0xFFFFFFFF，输出异或 0xFFFFFFFF，= zlib crc32）。
   * 内核 crc32_le() 是裸算法（不处理初始/最终异或），需：
   *     crc32_le(0xFFFFFFFF, ...) ^ 0xFFFFFFFF
   * ⚠️ 禁止用 crc32_be()（0x04C11DB7 大端、初始 0、无最终异或）：与固件端算法
   * 不匹配，固件 CRC 校验恒失败 → 休眠唤醒不触发（HIBERNATE_SPEC.md §5.4）。
   */
  crc = crc32_le(0xFFFFFFFF, (const unsigned char *)&mark,
                 offsetof(struct mw_hibernate_mark, crc32)) ^ 0xFFFFFFFF;
  mark.crc32 = crc;

  if (!efi.set_variable) {
    pr_warn("mwvd: EFI set_variable not available, hibernate mark skipped\n");
    return;
  }
  st = efi.set_variable((efi_char16_t *)MW_HIBERNATE_VAR_NAME,
                        &mw_hibernate_guid, MW_HIBERNATE_VAR_ATTR,
                        sizeof(mark), &mark);
  if (st != EFI_SUCCESS)
    pr_warn("mwvd: write hibernate mark failed: %d (resume falls back to manual boot)\n",
            mwvd_efi_status_to_err(st));
  else
    pr_info("mwvd: hibernate mark written (part=%s path=%s)\n",
            mark.part_uuid, mark.vdisk_path);
}

/* 清除休眠标记：休眠失败回滚（PM_POST_HIBERNATION）或恢复失败回退（PM_POST_RESTORE）时
 * 调用（未进入成功恢复状态）；成功 resume 跳回快照不触发 POST_*，标记由固件端 BootUI
 * 消费清除。 */
static void mwvd_hibernate_mark_clear(void)
{
  efi_status_t st;

  if (!efi.set_variable) {
    pr_warn("mwvd: EFI set_variable not available, hibernate mark not cleared\n");
    return;
  }
  st = efi.set_variable((efi_char16_t *)MW_HIBERNATE_VAR_NAME,
                        &mw_hibernate_guid, MW_HIBERNATE_VAR_ATTR,
                        0, NULL);
  if (st == EFI_NOT_FOUND) {
    /* 标记已不存在 = 已清除，视为成功（静默，不告警误导排查）。
     * 场景：S4 进入但主机未真正断电、同会话被唤醒时，固件对删除操作可能返回
     * EFI_NOT_FOUND（删除目标已达成）。物理机实测：write 成功后 PM_POST_HIBERNATION
     * 清标记返回 -2，但标记实际仍被 BootUI 读到并正确直接唤醒（桌面完整恢复），
     * 属固件删除操作的 EFI_NOT_FOUND，非功能性错误；其余错误仍告警。 */
    return;
  }
  if (st != EFI_SUCCESS)
    pr_warn("mwvd: clear hibernate mark failed: %d\n", mwvd_efi_status_to_err(st));
  else
    pr_info("mwvd: hibernate mark cleared\n");
}

/*
 * PM 处理：挂起/休眠（S3/S4）前后维护虚拟盘状态。
 *
 * PREPARE（S3/S4/恢复准备）：置系统级 pm_suspended 标记（仅状态记录/调试），
 *   并遍历全部虚拟盘（含差分父链）递归落盘（BAT/bitmap/VHDX log + fsync），防止休眠
 *   断电后镜像与内存不一致。仅做落盘屏障，不冻结请求队列——workqueue 已带
 *   WQ_FREEZABLE（挂起时由 freezer 冻结），此处再 blk_mq_freeze_queue 会与冻结的
 *   workqueue 互等死锁。
 * POST（唤醒/失败回退）：清系统级标记；将 VHDX log 重放排入各 vd workqueue 串行执行
 *   （收敛挂起期间镜像被外部改动的状态），失败置设备级 pm_replay_failed（per-device
 *   容错，不影响其余设备）；休眠失败回滚/恢复失败回退时清休眠标记。
 *
 * 说明：
 * - 内核无 PM_SUSPEND_FAIL/PM_HIBERNATION_FAIL 事件（仅 6 个 PREPARE/POST 事件）；
 * - 休眠/挂起失败回滚时，内核在失败路径仍会显式触发 PM_POST_*（hibernate.c
 *   Exit 标签 / suspend_finish）——POST 分支覆盖「失败回滚清标志」场景，标志不会
 *   因休眠失败而残留 true；
 * - 成功 resume 跳回快照（swsusp_arch_resume 不返回），不触发 POST_*——pm_suspended
 *   残留 true 但仅状态记录无碍；休眠标记由固件端 BootUI 消费清除；
 * - PREPARE 在 freeze_processes() 之后触发，此时 MWVD workqueue 已冻结（无并发
 *   IO），可安全落盘；POST 在 thaw_processes() 之后触发（系统已恢复）。
 */
static int mwvd_pm_event(struct notifier_block *nb, unsigned long mode,
                         void *unused)
{
  struct mwvd_device *vd;
  int id;

  switch (mode) {
  case PM_SUSPEND_PREPARE:
  case PM_HIBERNATION_PREPARE:
  case PM_RESTORE_PREPARE:
    WRITE_ONCE(mwvd_pm_suspended, true);
    /*
     * 遍历全部虚拟盘（含差分父链）递归落盘。
     * 用 mwvd_ctl_mutex 保护 idr 遍历（增删设备的 ioctl 路径持有该锁；freeze_processes()
     * 返回时持锁 ioctl 已执行完并释放锁，此处加锁不会死锁）。
     */
    mutex_lock(&mwvd_ctl_mutex);
    idr_for_each_entry(&mwvd_index_idr, vd, id) {
      if (vd && vd_flush_all(vd) < 0)
        /* per-device 容错：失败仅告警，继续处理其余设备 */
        pr_warn("mwvd%d: flush on suspend/hibernate failed\n",
                vd->vd_number);
    }
    mutex_unlock(&mwvd_ctl_mutex);
    /* S4 才写休眠标记（S3/RESTORE 不写），见休眠标记设计 */
    if (mode == PM_HIBERNATION_PREPARE)
      mwvd_hibernate_mark_write();
    break;

  case PM_POST_SUSPEND:
  case PM_POST_HIBERNATION:
  case PM_POST_RESTORE:
    /* 系统已整体唤醒/失败回退 → 清系统级状态标记（仅状态记录） */
    WRITE_ONCE(mwvd_pm_suspended, false);
    /*
     * replay 排入各 vd 的 workqueue 串行执行（与 I/O 同 worker 互斥）。
     * 不在 PM 回调线程直接调用 vd_replay_pending_log：POST 在 thaw 后触发，
     * workqueue 已解冻，PM 线程与 workqueue 并发会竞争 vdf_state。
     * per-device 容错：单设备 replay 失败不影响其余设备（mwvd_replay_workfn 内处理）。
     */
    mutex_lock(&mwvd_ctl_mutex);
    idr_for_each_entry(&mwvd_index_idr, vd, id) {
      if (vd && vd->workqueue)
        queue_work(vd->workqueue, &vd->replay_work);
    }
    mutex_unlock(&mwvd_ctl_mutex);
    /*
     * 清休眠标记：休眠失败回滚（PM_POST_HIBERNATION）或恢复失败回退（PM_POST_RESTORE）
     * 时，均说明没有进入「成功恢复」状态，应清除标记避免 BootUI 下次误判仍有休眠。
     * 注：成功 resume 跳回快照不触发 POST_*，标记由固件端 BootUI 消费清除。
     */
    if (mode == PM_POST_HIBERNATION || mode == PM_POST_RESTORE)
      mwvd_hibernate_mark_clear();
    break;

  default:
    break;
  }
  return NOTIFY_OK;
}

static struct notifier_block mwvd_pm_nb = {
  .notifier_call = mwvd_pm_event,
};

static int __init mwvd_init(void)
{
  int i;
  int err;

  /*
   * 固件 `MWVD` 代码载荷是唯一的格式后端来源（2.8.0 起纯载荷模式）：
   * 载荷缺失 / 校验失败 / 架构或 ABI 不匹配时直接拒载，不做静默降级。
   */
  err = mwvd_code_init();
  if (err)
    return err;

  part_shift = 0;
  if (max_part > 0)
  {
    part_shift = fls(max_part);

    /*
     * Adjust max_part according to part_shift as it is exported
     * to user space so that user can decide correct minor number
     * if [s]he want to create more devices.
     *
     * Note that -1 is required because partition 0 is reserved
     * for the whole disk.
     */
    max_part = (1UL << part_shift) - 1;
  }

  if ((1UL << part_shift) > DISK_MAX_PARTS)
  {
    err = -EINVAL;
    goto err_out;
  }

  if (max_loop > 1UL << (MINORBITS - part_shift))
  {
    err = -EINVAL;
    goto err_out;
  }

  err = misc_register(&mwvd_misc);
  if (err < 0)
    goto err_out;

  if (register_blkdev(MWVDISK_MAJOR, "mwvd") < 0)
  {
    err = -EIO;
    goto misc_out;
  }

  /* pre-create number of devices given by config or max_loop */
  if (mwvd_acpi_device_present())
  {
    for (i = 0; i < max_loop; i++)
      mwvd_control_add(i);
  }
  else
  {
    /* 无 ACPI 设备时连 max_loop 预创建的块设备也不创建，但驱动继续加载。 */
    pr_warn("mwvd: ACPI device %s not found; no vdisk will be created "
            "(driver stays loaded)\n", MWVD_ACPI_DEVICE_HID);
  }

  pr_info("mwvd: module " MWVD_MODULE_VERSION " loaded (firmware core %s, headers "
          VD_CORE_VERSION_STRING ")\n", vd_core_version());

  /* 自动加载固件配置并尝试挂载镜像所在分区 */
  if (!no_autoload) {
    err = mwvd_monitor();
    if (err)
      pr_warn("mwvd: monitor vdisk failed (%d)\n", err);
  }

  err = register_pm_notifier(&mwvd_pm_nb);
  if (err)
    pr_warn("mwvd: register PM notifier failed (%d)\n", err);

  /* 只读休眠分区查询属性：脚本在 udev 事件中读取，无需 ioctl/额外二进制。 */
  if (device_create_file(mwvd_misc.this_device, &dev_attr_hibernate))
    pr_warn("mwvd: failed to create hibernate sysfs attribute\n");

  return 0;

misc_out:
  misc_deregister(&mwvd_misc);
err_out:
  return err;
}

static void __exit mwvd_exit(void)
{
  struct mwvd_device *vd;
  int id;
  int err;

  err = mwvd_umonitor();
  if (err)
    pr_warn("mwvd: unmonitor vdisk failed (%d)\n", err);

  unregister_pm_notifier(&mwvd_pm_nb);

  device_remove_file(mwvd_misc.this_device, &dev_attr_hibernate);
  unregister_blkdev(MWVDISK_MAJOR, "mwvd");
  misc_deregister(&mwvd_misc);

  /*
   * There is no need to use mwvd_ctl_mutex here, for nobody else can
   * access mwvd_index_idr when this module is unloading (unless forced
   * module unloading is requested). If this is not a clean unloading,
   * we have no means to avoid kernel crash.
   */
  idr_for_each_entry(&mwvd_index_idr, vd, id)
      mwvd_remove(vd);

  idr_destroy(&mwvd_index_idr);

  /* 所有设备已移除，才允许释放运行时加载的闭源层机器码 */
  mwvd_code_exit();

  pr_info("mwvd: module unloaded\n");
}

module_init(mwvd_init);
module_exit(mwvd_exit);
