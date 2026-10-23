// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/**
 * mwcfg_ext.c — v2 通讯 blob 的“条目扩展记录”（ExtraData TLV）解析
 *
 * 权威规范：mw-docs COMMUNICATION_V2_SPEC.md §6.7（v2.5）。实现要点：
 *   - 记录只挂在**启动条目**（ENTRY_FLAG_BOOT_ENTRY）的载荷 ExtraData 区；
 *   - 尺寸/偏移一律使用显式常量（§6.1），禁止依赖 sizeof 计算跨端偏移；
 *   - 任何畸形（声明越界 / Size 非法 / 末尾残余）都安全放弃，不越界读；
 *   - 纯 C、无内核依赖 ⇒ 目标机内核与主机端测试程序（
 *     MultiwarePkg/Utilities/MwCfgExtTest）共用同一份实现。
 *
 * Copyright (c) 2025-2026 LU WEIFENG (wave.lu@gmail.com)
 * 版权所有 (c) 2025-2026 陆伟峰
 */
#include "mwcfg_ext.h"

/* 显式尺寸常量（与 COMMUNICATION_V2_SPEC §6.1/CONSTANTS_SPEC §7.1 一致） */
#define MWCFG_EXT_BLOB_HDR_SIZE    56u                              /* MW_CFG_BLOB */
#define MWCFG_EXT_ENTRY_HDR_SIZE   42u                              /* MW_CFG_ENTRY 头 */
#define MWCFG_EXT_PAYLOAD_FIXED    (37u + 16u + 260u + 16u + 16u + 256u + 4u) /* 605 */
#define MWCFG_EXT_RECORD_HDR       ((mw_u32)MW_CFG_EXT_RECORD_HDR_SIZE)       /* 4 */

/* ---- 无 libc 依赖的基础操作（内核与主机端皆可编译） ---- */

static void
mwcfg_ext_memcpy (
  void        *dst,
  const void  *src,
  mw_u32       n
  )
{
  mw_u8       *d = (mw_u8 *)dst;
  const mw_u8 *s = (const mw_u8 *)src;
  mw_u32       i;

  for (i = 0; i < n; i++) {
    d[i] = s[i];
  }
}

static void
mwcfg_ext_memzero (
  void    *dst,
  mw_u32   n
  )
{
  mw_u8 *d = (mw_u8 *)dst;
  mw_u32 i;

  for (i = 0; i < n; i++) {
    d[i] = 0;
  }
}

static mw_u32
mwcfg_ext_rd32 (
  const mw_u8  *p
  )
{
  return (mw_u32)p[0] | ((mw_u32)p[1] << 8) | ((mw_u32)p[2] << 16) | ((mw_u32)p[3] << 24);
}

static mw_u16
mwcfg_ext_rd16 (
  const mw_u8  *p
  )
{
  return (mw_u16)((mw_u16)p[0] | ((mw_u16)p[1] << 8));
}

/**
 * 取出启动条目的休眠存储分区扩展记录（见 mwcfg_ext.h）。
 */
int
mwcfg_ext_get_hibernate (
  const void                *blob,
  struct mwcfg_hibernate     *out
  )
{
  const mw_u8 *b = (const mw_u8 *)blob;
  mw_u32       total;
  mw_u16       count;
  mw_u32       off;
  mw_u16       i;

  if (blob == 0 || out == 0) {
    return MWCFG_EXT_EINVAL;
  }
  mwcfg_ext_memzero(out, sizeof(*out));

  /* 头部基本校验（magic / 条目数 / 总长） */
  if (mwcfg_ext_rd32(b) != MW_CFG_BLOB_MAGIC) {
    return MWCFG_EXT_ENOTBLOB;
  }
  count = mwcfg_ext_rd16(b + 12);
  total = mwcfg_ext_rd32(b + 8);
  if (count == 0 || count > MW_CFG_MAX_ENTRIES) {
    return MWCFG_EXT_ENOTBLOB;
  }
  if (total < MWCFG_EXT_BLOB_HDR_SIZE || total > MW_CFG_MAX_BLOB_SIZE) {
    return MWCFG_EXT_ENOTBLOB;
  }

  off = MWCFG_EXT_BLOB_HDR_SIZE;
  for (i = 0; i < count; i++) {
    const mw_u8 *e;
    mw_u16       entry_size;
    mw_u16       flags;
    mw_u16       payload_size;

    if (off + MWCFG_EXT_ENTRY_HDR_SIZE > total) {
      return MWCFG_EXT_EBROKEN;
    }

    e            = b + off;
    entry_size   = mwcfg_ext_rd16(e);
    flags        = mwcfg_ext_rd16(e + 2);
    payload_size = mwcfg_ext_rd16(e + 4);
    if (entry_size < MWCFG_EXT_ENTRY_HDR_SIZE || (off + (mw_u32)entry_size) > total) {
      return MWCFG_EXT_EBROKEN;
    }

    /* 记录只在启动条目上（生产者规则；其余条目直接跳过） */
    if ((flags & ENTRY_FLAG_ENABLED) && (flags & ENTRY_FLAG_BOOT_ENTRY) &&
        (payload_size >= MWCFG_EXT_PAYLOAD_FIXED)) {
      const mw_u8 *payload  = e + MWCFG_EXT_ENTRY_HDR_SIZE;
      const mw_u8 *ext_base = payload + MWCFG_EXT_PAYLOAD_FIXED;
      mw_u32       ext_avail;
      mw_u32       r_off = 0;

      /* ExtraDataSize 位于定长载荷末尾（偏移 601） */
      ext_avail = mwcfg_ext_rd32(payload + (MWCFG_EXT_PAYLOAD_FIXED - 4));
      if (ext_avail == 0) {
        return MWCFG_EXT_NORECORD;                        /* 未配置 ⇒ 记录缺失 */
      }
      if (ext_avail > (mw_u32)(payload_size - MWCFG_EXT_PAYLOAD_FIXED)) {
        return MWCFG_EXT_EBROKEN;                         /* 声明越界 */
      }

      while (r_off + MWCFG_EXT_RECORD_HDR <= ext_avail) {
        mw_u16 type = mwcfg_ext_rd16(ext_base + r_off);
        mw_u16 size = mwcfg_ext_rd16(ext_base + r_off + 2);

        if (size < MWCFG_EXT_RECORD_HDR || (size & 3u) != 0 ||
            (r_off + (mw_u32)size) > ext_avail) {
          return MWCFG_EXT_EBROKEN;                       /* 尺寸非法/越界 */
        }

        if (type == MW_CFG_EXT_TYPE_HIBERNATE_PARTITION) {
          const mw_u8 *body;
          mw_u16       body_size;

          if (size < (mw_u16)(MWCFG_EXT_RECORD_HDR + MW_CFG_EXT_HIBERNATE_PARTITION_SIZE)) {
            return MWCFG_EXT_EBROKEN;                     /* 记录体过短 */
          }
          body      = ext_base + r_off + MWCFG_EXT_RECORD_HDR;
          body_size = (mw_u16)(size - MWCFG_EXT_RECORD_HDR);

          /* 取 36 字符并补 NUL；统一小写（udev 用 /dev/disk/by-partuuid/<小写>） */
          mwcfg_ext_memcpy(out->part_uuid, body, 36);
          out->part_uuid[36] = '\0';
          for (i = 0; i < 36; i++) {
            if (out->part_uuid[i] >= 'A' && out->part_uuid[i] <= 'Z') {
              out->part_uuid[i] = (char)(out->part_uuid[i] - 'A' + 'a');
            }
          }
          out->usage = body[37];
          (void)body_size;
          return MWCFG_EXT_FOUND;
        }

        r_off += size;                                    /* 未知类型跳过 */
      }

      return (r_off == ext_avail) ? MWCFG_EXT_NORECORD : MWCFG_EXT_EBROKEN;
    }

    off += entry_size;
  }

  return MWCFG_EXT_NORECORD;                              /* 无启动条目/无记录 */
}
