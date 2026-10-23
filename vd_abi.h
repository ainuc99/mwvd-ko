// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/**
 * vd_abi.h — GPL 层与闭源层的纯 C 接口约定。
 *
 * 约束：
 *   1. 不得 #include 任何 Linux 内核头文件
 *   2. 类型使用自 typedef 的定长整数
 *   3. 指针使用 void *，字符串使用定长 char 数组
 *
 * Copyright (c) 2026 LU WEIFENG (wave.lu@gmail.com)
 * 版权所有 (c) 2026 陆伟峰
 */

#ifndef VD_ABI_H
#define VD_ABI_H

/* 定长整数类型（避免依赖 stdint.h 或 linux/types.h） */
#ifndef __VD_ABI_TYPES_DEFINED
#define __VD_ABI_TYPES_DEFINED
typedef unsigned char      vd_u8;
typedef signed char        vd_i8;
typedef unsigned short     vd_u16;
typedef signed short       vd_i16;
typedef unsigned int       vd_u32;
typedef signed int         vd_i32;
typedef unsigned long long vd_u64;
typedef signed long long   vd_i64;
#endif

/* ================================================================== */
/* Constants                                                          */
/* ================================================================== */

#define VD_MAX_SEGS     32
#define VD_PROBE_MIN    0x400000  /* 4MB */
#define VD_FILE_NAME    256
#define VD_UUID_STR_LEN 40
#define VD_EFI_TEXT_MAX 4096

/* ================================================================== */
/* Core library version                                               */
/* ================================================================== */

/* mwvd-core 库版本：各端驱动加载后可经 vd_core_version() 查询，用于识别所引用
 * 核心库版本；与宿主驱动自身版本相互独立。变更须同步更新此宏与实现。 */
#define VD_CORE_VERSION_MAJOR 1
#define VD_CORE_VERSION_MINOR 0
#define VD_CORE_VERSION_PATCH 1
#define VD_CORE_VERSION_STRING "1.0.1"

/* ================================================================== */
/* Format types                                                       */
/* ================================================================== */

enum {
    VD_FORMAT_UNKNOWN = 0,
    VD_FORMAT_RAW,
    VD_FORMAT_VHD,
    VD_FORMAT_VHDX,
    VD_FORMAT_MAX,
};

/* ================================================================== */
/* I/O operation codes                                                */
/* ================================================================== */

enum {
    VD_OP_READ         = 0,
    VD_OP_WRITE        = 1,
    VD_OP_ZERO         = 2,
    VD_OP_FALLOC_ZERO  = 3,
    VD_OP_FALLOC_PUNCH = 4,
    VD_OP_DISCARD      = 5,
    VD_OP_WRITE_ZEROES = 6,
};

/* ================================================================== */
/* Dirty flags                                                        */
/* ================================================================== */

enum {
    VD_DIRTY_BAT       = 1 << 0,
    VD_DIRTY_BITMAP    = 1 << 1,
    VD_DIRTY_LOG       = 1 << 2,
    VD_DIRTY_LOG_ZERO  = 1 << 3,
};

/* ================================================================== */
/* Memory management callbacks                                       */
/* ================================================================== */

typedef void *(*vdf_alloc_fn)(int size);
typedef void  (*vdf_free_fn)(void *p);
typedef void  (*vdf_guid_gen_fn)(vd_u8 guid[16]);

/* 文件读取回调：按绝对字节偏移读取任意文件区间到 dst。
 * ctx 由调用方指定；@return 实际读取字节数（<= len），负数表示错误。 */
typedef int (*vdf_read_fn)(void *ctx, vd_i64 offset, vd_u8 *dst, int len);

int vdf_probe(const vd_u8 *buf, int buf_len, vd_i64 file_size,
             int *format);

int vdf_state_size(int format);

/* 返回核心库版本字符串（如 "1.0.0"；静态存储，非 NULL）。 */
const char *vd_core_version(void);
/* 返回核心库版本号（编码 0xMMmmpp，如 0x010000）。 */
vd_u32 vd_core_version_code(void);

#define VD_OPEN_DONE      0
#define VD_OPEN_NEED_DATA 1

struct vdf_open_request {
    int     file_id;
    vd_i64 offset;
    vd_i64 length;
};

struct vdf_open_result {
    int     status;          /* VD_OPEN_DONE / VD_OPEN_NEED_DATA */
    int     format;
    int     sector_size;
    vd_i64 total_sectors;

    /* status == VD_OPEN_DONE 时以下字段可能有效 */
    char    parent_name[VD_FILE_NAME];
    char    parent_uuid[VD_UUID_STR_LEN];
    vd_i64 parent_timestamp;
    int     dirty_flags;

    /* status == VD_OPEN_NEED_DATA 时有效 */
    struct vdf_open_request need;
};

int vdf_open(void *state, vd_i64 file_size,
            const vd_u8 *buf, int buf_len,
            struct vdf_open_result *result,
            vdf_alloc_fn alloc, vdf_free_fn free,
            vdf_guid_gen_fn guid_gen,
            vdf_read_fn read, void *read_ctx);

void vdf_close(void *state, vdf_free_fn free);

struct vdf_segment {
    int     op;            /* VD_OP_* */
    int     file_id;
    vd_i64 file_offset;
    vd_i64 length;
};

struct vdf_translate_result {
    struct vdf_segment segs[VD_MAX_SEGS];
    int   seg_count;
    int   dirty_flags;
    int   block_size;      /* 0=无新分配 */
    int   truncated;       /* 1=段数超限被截断 */
    int   covered_sectors; /* 本次已覆盖的 512B 扇区数 */
};

void vdf_translate(void *state, vd_i64 sector, int count,
                  int op, struct vdf_translate_result *result);

struct vdf_flush_segment {
    int     type;           /* VD_DIRTY_* */
    const vd_u8 *data;
    int     data_len;
    vd_i64 file_offset;
};

int  vdf_flush(void *state, struct vdf_flush_segment *segs, int max_segs);
void vdf_flush_done(void *state);
/* 写失败后宿主调用：重置 flush 分页游标（保留 dirty），下次 vdf_flush 从起点
 * 重新生成全部脏段 —— 瞬时写失败可在重试中真正落盘，绝不静默成功。 */
void vdf_flush_reset(void *state);
int  vdf_flush_required(void *state);

struct vdf_efi_result {
    char part_uuid[64];
    char fs_type[32];
    char vdisk_path[VD_FILE_NAME];
    char vd_type[16];
};

int vdf_parse_efi(const char *efi_text, struct vdf_efi_result *result);

/* 返回值：填充条目数；负数表示数据无效。 */
int vdf_parse_efi_blob_v2(const void *blob, struct vdf_efi_result *results,
                          int max_results, int *total);

/* 物理内存映射回调（GPL 层提供的物理地址映射能力） */
typedef void *(*vdf_map_phys_fn)(vd_u64 phys, vd_u64 size);  /* 返回映射指针或 NULL */
typedef void  (*vdf_unmap_phys_fn)(void *ptr);

/* 获取固件配置数据块。
 * @out: 输出映射后的数据块指针（调用方用后须 memunmap）。
 * @return: 0=成功；负数=未找到/无效。 */
int vd_acquire_config_blob(vdf_map_phys_fn map, vdf_unmap_phys_fn unmap,
                           void **out);

/* 获取固件配置数据块：解析调用方传入的固件表原始字节（GPL 层经系统接口
 * 获取，不接触协议布局），校验后经 map 回调映射 blob 并校验头标识。
 * @tbl: 固件表原始字节；@tbl_len: 表长度。
 * @out: 输出映射后的数据块指针（调用方用后须 memunmap）。
 * @return: 0=成功；负数=未找到/无效。 */
int vd_acquire_config_blob_acpi(const void *tbl, int tbl_len,
                                vdf_map_phys_fn map, vdf_unmap_phys_fn unmap,
                                void **out);

struct vdf_bat_diag {
    int  total_entries;
    int  state_counts[8];
    vd_u64 last_entries[8];
    int  last_start;
    vd_u32 bat_len;
    vd_u32 log_length;

    vd_u32 vhd_features;
    vd_u64 vhd_original_size;
    vd_u64 vhd_current_size;
    vd_u32 vhd_disk_type;
};

void vdf_get_diag(void *state, struct vdf_bat_diag *diag);

#ifdef __cplusplus
}
#endif

#endif /* VD_ABI_H */
