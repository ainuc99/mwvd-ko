#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
# Copyright (c) 2025-2026 LU WEIFENG (wave.lu@gmail.com)
# 版权所有 (c) 2025-2026 陆伟峰
#
# gen-pubkey.sh — 从 Secure Boot db 证书生成 mwvd_code_pubkey.h
#
# 生成的头文件自带 SPDX 双许可标识与版权声明，无需再跑 spdx-headers.py。
#
# 用法:
#   tools/gen-pubkey.sh <证书.pem> [输出路径]        # 生成（默认覆盖 mwvd_code_pubkey.h）
#   tools/gen-pubkey.sh --check <证书.pem>           # 校验现有头文件是否与证书一致
#   MWVD_CERT=/path/cert.pem tools/gen-pubkey.sh     # 证书也可用环境变量给
#
# 依赖: openssl、xxd（vim-common / vim-tiny）
# 约定: 私钥不得入库、不得进固件；公钥不是秘密，可随驱动源码分发。
#
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DEFAULT="$REPO/mwvd_code_pubkey.h"

CHECK=0
case "${1:-}" in
  --check) CHECK=1; shift ;;
  -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
esac

CRT="${1:-${MWVD_CERT:-}}"
OUT="${2:-$OUT_DEFAULT}"
if [ -z "$CRT" ]; then
  echo "ERROR: 需要证书路径（参数 1 或环境变量 MWVD_CERT）" >&2
  exit 2
fi
[ -r "$CRT" ] || { echo "ERROR: 证书不可读: $CRT" >&2; exit 2; }
command -v openssl >/dev/null || { echo "ERROR: 需要 openssl" >&2; exit 2; }
command -v xxd     >/dev/null || { echo "ERROR: 需要 xxd（vim-common）" >&2; exit 2; }

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT
openssl x509 -in "$CRT" -pubkey -noout 2>/dev/null |
  openssl rsa -pubin -RSAPublicKey_out -outform DER -out "$tmp" 2>/dev/null
[ -s "$tmp" ] || { echo "ERROR: 无法从证书导出 RSAPublicKey: $CRT" >&2; exit 3; }
len="$(wc -c < "$tmp")"

gen() {
  cat <<'HDR'
// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * mwvd_code_pubkey.h — MWVD 代码载荷验签公钥（RSA，DER / PKCS#1 RSAPublicKey）
 *
 * 来源：Secure Boot db 证书（与固件侧代码载荷签名密钥同源）。
 *       私钥不得入库、不得进固件；公钥不是秘密，可随驱动源码分发。
 * 本文件由脚本生成，请勿手工编辑：
 *   tools/gen-pubkey.sh <Secure Boot db 证书.pem> [输出路径]
 *   tools/gen-pubkey.sh --check <证书.pem>      # 校验是否与证书一致
 *
 * Copyright (c) 2025-2026 LU WEIFENG (wave.lu@gmail.com)
 * 版权所有 (c) 2025-2026 陆伟峰
 */
#ifndef _MWVD_CODE_PUBKEY_H_
#define _MWVD_CODE_PUBKEY_H_

static const unsigned char mwvd_code_pubkey_der[] = {
HDR
  xxd -i < "$tmp"
  cat <<'FTR'
};

#define MWVD_CODE_PUBKEY_DER_LEN  ((u32)sizeof(mwvd_code_pubkey_der))

#endif /* _MWVD_CODE_PUBKEY_H_ */
FTR
}

if [ "$CHECK" -eq 1 ]; then
  if gen | diff -q - "$OUT" >/dev/null 2>&1; then
    echo "OK: $OUT 与证书一致（DER $len 字节）"
    exit 0
  fi
  echo "MISMATCH: $OUT 与证书不一致（证书导出的 DER 为 $len 字节）" >&2
  gen | diff -u "$OUT" - | head -20 >&2 || true
  exit 1
fi

gen > "$OUT"
echo "已生成 $OUT（DER $len 字节）"
