# mwvd-ko — Multiware 虚拟磁盘内核模块（GPL 层）

本仓库是 Multiware 云固件 Linux 端内核模块 `mwvd` 的 **GPL 层**源码，
**双许可**：`GPL-2.0` 或 `BSD-2-Clause`（与模块的 `MODULE_LICENSE("Dual BSD/GPL")`
一致）。可从两者中选择其一使用，详见 [许可](#许可)。

## 这个仓库包含什么、不包含什么

| 部分 | 是否在本仓库 | 说明 |
|---|---|---|
| 块设备驱动、虚拟盘调度、I/O 路径 | ✅ | `main.c` / `vd_core.c` |
| 固件代码载荷的读取、校验、验签、运行时加载 | ✅ | `mwvd_code.c` |
| v2 配置契约（ExtraData TLV）解析 | ✅ | `mwcfg_ext.c` |
| 安装与休眠自动配置脚本 | ✅ | `setup.sh` / `mwvd-*` |
| 格式后端（RAW / VHD / VHDX 实现） | ❌ | **不在本仓库**，见下节 |

### 格式后端由固件提供

自 2.8.0 起，模块**不再链接**格式后端代码。格式后端（RAW / VHD / VHDX 的翻译与
落盘逻辑）由**云固件**在启动时通过 ACPI `MWVD` 表以代码载荷形式交付，驱动在模块
初始化阶段完成下列校验后加载并调用：

```
ACPI MWVD 表 → 表/载荷自洽 → CRC-32 → RSA-2048 + SHA-256 验签
             → 架构匹配（x64/x86/arm64）→ ABI 主版本匹配
             → ELF ET_REL 重定位到内核可执行内存 → 填充函数表 g_mwvd_vdf
```

任一步失败 ⇒ 模块**拒绝加载**（内核日志给出失败步骤），不做静默降级。因此本模块
只能在**搭载 Multiware 云固件的机器**上工作；这是设计选择，不是缺陷。

## 构建

### 编译产物本身

```bash
make                    # 用当前运行内核的头文件编译
```

需要：`gcc`、目标内核的 `build` 头文件。**不需要**任何闭源二进制。

### 安装（DKMS，推荐）

```bash
sudo bash setup.sh
```

`setup.sh` 会：安装/校验 DKMS → 注册并构建模块 → 配置 initramfs 自动加载
（initramfs-tools / dracut / mkinitcpio 自动识别）→ 安装 udev 规则 →
安装休眠自动配置（`mwvd-hibernate-setup`）。

发行版适配与休眠配置逻辑见 `mwvd-distro.sh`；策略文件模板为
`mwvd-hibernate.default`（安装到 `/etc/default/mwvd-hibernate`）。

## 目录结构

```
main.c                  块设备接口（blk-mq、sysfs、ioctl、休眠标记）
vd_core.c               虚拟盘调度（格式探测、I/O 分发、日志重放）
mwcfg_ext.c             v2 配置契约（ExtraData TLV）解析
mwvd_code.c             固件 MWVD 代码载荷：读取/校验/验签/加载
mwvd.h / vd.h           内部接口
mwcfg_ext.h             契约解析接口
mwvd_code.h             载荷加载器接口（函数表 g_mwvd_vdf）
vd_abi.h                GPL 层 ↔ 格式后端的纯 C 协议（载荷实现它）
mwcfg_v2.h              v2 契约结构定义
mwvd_code_pubkey.h      载荷验签公钥（RSA-2048 / DER）
Makefile                构建入口（与 DKMS 共用同一份 makefile）
dkms.conf               DKMS 配置
setup.sh                安装入口
mwvd-distro.sh          发行版/initramfs/引导器适配库
mwvd-hibernate-setup    休眠自动配置（udev + systemd 触发）
testcase/               测试脚本
```

## 关于本仓库的维护方式

本仓库由上游 MWVD 主仓库的**受控导出**生成（`tools/sync-open.sh` + 文件清单），
保证公开代码与内部主线一致、不夹带私有代码或二进制。

- **请勿直接在本仓库修改源码**：上游同步会覆盖。发现问题请开 issue / PR 说明
  意图，改动在上游合并后由下一次同步带入。
- 提交信息里的 `sync: 从 MWVD <sha> 同步 GPL 层（vX.Y.Z）` 即本次同步对应的上游
  提交与版本。

## 许可

本仓库采用**双许可**，与模块的 `MODULE_LICENSE("Dual BSD/GPL")` 一致：

- `LICENSE` — **GPL-2.0** 全文（GNU General Public License v2.0）
- `LICENSE.BSD` — **BSD-2-Clause** 条款

使用时**任选其一**：选择 GPL-2.0 则适用 GPL 全文条款；选择 BSD-2-Clause 则只需
保留版权声明与免责声明。分发二进制（内核模块）时，选择 GPL-2.0 可与 Linux 内核
共存（内核为 GPL-2.0）；选择 BSD-2-Clause 时按双许可惯例仅限与本模块自身代码
组合使用，与内核链接仍受内核许可约束。
