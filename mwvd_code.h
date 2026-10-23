// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/**
 * mwvd_code.h — Linux 端「固件内嵌闭源机器码」支持（COMMUNICATION_V2_SPEC v2.8 §6.9）
 *
 * 职责：
 *   1. 从 ACPI `MWVD` 表读取内联代码载荷（`MW_CODE_BLOB`）；
 *   2. 按 §6.9.7 顺序校验：表边界 → 表体自洽 → 载荷头部自洽 → CRC-32 →
 *      验签（RSA-2048/SHA-256/PKCS#1 v1.5）→ 架构与 ABI 判据；
 *   3. 把可重定位目标文件（ELF ET_REL）加载到内核可执行内存并重定位；
 *   4. 解析闭源层导出符号，填充 GPL 层的函数表 `g_mwvd_vdf`。
 *
 * 失败语义（2.8.0 起 = 纯固件载荷模式）：
 *   不再链接内置闭源层 `vd.o`，载荷是唯一的格式后端来源。任何一步失败
 *   都使 `mwvd_code_init()` 返回错误，由 `mwvd_init()` 直接拒载（`modprobe`
 *   / initramfs 阶段即暴露），失败原因写入内核日志。不做静默降级。
 *
 * 架构限制：ELF ET_REL 加载器覆盖 x86_64 / i386 / arm64；其余架构上
 * `mwvd_elf_load()` 返回 `-EOPNOTSUPP`，模块会拒载。
 *
 * Copyright (c) 2026 LU WEIFENG (wave.lu@gmail.com)
 * 版权所有 (c) 2026 陆伟峰
 */
#ifndef _MWVD_CODE_H_
#define _MWVD_CODE_H_

#include "vd_abi.h"

/**
 * 闭源层函数表（GPL 层唯一的调用入口）。
 *
 * 成员名刻意不带 `vdf_` 前缀：`vd_abi.h` 保留给闭源层声明，
 * GPL 层在 `vd.h` 里用同名宏（`vdf_probe(...)` → `g_mwvd_vdf->probe(...)`）
 * 保持现有调用点不变。
 */
struct mwvd_vdf_api {
  /* 格式探测 / 状态尺寸 */
  int  (*probe)(const vd_u8 *buf, int buf_len, vd_i64 file_size, int *format);
  int  (*state_size)(int format);

  /* 核心库版本 */
  const char *(*core_version)(void);
  vd_u32 (*core_version_code)(void);

  /* 打开 / 关闭 */
  int  (*open)(void *state, vd_i64 file_size,
               const vd_u8 *buf, int buf_len,
               struct vdf_open_result *result,
               vdf_alloc_fn alloc, vdf_free_fn free,
               vdf_guid_gen_fn guid_gen,
               vdf_read_fn read, void *read_ctx);
  void (*close)(void *state, vdf_free_fn free);

  /* I/O 翻译 / 落盘 */
  void (*translate)(void *state, vd_i64 sector, int count, int op,
                    struct vdf_translate_result *result);
  int  (*flush)(void *state, struct vdf_flush_segment *segs, int max_segs);
  void (*flush_done)(void *state);
  void (*flush_reset)(void *state);
  int  (*flush_required)(void *state);

  /* v1 / v2 契约解析 */
  int  (*parse_efi)(const char *efi_text, struct vdf_efi_result *result);
  int  (*parse_efi_blob_v2)(const void *blob, struct vdf_efi_result *results,
                            int max_results, int *total);

  /* 契约 blob 定位（物理地址回调由 GPL 层提供） */
  int  (*acquire_config_blob)(vdf_map_phys_fn map, vdf_unmap_phys_fn unmap,
                              void **out);
  int  (*acquire_config_blob_acpi)(const void *tbl, int tbl_len,
                                   vdf_map_phys_fn map,
                                   vdf_unmap_phys_fn unmap, void **out);

  /* 诊断 */
  void (*get_diag)(void *state, struct vdf_bat_diag *diag);
};

/** 当前生效的闭源层函数表（模块初始化时确定，之后只读）。 */
extern const struct mwvd_vdf_api *g_mwvd_vdf;

/**
 * 初始化：加载固件 `MWVD` 代码载荷并填充 `g_mwvd_vdf`。
 *
 * @return 0 = 已加载固件载荷；负值 = 加载失败（调用方应拒绝加载模块）。
 */
int mwvd_code_init(void);

/** 卸载：释放运行时加载的闭源层机器码。 */
void mwvd_code_exit(void);

#endif /* _MWVD_CODE_H_ */
