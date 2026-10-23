/**
 * mwcfg_v2.h - Multiware 固件-OS 通讯协议 v2（Linux 内核端适配头）
 *
 * 与固件端 `Library/MwBootLib/MwCfg.h` 逐字段对齐（Linux 类型 u8/u16/u32/u64
 * 对应 UEFI 的 UINT8/UINT16/UINT32/UINT64）。内核端据此解析固件在启动前写入
 * 的 v2 blob（v2.4：ACPI 表 MWCF 主通道 + 0x7E00 x86 兼容通道；
 * v2.5：条目载荷 ExtraData 承载扩展记录 TLV；v2.6：MWCF 表体升级为 v2 ——
 * 契约自包含。后两项见 COMMUNICATION_V2_SPEC.md §6.6 / §6.7）。
 *
 * ⚠️ 本头是跨端布局**唯一权威**（规范 §8.1）。固件端 `Library/MwBootLib/MwCfg.h`
 * 与各端副本均以本头为准。
 *
 * 参考：design/DESIGN.md §10、coding/v2_config_blob.md
 * 版权所有 © 2026 LU WEIFENG (wave.lu@gmail.com). All rights reserved.
 */

#ifndef _MWCFG_V2_H_
#define _MWCFG_V2_H_

/*
 * 本头为纯 C 数据结构头：不依赖任何 Linux 内核头文件，
 * 使用标准 C 定长类型（unsigned char/...），固件-OS 通讯协议各端共同引用。
 */

/* 定长类型（标准 C，避免依赖内核头） */
typedef unsigned char      mw_u8;
typedef unsigned short     mw_u16;
typedef unsigned int       mw_u32;
typedef unsigned long long mw_u64;

#define MW_CFG_BLOB_MAGIC     0x4D574346U   /* 'MWCF' */
#define MW_CFG_VERSION_V2     0x00020001U   /* v2.1（无加密修订版） */
#define MW_CFG_MAX_ENTRIES    16
#define MW_CFG_MAX_BLOB_SIZE  65536         /* 64 KB */

/* 固定物理地址（x86 兼容通道；v2.4 起全架构主通道为 ACPI 表 MWCF） */
#define MW_CFG_PTR_BASE       0x7DFCU       /* UINT32 Magic ('MWCF') */
#define MW_CFG_PTR_BLOB       0x7E00U       /* UINT64 BlobPhysAddr */

/* MWCF ACPI 表（v2.4 起全架构统一主通道；v2.6 表体升级为“契约自包含”） */
#define MW_ACPI_TABLE_SIGNATURE      "MWCF"
#define MW_ACPI_CHANNEL_VERSION_V1   1
/* 最小合法表长 = v1 表（表体只放外部 blob 指针）
 * v2 表为 MW_ACPI_CONFIG_TABLE_SIZE + PayloadSize */
#define MW_ACPI_TABLE_MIN_SIZE       56
#define MW_ACPI_TABLE_BODY_OFFSET    36   /* ACPI 标准表头之后即为表体 */

/* EFI 配置表通道用的 GUID（读取方按 16 原始字节比对；不进根表的发布路径用它） */
#define MW_CFG_ACPI_TABLE_GUID \
  { 0x9D4E2A17, 0x8F3B, 0x4C6E, { 0x9A, 0x71, 0x2B, 0x5D, 0xC4, 0x88, 0x10, 0x33 } }

/* 表体版本 BodyVersion：区分“表体只放外部 blob 指针”与“表体自包含契约”。
 * ⚠️ 读取方 MUST NOT 依据 ChannelVersion 决定布局 —— 布局只认 BodyVersion；
 * 未知 BodyVersion（> 2）MUST 退化为 v1 读法（或拒绝），不得当作 v2 解析。 */
#define MW_ACPI_BODY_VERSION_V1     1   /* 旧：Header + Magic/ChannelVersion/BlobPhysAddr/BlobSize */
#define MW_ACPI_BODY_VERSION_V2     2   /* 新：追加 BodyVersion/PayloadOffset/PayloadSize + inline 负载 */

/* 表体尺寸（跨端布局常量，禁止依赖 sizeof 计算偏移） */
#define MW_ACPI_CONFIG_TABLE_V1_SIZE  56   /* 36 + 20：v1 表（Header.Length == 56） */
#define MW_ACPI_CONFIG_TABLE_SIZE     68   /* 36 + 32：v2 表头尺寸，也是 inline 负载起始偏移 */

/* 表体 v2 三个追加字段的**绝对偏移**（相对表首）—— 跨端契约的一部分：
 *   56 = 36(ACPI 头) + 20(v1 表体)   ⇒ BodyVersion
 *   60 / 64                          ⇒ PayloadOffset / PayloadSize */
#define MW_ACPI_BODY_VERSION_OFFSET   56
#define MW_ACPI_PAYLOAD_OFFSET_OFFSET 60
#define MW_ACPI_PAYLOAD_SIZE_OFFSET   64

/* 条目校验算法 */
#define MW_CFG_INTEGRITY_CRC32  1
#define MW_CFG_INTEGRITY_HMAC   2

/* 全局标志位（MW_CFG_BLOB.Flags） */
#define BLOB_FLAG_MULTI_ENTRY   (1U << 0)   /* EntryCount > 1 */
#define BLOB_FLAG_SECURE_BOOT   (1U << 1)   /* 创建时 Secure Boot 已启用 */
#define BLOB_FLAG_HMAC_USED     (1U << 2)   /* 至少一条目使用 HMAC-SHA256 */
#define BLOB_FLAG_V1_COMPAT     (1U << 3)   /* 同时写入了 v1 EFI Variable */

/* 条目标志位（MW_CFG_ENTRY.Flags） */
#define ENTRY_FLAG_ENABLED        (1U << 0) /* 条目有效 */
#define ENTRY_FLAG_SYSTEM_DISK    (1U << 1) /* 系统盘（驱动优先处理） */
#define ENTRY_FLAG_READONLY       (1U << 2) /* 只读挂载 */
#define ENTRY_FLAG_HWCONFIG_VALID (1U << 3) /* HwConfig 字段有效 */
#define ENTRY_FLAG_BOOT_ENTRY     (1U << 4) /* 本次实际启动的条目（主系统盘） */
#define ENTRY_FLAG_ATTACH_DISK    (1U << 5) /* 附加磁盘（数据盘）：不参与启动，启动后挂载 */

/* 扩展记录（ExtraData TLV，v2.5；权威规范 COMMUNICATION_V2_SPEC.md §6.7）
 * 条目载荷 ExtraData 区内可承载若干“类型化扩展记录”，用于在不改动定长布局、
 * 不递增 MW_CFG_VERSION_V2 的前提下追加可选信息：ExtraDataSize == 0 表示无记录；
 * 记录顺序不保证；未知 Type 必须跳过（前向兼容）。 */
#define MW_CFG_EXT_TYPE_HIBERNATE_PARTITION  0x0001U /* 休眠（S4）存储分区 */

/* 扩展记录尺寸显式常量（跨端禁止依赖 sizeof 计算偏移） */
#define MW_CFG_EXT_RECORD_HDR_SIZE           4       /* 记录头字节数 */
#define MW_CFG_EXT_HIBERNATE_PARTITION_SIZE  40      /* 休眠分区记录体字节数 */

/* 休眠分区记录 Usage 取值 */
#define MW_CFG_EXT_HIB_USAGE_UNSPECIFIED     0       /* 未指定 */
#define MW_CFG_EXT_HIB_USAGE_LINUX_SWAP      1       /* Linux swap（云固件统一 swap 分区） */
#define MW_CFG_EXT_HIB_USAGE_OTHER           2       /* 其他 */

#pragma pack(push, 1)

/* 配置块头部（56 字节），与固件端 MW_CFG_BLOB 对齐 */
typedef struct _MW_CFG_BLOB {
  mw_u32 Magic;              /* 'MWCF' */
  mw_u32 Version;            /* MW_CFG_VERSION_V2 */
  mw_u32 TotalSize;          /* blob 总字节数（头部 + 全部条目） */
  mw_u16 EntryCount;         /* 条目数量 (1..16) */
  mw_u16 Flags;              /* 全局标志位 */
  mw_u64 Timestamp;          /* 压缩 EFI_TIME */
  mw_u32 BlobChecksum;       /* 头部 CRC-32 */
  mw_u8  Reserved[28];       /* 预留，填充到 56 字节 */
} MW_CFG_BLOB;

/* 单条配置条目（头部固定 42 字节），与固件端 MW_CFG_ENTRY 对齐 */
typedef struct _MW_CFG_ENTRY {
  mw_u16 EntrySize;          /* 本条总字节数（42 + PayloadSize） */
  mw_u16 Flags;              /* 条目标志位 */
  mw_u16 PayloadSize;        /* 明文载荷字节数 */
  mw_u8  IntegrityAlgo;      /* 1=CRC-32(默认), 2=HMAC-SHA256 */
  mw_u8  Reserved[3];
  mw_u8  Integrity[32];      /* 校验值：覆盖本条目除本字段外的全部字节 */
  mw_u8  Payload[0];         /* 明文载荷 MW_CFG_PAYLOAD */
} MW_CFG_ENTRY;

/* 明文载荷，与固件端 MW_CFG_PAYLOAD 对齐 */
typedef struct _MW_CFG_PAYLOAD {
  char PartGuid[37];      /* GPT 分区 GUID（36 字符小写 ASCII + NULL）；MBR "xxxx-xx" */
  char FsType[16];        /* 支持范围 "fat"|"exfat"|"ntfs"|"ext4"；另有 "unknown"|"wholedisk"|"iso9660" 卷标识 */
  char VdiskPath[260];    /* 镜像文件在分区内的路径（"\vhdx\Win11.vhdx"） */
  char VdType[16];        /* "vhdx"|"vhd"|"iso"|"raw" */
  char OsType[16];        /* "windows"|"linux"|"macos"|"bsd"|"android"|"chrome"|"uefi"|"other" */
  char HwConfig[256];     /* 扩展硬件配置 JSON（可选） */
  mw_u32 ExtraDataSize;   /* 附加数据字节数（0 = 无） */
  mw_u8  ExtraData[0];    /* 附加数据（可选） */
} MW_CFG_PAYLOAD;

/* 扩展记录头（4 字节）：Size 含本头，4 字节对齐，>= MW_CFG_EXT_RECORD_HDR_SIZE */
typedef struct _MW_CFG_EXT_RECORD {
  mw_u16 Type;            /* MW_CFG_EXT_TYPE_*（见上方类型注册） */
  mw_u16 Size;            /* 本条记录总字节数（含本头） */
  /* mw_u8 Body[Size - MW_CFG_EXT_RECORD_HDR_SIZE]; */
} MW_CFG_EXT_RECORD;

/* Type = MW_CFG_EXT_TYPE_HIBERNATE_PARTITION 的记录体（40 字节） */
typedef struct _MW_CFG_EXT_HIBERNATE_PARTITION {
  char  PartGuid[37];     /* 与 MW_CFG_PAYLOAD.PartGuid 同口径：36 字符小写 ASCII + NULL */
  mw_u8 Usage;            /* MW_CFG_EXT_HIB_USAGE_* */
  mw_u8 Reserved[2];      /* 必须置 0（37+1+2 = 40：记录体 4 字节对齐） */
} MW_CFG_EXT_HIBERNATE_PARTITION;

/* ACPI 标准描述表头（36 字节，与 ACPI 规范一致） */
typedef struct _MW_ACPI_TABLE_HEADER {
  char   Signature[4];     /* "MWCF" */
  mw_u32 Length;           /* 整表长度：v1 = 56；v2 = MW_ACPI_CONFIG_TABLE_SIZE + PayloadSize */
  mw_u8  Revision;         /* 1 */
  mw_u8  Checksum;         /* 整表校验和：全字节和 mod 256 == 0（认证覆盖整表） */
  char   OemId[6];         /* "MWVD  " */
  char   OemTableId[8];    /* "MWCFCHNL" */
  mw_u32 OemRevision;      /* 0 */
  char   CreatorId[4];     /* "MWVD" */
  mw_u32 CreatorRevision;  /* 0 */
} MW_ACPI_TABLE_HEADER;     /* 36 字节 */

/* MWCF ACPI 配置表（v1 = 56 字节；v2 = 68 字节 + 紧跟其后的 inline 负载）。
 * blob 完整性已由 MW_CFG_BLOB 头 CRC + 逐条 CRC/HMAC 覆盖，表内不冗余存 blob CRC。
 *
 * ★ v2.6（契约自包含）：在旧 20 字节表体**后面追加** 3 个字段，并把 v2 blob 原始
 *   字节直接内联进表（`PayloadOffset` 处）。追加式扩展保证旧读取方（只读前 20 字节）
 *   不受影响；`BlobPhysAddr` 在 v2 表里指向**本表内的内联负载**，旧读取方按指针
 *   映射仍然有效，但读取方 MUST NOT 依赖它与 PayloadOffset 地址相等。
 *   读取顺序：inline 优先；PayloadSize == 0（或 Length == 56）才回落 BlobPhysAddr。 */
typedef struct _MW_ACPI_CONFIG_TABLE {
  MW_ACPI_TABLE_HEADER Header;      /* 36 */
  mw_u32 Magic;                      /* 'MWCF' (0x4D574346) */
  mw_u32 ChannelVersion;             /* MW_ACPI_CHANNEL_VERSION_V1 (1) */
  mw_u64 BlobPhysAddr;               /* v1：外部 blob 物理地址；v2：本表内 inline 负载物理地址 */
  mw_u32 BlobSize;                   /* blob 字节数（<= MW_CFG_MAX_BLOB_SIZE） */
  /* ── v2 追加（旧读取方不读这三个字段）── */
  mw_u32 BodyVersion;                /* MW_ACPI_BODY_VERSION_V2 (2) */
  mw_u32 PayloadOffset;              /* 相对表首的 inline 负载偏移（= MW_ACPI_CONFIG_TABLE_SIZE） */
  mw_u32 PayloadSize;                /* inline 负载字节数（= BlobSize；0 = 无内联负载） */
} MW_ACPI_CONFIG_TABLE;              /* 36 + 32 = 68 字节（+ 紧跟其后的 Payload[]） */

/* 尺寸与偏移护栏（各端引用本头即生效；用 typedef 形式以兼容 C89/C++） */
typedef char MW_ACPI_TABLE_HEADER_SIZE_ASSERT[
  (sizeof(MW_ACPI_TABLE_HEADER) == MW_ACPI_TABLE_BODY_OFFSET) ? 1 : -1];
typedef char MW_ACPI_CONFIG_TABLE_SIZE_ASSERT[
  (sizeof(MW_ACPI_CONFIG_TABLE) == MW_ACPI_CONFIG_TABLE_SIZE) ? 1 : -1];
#if defined(__clang__) || defined(__GNUC__)
typedef char MW_ACPI_BODY_VERSION_OFFSET_ASSERT[
  (__builtin_offsetof(MW_ACPI_CONFIG_TABLE, BodyVersion) == MW_ACPI_BODY_VERSION_OFFSET) ? 1 : -1];
typedef char MW_ACPI_PAYLOAD_OFFSET_OFFSET_ASSERT[
  (__builtin_offsetof(MW_ACPI_CONFIG_TABLE, PayloadOffset) == MW_ACPI_PAYLOAD_OFFSET_OFFSET) ? 1 : -1];
typedef char MW_ACPI_PAYLOAD_SIZE_OFFSET_ASSERT[
  (__builtin_offsetof(MW_ACPI_CONFIG_TABLE, PayloadSize) == MW_ACPI_PAYLOAD_SIZE_OFFSET) ? 1 : -1];
#endif /* __clang__ || __GNUC__ */

#pragma pack(pop)

#endif /* _MWCFG_V2_H_ */
