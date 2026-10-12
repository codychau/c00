# 家用服务器工具箱 (Home Server Toolbox)

一个基于 Qt6 的 Linux 桌面工具，用于管理家庭服务器的各种功能。

## 功能

- **📊 状态仪表** — 系统资源监控（CPU、内存、磁盘、网络）
- **💾 存储管理** — 块设备树形查看、分区挂载、S.M.A.R.T 健康检测
  - **就绪状态检测** — 自动比对 `/etc/fstab` 与实际挂载点，显示分区「就绪/未就绪/未配置」状态
- **🖥️ 虚机管理** — QEMU/KVM 虚拟机的一站式管理
  - 可视化编辑虚机配置（CPU、内存、磁盘、网络、端口转发等）
  - 创建/管理数据盘（支持 qcow2 创建）
  - 硬件直通（PCI 设备、Hugepages）
  - 自动启动
  - **等待挂载点** — 启动前主动挂载 fstab 中未就绪的设备，解决硬盘开机延迟挂载问题
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

## 看门狗自动重启

工具提供 `servertoolbox-watchdog` 系统服务（root 权限，开机自启）。当 **nginx 与 SSH 端口同时拒绝连接**（即系统 userspace 异常但内核尚存）时，会自动重启本机。

- 触发前会检查**重启冷却期**（`cooldown_sec`），避免频繁重启。
- 重启采用**分层降级**：先尝试 `systemctl reboot`（干净重启），若 15 秒内系统仍无响应，则通过内核 **Magic SysRq**（`sync → umount → reboot`）硬重启，绕过所有 userspace。
- 在「状态」页面点击 **🛡 安装看门狗** 可配置并安装该服务。安装对话框中还提供 **🔄 测试硬重启** 按钮，可直接验证内核级强制重启是否生效。

### 强制重启命令行

`server-toolbox` 支持 `--force-reboot` 参数，会直接执行上述内核级硬重启（需要 root 权限）：

```bash
sudo server-toolbox --force-reboot
```

> 说明：`--force-reboot` 通过写入 `/proc/sys/kernel/sysrq` 触发重启，仅在内核可响应时有效；非 root 下会因权限失败而不触发。

## 安装到本地

运行 `build-install.sh` 会自动安装系统依赖、编译并将程序安装到 `~/.local/bin`：

```bash
./build-install.sh
# 或（首次安装依赖时）
sudo ./build-install.sh
```

安装后通过 `~/.local/bin/server-toolbox` 运行。若该路径未在你的 `PATH` 中，可执行 `export PATH="$HOME/.local/bin:$PATH"`。

## 许可证

MIT License
