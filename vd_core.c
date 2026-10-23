// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * vd_core.c
 * Copyright (c) 2026 LU WEIFENG (wave.lu@gmail.com)
 * 版权所有 (c) 2026 陆伟峰
 */

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/falloc.h>
#include <linux/uio.h>
#include <linux/bio.h>
#include <linux/vmalloc.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/acpi.h>

#include "vd.h"
#include "vd_abi.h"

static void *vdf_kmalloc(int size) { return kvmalloc(size, GFP_KERNEL); }
static void vdf_kfree(void *p) { kvfree(p); }
static void vdf_guid_gen(vd_u8 guid[16]) { guid_gen((guid_t *)guid); }

static int mwvd_vdf_read(void *ctx, vd_i64 offset, vd_u8 *dst, int len)
{
  struct file *f = (struct file *)ctx;
  loff_t pos = (loff_t)offset;
  ssize_t r = kernel_read(f, dst, (size_t)len, &pos);
  return (int)r;
}

static void *vdf_phys_map(vd_u64 phys, vd_u64 size)
{
  return memremap((phys_addr_t)phys, (size_t)size, MEMREMAP_WB);
}

static void vdf_phys_unmap(void *ptr)
{
  memunmap(ptr);
}

/*
 * vd_get_config_blob — 获取固件配置数据块。
 * 协议表结构与数据块布局由闭源层解析（vd_abi.h 接口），不随 GPL 源码公开；
 * GPL 层仅负责获取固件表原始字节并转交闭源层。成功返回 blob 指针并置
 * *mapped = true（调用方用后 memunmap）；失败返回 NULL。
 * 表获取接口内部持互斥锁，须在进程上下文调用。
 */
void *vd_get_config_blob(bool *mapped)
{
  void *blob = NULL;

  if (!mapped)
    return NULL;
  *mapped = false;

  /* 优先经固件表定位；表解析在闭源层 */
#ifdef CONFIG_ACPI
  {
    struct acpi_table_header *tbl = NULL;
    acpi_status st;

    st = acpi_get_table("MWCF", 0, &tbl);
    if (ACPI_SUCCESS(st) && tbl) {
      if (vd_acquire_config_blob_acpi(tbl, (int)tbl->length,
                                      vdf_phys_map, vdf_phys_unmap,
                                      &blob) == 0) {
        acpi_put_table(tbl);
        *mapped = true;
        return blob;
      }
      acpi_put_table(tbl);
    }
  }
#endif

  /* 回退兼容通道（仅 IA32/X64 具备；非 x86 无该约定，直接返回 NULL，
   * 避免映射无效物理地址）。 */
#if defined(__x86_64__) || defined(__i386__)
  if (vd_acquire_config_blob(vdf_phys_map, vdf_phys_unmap, &blob) != 0)
    return NULL;

  *mapped = true;
  return blob;
#else
  return NULL;
#endif
}

static char *mw_dirname(const char *path)
{
  const char *slash = strrchr(path, '/');
  if (slash) {
    size_t len = slash - path;
    char *dir = kzalloc(len + 1, GFP_KERNEL);
    if (!dir) return NULL;
    memcpy(dir, path, len);
    dir[len] = '\0';
    return dir;
  }
  return NULL;
}

void *vd_open_image(struct file *file, loff_t file_size,
                           int vd_number, const char *tag,
                           struct vdf_open_result *result)
{
  int vdf_fmt;
  void *vdf_st = NULL;
  loff_t hdr_pos;
  int hdr_len;
  vd_u8 *hdr_buf;
  int buf_len;
  int vdf_ret;
  int vdf_loop_cnt;

  hdr_len = 4096;
  hdr_buf = kzalloc(hdr_len, GFP_KERNEL);
  if (!hdr_buf)
    return NULL;

  hdr_pos = 0;
  if (kernel_read(file, hdr_buf, hdr_len, &hdr_pos) <= 0) {
    pr_err("mwvd%d: %s header read failed\n", vd_number, tag);
    goto out_free_hdr;
  }

  if (file_size > hdr_len + 512) {
    vd_u8 *ext_buf = krealloc(hdr_buf, hdr_len + 512, GFP_KERNEL);
    if (ext_buf) {
      hdr_buf = ext_buf;
      hdr_pos = file_size - 512;
      if (kernel_read(file, hdr_buf + hdr_len, 512, &hdr_pos) > 0)
        hdr_len += 512;
    }
  }

  if (vdf_probe(hdr_buf, hdr_len, file_size, &vdf_fmt) != 0) {
    pr_err("mwvd%d: %s format probe failed (fmt=%d)\n", vd_number, tag, vdf_fmt);
    goto out_free_hdr;
  }

  {
    int st_size = vdf_state_size(vdf_fmt);
    if (st_size <= 0) {
      pr_err("mwvd%d: %s format state init failed (fmt=%d, size=%d)\n",
             vd_number, tag, vdf_fmt, st_size);
      goto out_free_hdr;
    }

    vdf_st = kzalloc(st_size, GFP_KERNEL);
    if (!vdf_st)
      goto out_free_hdr;
  }

  buf_len = hdr_len;
  *(int *)vdf_st = vdf_fmt;
  result->format = vdf_fmt;

  vdf_loop_cnt = 0;
  do {
    if (++vdf_loop_cnt > 1024) {
      pr_err("mwvd%d: open loop limit exceeded for %s\n",
             vd_number, tag);
      vdf_ret = -EIO;
      break;
    }
    vdf_ret = vdf_open(vdf_st, file_size, hdr_buf, buf_len,
                        result, vdf_kmalloc, vdf_kfree, vdf_guid_gen,
                        mwvd_vdf_read, (void *)file);
    if (vdf_ret < 0)
      break;

    if (result->status == VD_OPEN_NEED_DATA) {
      vd_i64 end_needed = result->need.offset + (vd_i64)result->need.length;
      if (end_needed > buf_len) {
        vd_u8 *new_buf;
        if (end_needed > 0x7FFFFFFF) {
          pr_err("mwvd%d: %s open data range too large (%lld)\n",
                 vd_number, tag, (long long)end_needed);
          vdf_ret = -EIO;
          break;
        }
        if (end_needed > 0x400000) {
          new_buf = vzalloc(end_needed);
          if (new_buf)
            memcpy(new_buf, hdr_buf, buf_len);
          kvfree(hdr_buf);
        } else {
          new_buf = krealloc(hdr_buf, end_needed, GFP_KERNEL);
        }
        if (!new_buf) {
          pr_err("mwvd%d: %s alloc(%d) failed\n", vd_number, tag,
                 (int)end_needed);
          vdf_close(vdf_st, vdf_kfree);
          kfree(vdf_st);
          vdf_st = NULL;
          break;
        }
        if ((vd_i64)buf_len < result->need.offset) {
          hdr_pos = buf_len;
          kernel_read(file, new_buf + buf_len,
                      result->need.offset - (vd_i64)buf_len, &hdr_pos);
        }
        hdr_pos = result->need.offset;
        kernel_read(file, new_buf + result->need.offset,
                    result->need.length, &hdr_pos);
        hdr_buf = new_buf;
        buf_len = (int)end_needed;
      }
    }
  } while (result->status != VD_OPEN_DONE && vdf_ret >= 0);

  if (vdf_ret < 0 || result->status != VD_OPEN_DONE) {
    pr_err("mwvd%d: %s open failed (ret=%d, status=%d)\n",
           vd_number, tag, vdf_ret, result->status);
    vdf_close(vdf_st, vdf_kfree);
    kfree(vdf_st);
    vdf_st = NULL;
  }

out_free_hdr:
  kvfree(hdr_buf);
  return vdf_st;
}

int vd_open_parent_image(struct mwvd_device *vd,
                         struct vdf_open_result *result, int depth)
{
  struct mwvd_device *pv;
  char *dir = NULL, *parent_path = NULL;
  struct file *pf = NULL;
  const char *pname = result->parent_name;
  int plen = strlen(pname);
  const char *base;
  int ret;

  if (depth >= MWVD_PARENT_CHAIN_MAX) {
    pr_err("mwvd%d: parent chain exceeds depth limit %d\n",
           vd->vd_number, MWVD_PARENT_CHAIN_MAX);
    return -ELOOP;
  }

  pv = kzalloc(sizeof(*pv), GFP_KERNEL);
  if (!pv)
    return -ENOMEM;

  /* 父设备继承子设备的 root_path（挂载上下文）：自动加载（vd_mon）路径下父/祖父
   * 镜像必须与子镜像以同一 root_path 打开。否则递归到祖父级时 pv->root_path 为 0，
   * 退化为 filp_open 按全局命名空间绝对路径查找；而 vfs_kern_mount 的私有挂载并未
   * 挂入命名空间，该绝对路径在全局不可见 → 父（depth0，走 root_path）能开、祖父
   * （depth>=1，走 filp_open）打不开（"超过2层"失败的根因）。 */
  if (vd->root_path.mnt) {
    pv->root_path = vd->root_path;
    path_get(&pv->root_path);
  }

  while (plen >= 2 && pname[0] == '.' &&
         (pname[1] == '\\' || pname[1] == '/')) {
    pname += 2;
    plen -= 2;
  }

  base = pname + plen;
  while (base > pname) {
    if (*(base - 1) == '/' || *(base - 1) == '\\')
      break;
    base--;
  }

  if (vd->vd_file_name[0]) {
    dir = mw_dirname(vd->vd_file_name);
    if (dir) {
      int dlen = strlen(dir);
      int blen = plen - (int)(base - pname);
      parent_path = kzalloc(dlen + 1 + blen + 1, GFP_KERNEL);
      if (parent_path) {
        memcpy(parent_path, dir, dlen);
        parent_path[dlen] = '/';
        memcpy(parent_path + dlen + 1, base, blen);
      }
      kfree(dir);
    }
  }
  if (!parent_path)
    parent_path = kstrdup(pname, GFP_KERNEL);
  if (!parent_path) {
    path_put(&pv->root_path);
    kfree(pv);
    return -ENOMEM;
  }

  if (vd->root_path.mnt)
    pf = vd_file_open_root(&vd->root_path, parent_path,
                           O_RDONLY | O_LARGEFILE, 0);
  else
    pf = filp_open(parent_path, O_RDONLY | O_LARGEFILE, 0);
  if (IS_ERR(pf)) {
    pr_err("mwvd%d: parent file open failed: %s (%ld)\n",
           vd->vd_number, parent_path, PTR_ERR(pf));
    kfree(parent_path);
    path_put(&pv->root_path);
    kfree(pv);
    return -EIO;
  }

  pv->vd_number = vd->vd_number;
  pv->vd_backing_file = pf;
  pv->FileSize = i_size_read(pf->f_mapping->host);
  memcpy(pv->vd_file_name, parent_path,
         min(strlen(parent_path), (size_t)(MWVD_NAME_SIZE - 1)));
  kfree(parent_path);

  {
    struct vdf_open_result presult;
    void *pst = vd_open_image(pf, pv->FileSize, vd->vd_number, "parent",
                              &presult);
    if (!pst) {
      pr_err("mwvd%d: parent image parse failed: %s\n",
             vd->vd_number, pv->vd_file_name);
      fput(pf);
      path_put(&pv->root_path);
      kfree(pv);
      return -EIO;
    }
    pv->vdf_state = pst;
    pr_debug("mwvd%d: parent image parsed: format=%d sectors=%lld\n",
             vd->vd_number, presult.format,
             (long long)presult.total_sectors);

    if (vd->vdf_state && *(int *)vd->vdf_state == VD_FORMAT_VHDX &&
        presult.format != VD_FORMAT_VHDX) {
      pr_err("mwvd%d: VHDX parent %s is not VHDX (fmt=%d); "
             "VHDX differencing requires VHDX parent\n",
             vd->vd_number, pv->vd_file_name, presult.format);
      vdf_close(pst, vdf_kfree);
      pv->vdf_state = NULL;
      fput(pf);
      path_put(&pv->root_path);
      kfree(pv);
      return -EINVAL;
    }

    if (result->total_sectors != presult.total_sectors ||
        result->sector_size != presult.sector_size) {
      pr_err("mwvd%d: parent %s geometry mismatch "
             "(child sectors=%lld ssize=%d, parent sectors=%lld ssize=%d); "
             "differencing requires matching size & sector size\n",
             vd->vd_number, pv->vd_file_name,
             (long long)result->total_sectors, result->sector_size,
             (long long)presult.total_sectors, presult.sector_size);
      vdf_close(pst, vdf_kfree);
      pv->vdf_state = NULL;
      fput(pf);
      path_put(&pv->root_path);
      kfree(pv);
      return -EINVAL;
    }

    if (presult.parent_name[0] || presult.parent_uuid[0]) {
      ret = vd_open_parent_image(pv, &presult, depth + 1);
      if (ret < 0) {
        pr_err("mwvd%d: failed to open grandparent of %s: %d\n",
               vd->vd_number, pv->vd_file_name, ret);
        vdf_close(pst, vdf_kfree);
        pv->vdf_state = NULL;
        fput(pf);
        path_put(&pv->root_path);
        kfree(pv);
        return ret;
      }
    }
  }

  vd->ParentVDisk = pv;
  /* 链深度标注：递归先打开最深层，日志按“爷爷→父”顺序出现；不加 depth
   * 会让人误读为“父镜像变成爷爷镜像”。depth=0 为直接父，1=祖父，2=曾祖父… */
  pr_info("mwvd%d: parent chain depth %d: opened %s size=%lld (child=%s)\n",
          vd->vd_number, depth, pv->vd_file_name, pv->FileSize,
          vd->vd_file_name[0] ? vd->vd_file_name : "(none)");
  return 0;
}

static int vd_flush_vdf_state(struct mwvd_device *vd);

void vd_close_image(struct mwvd_device *vd)
{
  if (vd->ParentVDisk) {
    struct mwvd_device *pv = vd->ParentVDisk;
    vd_close_image(pv);
    if (pv->vd_backing_file) {
      fput(pv->vd_backing_file);
      pv->vd_backing_file = NULL;
    }
    if (pv->root_path.mnt) {
      path_put(&pv->root_path);
      memset(&pv->root_path, 0, sizeof(pv->root_path));
    }
    kfree(pv);
    vd->ParentVDisk = NULL;
  }

  if (vd->vdf_state) {
    if (vdf_flush_required(vd->vdf_state)) {
      if (vd_flush_vdf_state(vd) < 0)
        pr_warn("mwvd%d: flush on close failed\n", vd->vd_number);
    }
    vdf_close(vd->vdf_state, vdf_kfree);
    vd->vdf_state = NULL;
  }
}

static int vdf_map_req_op(unsigned int req_op, unsigned int cmd_flags)
{
    switch (req_op) {
    case REQ_OP_READ:
        return VD_OP_READ;
    case REQ_OP_WRITE:
        return VD_OP_WRITE;
    case REQ_OP_FLUSH:
        return VD_OP_WRITE;
    case REQ_OP_DISCARD:
        return VD_OP_DISCARD;
    case REQ_OP_WRITE_ZEROES:
        if (cmd_flags & REQ_NOUNMAP)
            return VD_OP_FALLOC_ZERO;
        return VD_OP_WRITE_ZEROES;
    default:
        return -1;
    }
}

static int vd_zero_file_range(struct file *f, loff_t off, loff_t len)
{
    size_t left;
    char *zero;
    loff_t pos = off;
    int ret;

    if (len <= 0)
        return 0;
    if (f->f_op->fallocate) {
        ret = f->f_op->fallocate(f, FALLOC_FL_ZERO_RANGE, off, len);
        if (ret != -EOPNOTSUPP && ret != -EINVAL)
            return ret;
    }
    zero = kzalloc(4096, GFP_KERNEL);
    if (!zero)
        return -ENOMEM;
    left = (size_t)len;
    ret = 0;
    while (left > 0) {
        size_t chunk = left > 4096 ? 4096 : left;
        ssize_t wr = kernel_write(f, zero, chunk, &pos);
        if (wr != (ssize_t)chunk) {
            ret = wr < 0 ? (int)wr : -EIO;
            break;
        }
        left -= chunk;
    }
    kfree(zero);
    return ret;
}

static int vd_flush_vdf_state(struct mwvd_device *vd)
{
    struct vdf_flush_segment *fsegs;
    int n, i, ret = 0;

    /* 栈上 fsegs[32]（24B×32=768B，叠加其余局部量超 1024）会触发
     * -Wframe-larger-than=1024 警告；改动态分配（vdf_flush 仅在本次调用内
     * 填充/消费 fsegs，无需跨调用保留）。 */
    fsegs = kmalloc_array(32, sizeof(*fsegs), GFP_KERNEL);
    if (!fsegs)
        return -ENOMEM;

    /* 游标分页：vdf_flush 每次生成一批脏段（≤32），循环直至全部生成（返回 0），
     * 支持单次写跨 >32 chunk（>32 脏 bitmap/BAT）。任一段写失败 →
     * vdf_flush_reset（保留 dirty、重置游标）→ 下次 flush 从起点重发全部段，
     * 瞬时写失败可在重试中真正落盘，绝不静默成功。 */
    for (;;) {
        n = vdf_flush(vd->vdf_state, fsegs, 32);
        if (n <= 0)
            break;
        for (i = 0; i < n; i++) {
            loff_t fpos = fsegs[i].file_offset;
            if (fsegs[i].type == VD_DIRTY_LOG_ZERO) {
                ret = vd_zero_file_range(vd->vd_backing_file,
                                         fsegs[i].file_offset,
                                         fsegs[i].data_len);
            } else {
                ret = kernel_write(vd->vd_backing_file, fsegs[i].data,
                                   fsegs[i].data_len, &fpos);
                pr_debug("mwvd%d: flush seg[%d] type=%d offset=%lld len=%d ret=%d\n",
                        vd->vd_number, i, fsegs[i].type,
                        fsegs[i].file_offset, fsegs[i].data_len, ret);
                if (ret >= 0 && ret != fsegs[i].data_len) {
                    pr_err("mwvd%d: flush seg[%d] short write (%d/%d)\n",
                           vd->vd_number, i, ret, fsegs[i].data_len);
                    ret = -EIO;
                }
            }
            if (ret < 0) {
                vdf_flush_reset(vd->vdf_state);
                kfree(fsegs);
                return ret;
            }
        }
    }
    kfree(fsegs);
    vdf_flush_done(vd->vdf_state);
    return 0;
}

int vd_replay_pending_log(struct mwvd_device *vd)
{
    if (vd && vd->vdf_state && vdf_flush_required(vd->vdf_state)) {
        int ret = vd_flush_vdf_state(vd);
        if (ret < 0) {
            pr_warn("mwvd%d: failed to replay pending VHDX log\n",
                    vd->vd_number);
            return ret;
        }
    }
    return 0;
}

/*
 * 挂起/休眠（S3/S4）前落盘屏障：递归 flush 差分父链 + 当前镜像元数据
 * （BAT/bitmap/VHDX log），并 fsync backing 文件，防止休眠断电后镜像与内存不一致。
 * 不关闭镜像、不冻结请求队列（配合 WQ_FREEZABLE 使用，避免与冻结 workqueue 互等死锁）。
 * 由 PM notifier 在 freeze_processes() 之后调用，此时 workqueue 已冻结、无并发 IO。
 * @return 0 成功；负值为错误码。
 */
int vd_flush_all(struct mwvd_device *vd)
{
    int ret = 0;

    if (vd == NULL)
        return 0;

    /* 只读镜像跳过落盘（只读无写入，flush/fsync 无必要） */
    if (vd->vd_flags & MWVD_FLAGS_READ_ONLY)
        return 0;

    /* 差分父镜像先落盘 */
    if (vd->ParentVDisk) {
        ret = vd_flush_all(vd->ParentVDisk);
        if (ret < 0)
            return ret;
    }

    /* 当前镜像元数据（BAT/bitmap/VHDX log） */
    if (vd->vdf_state && vdf_flush_required(vd->vdf_state)) {
        ret = vd_flush_vdf_state(vd);
        if (ret < 0)
            return ret;
    }

    /* backing 文件数据落盘 */
    if (vd->vd_backing_file && vd->vd_backing_file->f_op->fsync)
        ret = vfs_fsync(vd->vd_backing_file, 0);

    return ret;
}

static int vd_read_virtual(struct mwvd_device *vd, vd_i64 sector, int count,
                           struct iov_iter *iter)
{
    vd_i64 cur = sector;
    int remaining = count;

    if (!vd || !vd->vdf_state || !vd->vd_backing_file)
        return -EIO;

    while (remaining > 0) {
        struct vdf_translate_result tres;
        int i;

        vdf_translate(vd->vdf_state, cur, remaining, VD_OP_READ, &tres);
        if (tres.seg_count <= 0)
            return -EIO;

        for (i = 0; i < tres.seg_count; i++) {
            struct vdf_segment *seg = &tres.segs[i];
            size_t len = (size_t)seg->length;

            if (len <= 0)
                continue;

            if (seg->op == VD_OP_ZERO) {
                struct iov_iter tmp = *iter;
                iov_iter_truncate(&tmp, len);
                iov_iter_zero(len, &tmp);
                iov_iter_advance(iter, len);
            } else if (seg->file_id == 1) {
                int rc = vd_read_virtual(vd->ParentVDisk, seg->file_offset,
                                         (int)(len >> 9), iter);
                if (rc < 0)
                    return rc;
            } else {
                struct file *pf = vd->vd_backing_file;
                loff_t pos = seg->file_offset;
                ssize_t r;
                struct iov_iter tmp = *iter;

                iov_iter_truncate(&tmp, len);
                r = vfs_iter_read(pf, &tmp, &pos, 0);
                if (r < 0)
                    return r;
                if (r < (ssize_t)len) {
                    iov_iter_zero((size_t)(len - r), &tmp);
                }
                iov_iter_advance(iter, len);
            }

            cur += len >> 9;
            remaining -= (int)(len >> 9);
            if (remaining < 0)
                remaining = 0;
        }
    }
    return 0;
}

static int vd_extend_new_blocks(struct mwvd_device *vd,
                                struct vdf_translate_result *tres)
{
    if (tres->block_size > 0) {
        struct file *f = vd->vd_backing_file;
        loff_t fsize = i_size_read(f->f_mapping->host);
        int si;
        for (si = 0; si < tres->seg_count; si++) {
            struct vdf_segment *seg = &tres->segs[si];
            if (seg->op == VD_OP_WRITE) {
                loff_t block_start = (seg->file_offset / tres->block_size)
                                     * tres->block_size;
                loff_t block_end = block_start + tres->block_size;
                if (block_end > fsize) {
                    loff_t zero_pos = block_end - 1;
                    ssize_t wr = kernel_write(f, "\0", 1, &zero_pos);
                    if (wr != 1)
                        return wr < 0 ? (int)wr : -EIO;
                    fsize = block_end;
                }
            }
        }
    }
    return 0;
}

int vd_handle_request(struct mwvd_device *vd, struct request *rq)
{
    struct vdf_translate_result tres;
    int vdf_op;
    int64_t sector;
    unsigned int count;
    int ret = 0;
    int i;

    vdf_op = vdf_map_req_op(req_op(rq), rq->cmd_flags);
    if (vdf_op < 0)
        return -EIO;

    sector = (int64_t)blk_rq_pos(rq);
    count = blk_rq_sectors(rq);

    switch (req_op(rq))
    {
    case REQ_OP_FLUSH:
    {
        if (vd->vdf_state && vdf_flush_required(vd->vdf_state))
        {
            ret = vd_flush_vdf_state(vd);
            if (ret < 0)
                return ret;
        }
        if (vd->vd_backing_file->f_op->fsync)
            ret = vfs_fsync(vd->vd_backing_file, 0);
        return ret;
    }

    case REQ_OP_DISCARD:
    case REQ_OP_WRITE_ZEROES:
        if (!vd->vdf_state)
            return -EIO;
        {
            struct file *f = vd->vd_backing_file;
            int64_t done = 0;
            int total_dirty = 0;

            do {
                vdf_translate(vd->vdf_state, sector + done,
                              count - (int)done, vdf_op, &tres);
                total_dirty |= tres.dirty_flags;

                for (i = 0; i < tres.seg_count; i++) {
                    struct vdf_segment *seg = &tres.segs[i];
                    if (seg->op == VD_OP_FALLOC_PUNCH || seg->op == VD_OP_DISCARD) {
                        if (f->f_op->fallocate)
                            ret = f->f_op->fallocate(f,
                                FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                                seg->file_offset, seg->length);
                        if (ret == -EOPNOTSUPP || ret == -EINVAL)
                            ret = 0;
                    } else if (seg->op == VD_OP_WRITE_ZEROES ||
                               seg->op == VD_OP_FALLOC_ZERO) {
                        ret = vd_zero_file_range(f, seg->file_offset,
                                                 seg->length);
                    }
                    if (ret < 0)
                        return ret;
                }

                if (!tres.truncated)
                    break;
                if (tres.covered_sectors <= 0)
                    return -EIO;
                done += tres.covered_sectors;
            } while (done < count);

            if (total_dirty) {
                ret = vd_flush_vdf_state(vd);
                if (ret < 0)
                    return ret;
            }
        }
        return 0;

    case REQ_OP_READ:
    case REQ_OP_WRITE:
    {
        struct bio *bio;
        int total_dirty;

        if (!vd->vdf_state)
            return -EIO;

        vdf_translate(vd->vdf_state, sector, count, vdf_op, &tres);
        ret = vd_extend_new_blocks(vd, &tres);
        if (ret < 0)
            return ret;
        total_dirty = tres.dirty_flags;

        {
            int seg_idx = 0;
            int64_t processed = 0;
            int iter_dir = (vdf_op == VD_OP_WRITE) ? ITER_SOURCE : ITER_DEST;

            __rq_for_each_bio(bio, rq)
            {
                struct bio_vec bvec;
                struct bvec_iter bv_iter;

                bio_for_each_segment(bvec, bio, bv_iter)
                {
                    struct iov_iter iter_state;
                    size_t len = bvec.bv_len;

                    iov_iter_bvec(&iter_state, iter_dir, &bvec, 1, len);

                    while (len > 0)
                    {
                        if (seg_idx >= tres.seg_count)
                        {
                            if (processed >= count)
                                break;
                            vdf_translate(vd->vdf_state, sector + processed,
                                          count - processed, vdf_op, &tres);
                            if (tres.seg_count <= 0)
                                return -EIO;
                            seg_idx = 0;
                            total_dirty |= tres.dirty_flags;
                            ret = vd_extend_new_blocks(vd, &tres);
                            if (ret < 0)
                                return ret;
                        }

                        {
                            struct vdf_segment *seg = &tres.segs[seg_idx];
                            size_t this_len = seg->length < (vd_i64)len ?
                                              (size_t)seg->length : len;

                            if (this_len <= 0) { seg_idx++; continue; }

                            if (seg->op == VD_OP_ZERO)
                            {
                                iov_iter_zero(this_len, &iter_state);
                            }
                            else if (seg->file_id == 1)
                            {
                                if (vd->ParentVDisk)
                                {
                                    int rc = vd_read_virtual(vd->ParentVDisk,
                                                    seg->file_offset,
                                                    (int)(this_len >> 9),
                                                    &iter_state);
                                    if (rc < 0)
                                        return rc;
                                }
                                else
                                {
                                    iov_iter_zero(this_len, &iter_state);
                                }
                                seg->file_offset += (this_len >> 9);
                            }
                            else
                            {
                                struct file *target_file = vd->vd_backing_file;
                                loff_t pos = seg->file_offset;
                                ssize_t r;

                                iov_iter_truncate(&iter_state, this_len);
                                if (seg->op == VD_OP_READ) {
                                    r = vfs_iter_read(target_file,
                                                      &iter_state, &pos, 0);
                                } else
                                    r = vfs_iter_write(target_file,
                                                       &iter_state, &pos, 0);
                                if (r < 0)
                                    return r;
                                iov_iter_reexpand(&iter_state, len - this_len);
                                seg->file_offset += this_len;
                            }

                            len -= this_len;
                            processed += this_len >> 9;
                            seg->length -= this_len;
                            if (seg->length == 0)
                                seg_idx++;
                        }
                    }

                    if (len > 0)
                        return -EIO;
                }
            }

            if (processed != count)
                return -EIO;
        }

        if (total_dirty)
        {
            ret = vd_flush_vdf_state(vd);
            if (ret < 0)
                return ret;
        }
        return 0;
    }

    default:
        return -EIO;
    }
}
