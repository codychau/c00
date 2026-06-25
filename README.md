# 家用服务器工具箱 (Home Server Toolbox)

一个基于 Qt6 的 Linux 桌面工具，用于管理家庭服务器的各种功能。

## 功能

- **📊 状态仪表** — 系统资源监控（CPU、内存、磁盘、网络）
- **💾 存储管理** — 块设备树形查看、分区挂载、S.M.A.R.T 健康检测
- **🖥️ 虚机管理** — QEMU/KVM 虚拟机的一站式管理
  - 可视化编辑虚机配置（CPU、内存、磁盘、网络、端口转发等）
  - 创建/管理数据盘（支持 qcow2 创建）
  - 硬件直通（PCI 设备、Hugepages）
  - 自动启动
  - 一键启动/停止，VNC 连接
- **🎮 娱乐选项** — 音量及分辨率控制 [实验性]
- **🤖 AI 选项** — AI 对话
- **⚙️ 服务控制** — 管理系统服务
- **📋 日志管理** — 应用日志 + 系统日志查看
- **📂 文件挂载** — 磁盘设备挂载管理

## 依赖

- Qt6 (Core, Widgets)
- toml++ (C++17 TOML 解析库，已集成)
- QEMU (运行时，虚机管理需要)
- `systemd` (服务管理需要)

## 构建

```bash
# 安装依赖 (Arch Linux / Manjaro)
sudo pacman -S qt6-base cmake gcc

# 构建
mkdir build && cd build
cmake ..
make -j$(nproc)

# 运行
./server-toolbox
```

## 配置文件

虚拟机配置存储在 `~/.config/ServerToolbox/vms.toml`，使用 TOML 格式。

## 许可证

MIT License
