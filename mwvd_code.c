// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * mwvd_code.c — Linux 端「固件内嵌闭源机器码」实现
 *
 * 规范：COMMUNICATION_V2_SPEC v2.8 §6.8 / §6.9（mw-docs）
 *   读取顺序：ACPI `MWVD` 表 → 表边界 → 表体自洽 → 载荷头部自洽 →
 *             CRC-32 → 验签 → 架构 / ABI 判据 → 加载（§6.9.7）。
 *
 * 运行时加载可执行内存的说明（重要）：
 *   内核未导出 `module_alloc` / `set_memory_x` / `flush_icache_range`，
 *   纯内核模块没有公开 API 分配“可执行”页。本实现通过 kprobe 解析
 *   `set_memory_x` 地址，对 `vmalloc` 区域清除 NX 位；这是本端**唯一**
 *   非公开接口依赖（CONFIG_KPROBES 缺失时直接回退链接期实现）。
 *   若不接受该依赖，可把载荷改为 DKMS 构建期提取 + 链接期使用，
 *   但那样会重新触发 objtool 的 retpoline/rethunk 检查（见设计讨论）。
 *
 * 失败语义（2.8.0 起 = 纯固件载荷模式）：
 *          本模块不再链接内置闭源层 `vd.o`，固件 `MWVD` 表载荷是**唯一**
 *          的格式后端来源。任何一步校验/加载失败都使模块加载失败
 *          （返回错误码，`modprobe`/initramfs 阶段即暴露），并打印失败
 *          步骤——不做静默降级，避免出现「驱动在、盘不在」的隐性问题。
 *
 * Copyright (c) 2026 LU WEIFENG (wave.lu@gmail.com)
 * 版权所有 (c) 2026 陆伟峰
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/acpi.h>
#include <linux/crc32.h>
#include <linux/elf.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
#include <linux/execmem.h>
#endif

#include <crypto/akcipher.h>
#include <crypto/hash.h>
#include <linux/scatterlist.h>

#if defined(CONFIG_KPROBES)
#include <linux/kprobes.h>
#endif

#include "vd_abi.h"
#include "mwvd_code.h"
#include "mwvd_code_pubkey.h"

/* ------------------------------------------------------------------ */
/* 规范常量（§6.9.1 / §6.9.4）                                          */
/* ------------------------------------------------------------------ */

#define MWVD_ACPI_CODE_SIG          "MWVD"
#define MWVD_ACPI_CODE_BODY_MAGIC   0x4D575644U
#define MWVD_ACPI_CODE_CHANNEL_V1   1U
#define MWVD_ACPI_CODE_TABLE_SIZE   68U
#define MWVD_ACPI_CODE_BODY_VER     2U
#define MWVD_CODE_MAX_BLOB_SIZE     (1024U * 1024U)
#define MWVD_CODE_HEADER_SIZE       72U
#define MWVD_CODE_MACHINE_ELF_REL   1U
#define MWVD_CODE_SIG_ALG_RSA2048   1U
#define MWVD_CODE_SIG_SIZE          256U
#define MWVD_CODE_FORMAT_V1         1U
#define MWVD_CODE_KEY_ID            0U

/* 表体绝对偏移（相对表首，§6.9.2） */
#define MWVD_TBL_MAGIC_OFF          36U
#define MWVD_TBL_CHANNEL_OFF        40U
#define MWVD_TBL_PHYS_OFF           44U
#define MWVD_TBL_SIZE_OFF           52U
#define MWVD_TBL_BODY_VER_OFF       56U
#define MWVD_TBL_PAYLOAD_OFF_OFF    60U
#define MWVD_TBL_PAYLOAD_SIZE_OFF   64U

/* 载荷头偏移（§6.9.4） */
#define MWVD_HDR_FORMAT_OFF          4U
#define MWVD_HDR_HDRSIZE_OFF         6U
#define MWVD_HDR_ARCH_OFF            8U
#define MWVD_HDR_ABI_MAJOR_OFF      24U
#define MWVD_HDR_ABI_MINOR_OFF      28U
#define MWVD_HDR_MACHINE_OFF        32U
#define MWVD_HDR_FLAGS_OFF          36U
#define MWVD_HDR_CODE_OFF_OFF       40U
#define MWVD_HDR_CODE_SIZE_OFF      44U
#define MWVD_HDR_CRC_OFF            48U
#define MWVD_HDR_SIG_ALG_OFF        52U
#define MWVD_HDR_SIG_OFF_OFF        56U
#define MWVD_HDR_SIG_SIZE_OFF       60U
#define MWVD_HDR_BUILD_OFF          64U
#define MWVD_HDR_KEY_ID_OFF         68U

#if defined(__x86_64__)
#define MWVD_CODE_ARCH_LITERAL      "x64"
#define MWVD_ELF_MACHINE            62U   /* EM_X86_64 */
#define MWVD_ELF_CLASS              ELFCLASS64
#define MWVD_ELF_LOAD_SUPPORTED     1
#define MWVD_ELF_KIND_X86_64        1
#elif defined(__i386__)
#define MWVD_CODE_ARCH_LITERAL      "x86"
#define MWVD_ELF_MACHINE            3U    /* EM_386 */
#define MWVD_ELF_CLASS              ELFCLASS32
#define MWVD_ELF_LOAD_SUPPORTED     1
#define MWVD_ELF_KIND_I386          1
#elif defined(__aarch64__)
#define MWVD_CODE_ARCH_LITERAL      "arm64"
#define MWVD_ELF_MACHINE            183U  /* EM_AARCH64 */
#define MWVD_ELF_CLASS              ELFCLASS64
#define MWVD_ELF_LOAD_SUPPORTED     1
#define MWVD_ELF_KIND_ARM64         1
#else
#define MWVD_CODE_ARCH_LITERAL      NULL
#define MWVD_ELF_LOAD_SUPPORTED     0
#endif

/*
 * AArch64 ELF 重定位常量：内核 <linux/elf.h> 不保证提供（不同版本差异），
 * 按 ABI 取值定义；已定义时以头文件为准。
 */
#if defined(MWVD_ELF_KIND_ARM64)
#define MWVD_ARM64_TRAMP_SIZE       16
#ifndef R_AARCH64_ABS64
#define R_AARCH64_ABS64                 257
#endif
#ifndef R_AARCH64_PREL32
#define R_AARCH64_PREL32                261
#endif
#ifndef R_AARCH64_ADR_PREL_PG_HI21
#define R_AARCH64_ADR_PREL_PG_HI21      275
#endif
#ifndef R_AARCH64_ADD_ABS_LO12_NC
#define R_AARCH64_ADD_ABS_LO12_NC       277
#endif
#ifndef R_AARCH64_LDST8_ABS_LO12_NC
#define R_AARCH64_LDST8_ABS_LO12_NC     278
#endif
#ifndef R_AARCH64_TSTBR14
#define R_AARCH64_TSTBR14               279
#endif
#ifndef R_AARCH64_CONDBR19
#define R_AARCH64_CONDBR19              280
#endif
#ifndef R_AARCH64_JUMP26
#define R_AARCH64_JUMP26                282
#endif
#ifndef R_AARCH64_CALL26
#define R_AARCH64_CALL26                283
#endif
#ifndef R_AARCH64_LDST16_ABS_LO12_NC
#define R_AARCH64_LDST16_ABS_LO12_NC    284
#endif
#ifndef R_AARCH64_LDST32_ABS_LO12_NC
#define R_AARCH64_LDST32_ABS_LO12_NC    285
#endif
#ifndef R_AARCH64_LDST64_ABS_LO12_NC
#define R_AARCH64_LDST64_ABS_LO12_NC    286
#endif
#ifndef R_AARCH64_LDST128_ABS_LO12_NC
#define R_AARCH64_LDST128_ABS_LO12_NC   299
#endif
#endif /* MWVD_ELF_KIND_ARM64 */

#if defined(MWVD_ELF_KIND_I386)
#ifndef R_386_32
#define R_386_32                        1
#endif
#ifndef R_386_PC32
#define R_386_PC32                      2
#endif
#endif /* MWVD_ELF_KIND_I386 */

#define MWVD_CODE_MAX_IMAGE_SIZE     (4U * 1024U * 1024U)
#define MWVD_CODE_MAX_SYM_PUTS       8

/* ------------------------------------------------------------------ */
/* 当前生效的闭源层函数表                                                */
/* ------------------------------------------------------------------ */
/*
 * 2.8.0 起只有固件 `MWVD` 载荷一个来源（SPEC §6.9），不再链接内置 `vd.o`，
 * 因而没有静态初始值：由 `mwvd_code_init()` 在模块初始化第一步填好，
 * `main.c` 在它返回成功之后才会发起任何 vd 操作。
 */
const struct mwvd_vdf_api *g_mwvd_vdf;

/* ------------------------------------------------------------------ */
/* 小端读取（x86_64 / i386 / arm64 均为小端）                            */
/* ------------------------------------------------------------------ */

static u16 mwvd_rd16(const u8 *p)
{
  u16 v;
  memcpy(&v, p, sizeof(v));
  return v;
}

static u32 mwvd_rd32(const u8 *p)
{
  u32 v;
  memcpy(&v, p, sizeof(v));
  return v;
}

/* ------------------------------------------------------------------ */
/* 可执行内存：kprobe 解析 module_alloc（本端唯一非公开接口依赖）         */
/* ------------------------------------------------------------------ */
/*
 * 不能用 vmalloc：`vd.b` 以 `-mcmodel=kernel` 编译，代码/数据中的 32 位
 * 符号扩展寻址只覆盖内核映射高 2GB（0xffffffff80000000 起）。vmalloc
 * 区域（0xffffc900...）符号扩展后地址不同，执行会立即错乱。
 * module_alloc（5.10~6.9）/ execmem_alloc（6.10+）返回模块区域，且页
 * 属性为可执行，正好满足要求。
 *
 * 内核未导出 module_alloc / execmem_alloc；用 kprobe 解析地址是纯模块
 * 没有公开 API 时的唯一办法（CONFIG_KPROBES 缺失时回退链接期实现）。
 */
/*
 * 1 = 本次镜像用 execmem_alloc_rw() 分配（可写、不可执行），写完要补执行权限。
 * 必须定义在条件编译之外：mwvd_code_make_exec()/mwvd_code_free_exec() 在
 * 6.10 以下内核上也引用该名字（5.15 实测 undeclared），低版本恒为 0。
 */
static int mwvd_exec_used_rw;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
typedef void *(*mwvd_exec_alloc_fn)(enum execmem_type type, size_t size);
typedef void (*mwvd_exec_free_fn)(void *addr);
typedef int (*mwvd_exec_restore_rox_fn)(void *ptr, size_t size);
static mwvd_exec_alloc_fn mwvd_exec_alloc;
static mwvd_exec_alloc_fn mwvd_exec_alloc_rw;
static mwvd_exec_free_fn mwvd_exec_free;
static mwvd_exec_restore_rox_fn mwvd_exec_restore_rox;
#else
typedef void *(*mwvd_module_alloc_fn)(unsigned long size);
typedef void (*mwvd_module_free_fn)(void *addr);
static mwvd_module_alloc_fn mwvd_module_alloc;
static mwvd_module_free_fn mwvd_module_free;
#endif

typedef int (*mwvd_set_memory_x_fn)(unsigned long addr, int numpages);
static mwvd_set_memory_x_fn mwvd_set_memory_x;

static void *mwvd_code_lookup_symbol(const char *name)
{
#if defined(CONFIG_KPROBES)
  struct kprobe kp = { .symbol_name = name };
  void *addr;

  if (register_kprobe(&kp) < 0)
    return NULL;
  addr = (void *)kp.addr;
  unregister_kprobe(&kp);
  return addr;
#else
  return NULL;
#endif
}

static int mwvd_code_lookup_exec_api(void)
{
  if (!mwvd_set_memory_x)
    mwvd_set_memory_x = (mwvd_set_memory_x_fn)mwvd_code_lookup_symbol("set_memory_x");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
  if (!mwvd_exec_alloc)
    mwvd_exec_alloc = (mwvd_exec_alloc_fn)mwvd_code_lookup_symbol("execmem_alloc");
  if (!mwvd_exec_alloc_rw)
    mwvd_exec_alloc_rw = (mwvd_exec_alloc_fn)mwvd_code_lookup_symbol("execmem_alloc_rw");
  if (!mwvd_exec_restore_rox)
    mwvd_exec_restore_rox =
      (mwvd_exec_restore_rox_fn)mwvd_code_lookup_symbol("execmem_restore_rox");
  if (!mwvd_exec_free)
    mwvd_exec_free = (mwvd_exec_free_fn)mwvd_code_lookup_symbol("execmem_free");

  /*
   * 新内核（实测 7.2.3）把 execmem 的代码区默认映射成只读可执行（W^X）：
   * 对 execmem_alloc() 返回的地址直接 memset/memcpy 会 #PF。
   * 有 execmem_alloc_rw() 时可以拿可写内存；否则保留旧路径（当时可写）。
   */
  if ((!mwvd_exec_alloc && !mwvd_exec_alloc_rw) || !mwvd_exec_free ||
      !mwvd_set_memory_x)
    return -EOPNOTSUPP;
#else
  if (!mwvd_module_alloc)
    mwvd_module_alloc = (mwvd_module_alloc_fn)mwvd_code_lookup_symbol("module_alloc");
  if (!mwvd_module_free)
    mwvd_module_free = (mwvd_module_free_fn)mwvd_code_lookup_symbol("module_memfree");

  if (!mwvd_module_alloc || !mwvd_module_free || !mwvd_set_memory_x)
    return -EOPNOTSUPP;
#endif
  return 0;
}

static int mwvd_code_make_exec(void *addr, size_t size)
{
  if (!mwvd_set_memory_x)
    return -EOPNOTSUPP;
  /* 用 execmem_alloc_rw() 分配的内存默认不带执行权限，这里统一补上；
   * 不用 execmem_restore_rox()：载荷的数据段与代码段同块，收成只读会
   * 让运行期写数据失败。 */
  mwvd_exec_used_rw = 0;
  return mwvd_set_memory_x((unsigned long)addr,
                           (int)(PAGE_ALIGN(size) >> PAGE_SHIFT));
}

static void *mwvd_code_alloc_exec(size_t size)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
  /*
   * 优先分配“可写”的代码内存：6.16+/7.x 上 execmem_alloc(EXECMEM_MODULE_TEXT)
   * 返回只读可执行页，直接写会触发权限 #PF（Arch 7.2.3 实测在 memset_orig 崩）。
   * 载荷的 .data/.bss 与 .text 在同一块内存里、运行期可能写数据，因此写完只补
   * 执行权限（见 mwvd_code_make_exec），不收成只读。
   */
  if (mwvd_exec_alloc_rw) {
    void *p = mwvd_exec_alloc_rw(EXECMEM_MODULE_TEXT, size);

    if (p) {
      mwvd_exec_used_rw = 1;
      return p;
    }
  }
  if (!mwvd_exec_alloc)
    return NULL;
  return mwvd_exec_alloc(EXECMEM_MODULE_TEXT, size);
#else
  if (!mwvd_module_alloc)
    return NULL;
  return mwvd_module_alloc(size);
#endif
}

static void mwvd_code_free_exec(void *addr)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
  mwvd_exec_used_rw = 0;
  if (addr && mwvd_exec_free)
    mwvd_exec_free(addr);
#else
  if (addr && mwvd_module_free)
    mwvd_module_free(addr);
#endif
}

/* ------------------------------------------------------------------ */
/* ACPI `MWVD` 表读取                                                   */
/* ------------------------------------------------------------------ */

#if IS_ENABLED(CONFIG_ACPI)
static int mwvd_code_read_table(u8 **out, u32 *out_len)
{
  struct acpi_table_header *tbl = NULL;
  acpi_status st;
  u32 magic, channel, blob_size, body_ver, poff, psize;
  int ret = -ENOENT;

  *out = NULL;
  *out_len = 0;

  st = acpi_get_table(MWVD_ACPI_CODE_SIG, 0, &tbl);
  if (ACPI_FAILURE(st) || !tbl)
    return -ENOENT;

  if (tbl->length < MWVD_ACPI_CODE_TABLE_SIZE) {
    ret = -EINVAL;
    goto out;
  }

  magic     = mwvd_rd32((const u8 *)tbl + MWVD_TBL_MAGIC_OFF);
  channel   = mwvd_rd32((const u8 *)tbl + MWVD_TBL_CHANNEL_OFF);
  blob_size = mwvd_rd32((const u8 *)tbl + MWVD_TBL_SIZE_OFF);
  body_ver  = mwvd_rd32((const u8 *)tbl + MWVD_TBL_BODY_VER_OFF);
  poff      = mwvd_rd32((const u8 *)tbl + MWVD_TBL_PAYLOAD_OFF_OFF);
  psize     = mwvd_rd32((const u8 *)tbl + MWVD_TBL_PAYLOAD_SIZE_OFF);

  if (magic != MWVD_ACPI_CODE_BODY_MAGIC ||
      channel != MWVD_ACPI_CODE_CHANNEL_V1 ||
      body_ver != MWVD_ACPI_CODE_BODY_VER ||
      poff < MWVD_ACPI_CODE_TABLE_SIZE ||
      psize == 0 || psize > MWVD_CODE_MAX_BLOB_SIZE ||
      blob_size != psize ||
      (u64)poff + psize > tbl->length) {
    ret = -EINVAL;
    goto out;
  }

  *out = kmemdup((const u8 *)tbl + poff, psize, GFP_KERNEL);
  if (!*out) {
    ret = -ENOMEM;
    goto out;
  }
  *out_len = psize;
  ret = 0;

out:
  acpi_put_table(tbl);
  return ret;
}
#else
static int mwvd_code_read_table(u8 **out, u32 *out_len)
{
  *out = NULL;
  *out_len = 0;
  return -EOPNOTSUPP;
}
#endif

/* ------------------------------------------------------------------ */
/* 验签：RSA-2048 / SHA-256 / PKCS#1 v1.5                               */
/* ------------------------------------------------------------------ */

#if IS_ENABLED(CONFIG_CRYPTO)
static const u8 mwvd_sha256_digestinfo_prefix[19] = {
  0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48,
  0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
};

static int mwvd_code_verify_sig(const u8 *data, u32 data_len,
                                const u8 *sig, u32 sig_len)
{
  struct crypto_shash *hash;
  struct crypto_akcipher *ak;
  struct akcipher_request *req = NULL;
  struct scatterlist src, dst;
  u8 digest[32];
  u8 *out = NULL;
  u32 i;
  int ret;

  if (sig_len != MWVD_CODE_SIG_SIZE)
    return -EINVAL;

  hash = crypto_alloc_shash("sha256", 0, 0);
  if (IS_ERR(hash))
    return PTR_ERR(hash);

  {
    SHASH_DESC_ON_STACK(desc, hash);
    desc->tfm = hash;
    ret = crypto_shash_digest(desc, data, data_len, digest);
  }
  crypto_free_shash(hash);
  if (ret)
    return ret;

  ak = crypto_alloc_akcipher("rsa", 0, 0);
  if (IS_ERR(ak))
    return PTR_ERR(ak);

  ret = crypto_akcipher_set_pub_key(ak, mwvd_code_pubkey_der,
                                    MWVD_CODE_PUBKEY_DER_LEN);
  if (ret)
    goto out_ak;

  req = akcipher_request_alloc(ak, GFP_KERNEL);
  if (!req) {
    ret = -ENOMEM;
    goto out_ak;
  }

  out = kmalloc(MWVD_CODE_SIG_SIZE, GFP_KERNEL);
  if (!out) {
    ret = -ENOMEM;
    goto out_req;
  }

  sg_init_one(&src, sig, sig_len);
  sg_init_one(&dst, out, MWVD_CODE_SIG_SIZE);
  akcipher_request_set_crypt(req, &src, &dst, sig_len,
                             MWVD_CODE_SIG_SIZE);

  ret = crypto_akcipher_encrypt(req);
  if (ret)
    goto out_req;

  if (req->dst_len != MWVD_CODE_SIG_SIZE ||
      out[0] != 0x00 || out[1] != 0x01) {
    ret = -EKEYREJECTED;
    goto out_req;
  }

  i = 2;
  while (i < MWVD_CODE_SIG_SIZE && out[i] == 0xff)
    i++;
  if (i >= MWVD_CODE_SIG_SIZE || out[i] != 0x00) {
    ret = -EKEYREJECTED;
    goto out_req;
  }
  i++;

  if (MWVD_CODE_SIG_SIZE - i !=
          sizeof(mwvd_sha256_digestinfo_prefix) + sizeof(digest) ||
      memcmp(out + i, mwvd_sha256_digestinfo_prefix,
             sizeof(mwvd_sha256_digestinfo_prefix)) != 0 ||
      memcmp(out + i + sizeof(mwvd_sha256_digestinfo_prefix),
             digest, sizeof(digest)) != 0) {
    ret = -EKEYREJECTED;
    goto out_req;
  }

  ret = 0;

out_req:
  kfree(out);
  akcipher_request_free(req);
out_ak:
  crypto_free_akcipher(ak);
  return ret;
}
#else
static int mwvd_code_verify_sig(const u8 *data, u32 data_len,
                                const u8 *sig, u32 sig_len)
{
  return -EOPNOTSUPP;
}
#endif

/* ------------------------------------------------------------------ */
/* 载荷校验（§6.9.7 第 1–6 步）                                          */
/* ------------------------------------------------------------------ */

struct mwvd_code_meta {
  char arch[16];
  u32 abi_major;
  u32 abi_minor;
  u32 code_build;
  u32 code_offset;
  u32 code_size;
};

static int mwvd_code_validate(const u8 *blob, u32 len,
                              struct mwvd_code_meta *meta)
{
  u16 format, hdr_size;
  u32 machine, flags, code_off, code_size, crc, sig_alg;
  u32 sig_off, sig_size, key_id, abi_major, abi_minor, build;
  u32 calc_crc;
  int ret;

  if (len < MWVD_CODE_HEADER_SIZE)
    return -EINVAL;

  if (memcmp(blob, "MWC1", 4) != 0)
    return -EINVAL;

  format = mwvd_rd16(blob + MWVD_HDR_FORMAT_OFF);
  hdr_size = mwvd_rd16(blob + MWVD_HDR_HDRSIZE_OFF);
  if (format != MWVD_CODE_FORMAT_V1 || hdr_size < MWVD_CODE_HEADER_SIZE)
    return -EINVAL;

  memcpy(meta->arch, blob + MWVD_HDR_ARCH_OFF, sizeof(meta->arch));
  if (meta->arch[sizeof(meta->arch) - 1] != '\0')
    return -EINVAL;
  if (!MWVD_CODE_ARCH_LITERAL || strcmp(meta->arch, MWVD_CODE_ARCH_LITERAL) != 0)
    return -EOPNOTSUPP;

  abi_major = mwvd_rd32(blob + MWVD_HDR_ABI_MAJOR_OFF);
  abi_minor = mwvd_rd32(blob + MWVD_HDR_ABI_MINOR_OFF);
  machine   = mwvd_rd32(blob + MWVD_HDR_MACHINE_OFF);
  flags     = mwvd_rd32(blob + MWVD_HDR_FLAGS_OFF);
  code_off  = mwvd_rd32(blob + MWVD_HDR_CODE_OFF_OFF);
  code_size = mwvd_rd32(blob + MWVD_HDR_CODE_SIZE_OFF);
  crc       = mwvd_rd32(blob + MWVD_HDR_CRC_OFF);
  sig_alg   = mwvd_rd32(blob + MWVD_HDR_SIG_ALG_OFF);
  sig_off   = mwvd_rd32(blob + MWVD_HDR_SIG_OFF_OFF);
  sig_size  = mwvd_rd32(blob + MWVD_HDR_SIG_SIZE_OFF);
  build     = mwvd_rd32(blob + MWVD_HDR_BUILD_OFF);
  key_id    = mwvd_rd32(blob + MWVD_HDR_KEY_ID_OFF);

  if (machine != MWVD_CODE_MACHINE_ELF_REL ||
      flags != 0 ||
      code_off != hdr_size ||
      (u64)sig_off != (u64)code_off + code_size ||
      (u64)sig_off + sig_size != len ||
      sig_alg != MWVD_CODE_SIG_ALG_RSA2048 ||
      sig_size != MWVD_CODE_SIG_SIZE ||
      key_id != MWVD_CODE_KEY_ID)
    return -EINVAL;

  if (abi_major != VD_CORE_VERSION_MAJOR)
    return -EPROTO;

  calc_crc = crc32_le(0xFFFFFFFFU, blob + code_off, code_size) ^ 0xFFFFFFFFU;
  if (calc_crc != crc)
    return -EBADMSG;

  ret = mwvd_code_verify_sig(blob, sig_off, blob + sig_off, sig_size);
  if (ret)
    return ret;

  meta->abi_major = abi_major;
  meta->abi_minor = abi_minor;
  meta->code_build = build;
  meta->code_offset = code_off;
  meta->code_size = code_size;
  return 0;
}

/* ------------------------------------------------------------------ */
/* ELF ET_REL 加载器（x86_64 / i386 / arm64）                          */
/* ------------------------------------------------------------------ */

struct mwvd_code_image {
  void *base;
  size_t size;
  struct mwvd_vdf_api api;
  char *sym_names[MWVD_CODE_MAX_SYM_PUTS];
  int sym_count;
};

#if defined(MWVD_ELF_LOAD_SUPPORTED) && MWVD_ELF_LOAD_SUPPORTED

static void mwvd_elf_cleanup(struct mwvd_code_image *img);

#if defined(MWVD_ELF_KIND_I386)
/*
 * i386 的 vd.b 会引用 GCC libgcc 的 64 位除法辅助函数（__divdi3 等）。
 * 这些函数由内核 vmlinux 链接，但通常不在导出符号表中，__symbol_get()
 * 解析不到；在 GPL 层提供同名实现，供固件载荷的未定义符号解析使用。
 */
s64 __divdi3(s64 a, s64 b);
s64 __moddi3(s64 a, s64 b);
u64 __udivdi3(u64 a, u64 b);
u64 __umoddi3(u64 a, u64 b);
s64 __divmoddi4(s64 a, s64 b, s64 *rem);

static u64 mwvd_udivmod64(u64 n, u64 d, u64 *rem)
{
  u64 q = 0, r = 0;
  int i;

  if (!d) {
    if (rem)
      *rem = 0;
    return 0;
  }

  for (i = 63; i >= 0; i--) {
    u64 carry = r >> 63;

    r = (r << 1) | ((n >> i) & 1U);
    if (carry || r >= d) {
      r -= d;
      q |= (u64)1 << i;
    }
  }

  if (rem)
    *rem = r;
  return q;
}

static u64 mwvd_abs64(s64 v)
{
  return v < 0 ? (u64)0 - (u64)v : (u64)v;
}

s64 __divdi3(s64 a, s64 b)
{
  u64 q = mwvd_udivmod64(mwvd_abs64(a), mwvd_abs64(b), NULL);

  if ((a < 0) ^ (b < 0))
    return (s64)((u64)0 - q);
  return (s64)q;
}

s64 __moddi3(s64 a, s64 b)
{
  u64 r;

  mwvd_udivmod64(mwvd_abs64(a), mwvd_abs64(b), &r);
  return a < 0 ? (s64)((u64)0 - r) : (s64)r;
}

u64 __udivdi3(u64 a, u64 b)
{
  return mwvd_udivmod64(a, b, NULL);
}

u64 __umoddi3(u64 a, u64 b)
{
  u64 r;

  mwvd_udivmod64(a, b, &r);
  return r;
}

s64 __divmoddi4(s64 a, s64 b, s64 *rem)
{
  s64 q = __divdi3(a, b);

  if (rem)
    *rem = __moddi3(a, b);
  return q;
}
#endif /* MWVD_ELF_KIND_I386 */

/*
 * 载荷用到的通用内存函数：**直接在 GPL 层提供**，不经内核符号解析。
 *
 * 历史：先试 `__symbol_get("memcpy")`，6.12 起内核只接受 GPLONLY 导出符号，
 * 会打印 "Failing symbol_get of non-GPLONLY symbol memcpy" 并返回 NULL；
 * 再退 kprobe 解析也拿不到（x86 的 memcpy/memset 在 kprobe 黑名单）。
 * 因此改为模块内实现 + 直映射：载荷里的 `memcpy`/`memset` 未定义符号
 * 直接指向这里的包装函数（`from_kernel = false`，cleanup 时不会
 * `__symbol_put`）。
 */
static void *mwvd_payload_memcpy(void *dst, const void *src, size_t n)
{
  return memcpy(dst, src, n);
}

static void *mwvd_payload_memset(void *dst, int c, size_t n)
{
  return memset(dst, c, n);
}

static void *mwvd_elf_kernel_symbol(const char *name, bool *from_kernel)
{
  *from_kernel = false;

  if (strcmp(name, "memcpy") == 0)
    return (void *)mwvd_payload_memcpy;
  if (strcmp(name, "memset") == 0)
    return (void *)mwvd_payload_memset;

#if defined(MWVD_ELF_KIND_I386)
  if (strcmp(name, "__divdi3") == 0)
    return (void *)__divdi3;
  if (strcmp(name, "__divmoddi4") == 0)
    return (void *)__divmoddi4;
  if (strcmp(name, "__moddi3") == 0)
    return (void *)__moddi3;
  if (strcmp(name, "__udivdi3") == 0)
    return (void *)__udivdi3;
  if (strcmp(name, "__umoddi3") == 0)
    return (void *)__umoddi3;
#endif

  return NULL;
}

#if defined(MWVD_ELF_KIND_X86_64) || defined(MWVD_ELF_KIND_ARM64)

static u64 mwvd_elf_symbol_value(const Elf64_Sym *sym,
                                 const u64 *seg_off,
                                 u8 *base,
                                 int shnum)
{
  if (sym->st_shndx == SHN_ABS)
    return sym->st_value;
  if (sym->st_shndx == SHN_UNDEF)
    return 0;                       /* 由调用方查缓存 */
  if (sym->st_shndx >= shnum)
    return 0;
  return (u64)(uintptr_t)(base + seg_off[sym->st_shndx] + sym->st_value);
}

static int mwvd_elf_resolve_undef(const Elf64_Sym *symtab, u32 nsyms,
                                  const u8 *strtab, u32 strtab_size,
                                  u64 *sym_val, struct mwvd_code_image *img)
{
  u32 i;
  int ret = 0;

  for (i = 1; i < nsyms; i++) {
    const Elf64_Sym *sym = &symtab[i];
    const char *name;
    void *addr;
    bool from_kernel;

    if (sym->st_shndx != SHN_UNDEF)
      continue;

    if (sym->st_name == 0 || sym->st_name >= strtab_size)
      return -ENOEXEC;

    name = (const char *)strtab + sym->st_name;
    if (memchr(name, '\0', strtab_size - sym->st_name) == NULL)
      return -ENOEXEC;
    addr = mwvd_elf_kernel_symbol(name, &from_kernel);
    if (!addr)
      return -ENOENT;

    sym_val[i] = (u64)(uintptr_t)addr;

    if (!from_kernel)
      continue;
    if (img->sym_count < MWVD_CODE_MAX_SYM_PUTS) {
      char *dup = kstrdup(name, GFP_KERNEL);
      if (!dup) {
        ret = -ENOMEM;
        break;
      }
      img->sym_names[img->sym_count++] = dup;
    } else {
      ret = -E2BIG;
      break;
    }
  }

  return ret;
}

#endif /* ELF64 通用：symbol_value / resolve_undef */

#if defined(MWVD_ELF_KIND_X86_64)

static int mwvd_elf_apply_rela_x86_64(const Elf64_Rela *rela, u32 nrela,
                                      const Elf64_Shdr *target_sh,
                                      u64 target_off, u8 *base,
                                      const u64 *sym_val, u32 nsyms)
{
  u32 j;

  for (j = 0; j < nrela; j++) {
    const Elf64_Rela *r = &rela[j];
    u32 type = ELF64_R_TYPE(r->r_info);
    u32 si = ELF64_R_SYM(r->r_info);
    u64 P = (u64)(uintptr_t)(base + target_off + r->r_offset);
    s64 A = r->r_addend;
    u64 S;
    u64 val;
    u32 width;

    if (si >= nsyms)
      return -ENOEXEC;

    switch (type) {
    case R_X86_64_64:
      width = sizeof(u64);
      break;
    case R_X86_64_32S:
    case R_X86_64_PLT32:
    case R_X86_64_PC32:
      width = sizeof(u32);
      break;
    default:
      return -ENOEXEC;
    }

    if (r->r_offset + width > target_sh->sh_size)
      return -ENOEXEC;

    S = sym_val[si];

    switch (type) {
    case R_X86_64_64:
      val = S + (u64)A;
      memcpy((void *)(uintptr_t)P, &val, sizeof(val));
      break;

    case R_X86_64_32S:
      val = S + (u64)A;
      if ((s64)val != (s64)(s32)val)
        return -EOVERFLOW;
      memcpy((void *)(uintptr_t)P, &val, sizeof(u32));
      break;

    case R_X86_64_PLT32:
    case R_X86_64_PC32:
      val = S + (u64)A - P;
      if ((s64)val != (s64)(s32)val)
        return -EOVERFLOW;
      memcpy((void *)(uintptr_t)P, &val, sizeof(u32));
      break;

    default:
      return -ENOEXEC;
    }
  }

  return 0;
}

#define MWVD_ELF_APPLY_RELA mwvd_elf_apply_rela_x86_64

#elif defined(MWVD_ELF_KIND_ARM64)

static int mwvd_elf_apply_rela_arm64(const Elf64_Rela *rela, u32 nrela,
                                     const Elf64_Shdr *target_sh,
                                     u64 target_off, u8 *base,
                                     const u64 *sym_val, u32 nsyms)
{
  u32 j;

  for (j = 0; j < nrela; j++) {
    const Elf64_Rela *r = &rela[j];
    u32 type = ELF64_R_TYPE(r->r_info);
    u32 si = ELF64_R_SYM(r->r_info);
    u64 P = (u64)(uintptr_t)(base + target_off + r->r_offset);
    s64 A = r->r_addend;
    u64 S;
    u64 val64;
    u32 val32, insn;
    s64 off;

    if (si >= nsyms || r->r_offset + sizeof(u32) > target_sh->sh_size)
      return -ENOEXEC;

    S = sym_val[si];
    memcpy(&insn, (void *)(uintptr_t)P, sizeof(insn));

    switch (type) {
    case R_AARCH64_ABS64:
      if (r->r_offset + sizeof(u64) > target_sh->sh_size)
        return -ENOEXEC;
      val64 = S + (u64)A;
      memcpy((void *)(uintptr_t)P, &val64, sizeof(val64));
      break;

    case R_AARCH64_PREL32:
      val32 = (u32)(S + (u64)A - P);
      memcpy((void *)(uintptr_t)P, &val32, sizeof(val32));
      break;

    case R_AARCH64_CALL26:
    case R_AARCH64_JUMP26:
      off = (s64)(S + (u64)A - P);
      if (off & 3)
        return -ENOEXEC;
      off >>= 2;
      if (off < -(1LL << 25) || off >= (1LL << 25))
        return -EOVERFLOW;
      insn = (insn & 0xFC000000U) | ((u32)off & 0x03FFFFFFU);
      memcpy((void *)(uintptr_t)P, &insn, sizeof(insn));
      break;

    case R_AARCH64_ADR_PREL_PG_HI21:
      off = (s64)((S + (u64)A) & ~0xFFFULL) - (s64)(P & ~0xFFFULL);
      off >>= 12;
      if (off < -(1LL << 20) || off >= (1LL << 20))
        return -EOVERFLOW;
      insn &= ~((0x7FFFFU << 5) | (0x3U << 29));
      insn |= ((u32)(off & 0x3) << 29) |
              (((u32)(off >> 2) & 0x7FFFFU) << 5);
      memcpy((void *)(uintptr_t)P, &insn, sizeof(insn));
      break;

    case R_AARCH64_ADD_ABS_LO12_NC:
    case R_AARCH64_LDST8_ABS_LO12_NC:
    case R_AARCH64_LDST16_ABS_LO12_NC:
    case R_AARCH64_LDST32_ABS_LO12_NC:
    case R_AARCH64_LDST64_ABS_LO12_NC:
    case R_AARCH64_LDST128_ABS_LO12_NC:
    {
      u32 shift = (type == R_AARCH64_LDST16_ABS_LO12_NC) ? 1 :
                  (type == R_AARCH64_LDST32_ABS_LO12_NC) ? 2 :
                  (type == R_AARCH64_LDST64_ABS_LO12_NC) ? 3 :
                  (type == R_AARCH64_LDST128_ABS_LO12_NC) ? 4 : 0;
      u32 imm = (u32)((S + (u64)A) & 0xFFFU) >> shift;

      insn = (insn & ~(0xFFFU << 10)) | ((imm & 0xFFFU) << 10);
      memcpy((void *)(uintptr_t)P, &insn, sizeof(insn));
      break;
    }

    case R_AARCH64_CONDBR19:
      off = (s64)(S + (u64)A - P);
      if (off & 3)
        return -ENOEXEC;
      off >>= 2;
      if (off < -(1LL << 18) || off >= (1LL << 18))
        return -EOVERFLOW;
      insn = (insn & ~(0x7FFFFU << 5)) | (((u32)off & 0x7FFFFU) << 5);
      memcpy((void *)(uintptr_t)P, &insn, sizeof(insn));
      break;

    case R_AARCH64_TSTBR14:
      off = (s64)(S + (u64)A - P);
      if (off & 3)
        return -ENOEXEC;
      off >>= 2;
      if (off < -(1LL << 13) || off >= (1LL << 13))
        return -EOVERFLOW;
      insn = (insn & ~(0x3FFFU << 5)) | (((u32)off & 0x3FFFU) << 5);
      memcpy((void *)(uintptr_t)P, &insn, sizeof(insn));
      break;

    default:
      return -ENOEXEC;
    }
  }

  return 0;
}

#define MWVD_ELF_APPLY_RELA mwvd_elf_apply_rela_arm64

/*
 * arm64 指令缓存与数据缓存不保证一致：写完代码后必须按 IminLine/DminLine
 * 做 dc cvau + ic ivau。上述内核接口未导出，直接按 CTR_EL0 实现。
 */
static void mwvd_elf_flush_icache(const void *start, size_t size)
{
  unsigned long ctr;
  unsigned int dmin, imin;
  unsigned long addr, end;

  asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
  dmin = 4U << ((ctr >> 16) & 0xFU);
  imin = 4U << (ctr & 0xFU);
  addr = (unsigned long)start;
  end = addr + size;

  for (; addr < end; addr += dmin)
    asm volatile("dc cvau, %0" :: "r"(addr) : "memory");
  asm volatile("dsb ish" ::: "memory");

  for (addr = (unsigned long)start; addr < end; addr += imin)
    asm volatile("ic ivau, %0" :: "r"(addr) : "memory");
  asm volatile("dsb ish" ::: "memory");
  asm volatile("isb" ::: "memory");
}

/*
 * arm64 的 CALL26 只有 ±128MB 范围，而模块区（MODULES_VADDR..MODULES_END，
 * 5.15 为 128MB）到内核 text（KIMAGE_VADDR = MODULES_END）的 memcpy/memset
 * 距离可能越界。为每个未定义符号生成 16B trampoline：
 *   adrp x16, target; add x16, x16, #:lo12:target; br x16; nop
 * trampoline 与模块代码同一区域，CALL26 必然在范围内；ADRP 覆盖 ±4GB。
 */
static int mwvd_elf_arm64_make_tramp(u32 *insn, u64 target, u64 tramp)
{
  s64 off = (s64)((target & ~0xFFFULL) - (tramp & ~0xFFFULL));

  off >>= 12;
  if (off < -(1LL << 20) || off >= (1LL << 20))
    return -EOVERFLOW;

  insn[0] = 0x90000010U | ((u32)(off & 0x3) << 29) |
            (((u32)(off >> 2) & 0x7FFFFU) << 5);   /* adrp x16, target */
  insn[1] = 0x91000210U | (((u32)target & 0xFFFU) << 10); /* add x16,#lo12 */
  insn[2] = 0xD61F0200U;                                  /* br x16 */
  insn[3] = 0xD503201FU;                                  /* nop */
  return 0;
}

#endif /* MWVD_ELF_KIND_X86_64 / ARM64 */

#if defined(MWVD_ELF_KIND_X86_64) || defined(MWVD_ELF_KIND_ARM64)

static int mwvd_elf_fill_api(const u8 *code, u32 len,
                             const Elf64_Ehdr *eh, const Elf64_Shdr *sh,
                             const u64 *seg_off, u8 *base,
                             struct mwvd_vdf_api *api)
{
  const Elf64_Shdr *symtab_sh = NULL;
  const Elf64_Sym *symtab;
  const u8 *strtab;
  u32 strtab_size = 0;
  u32 nsyms;
  u32 i;

  for (i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type == SHT_SYMTAB) {
      symtab_sh = &sh[i];
      break;
    }
  }
  if (!symtab_sh || symtab_sh->sh_link >= eh->e_shnum ||
      (u64)symtab_sh->sh_offset + symtab_sh->sh_size > len ||
      (u64)sh[symtab_sh->sh_link].sh_offset +
          sh[symtab_sh->sh_link].sh_size > len)
    return -ENOEXEC;

  symtab = (const Elf64_Sym *)(code + symtab_sh->sh_offset);
  strtab = code + sh[symtab_sh->sh_link].sh_offset;
  strtab_size = sh[symtab_sh->sh_link].sh_size;
  nsyms = symtab_sh->sh_size / sizeof(Elf64_Sym);
  if (nsyms == 0)
    return -ENOEXEC;

  memset(api, 0, sizeof(*api));

#define MWVD_ELF_FIND(member, symname)                                      \
  do {                                                                      \
    u32 _k;                                                                 \
    api->member = NULL;                                                     \
    for (_k = 1; _k < nsyms; _k++) {                                        \
      const Elf64_Sym *_s = &symtab[_k];                                    \
      if (ELF64_ST_TYPE(_s->st_info) != STT_FUNC ||                         \
          _s->st_shndx == SHN_UNDEF || _s->st_shndx >= eh->e_shnum ||       \
          _s->st_name >= strtab_size)                                       \
        continue;                                                           \
      if (strcmp((const char *)strtab + _s->st_name, symname) == 0) {       \
        api->member = (void *)(uintptr_t)(base + seg_off[_s->st_shndx] +    \
                                         _s->st_value);                     \
        break;                                                              \
      }                                                                     \
    }                                                                       \
    if (!api->member)                                                       \
      return -ENOENT;                                                       \
  } while (0)

  MWVD_ELF_FIND(probe, "vdf_probe");
  MWVD_ELF_FIND(state_size, "vdf_state_size");
  MWVD_ELF_FIND(core_version, "vd_core_version");
  MWVD_ELF_FIND(core_version_code, "vd_core_version_code");
  MWVD_ELF_FIND(open, "vdf_open");
  MWVD_ELF_FIND(close, "vdf_close");
  MWVD_ELF_FIND(translate, "vdf_translate");
  MWVD_ELF_FIND(flush, "vdf_flush");
  MWVD_ELF_FIND(flush_done, "vdf_flush_done");
  MWVD_ELF_FIND(flush_reset, "vdf_flush_reset");
  MWVD_ELF_FIND(flush_required, "vdf_flush_required");
  MWVD_ELF_FIND(parse_efi, "vdf_parse_efi");
  MWVD_ELF_FIND(parse_efi_blob_v2, "vdf_parse_efi_blob_v2");
  MWVD_ELF_FIND(acquire_config_blob, "vd_acquire_config_blob");
  MWVD_ELF_FIND(acquire_config_blob_acpi, "vd_acquire_config_blob_acpi");
  MWVD_ELF_FIND(get_diag, "vdf_get_diag");
#undef MWVD_ELF_FIND

  return 0;
}

#endif /* ELF64 通用：fill_api */

#if defined(MWVD_ELF_KIND_X86_64) || defined(MWVD_ELF_KIND_ARM64)

static int mwvd_elf_load(const u8 *code, u32 len, struct mwvd_code_image *img)
{
  const Elf64_Ehdr *eh;
  const Elf64_Shdr *sh;
  const Elf64_Shdr *symtab_sh = NULL;
  const Elf64_Sym *symtab;
  const u8 *strtab;
  u32 symtab_index = 0;
  u32 strtab_size;
  u64 *seg_off = NULL;
  u64 *sym_val = NULL;
  u32 nsyms;
  u8 *base = NULL;
#if defined(MWVD_ELF_KIND_ARM64)
  size_t sections_total = 0;
#endif
  size_t total = 0;
  u32 i;
  int ret;

  if (len < sizeof(*eh))
    return -ENOEXEC;

  eh = (const Elf64_Ehdr *)code;
  if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
      eh->e_ident[EI_CLASS] != MWVD_ELF_CLASS ||
      eh->e_type != ET_REL ||
      eh->e_machine != MWVD_ELF_MACHINE ||
      eh->e_shentsize != sizeof(Elf64_Shdr) ||
      eh->e_shnum == 0 ||
      eh->e_shoff > len ||
      (u64)eh->e_shoff + (u64)eh->e_shnum * sizeof(Elf64_Shdr) > len)
    return -ENOEXEC;

  sh = (const Elf64_Shdr *)(code + eh->e_shoff);
  seg_off = kvcalloc(eh->e_shnum, sizeof(u64), GFP_KERNEL);
  if (!seg_off)
    return -ENOMEM;

  for (i = 0; i < eh->e_shnum; i++) {
    u64 align;

    if (!(sh[i].sh_flags & SHF_ALLOC))
      continue;
    align = sh[i].sh_addralign ? sh[i].sh_addralign : 1;
    total = ALIGN(total, align);
    seg_off[i] = total;
    total += sh[i].sh_size;
  }

#if defined(MWVD_ELF_KIND_ARM64)
  sections_total = ALIGN(total, 16);
  /* 为每个未定义符号预留 16B trampoline（CALL26 只能到 ±128MB） */
  total = sections_total +
          (size_t)MWVD_CODE_MAX_SYM_PUTS * MWVD_ARM64_TRAMP_SIZE;
#endif
  if (total == 0 || total > MWVD_CODE_MAX_IMAGE_SIZE) {
    ret = -ENOEXEC;
    goto out_free_off;
  }

  base = mwvd_code_alloc_exec(PAGE_ALIGN(total));
  if (!base) {
    ret = -ENOMEM;
    goto out_free_off;
  }
  memset(base, 0, PAGE_ALIGN(total));

  for (i = 0; i < eh->e_shnum; i++) {
    if (!(sh[i].sh_flags & SHF_ALLOC))
      continue;
    if (sh[i].sh_type == SHT_NOBITS)
      continue;
    /* 可分配段（PROGBITS / NOTE 等）按原字节复制；重定位段不具 SHF_ALLOC */
    if ((u64)sh[i].sh_offset + sh[i].sh_size > len) {
      ret = -ENOEXEC;
      goto out_free_base;
    }
    memcpy(base + seg_off[i], code + sh[i].sh_offset, sh[i].sh_size);
  }

  for (i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type == SHT_SYMTAB) {
      symtab_sh = &sh[i];
      symtab_index = i;
      break;
    }
  }
  if (!symtab_sh || symtab_sh->sh_link >= eh->e_shnum ||
      (u64)symtab_sh->sh_offset + symtab_sh->sh_size > len ||
      (u64)sh[symtab_sh->sh_link].sh_offset + sh[symtab_sh->sh_link].sh_size > len) {
    ret = -ENOEXEC;
    goto out_free_base;
  }

  symtab = (const Elf64_Sym *)(code + symtab_sh->sh_offset);
  strtab = code + sh[symtab_sh->sh_link].sh_offset;
  strtab_size = sh[symtab_sh->sh_link].sh_size;
  nsyms = symtab_sh->sh_size / sizeof(Elf64_Sym);
  if (nsyms == 0) {
    ret = -ENOEXEC;
    goto out_free_base;
  }

  sym_val = kvcalloc(nsyms, sizeof(u64), GFP_KERNEL);
  if (!sym_val) {
    ret = -ENOMEM;
    goto out_free_base;
  }

  for (i = 1; i < nsyms; i++) {
    const Elf64_Sym *sym = &symtab[i];

    if (sym->st_shndx == SHN_UNDEF)
      continue;
    sym_val[i] = mwvd_elf_symbol_value(sym, seg_off, base, eh->e_shnum);
  }

  ret = mwvd_elf_resolve_undef(symtab, nsyms, strtab, strtab_size,
                              sym_val, img);
  if (ret)
    goto out_free_sym;

#if defined(MWVD_ELF_KIND_ARM64)
  {
    u32 tramp_idx = 0;

    for (i = 1; i < nsyms; i++) {
      u64 target;
      u64 tramp;

      if (symtab[i].st_shndx != SHN_UNDEF)
        continue;
      if (tramp_idx >= MWVD_CODE_MAX_SYM_PUTS) {
        ret = -E2BIG;
        goto out_free_sym;
      }

      target = sym_val[i];
      tramp = (u64)(uintptr_t)(base + sections_total +
                               (size_t)tramp_idx * MWVD_ARM64_TRAMP_SIZE);
      ret = mwvd_elf_arm64_make_tramp((u32 *)(uintptr_t)tramp,
                                      target, tramp);
      if (ret)
        goto out_free_sym;
      sym_val[i] = tramp;
      tramp_idx++;
    }
  }
#endif

  for (i = 0; i < eh->e_shnum; i++) {
    const Elf64_Shdr *rt;
    const Elf64_Rela *rela;
    u32 nrela;

    if (sh[i].sh_type != SHT_RELA)
      continue;
    if (sh[i].sh_info >= eh->e_shnum ||
        !(sh[sh[i].sh_info].sh_flags & SHF_ALLOC))
      continue;
    if (sh[i].sh_link != symtab_index) {
      ret = -ENOEXEC;
      goto out_free_sym;
    }

    rt = &sh[sh[i].sh_info];
    if ((u64)sh[i].sh_offset + sh[i].sh_size > len) {
      ret = -ENOEXEC;
      goto out_free_sym;
    }
    rela = (const Elf64_Rela *)(code + sh[i].sh_offset);
    nrela = sh[i].sh_size / sizeof(Elf64_Rela);

    ret = MWVD_ELF_APPLY_RELA(rela, nrela, rt, seg_off[sh[i].sh_info],
                              base, sym_val, nsyms);
    if (ret)
      goto out_free_sym;
  }

  ret = mwvd_code_make_exec(base, total);
  if (ret)
    goto out_free_sym;

#if defined(MWVD_ELF_KIND_ARM64)
  mwvd_elf_flush_icache(base, total);
#endif

  ret = mwvd_elf_fill_api(code, len, eh, sh, seg_off, base, &img->api);
  if (ret)
    goto out_free_sym;

  img->base = base;
  img->size = PAGE_ALIGN(total);
  kvfree(sym_val);
  kvfree(seg_off);
  return 0;

out_free_sym:
  mwvd_elf_cleanup(img);
  kvfree(sym_val);
out_free_base:
  mwvd_code_free_exec(base);
out_free_off:
  kvfree(seg_off);
  return ret;
}

#undef MWVD_ELF_APPLY_RELA

#elif defined(MWVD_ELF_KIND_I386)

/*
 * i386：ELF32 ET_REL，重定位使用 SHT_REL（addend 存放在目标位置）。
 * vd.b 实际只用 R_386_32 / R_386_PC32；未定义符号是编译器 64 位除法
 * 辅助函数（__divdi3 等），必须由内核导出符号解析。
 */
static u64 mwvd_elf_symbol_value32(const Elf32_Sym *sym,
                                   const u64 *seg_off, u8 *base, int shnum)
{
  if (sym->st_shndx == SHN_ABS)
    return sym->st_value;
  if (sym->st_shndx == SHN_UNDEF)
    return 0;
  if (sym->st_shndx >= shnum)
    return 0;
  return (u64)(uintptr_t)(base + seg_off[sym->st_shndx] + sym->st_value);
}

static int mwvd_elf_resolve_undef32(const Elf32_Sym *symtab, u32 nsyms,
                                    const u8 *strtab, u32 strtab_size,
                                    u64 *sym_val, struct mwvd_code_image *img)
{
  u32 i;
  int ret = 0;

  for (i = 1; i < nsyms; i++) {
    const Elf32_Sym *sym = &symtab[i];
    const char *name;
    void *addr;
    bool from_kernel;

    if (sym->st_shndx != SHN_UNDEF)
      continue;
    if (sym->st_name == 0 || sym->st_name >= strtab_size)
      return -ENOEXEC;

    name = (const char *)strtab + sym->st_name;
    if (memchr(name, '\0', strtab_size - sym->st_name) == NULL)
      return -ENOEXEC;

    addr = mwvd_elf_kernel_symbol(name, &from_kernel);
    if (!addr)
      return -ENOENT;

    sym_val[i] = (u64)(uintptr_t)addr;

    if (!from_kernel)
      continue;
    if (img->sym_count < MWVD_CODE_MAX_SYM_PUTS) {
      char *dup = kstrdup(name, GFP_KERNEL);
      if (!dup) {
        ret = -ENOMEM;
        break;
      }
      img->sym_names[img->sym_count++] = dup;
    } else {
      ret = -E2BIG;
      break;
    }
  }

  return ret;
}

static int mwvd_elf_apply_rel32(const Elf32_Rel *rel, u32 nrel,
                                const Elf32_Shdr *target_sh,
                                u64 target_off, u8 *base,
                                const u64 *sym_val, u32 nsyms)
{
  u32 j;

  for (j = 0; j < nrel; j++) {
    const Elf32_Rel *r = &rel[j];
    u32 type = ELF32_R_TYPE(r->r_info);
    u32 si = ELF32_R_SYM(r->r_info);
    u64 P = (u64)(uintptr_t)(base + target_off + r->r_offset);
    u32 A, S, val;

    if (si >= nsyms || r->r_offset + sizeof(u32) > target_sh->sh_size)
      return -ENOEXEC;

    A = 0;
    memcpy(&A, (void *)(uintptr_t)P, sizeof(A));
    S = (u32)sym_val[si];

    switch (type) {
    case R_386_32:
      val = S + A;
      break;
    case R_386_PC32:
      val = S + A - (u32)P;
      break;
    default:
      return -ENOEXEC;
    }

    memcpy((void *)(uintptr_t)P, &val, sizeof(val));
  }

  return 0;
}

static int mwvd_elf_fill_api32(const u8 *code, u32 len,
                               const Elf32_Ehdr *eh, const Elf32_Shdr *sh,
                               const u64 *seg_off, u8 *base,
                               struct mwvd_vdf_api *api)
{
  const Elf32_Shdr *symtab_sh = NULL;
  const Elf32_Sym *symtab;
  const u8 *strtab;
  u32 strtab_size = 0;
  u32 nsyms;
  u32 i;

  for (i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type == SHT_SYMTAB) {
      symtab_sh = &sh[i];
      break;
    }
  }
  if (!symtab_sh || symtab_sh->sh_link >= eh->e_shnum ||
      (u64)symtab_sh->sh_offset + symtab_sh->sh_size > len ||
      (u64)sh[symtab_sh->sh_link].sh_offset +
          sh[symtab_sh->sh_link].sh_size > len)
    return -ENOEXEC;

  symtab = (const Elf32_Sym *)(code + symtab_sh->sh_offset);
  strtab = code + sh[symtab_sh->sh_link].sh_offset;
  strtab_size = sh[symtab_sh->sh_link].sh_size;
  nsyms = symtab_sh->sh_size / sizeof(Elf32_Sym);
  if (nsyms == 0)
    return -ENOEXEC;

  memset(api, 0, sizeof(*api));

#define MWVD_ELF_FIND32(member, symname)                                    \
  do {                                                                      \
    u32 _k;                                                                 \
    api->member = NULL;                                                     \
    for (_k = 1; _k < nsyms; _k++) {                                        \
      const Elf32_Sym *_s = &symtab[_k];                                    \
      if (ELF32_ST_TYPE(_s->st_info) != STT_FUNC ||                         \
          _s->st_shndx == SHN_UNDEF || _s->st_shndx >= eh->e_shnum ||       \
          _s->st_name >= strtab_size)                                       \
        continue;                                                           \
      if (strcmp((const char *)strtab + _s->st_name, symname) == 0) {       \
        api->member = (void *)(uintptr_t)(base + seg_off[_s->st_shndx] +    \
                                         _s->st_value);                     \
        break;                                                              \
      }                                                                     \
    }                                                                       \
    if (!api->member)                                                       \
      return -ENOENT;                                                       \
  } while (0)

  MWVD_ELF_FIND32(probe, "vdf_probe");
  MWVD_ELF_FIND32(state_size, "vdf_state_size");
  MWVD_ELF_FIND32(core_version, "vd_core_version");
  MWVD_ELF_FIND32(core_version_code, "vd_core_version_code");
  MWVD_ELF_FIND32(open, "vdf_open");
  MWVD_ELF_FIND32(close, "vdf_close");
  MWVD_ELF_FIND32(translate, "vdf_translate");
  MWVD_ELF_FIND32(flush, "vdf_flush");
  MWVD_ELF_FIND32(flush_done, "vdf_flush_done");
  MWVD_ELF_FIND32(flush_reset, "vdf_flush_reset");
  MWVD_ELF_FIND32(flush_required, "vdf_flush_required");
  MWVD_ELF_FIND32(parse_efi, "vdf_parse_efi");
  MWVD_ELF_FIND32(parse_efi_blob_v2, "vdf_parse_efi_blob_v2");
  MWVD_ELF_FIND32(acquire_config_blob, "vd_acquire_config_blob");
  MWVD_ELF_FIND32(acquire_config_blob_acpi, "vd_acquire_config_blob_acpi");
  MWVD_ELF_FIND32(get_diag, "vdf_get_diag");
#undef MWVD_ELF_FIND32

  return 0;
}

static int mwvd_elf_load(const u8 *code, u32 len, struct mwvd_code_image *img)
{
  const Elf32_Ehdr *eh;
  const Elf32_Shdr *sh;
  const Elf32_Shdr *symtab_sh = NULL;
  const Elf32_Sym *symtab;
  const u8 *strtab;
  u32 symtab_index = 0;
  u32 strtab_size = 0;
  u32 nsyms = 0;
  u64 *seg_off = NULL;
  u64 *sym_val = NULL;
  u8 *base = NULL;
  size_t total = 0;
  u32 i;
  int ret;

  if (len < sizeof(*eh))
    return -ENOEXEC;

  eh = (const Elf32_Ehdr *)code;
  if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
      eh->e_ident[EI_CLASS] != ELFCLASS32 ||
      eh->e_type != ET_REL ||
      eh->e_machine != MWVD_ELF_MACHINE ||
      eh->e_shentsize != sizeof(Elf32_Shdr) ||
      eh->e_shnum == 0 ||
      eh->e_shoff > len ||
      (u64)eh->e_shoff + (u64)eh->e_shnum * sizeof(Elf32_Shdr) > len)
    return -ENOEXEC;

  sh = (const Elf32_Shdr *)(code + eh->e_shoff);
  seg_off = kvcalloc(eh->e_shnum, sizeof(u64), GFP_KERNEL);
  if (!seg_off)
    return -ENOMEM;

  for (i = 0; i < eh->e_shnum; i++) {
    u64 align;

    if (!(sh[i].sh_flags & SHF_ALLOC))
      continue;
    align = sh[i].sh_addralign ? sh[i].sh_addralign : 1;
    total = ALIGN(total, align);
    seg_off[i] = total;
    total += sh[i].sh_size;
  }

  if (total == 0 || total > MWVD_CODE_MAX_IMAGE_SIZE) {
    ret = -ENOEXEC;
    goto out_free_off;
  }

  base = mwvd_code_alloc_exec(PAGE_ALIGN(total));
  if (!base) {
    ret = -ENOMEM;
    goto out_free_off;
  }
  memset(base, 0, PAGE_ALIGN(total));

  for (i = 0; i < eh->e_shnum; i++) {
    if (!(sh[i].sh_flags & SHF_ALLOC))
      continue;
    if (sh[i].sh_type == SHT_NOBITS)
      continue;
    if ((u64)sh[i].sh_offset + sh[i].sh_size > len) {
      ret = -ENOEXEC;
      goto out_free_base;
    }
    memcpy(base + seg_off[i], code + sh[i].sh_offset, sh[i].sh_size);
  }

  for (i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type == SHT_SYMTAB) {
      symtab_sh = &sh[i];
      symtab_index = i;
      break;
    }
  }
  if (!symtab_sh || symtab_sh->sh_link >= eh->e_shnum ||
      (u64)symtab_sh->sh_offset + symtab_sh->sh_size > len ||
      (u64)sh[symtab_sh->sh_link].sh_offset +
          sh[symtab_sh->sh_link].sh_size > len) {
    ret = -ENOEXEC;
    goto out_free_base;
  }

  symtab = (const Elf32_Sym *)(code + symtab_sh->sh_offset);
  strtab = code + sh[symtab_sh->sh_link].sh_offset;
  strtab_size = sh[symtab_sh->sh_link].sh_size;
  nsyms = symtab_sh->sh_size / sizeof(Elf32_Sym);
  if (nsyms == 0) {
    ret = -ENOEXEC;
    goto out_free_base;
  }

  sym_val = kvcalloc(nsyms, sizeof(u64), GFP_KERNEL);
  if (!sym_val) {
    ret = -ENOMEM;
    goto out_free_base;
  }

  for (i = 1; i < nsyms; i++) {
    const Elf32_Sym *sym = &symtab[i];

    if (sym->st_shndx == SHN_UNDEF)
      continue;
    sym_val[i] = mwvd_elf_symbol_value32(sym, seg_off, base, eh->e_shnum);
  }

  ret = mwvd_elf_resolve_undef32(symtab, nsyms, strtab, strtab_size,
                                 sym_val, img);
  if (ret)
    goto out_free_sym;

  for (i = 0; i < eh->e_shnum; i++) {
    const Elf32_Shdr *rt;
    const Elf32_Rel *rel;
    u32 nrel;

    if (sh[i].sh_type != SHT_REL)
      continue;
    if (sh[i].sh_info >= eh->e_shnum ||
        !(sh[sh[i].sh_info].sh_flags & SHF_ALLOC))
      continue;
    if (sh[i].sh_link != symtab_index) {
      ret = -ENOEXEC;
      goto out_free_sym;
    }

    rt = &sh[sh[i].sh_info];
    if ((u64)sh[i].sh_offset + sh[i].sh_size > len) {
      ret = -ENOEXEC;
      goto out_free_sym;
    }
    rel = (const Elf32_Rel *)(code + sh[i].sh_offset);
    nrel = sh[i].sh_size / sizeof(Elf32_Rel);

    ret = mwvd_elf_apply_rel32(rel, nrel, rt, seg_off[sh[i].sh_info],
                               base, sym_val, nsyms);
    if (ret)
      goto out_free_sym;
  }

  ret = mwvd_code_make_exec(base, total);
  if (ret)
    goto out_free_sym;

  ret = mwvd_elf_fill_api32(code, len, eh, sh, seg_off, base, &img->api);
  if (ret)
    goto out_free_sym;

  img->base = base;
  img->size = PAGE_ALIGN(total);
  kvfree(sym_val);
  kvfree(seg_off);
  return 0;

out_free_sym:
  mwvd_elf_cleanup(img);
  kvfree(sym_val);
out_free_base:
  mwvd_code_free_exec(base);
out_free_off:
  kvfree(seg_off);
  return ret;
}

#endif /* MWVD_ELF_KIND_X86_64 / ARM64 / I386 */

static void mwvd_elf_cleanup(struct mwvd_code_image *img)
{
  int i;

  for (i = 0; i < img->sym_count; i++) {
    if (img->sym_names[i]) {
      __symbol_put(img->sym_names[i]);
      kfree(img->sym_names[i]);
      img->sym_names[i] = NULL;
    }
  }
  img->sym_count = 0;
}

static void mwvd_elf_unload(struct mwvd_code_image *img)
{
  if (!img->base)
    return;

  mwvd_code_free_exec(img->base);
  img->base = NULL;
  mwvd_elf_cleanup(img);
}

#else /* !MWVD_ELF_LOAD_SUPPORTED */

static int mwvd_elf_load(const u8 *code, u32 len, struct mwvd_code_image *img)
{
  return -EOPNOTSUPP;
}

static void mwvd_elf_unload(struct mwvd_code_image *img)
{
  (void)img;
}

#endif /* MWVD_ELF_LOAD_SUPPORTED */

/* ------------------------------------------------------------------ */
/* 初始化 / 卸载                                                        */
/* ------------------------------------------------------------------ */

static struct mwvd_code_image g_code_image;

static int mwvd_code_try_firmware(void)
{
  struct mwvd_code_meta meta;
  u8 *payload = NULL;
  u32 payload_len = 0;
  int ret;

  ret = mwvd_code_read_table(&payload, &payload_len);
  if (ret)
    return ret;

  ret = mwvd_code_validate(payload, payload_len, &meta);
  if (ret) {
    pr_info("mwvd: firmware MWVD payload rejected (step=validate, ret=%d)\n", ret);
    goto out;
  }

  ret = mwvd_code_lookup_exec_api();
  if (ret) {
    pr_info("mwvd: executable memory API unavailable (ret=%d)\n", ret);
    goto out;
  }

  ret = mwvd_elf_load(payload + meta.code_offset, meta.code_size,
                      &g_code_image);
  if (ret) {
    pr_info("mwvd: firmware MWVD payload rejected (step=load, ret=%d)\n", ret);
    goto out;
  }

  g_mwvd_vdf = &g_code_image.api;
  pr_info("mwvd: firmware core loaded (arch=%s abi=%u.%u build=%u code=%u bytes)\n",
          meta.arch, meta.abi_major, meta.abi_minor, meta.code_build,
          meta.code_size);
  ret = 0;

out:
  kfree(payload);
  return ret;
}

int mwvd_code_init(void)
{
  int ret;

  ret = mwvd_code_try_firmware();
  if (ret) {
    /*
     * 纯固件载荷模式：没有内置闭源层可回退。没有格式后端时任何 vd I/O
     * 都毫无意义，因此直接拒载，让问题在 modprobe / initramfs 阶段
     * 就暴露，而不是留下一个「加载成功但挂不出盘」的驱动。
     */
    pr_err("mwvd: firmware MWVD payload unavailable (ret=%d); "
           "this build requires cloud firmware (no built-in core)\n", ret);
    g_mwvd_vdf = NULL;
    return ret;
  }

  return 0;
}

void mwvd_code_exit(void)
{
  if (!g_mwvd_vdf)
    return;

  mwvd_elf_unload(&g_code_image);
  g_mwvd_vdf = NULL;
}
