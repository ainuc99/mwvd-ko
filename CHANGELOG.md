# CHANGELOG

云固件虚拟磁盘内核模块（mwvd）版本变更记录。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。  
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)：主版本.次版本.修订号。

## [Unreleased]

### 新增（工具）

- **GPL 层开源导出机制**：新增 `tools/open-manifest.txt`（白名单 + 路径映射）与
  `tools/sync-open.sh`（清单驱动 + `rsync --delete` 严格镜像），把本仓库的 GPL 层
  单向导出到开源仓库 `mwvd-ko`。
  - 强制拒绝闭源层条目（`core/vdf_*.c`、`prebuild/*.b`、`Private/`），导出后再
    复核暂存区；
  - 开源版 `README.md` 与 `.gitignore` 由 `tools/open/` 下的模板生成；
  - `--dry-run` 预览、`--commit` 在目标仓库提交（提交信息带来源提交与版本）、
    `--push` 推送。
  - 首次导出经 `linux-6.12` 内核树独立编译验证：产物 `mwvd.ko` 无任何
    `vdf_*` / `__symbol_get` 未定义符号。
- `testcase/test_kernel_matrix.sh` 的多版内核根目录默认值由内部路径改为
  `$HOME/linux-kernels`（可用第一个参数覆盖），便于开源用户使用。

## [2.8.3] - 2026-10-23

### 修复

- **休眠模式配置的误报警告**：`mwvd_configure_hibernate_mode()` 返回 `2` 表示
  “已是目标值”，但 `mwvd_adapter_apply` 用 `|| mwvd_warn` 判断，把 `2` 也当失败
  ⇒ 日志同时出现 `hibernate mode already HibernateMode=shutdown; nothing to do`
  与 `cannot configure hibernate mode; set HibernateMode=shutdown ... manually`
  两句矛盾的话。改为**只在返回 1（写入失败）时告警**。

### 验证（2.8.2 包在三个发行版上回归）

| 发行版 | 内核 | initramfs / 引导 | 启动 | S4 休眠/恢复 |
|---|---|---|---|---|
| Ubuntu 20.04.5 | 5.15.0-139 | initramfs-tools + GRUB | PASS | PASS |
| Fedora 44 | 7.0.4 | dracut + BLS | PASS | PASS |
| Arch Linux | 7.2.3 | mkinitcpio + UKI | PASS | PASS |

- 三个系统均由**旧版本升级到 2.8.x**（Ubuntu 2.8.0 / Fedora 2.7.0 / Arch 2.7.0），
  启动后均为 `firmware core loaded (arch=x64 abi=1.0 build=26048 code=33744 bytes)`
  + `module 2.8.x loaded`，根分区在 `mwvd0` 上正常；
- S4 均为：`systemctl hibernate` → QEMU 断电 → 上电后固件
  `SLEEPING mark found` → `hybrid resume` → `mark cleared`，内核
  `Restarting tasks: Done`，**uptime 延续**；
- `2.8.2` 自动把 `/sys/power/disk` 设为 `platform [shutdown] …`，三个系统均生效。

## [2.8.2] - 2026-10-23

### 新增

- **休眠模式自动配置（默认 `shutdown`）**：`mwvd-hibernate-setup` 在写 resume /
  initramfs 配置的同时，把 systemd 的 `HibernateMode` 固定为 `shutdown`
  （写 `/etc/systemd/sleep.conf` 的 `[Sleep]` 段，逐行保留其它配置、内容未变不
  重写），并立即写 `/sys/power/disk` 使当前启动即生效。
  背景：Debian 13（6.12.63）在云固件下用默认 `platform` 模式会**静默回滚**——
  `Creating image:` 之后直接 `ACPI: PM: Waking up from system sleep state S4`
  + `Restarting tasks`，swap 里没有任何镜像（无 `S1SUSPEND`）随后关机；
  改 `shutdown` 后 S4 正常。云固件场景的断电/上电恢复本由固件接管，
  不需要内核走 ACPI S4 平台钩子。
  策略项 `MWVD_HIBERNATE_DISK_MODE`：`shutdown`（默认）/ `platform`
  （不管理，交回 systemd/ACPI 默认）/ `reboot` / `suspend` / `test_resume`，
  见 `/etc/default/mwvd-hibernate`。

### 验证

- Debian 13.3.0（6.12.63+deb13-amd64）+ 云固件：写入后
  `grep HibernateMode /etc/systemd/sleep.conf` → `HibernateMode=shutdown`，
  `/sys/power/disk` → `platform [shutdown] …`；**重启后仍为 `[shutdown]`**
  （systemd 已应用该配置）。

### 测试

- `testcase/test_hibernate_scripts.sh`：24 → **41 项 PASS / 0 FAIL**（新增 17 项
  sleep-mode 用例：新建 `[Sleep]` 段 / 替换旧值 / 幂等 / `platform` 不管理 /
  未知值回退 shutdown / 保留文件其它内容）。

## [2.8.1] - 2026-10-23

### 修复

- **6.12+ 内核上固件载荷加载失败（`memcpy`/`memset` 符号解析）**：载荷的未定义
  符号原先用 `__symbol_get()` 解析内核导出的 `memcpy`/`memset`；6.12 起
  `__symbol_get()` **只接受 GPLONLY 导出符号**，内核打印
  `Failing symbol_get of non-GPLONLY symbol memcpy.` 并返回 NULL，使
  `mwvd_code_init()` 返回 `-ENOENT`、纯载荷模式拒载 ⇒ 根盘不出现、initramfs
  掉到 busybox shell（Debian 13 / 6.12.63 实测）。kprobe 兜底同样不可用
  （x86 的 `memcpy`/`memset` 在 kprobe 黑名单）。现改为**在 GPL 层直接提供
  包装函数**（`mwvd_payload_memcpy` / `mwvd_payload_memset`），载荷的同名未定义
  符号直接映射过去，不再依赖 `__symbol_get`（`from_kernel=false`，cleanup 不会
  `__symbol_put`）。驱动层不再需要任何内核符号解析。

### 验证

- **Debian 13.3.0（6.12.63+deb13-amd64）+ 云固件 QEMU**（模拟用户升级）：
  系统已装旧版 `mwvd.ko.xz`（非 dkms）⇒ `setup.sh` 自动装 dkms、构建 2.8.x、
  归档旧模块、重建 initramfs；重启后
  `mwvd: firmware core loaded (arch=x64 abi=1.0 build=26048 code=33744 bytes)`、
  `module 2.8.1 loaded`，`mwvd0` 分区枚举正常，根 `/dev/mwvd0p2 ext4`。
- **S4 混合休眠/恢复**：`systemctl hibernate`（`/sys/power/disk=shutdown`）⇒
  固件 `Hibernate wake: SLEEPING mark found` → `image entry found, preparing
  hybrid resume` → `mark cleared, handing over to OS resume`；内核
  `ACPI: PM: Waking up from system sleep state S4` / `Restarting tasks ... done`，
  uptime 延续（4 min → 6 min）。

### 已知问题

- Debian 13（6.12）在云固件下用默认 `platform` 休眠模式会**静默回滚**：
  `Creating image:` 之后直接 `ACPI: PM: Waking up from system sleep state S4` +
  `Restarting tasks`（无镜像写入、swap 里无 `S1SUSPEND`），随后关机。
  `echo shutdown > /sys/power/disk` 后 S4 正常。建议发行版/部署侧把休眠模式
  固定为 `shutdown`（systemd：`/etc/systemd/sleep.conf` 的
  `HibernateMode=shutdown`）。

## [2.8.0] - 2026-10-23

### 变更（不兼容）

- **去掉内置闭源层：纯固件载荷模式**。`mwvd.ko` 不再链接 `vd.o`
  （即 `prebuild/<arch>/vd.*.b`），格式后端的唯一来源是固件经 ACPI `MWVD`
  表发布的代码载荷（`COMMUNICATION_V2_SPEC` §6.9）。`mwvd_code_init()` 加载失败
  时 `mwvd_init()` 直接返回错误、模块拒载，并在内核日志打印失败原因与步骤
  （表缺失 / 头自洽 / CRC / 验签 / 架构 / ABI / 可执行内存 API / ELF 加载）。
  依据：mwvd 必然运行在云固件启动模式下，固件载荷总是存在；静默降级会留下
  「驱动在、盘不在」的隐性问题，比拒绝加载更难排查。
  - **影响**：固件未发布 `MWVD` 表、`abi_major` 不匹配（当前 `1`）、验签失败，
    或架构不在 `x86_64 / i386 / arm64` 内（如 loongarch64）时，模块无法加载。
    驱动与固件必须配套升级。
  - **收益**：闭源层与内核/发行版彻底解耦（不再需要按内核缓解配置分发
    retpoline/plain 变体），DKMS 构建不再依赖 `prebuild/` 目录，`.ko` 体积减少
    约 33 KB 闭源层代码。
  - `make prebuild` 与 `prebuild/<arch>/vd.*.b` **保留**，用途改为仅供固件侧
    打包 `MWVD` 载荷。
- **版本号纠正**：`dkms.conf` 的 `PACKAGE_VERSION` 长期滞后在 `2.3.4`，本次与
  `MWVD_MODULE_VERSION` 一并升到 `2.8.0`。

### 验证

- **全量内核编译矩阵**（`testcase/test_kernel_matrix.sh`，25 棵内核树）：
  `5.10 / 5.15 / 6.0 / 6.1 / 6.2 / 6.3 / 6.4 / 6.5 / 6.6 / 6.7 / 6.8 / 6.9 /
  6.10 / 6.11 / 6.12 / 6.13 / 6.14 / 6.15 / 6.16 / 6.17 / 6.18 / 6.19 /
  7.0 / 7.1 / 7.2` **全部编译+链接通过，0 warning**（含 `6.10` execmem
  分支切换点）。
- Ubuntu 20.04（内核 5.15.0-139）+ 云固件 QEMU：DKMS 构建安装成功后重启，
  固件日志 `Hibernate wake` 路径正常，内核侧
  `mwvd: firmware core loaded (arch=x64 abi=1.0 build=26048 code=33744 bytes)`、
  `mwvd: module 2.8.0 loaded (firmware core 1.0.1, headers 1.0.1)`；
  `mwvd0` 及其分区正常枚举，根文件系统 `/dev/mwvd0p2` 可读写。

## [2.7.0] - 2026-10-18

### 新增

- **只读 sysfs 休眠分区查询属性**：新增
  `/sys/class/misc/mwvd-control/hibernate`，按 `key=value` 输出
  `valid` / `usage` / `resolved` / `part_uuid` / `device_path`，供 udev 触发的
  shell 脚本直接读取；无需 ioctl，也无需新增 `mwctl` 等用户态二进制。
  属性在模块初始化完成休眠记录解析后创建，卸载时移除。设计见
  `design/HIBERNATE-UDEV-AUTOCONFIG-DESIGN.md` §5.1。
- **休眠分区自动配置脚本**：新增 `mwvd-hibernate.rules`、
  `mwvd-hibernate-setup.service`、`mwvd-hibernate-setup`、`mwvd-distro.sh` 与
  `/etc/default/mwvd-hibernate` 策略模板；`setup.sh` 负责安装。支持
  initramfs-tools / dracut / mkinitcpio，自动校验 swap 并按需激活、写入
  `/etc/mwvd/hibernate.conf` 与发行版 resume 配置；不新增用户态二进制。
- **引导器 cmdline 自动配置 `resume=`**：`mwvd-hibernate-setup` 写完 initramfs
  配置后，自动把 `resume=UUID=<swap uuid>` 写入引导器——BLS 条目
  （`/boot/loader/entries/*.conf`）、`/etc/kernel/cmdline`（kernel-install 模板）
  与 `/etc/default/grub` 的 `GRUB_CMDLINE_LINUX`；没有 BLS 时再用 `update-grub` /
  `grub2-mkconfig` / `grub-mkconfig` 重新生成使其立即生效。逐文件保留
  `.mwvd.bak` 备份、内容未变化时不重写、旧 `resume=` token 会被替换而不是叠加；
  只有三条落点都写不进去时才提示人工添加。`MWVD_HIBERNATE_MANAGE_BOOT=0`
  可关闭自动改写。Fedora 44 + dracut 实测：自动写入后重启，`/proc/cmdline`
  带 `resume=UUID=`、`/sys/power/resume` 指向固件声明的 swap 分区，
  `systemctl hibernate` → 重新上电后 uptime 延续（S4 恢复成功）。
  Ubuntu 20.04 + initramfs-tools 实测：`GRUB_CMDLINE_LINUX=""` 空值起步被正确
  改写为单一 `resume=UUID=`（`grub.cfg` 各 kernel 条目同步、备份在
  `/etc/default/grub.mwvd.bak`、`conf.d/` 只有 `mwvd-resume.conf`），二次执行
  不叠加；重启后 `/proc/cmdline` 生效，`systemctl hibernate` → 固件
  `Hibernate wake: SLEEPING mark found` → `mark cleared`，内核
  `Restarting tasks ... done`、uptime 延续（S4 恢复成功）。

### 修复

- **休眠标记的镜像路径改为固件条目路径**：此前写 `vd_file_name`（宿主侧挂载
  路径），与 BootUI 菜单条目的 `\` 分隔绝对路径匹配不上，休眠唤醒时固件只会
  打印 `Hibernate wake: image unavailable, sleep state cleared.` 并清标记。
  现在保存 v2 条目的镜像路径（`vd_fw_path`），写标记时统一转换为 `\` 分隔的
  绝对路径；无固件路径时不写标记（固件按正常流程启动，内核仍可凭 initramfs
  的 `resume=` 恢复）。已在 Ubuntu 20.04 + 云固件 QEMU 环境连续两轮
  S4→唤醒验证通过。
- **initramfs-tools 备份不能放在 `conf.d`**：initramfs-tools 会 source
  `conf.d/` 下的所有文件，旧版本留下的 `mwvd-resume.conf.mwvd.bak` 会被当成
  配置，导致旧 `RESUME` 覆盖新值。备份改到 `/var/backups/mwvd/`，并清理旧版
  遗留的 `conf.d/*.mwvd.bak`（清理后强制重建 initramfs）。
- **`setup.sh` 的模块版本号不再硬编码**：原先固定为 `2.0.0`，导致
  `/usr/src/mwvd-2.0.0` 与 DKMS 条目 `mwvd/2.0.0` 和实际模块版本（2.7.0）
  不一致（Arch 实测命中）。改为从 `main.c` 的 `MWVD_MODULE_VERSION` 提取
  （与 Makefile 同源），取不到时直接报错退出。
- **`setup.sh` 补上可执行位**：仓库中此前是 100644，`git archive` 出来的包里
  不能直接 `./setup.sh`（只能 `bash setup.sh`）。
- **`setup.sh` 先切到脚本目录**：脚本内部全部用相对路径（`./mwvd.rules`、
  `cp -a ./*`），从别处调用（如 `sudo bash /root/mwvd-2.7.0/setup.sh`）会
  找不到文件、拷出空的 `/usr/src/mwvd-<版本>/`（Arch 实测命中）。
- **`setup.sh` 兼容 dkms 3.x 的模块列表格式**：Fedora 44 的 `dkms status` 输出
  是 `mwvd/2.7.0, <内核>, x86_64: installed`，而脚本只按 `mwvd,` 匹配
  （dkms 2.x 格式，Ubuntu 20.04 如此）⇒ 旧版本永不被清理（实测装完 2.7.0 后
  仍残留 `mwvd/1.2.0`、`mwvd/2.3.8`），幂等判断也失效（重复执行会走
  `dkms add` 并报错）。现在同时兼容 `mwvd, 2.7.0, ...` 与 `mwvd/2.7.0, ...`。
- **dracut 适配器只重建当前内核的 initramfs**：原 `dracut -f --regenerate-all`
  在 Fedora 44（dracut 108）上单次约 10 分钟（rescue + 每个内核一个），且对
  没有 mwvd 模块的内核必然失败（`dracut-install: Failed to find module 'mwvd'`）
  而整体退出码仍为 0。改为默认 `dracut -f`；需要全量时设
  `MWVD_DRACUT_REGENERATE_ALL=1`。实测 10 分钟 → 23 秒，其余内核的 initramfs
  不再被触碰。
- **先写引导器 cmdline，再重建 initramfs/UKI**：原实现先跑适配器（重建）再写
  `resume=`，对 **UKI 类引导**（Arch 的 `EFI/Linux/*.efi`、systemd-boot）无效——
  UKI 会把 cmdline **内嵌**进镜像，重建时读到的还是旧 cmdline（实测：
  `/etc/kernel/cmdline` 已带 `resume=`，但 UKI 的 `.cmdline` section 里没有）。
  现在 `mwvd_adapter_apply` 先配置 cmdline（三道落点）再调适配器重建；
  Fedora（BLS）下顺序调换无副作用（主机端 24 项单测全过）。
- **`mwvd_exec_used_rw` 定义移出 6.10 条件块（低版本内核编译失败）**：该静态
  变量原先定义在 `#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)` 内，而
  `mwvd_code_make_exec()` / `mwvd_code_free_exec()` 在低版本内核上同样引用它
  ⇒ Ubuntu 20.04（5.15.0-139）DKMS 构建报
  `'mwvd_exec_used_rw' undeclared`，`dkms install` 失败、模块根本没装上。现改
  为无条件定义（低版本恒为 0），6.10+ 行为不变。

## [2.6.0] - 2026-10-18

### 新增

- **ACPI 设备 ID 门控（`_HID = MWVD0001`）**：模块加载时精确检查固件发布的
  ACPI 设备；无设备时驱动仍正常加载、`/dev/mwvd-control` 保留，但：
  - 不创建/绑定任何虚拟磁盘，`max_mwvd` 预创建也被跳过；
  - `mwvd_monitor()` 在任何 `schedule_delayed_work()` 之前退出，不产生后台轮询；
  - 自动挂载配置中的无效 `part_uuid`（空、`<null guid>`、非 36 字符）条目直接跳过，
    避免 UUID 永久不存在时每 100ms 无限重试。

## [2.5.8] - 2026-10-18

### 新增

- **固件 `MWVD` 代码载荷支持 i386 / arm64**：按载荷头 `Arch` 字面量
  （`x64` / `x86` / `arm64`）选择目标架构；i386 走 ELF32 `ET_REL`（`SHT_REL`，
  `R_386_32` / `R_386_PC32`），arm64 走 ELF64 `ET_REL`（`SHT_RELA`，
  含 `dc cvau` / `ic ivau` 指令缓存刷新与 `CALL26` 远调用 trampoline）。
  i386 的 `__divdi3` / `__divmoddi4` / `__moddi3` / `__udivdi3` / `__umoddi3`
  由 GPL 层按 libgcc 语义实现（内核通常不导出这些符号）。
  无法加载时仍回退到链接期内置核心，与 x86_64 行为一致。

### 修复

- **arm64（`CONFIG_VMAP_STACK=y`）验签输出缓冲**：`u8 out[256]` 位于内核栈，
  `sg_init_one()` 对其调用 `virt_to_page()` 会得到非法页，RSA 验签在
  `flush_dcache_page()` 触发 level 0 translation fault；改为
  `kmalloc(MWVD_CODE_SIG_SIZE)` 并在请求清理路径释放。

## [2.4.0] - 2026-09-21

### 新增

- **固件声明的休眠（S4）存储分区（v2 通讯扩展记录）**：按
  `mw-docs/specs/COMMUNICATION_V2_SPEC.md` **v2.5 §6.7**，从固件 v2 配置块的
  **启动条目**载荷 `ExtraData` 区解析类型化扩展记录（TLV，首项 `0x0001` = 休眠存储
  分区，上游为固件 `mw.config` 的 `hibernate <partuuid>`）。用途：swap/resume 部署免
  手抄 UUID 与运行时校验。
  - 新增 `mwcfg_ext.c` / `mwcfg_ext.h`（GPL 层，纯 C、无内核依赖，故主机端测试可
    直接复用同一份实现）：`mwcfg_ext_get_hibernate(blob, &out)` 逐条扫描 TLV，只认
    启动条目（`ENTRY_FLAG_BOOT_ENTRY`），任何畸形（声明越界 / `Size` 非法 / 末尾残余）
    均安全放弃且不越界读；`part_uuid` 统一归一化为小写（供 `/dev/disk/by-partuuid/` 匹配）。
  - 模块加载时解析一次（`mwvd_monitor`，须在解除 blob 映射前），串口打印结果。
  - 新增 `/dev/mwvd-control` ioctl **`MWVD_CTL_GET_HIBERNATE`（0x4C83）** 与
    `struct mwvd_hibernate`（`valid` / `usage` / `resolved` / `part_uuid` / `device_path`）：
    查询固件声明值，并尝试解析为具体块设备路径（复用 `mw_resolve_device_by_uuid`）。
  - `mwctl` 新增 **`-H`**：打印固件声明的休眠分区与对应设备（未声明时明确提示）。
- **固件只声明意图**：是否真为合法 swap、容量是否足够等语义判断由部署工具负责，
  本模块不做假设（规范 §6.7 生产者规则 4）。

### 变更

- **构建/发布/跨内核检查纳入 `mwcfg_ext.c`**：`mwvd-objs`、`make release` 复制清单、
  `make check` / `check-one` 的 GPL 层清单同步；临时检查目录展平复制
  `common/mwcfg_v2.h`（`mwcfg_ext.h` 引用）。
- 版本号 `2.3.8` → `2.4.0`（新增接口与能力，属次版本变更）。

### 验证

- **跨内核编译检查**：`make check` → 23/25 内核通过（linux-6.6 / linux-7.2 为
  **源码树未配置**（无 `.config`）导致，与本次改动无关）。
- **主机端跨端验证**：`MultiwarePkg/Utilities/MwCfgExtTest`（ASAN+UBSAN）直链本仓
  **真实** `mwcfg_ext.c` 与固件镜像构建：含记录 / 未配置 / 仅附加盘条目 / magic 不符 /
  `ExtraDataSize` 越界 / `Size` 为 0 或非 4 倍数或越界 / 未知类型在前 / 仅未知类型 /
  大写归一化 共 17 项全通过（全套 63 项）。
- 未覆盖：模块**加载后**的 ioctl 运行时行为（本机无可用内核头文件，需在目标机上
  `insmod` + `mwctl -H` 复核）。

## [2.3.8] - 2026-08-25

### 新增

- **`mwctl -l` 设备列表命令（新增 `MWVD_GET_STATUS` ioctl）**：新增内核
  `MWVD_GET_STATUS`（0x4C02）与 `mwvd_status` 结构体，返回设备号 / 状态 /
  标志 / 镜像大小 / 镜像路径；`mwctl` 新增 `-l` 列出所有 mwvd 设备状态。
  严格校验设备名（`mwvd`+纯数字，排除 `mwvd0p1` 分区节点误匹配），
  避免一条挂载重复输出多条记录。
- **`mwctl` 工具全面增强**：`-c` 自动分配设备并关联镜像（优先复用空闲设备，
  无空闲自动新建；`open_wait` 等待 udev 异步创建设备节点）、`-d` 解绑并注销
  设备（按需释放设备号）、`-l` 列表。
- **跨架构构建支持（Makefile / Makefile.dkms）**：prebuild 支持 `ARCH` /
  `CROSS_COMPILE` / `O` 透传与跨架构构建；i386 显式 `-m elf_i386` 仿真链接
  闭源层；非 x86 系（ARM64 / LOONGARCH64）仅产出 plain 变体（无 retpoline /
  IBT 概念）；DKMS 下 i386 归一化到 `prebuild/i386`（不再并入 x86）。

### 修复

- **5.15 上 `mwctl -d` 进程被杀死（Killed）、设备残留**：`mwvd_remove` 直接
  `del_gendisk → blk_mq_free_tag_set → put_disk`，而 5.14~5.19 的
  `loop_remove` 在释放 tag_set 前须先 `blk_cleanup_queue`；缺此步导致
  `put_disk → disk_release → blk_put_queue → blk_mq_release` 访问已释放的
  `hctx->tags` → UAF Oops，进程被内核杀死、设备停在 RUNDOWN 残留。
  修复：5.14~5.19 分支在 `blk_mq_free_tag_set` 前补 `blk_cleanup_queue`
  （6.0+ 块层已重构且有 `.free_disk` 回调，无需此步，保持与 6.x loop 一致）。
- **5.15 detach 时分区重扫满屏 EIO**：`__mwvd_clr_fd` 已释放 backing 后，
  partscan 分支 `bdev_disk_changed` 重扫分区，因容量仍为旧值而尝试读完整
  分区表（sector 0/24）→ 全部 `I/O error`、`unable to read partition table`，
  旧分区不随 detach 移除。修复：重扫前 `set_capacity(vd->vd_disk, 0)`
  （对齐 loop `__loop_clr_fd`），容量归零后重扫仅移除旧分区、不再读 backing。
- **5.10/5.15 启动/关闭空指针解引用 Oops**：`__mwvd_clr_fd` 先置
  `vd->vd_device = NULL`，`<6.0` 分支再 `invalidate_bdev(vd->vd_device)`
  → `invalidate_bdev(NULL)` 内核 Oops。修复：入口保存 bdev 副本、置 NULL 后
  `if (bdev) invalidate_bdev(bdev)`（对齐 loop `__loop_clr_fd`）。
- **configure partscan 日志误报 0**：`pr_info` 在 `vd_flags |= PARTSCAN` 之前
  执行，`partscan=%d` 恒为 0（即使 `max_part=8 → part_shift=4` 实际已启用分区
  扫描），误导调试。修复：先计算 `partscan` 再打印。
- **旧内核模块加载报 `Unknown symbol efi_status_to_err`**：`efi_status_to_err`
  在 5.10/5.15 未 `EXPORT_SYMBOL_GPL`（6.0 才导出），模块加载期报 `err -2`。
  修复：模块自实现 `mwvd_efi_status_to_err`（映射表与内核 efi.c 一致）。
- **GD_SUPPRESS_PART_SCAN backport 保护**：发行版（如 Ubuntu 5.15.0-139）已
  backport `GD_SUPPRESS_PART_SCAN`（genhd.h），上游 5.15~6.0 未引入 → 原
  `#define` 会 `-Wredefined`。修复：`#ifndef GD_SUPPRESS_PART_SCAN` 保护，
  clear/set 统一走 `GD_SUPPRESS_PART_SCAN`。
- **编译告警清理**：`mwvd_monitor` 栈上 `vdf_efi_result entries[]`（≈368B×4）
  与 `vd_flush_vdf_state` 栈上 `fsegs[32]`（768B）触发
  `-Wframe-larger-than=1024`；分别改静态数组 / 动态分配（`kmalloc_array` +
  错误路径 `kfree`）。

### 变更

- 版本号 2.3.7 → 2.3.8（修订号提升：错误修正 + 新增列表命令）。
- `check_parent.py` 迁移至 `tools/`（辅助脚本归入 `tools/`）；README 的
  mwctl 用法（`-c` 自动分配 / `-l` 列表 / `-d` 解绑注销）与目录结构同步更新。

## [2.3.7] - 2026-08-21

### 修复

- **差分盘 flush 脏段数 >32 数据丢失（Windows 关机/重启 0x7A 根因，Linux 同步适配）**：
  差分 VHDX 一次写请求可同时弄脏超过 32 个 chunk 的 bitmap + BAT，而核心库旧实现
  `vdf_flush` 单次最多生成 32 段、`vdf_flush_done` 无条件清全部 dirty → 未落盘脏段
  静默丢弃。核心库已升 `1.0.1`（游标分页 + `vdf_flush_reset`，见 `core/CHANGELOG.md`）。
  本模块侧同步：
  - **`vd_flush_vdf_state` 改为游标分页循环**：`vdf_flush` 循环调用直至返回 0，再
    `vdf_flush_done`；任一段写失败调 `vdf_flush_reset`（保留 dirty、重置游标）并返回
    错误 → 下次 FLUSH/重试从起点重发全部段，瞬时写失败可真正落盘，绝不静默成功；
  - **补短写检测**：`kernel_write` 返回正值但 < 段长视为 `-EIO`（此前仅查负值）；
  - **`mwimg_flush` 同步改造**（用户态复刻工具一致行为）。
- **日志重放段写失败不再丢失**：核心库日志重放段 dirty 保留到 `flush_done`，写失败重试
  时重放段仍完整。
- **多层差分链（>2 层）祖父镜像打不开（vd_mon 自动加载路径）**：`vd_open_parent_image`
  递归到祖父级时 `pv->root_path` 为 0（`kzalloc` 未继承），退化为 `filp_open` 按全局
  命名空间绝对路径查找；而 vd_mon 用 `vfs_kern_mount` 创建的私有内核挂载未挂入命名
  空间，该路径全局不可见 → 父（depth 0，走 `root_path`）能开、祖父（depth ≥1）打不开。
  修复：父设备继承子设备 `root_path`（`path_get`），所有错误路径与 `vd_close_image`
  释放（`path_put`）；父链成功日志加 `depth` 标注，消除“父变爷爷”误读（递归先打开
  最深层，祖父先于父打印）。

### 变更

- 版本号 2.3.6 → 2.3.7（修订号提升：错误修正）；核心库 `1.0.0` → `1.0.1`。

## [2.3.6] - 2026-08-16

### 新增

- **引入 mwvd-core 核心库与 mw-common 协议头（多平台单一事实源）**：虚拟盘格式层
  （`vdf_*.c`）独立为 mwvd-core 核心库（`core/`），固件↔OS 通讯协议头独立为 mw-common
  （`common/mwcfg_v2.h`），均经 git subtree 引用；构建/引用路径相应调整
  （`-I$(src)/core -I$(src)/common`、`vd.o` 合并规则、`CFLAGS_REMOVE_core/vdf_*.o` 等）。
- **核心库版本识别**：模块启动时打印驱动版本与核心库版本
  （`mwvd: module 2.3.6 loaded (core 1.0.0)`）；核心库提供 `vd_core_version()` /
  `vd_core_version_code()` 运行时查询接口（`VD_CORE_VERSION_*` 宏，当前 1.0.0）。

### 变更

- 版本号 2.3.5 → 2.3.6（修订号提升：采用核心库 + 版本识别）。

## [2.3.5] - 2026-08-15

### 修复

- **休眠标记写入布局与 CRC 算法修正（物理机实测休眠唤醒不触发根因）**：
  休眠标记 `MwHibernateState` 由内核写入、固件 BootUI 消费。物理机实测：efivar 有
  标记（SLEEPING）但 BootUI 不自动唤醒、回主界面，手动点镜像可恢复。静态审核 +
  端到端验证定位内核侧两处违反 `HIBERNATE_SPEC.md` §5 固定布局/校验规则：
  - **结构未 1 字节对齐**：`struct mw_hibernate_mark` 未 `__packed`，x86-64 下
    `u64 timestamp` 8 字节对齐在 timestamp 前插入 7 字节 padding → `sizeof=336`
    （规范 325）、`offsetof(crc32)=328`（规范 321）。写入 `sizeof(mark)`=336 字节 →
    固件 `GetVariable(325)` 返回 `EFI_BUFFER_TOO_SMALL` → 标记读取失败 → 唤醒不触发。
    修复：结构加 `__packed`（`struct __packed mw_hibernate_mark`）。
  - **CRC 用 `crc32_be` 与固件端不匹配**：`crc32_be(0,...)`（0x04C11DB7 大端、初始 0、
    无最终异或）≠ 固件 `gBS->CalculateCrc32`（标准 IEEE CRC-32，0xEDB88320、初始/终
    异或 0xFFFFFFFF）。即使布局修对，CRC 也必然不匹配（固件报 `EFI_COMPROMISED_DATA`）。
    修复：改用 `crc32_le(0xFFFFFFFF, ...) ^ 0xFFFFFFFF`（= 标准 IEEE CRC-32，与固件一致）。
  - **补编译期断言**（规范 §5.3 要求，此前缺失）：`_Static_assert(sizeof == 325)` +
    `offsetof(crc32) == 321`；新增显式常量 `MW_HIBERNATE_MARK_SIZE`。
  - 规范 `HIBERNATE_SPEC.md` §5.4 同步明确"标准 IEEE CRC-32、禁止 crc32_be 变体"（v1.0.1）。
- **休眠标记清除对 `EFI_NOT_FOUND` 静默（S4 未真正断电场景去误导告警）**：
  `mwvd_hibernate_mark_clear()` 删除标记时，固件返回 `EFI_NOT_FOUND`（-ENOENT，变量已
  不存在 = 删除目标已达成）即视为清除成功并静默返回，不再 `pr_warn`。场景：物理机"进入
  S4 但主机未真正断电、同会话被唤醒"——实测 write 成功后 `PM_POST_HIBERNATION` 清标记
  返回 -2，但标记实际仍被 BootUI 读到并正确直接唤醒（桌面完整恢复），属固件删除操作的
  `EFI_NOT_FOUND`，非功能性错误；其余错误仍告警。

### 变更

- 版本号 2.3.4 → 2.3.5（修订号提升：错误修正）。

## [2.3.4] - 2026-08-15

### 修复

- **0x7E00 回退通道加非 x86 架构保护**：`vd_get_config_blob()` 在 ACPI 表 MWCF 缺失
  时，仅 IA32/X64（`__x86_64__` / `__i386__`）回退读取 0x7E00 固定物理地址；非 x86
  （ARM64 / LOONGARCH64 / RISC-V）无低端实模式内存约定，0x7E00 物理地址无效，
  直接返回 NULL，避免映射无效地址。ACPI 表为全架构统一主通道（v2.4），
  非 x86 不依赖 0x7E00 兼容通道。

### 变更

- 版本号 2.3.3 → 2.3.4（修订号提升：错误修正）。

## [2.3.3] - 2026-08-15

### 新增

- **MWCF ACPI 表通道（v2.4 全架构统一主通道）**：固件 v2.4 起以 ACPI 自定义表
  `MWCF` 作为全架构定位通道（x86 / ARM64 / LOONGARCH64 / RISC-V），经
  `acpi_get_table("MWCF")` 获取表内 blob 物理地址；`vd_get_config_blob()` 优先
  ACPI 表，失败回退 0x7E00 固定物理地址（x86 兼容通道）。blob 仍位于
  `EfiRuntimeServicesData`（ExitBootServices 后内存图保留）。
- **`mwcfg_v2.h` 新增 `MW_ACPI_CONFIG_TABLE` 布局**（56B = ACPI 标准头 36B +
  Magic + ChannelVersion + BlobPhysAddr + BlobSize），表内不冗余存 blob CRC
  （blob 完整性由头部 CRC + 逐条 CRC/HMAC 覆盖）。

### 变更

- 版本号 2.2.3 → 2.3.3（次版本提升：新增 ACPI 表 MWCF 通道功能；2.2.x 系列含
  此前 ACPI 通道实现但未单独记录）。

## [2.2.3] - 2026-08-13

### 新增

- **休眠（S4）支持**（云固件虚拟盘承载 rootfs 场景；设计说明书
  `design/HIBERNATE-DESIGN.md`）：
  - 全局 `mwvd_pm_suspended` 系统级 PM 状态标记：`PREPARE` 置 / `POST` 清，
    失败回滚由 `POST` 覆盖（内核无 `PM_*_FAIL` 事件，失败路径仍显式触发 POST）。
  - PM notifier `POST` 分支：遍历 idr 全部 vd 重放 VHDX log
    （`vd_replay_pending_log` 由 void 改为返回错误码），per-device 容错——
    失败仅告警并置设备级 `pm_replay_failed`，不影响其余设备。
  - 设备级 `pm_replay_failed`（`vd.h` 新字段）：I/O 路径驱动内先重试 replay，
    成功清标记放行、重试失败返回 -EIO（对齐 NVMe/SCSI 尽力恢复惯例）。
  - `vd_flush_all` 对只读镜像跳过 flush/fsync。
  - **休眠标记（EFI 变量 `MwHibernateState`）**：内核侧先行定义权威布局
    （GUID e7c9f0a5-…，`part_uuid`+`vdisk_path` 定位镜像），S4 前写 / 恢复后清；
    RT 服务失败降级为继续休眠 + 告警。
  - `mwvd_device` 增加 `vd_part_uuid`：自动加载路径保存，供休眠标记定位镜像。
- **QEMU 休眠验证操作手册**：`design/QEMU-HIBERNATE-TEST.md`（阶段 1 纯内核 /
  阶段 2 MWVD 完整路径可复现流程，QEMU 下已实测通过）。

### 修复

- **修复 S4 恢复后 `mwvd_pm_suspended` 残留致命缺陷**：移除挂起期 I/O 拒绝检查
  （S4 恢复跳回快照不触发 `PM_POST_RESTORE`，该标记残留 true 会导致恢复后
  rootfs 不可访问）。
- **修复 POST 分支 replay 与并发 I/O 竞争**（`vdf_state` 无锁竞争）：改为
  per-device workqueue 串行执行 replay，与正常 I/O 同 worker 互斥。
- **统一休眠标记载荷布局**：`part_uuid` 由 `[64]` 改为 `[37]`，对齐设计说明书
  权威布局与 v2 契约 `MW_CFG_PAYLOAD`（PartGuid[37]+VdiskPath[260]），消除跨端
  （内核 vs 固件）布局不一致。
- **修复恢复失败回退标记残留**：`PM_POST_RESTORE`（恢复失败回退）也清休眠标记，
  避免 BootUI 下次启动误判仍有休眠而重复进入唤醒路径。
- **休眠标记写入锁内拷贝 + 解绑清理**：idr 遍历与字段拷贝移入 `mwvd_ctl_mutex`
  （防设备并发解绑悬垂），解绑时清 `vd_part_uuid`（与 `vd_file_name` 一致）。

### 变更

- 版本号 2.1.3 → 2.2.3（次版本提升：新增休眠 S4 支持）。
- 工具统一命名：`mwsetup` 改名为 `mwctl`。
- 跨内核验证：5.10 / 6.6 / 6.18 编译通过。

## [2.1.3] - 2026-08-13

### 新增

- **按需生成设备（ondemand）**：默认不再预创建 `mwvdN` 设备节点，镜像到来时
  动态创建 `mwvdN`、解绑时动态注销设备，避免固定预创建设备数（旧 `max_loop=4`）。
  可通过模块参数 `max_mwvd` 指定预创建数量（或作为初始设备数）。
  新增 `mwvd_mon_load_lock` 互斥锁串行化 monitor 按需加载（取空闲设备 + 绑定），
  避免并发镜像条目取到同一设备。

### 变更

- 版本号 2.0.3 → 2.1.3（次版本提升：新增按需生成设备功能）。

## [2.0.3] - 2026-08-13

### 修复

- **修复 VHDX 差分盘 DISCARD 部分释放整块导致的数据丢失**：fstrim/TRIM 只覆盖
  VHDX 块的一部分时，DISCARD 路径无条件把整块 BAT 条目置为 `NOT_PRESENT`，但只清
  理 bitmap 中丢弃的子区间 → 块内未 discard 的在用扇区 bitmap 仍置位，读路径对
  `NOT_PRESENT + bitmap=1` 返回 ZERO → 在用 ext4 元数据（块位图 / 目录索引 /
  Inode）被读成零，apt upgrade 时文件系统损坏、grub 引导失败。
  修复：仅当 discard 覆盖整块才释放 BAT 条目；部分 discard 只清 bitmap（+ punch
  hole），块保持分配，未 discard 扇区继续从子镜像读取。

### 变更

- 版本号 2.0.2 → 2.0.3。

## [2.0.2] - 2026-08-11

### 修复

- **修复 VHDX 差分盘 bitmap LRU 淘汰后重载清零导致的数据丢失**：
  `vhdx_bitmap_lazy_load` 用挂载时固定的 `s->file_size` 判断块是否已在文件内；
  会话内新分配的 bitmap 块偏移必然 ≥ 挂载时文件大小，LRU 淘汰后重载会把已落盘的
  bitmap 误判为稀疏而零初始化，flush 时清掉已标记扇区位 → 读回退父镜像丢数据
  （Linux 升级写新内核后读取异常、grub 引导失败）。
  修复：移除 `fo < s->file_size` 过滤，读回调对稀疏空洞 / 越界自然返回 0 / 短读，
  补零后即正确的全 0 语义。

### 变更

- 版本号 2.0.1 → 2.0.2。
- 新增回归测试 `mwimg/lru_test.c`（单会话 LRU 淘汰复现）、`mwimg/bvec_write_test.c`
  （内核 bvec 写路径模拟）。

## [2.0.1] - 2026-08-09

### 新增

- **闭源层 vd.b 多变体机制**：`prebuild/x86/` 下按目标内核缓解配置生成两个预编译变体
  - `vd.retpoline.b` — 保留 retpoline、return 用普通 ret（面向 RETPOLINE 内核，含 5.10~5.18 与 5.19+）
  - `vd.plain.b` — 普通 ret + 普通间接调用（任意内核兜底，`vd.b` 为其兼容别名）
- **`Makefile.dkms` 构建期变体选择**：每次内核升级 DKMS 重新构建时，按目标内核配置
  （`CONFIG_RETPOLINE` / `CONFIG_MITIGATION_RETPOLINE`）自动选择匹配变体；
  所选变体缺失时**兜底回退 `vd.plain.b`**，保证任何内核都能构建成功、不会加载失败。
- **`prebuild` 变体感知校验**：对每个变体校验符号引用（`retpoline` 必须引用
  `__x86_indirect_thunk_*` 且不得引用 `__x86_return_thunk`、`plain` 不得引用任何缓解
  符号）、ENDBR64 存在、无 `__fentry__`，防止回归。

### 修复

- **修复加载期内核警告** `Unpatched return thunk in use. This should not happen!`：
  此前闭源层 vd.b 引用了 `__x86_return_thunk`/`__x86_indirect_thunk_*` 但缺少对应注解，
  在开启 RETHUNK/RETPOLINE 缓解的内核上触发该 WARN_ONCE。经实测，预编译 vd.b 的
  `.return_sites` 由**目标机** objtool 在最终链接时生成、跨机不可靠（6.17 上即使选中
  return-thunk 变体仍复现该警告），因此闭源层**一律不引用 `__x86_return_thunk`**（return
  用普通 ret），从根本上消除该警告，与目标机 objtool 行为无关。
- **修复 `prebuild` 目标 shell 语法错误**（`expecting done`）：`\` 续行 recipe 中混入
  `#` 注释行导致 bash 将后续逻辑行整体注释；注释已移出 recipe，置于 Make 注释层。

### 变更

- 版本号 2.0.0 → 2.0.1。
- 闭源层所有变体统一编译底座：保留 `-mcmodel=kernel`、`-fcf-protection=branch`
  （入口 ENDBR64，IBT 内核安全、非 IBT 内核为 NOP）、`-fno-stack-protector`；
  统一剔除 ftrace 钩子（`-pg`/`-mfentry`/`-mrecord-mcount` 等），不再依赖 `__fentry__`，
  提升对关闭 ftrace 内核的兼容性。

## [2.0.0] - 2026-08-08

首个纳入本 CHANGELOG 的版本。主要特性与修复（依据开发历史整理）：

### 新增

- **固件通信协议 v2**（v2 config blob）：`vd_abi.h` 定义 GPL↔闭源层通信协议，
  闭源层 `vdf_parse_efi_blob_v2()` / `vd_acquire_config_blob()` 完成配置数据块获取与解析。
- **多镜像自动挂载**：`mwvd_monitor` 监控固件配置，自动加载并挂载多个虚拟磁盘镜像。
- **格式支持**：VHD / VHDX / RAW，含差分镜像；新增 VHD 审计报告与 VHDX 格式审查报告。
- **benchmark 测试程序**。

### 修复

- **内核兼容性**：修复 7.1 内核兼容性问题；修复跨内核版本兼容问题；修复跨架构大小端错误。
- **格式缺陷**：修复 VHD 差分代码问题、VHDX 差分格式缺陷、bitmap 错误、4K 扇区对齐与
  关闭镜像缺陷、区块状态处理逻辑、metadata flag 判断、write_zero 错误、raw 初始化值错误。
- **I/O 与设备**：修复差分镜像读取次数超限导致的 I/O 内核错误提示；修复镜像打开失败后
  设备无法再使用的问题。
- **挂载**：增强文件系统挂载失败的处理机制；修复多镜像挂载参数错误、partuuid 变量长度错误。
- **稳定性**：修复多线程问题与两处内存泄漏；修复 fstype 引用泄漏。
- **构建**：修正 vd.b 创建时丢失符号的编译错误。

### 变更 / 加固

- 修复私密代码函数名和路径泄漏问题；移除发布包中的 `vdf_string.h` 内部头文件（闭源保护）。
- 注释统一规范到规范原文的章节编号。
