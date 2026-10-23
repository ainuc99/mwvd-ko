// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/**
 * mwcfg_ext.h — v2 通讯 blob 的“条目扩展记录”（ExtraData TLV）解析接口
 *
 * 权威规范：mw-docs COMMUNICATION_V2_SPEC.md §6.7（v2.5）；常量取值：
 * CONSTANTS_SPEC.md §7.1。协议头 `common/mwcfg_v2.h`（subtree，只 pull 不 push）。
 *
 * 纯 C：不依赖内核头文件与内核 API，目标机内核（GPL 层）与主机端测试程序共用。
 *
 * Copyright (c) 2025-2026 LU WEIFENG (wave.lu@gmail.com)
 * 版权所有 (c) 2025-2026 陆伟峰
 */
#ifndef _MWCFG_EXT_H_
#define _MWCFG_EXT_H_

#include "mwcfg_v2.h"

/* 返回码 */
#define MWCFG_EXT_FOUND      1   /* 找到记录，*out 已填充 */
#define MWCFG_EXT_NORECORD   0   /* 无该记录（固件未配置 / 记录缺失即“未设置”） */
#define MWCFG_EXT_ENOTBLOB  (-1) /* 不是本协议 blob（magic/尺寸非法） */
#define MWCFG_EXT_EBROKEN   (-2) /* blob 或记录区损坏（安全放弃，不得越界读） */
#define MWCFG_EXT_EINVAL    (-3) /* 参数错误 */

/* 休眠存储分区（Type = MW_CFG_EXT_TYPE_HIBERNATE_PARTITION）解析结果 */
struct mwcfg_hibernate
{
  char  part_uuid[MW_CFG_EXT_HIBERNATE_PARTITION_SIZE - 2]; /* 37：36 字符小写 + NUL */
  mw_u8 usage;                                              /* MW_CFG_EXT_HIB_USAGE_* */
};

/**
 * 从 v2 配置块中取出“启动条目”携带的休眠存储分区扩展记录。
 *
 * @param blob  固件发布的 MW_CFG_BLOB（须仍在映射期内；读取完即可解除映射）
 * @param out   输出（成功时填充；调用前无需初始化）
 *
 * @return MWCFG_EXT_FOUND / MWCFG_EXT_NORECORD / 负值错误码（见上）
 */
int mwcfg_ext_get_hibernate(const void *blob, struct mwcfg_hibernate *out);

#endif /* _MWCFG_EXT_H_ */
