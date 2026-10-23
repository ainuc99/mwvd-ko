# MWVD 内核模块测试套件

## 文件结构

```
testcase/
├── README.md              # 本文件
├── run_all.sh             # 测试运行器（主入口）
├── test_common.sh         # 公共函数库
├── test_basic.sh          # 基础功能：模块加载/RAW 设备
├── test_vhdx.sh           # VHDX 格式专项
├── test_vpc.sh            # VHD/VPC 格式专项
├── test_partition.sh      # 分区表和文件系统
├── test_stress.sh         # 压力测试（可选，较慢）
├── test_hibernate_scripts.sh # 休眠自动配置 shell 脚本（无需 root/模块）
└── test_kernel_matrix.sh  # 多版本内核编译矩阵（纯载荷模式回归，无需 root）
```

## 前置条件

```bash
# 必需工具
sudo apt install coreutils fdisk e2fsprogs

# VHDX/VHD 镜像创建需要
sudo apt install qemu-utils

# 编译内核模块
cd .. && make
```

## 运行测试

```bash
cd testcase

# 运行所有测试
sudo ./run_all.sh

# 仅运行特定测试
sudo ./run_all.sh basic              # 基础功能
sudo ./run_all.sh vhdx               # VHDX 格式
sudo ./run_all.sh vpc                # VHD/VPC 格式
sudo ./run_all.sh partition          # 分区和文件系统
sudo ./run_all.sh basic vhdx vpc     # 组合

# 包含压力测试（较慢，约 5-10 分钟）
sudo ./run_all.sh -s

# 单独测试休眠自动配置脚本（无需 root、无需加载模块）
./test_hibernate_scripts.sh

# 查看 dmesg 日志辅助调试
sudo dmesg | tail -50
```

## 测试覆盖范围

| 模块 | 覆盖内容 |
|------|---------|
| `test_basic` | 模块加载/卸载、RAW 设备创建/读写、多设备并发 |
| `test_vhdx` | 固定/动态/差分 VHDX 格式、跨 block 边界读写、DISCARD |
| `test_vpc` | 固定/动态/差分 VHD 格式、高扇区号写入、父路径解析 |
| `test_partition` | fdisk MBR/GPT、mkfs.ext4、挂载/卸载、亲子数据隔离 |
| `test_stress` | 500 次随机 4K 写入、3 设备并发、20 次反复挂载、模块重载持久性 |
| `test_hibernate_scripts` | helper 分支/退出码、initramfs-tools / dracut / mkinitcpio 配置生成与幂等性 |

## 注意事项

- 需要 root 权限运行（`sudo`）
- 测试过程中 `/dev/mwvd*` 设备会被反复创建和销毁
- 压力测试会创建 ~256MB 的临时文件在 `/tmp`，需确保有足够磁盘空间
- VHDX/VHD **差分（differencing）测试**需要 qemu-img >= 8.0 支持。
  Debian/Ubuntu 的 qemu-img 6.2 尚不支持。差分测试会自动检测并跳过。
