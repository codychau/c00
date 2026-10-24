# 家用服务器工具箱 (Home Server Toolbox)

一个基于 Qt6 的 Linux 桌面工具，用于管理家庭服务器的各种功能。

## 功能

- **📊 状态仪表** — 系统资源监控（CPU、内存、磁盘、网络）
- **🎬 场景设定** — 按时间段自动启动 / 停止指定服务与虚机
  - 场景增删改，可为每个场景设置开始 / 结束时间与重复星期
  - 不同场景可分别控制多项系统 / 用户服务，以及虚机管理中的虚机
  - 定时执行需要保持工具箱运行，配置保存在 `~/.config/ServerToolbox/scenes.toml`
- **💾 存储管理** — 块设备树形查看、分区挂载、S.M.A.R.T 健康检测
  - **就绪状态检测** — 自动比对 `/etc/fstab` 与实际挂载点，显示分区「就绪/未就绪/未配置」状态
  - **隐藏 loop 设备** — 默认过滤 ISO、Snap、AppImage 等 loop 镜像挂载条目，可勾选显示；刷新时尽量保留当前选中的真实设备
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

工具提供 `servertoolbox-watchdog` 系统服务（root 权限，开机自启）。它周期探测 nginx 端口；一旦 nginx 拒绝连接，立即对 SSH 做**协议握手**验证。当 **nginx 拒绝连接且 SSH 无法完成握手**（连接被拒、卡死无响应或超时，即系统 userspace 异常但内核尚存）时，会自动重启本机。

- SSH 健康判定以收到 `SSH-...` 协议标识串为准，而非仅 TCP 连通：sshd 卡死或系统挂起时内核仍可能替监听套接字完成三次握手，只看端口会漏判。
- 触发前会检查**重启冷却期**（`cooldown_sec`），避免频繁重启。
- 重启采用**分层降级**：先尝试 `systemctl reboot`（干净重启），若 15 秒内系统仍无响应，则通过内核 **Magic SysRq**（`sync → umount → reboot`）硬重启，绕过所有 userspace。
- 在「状态」页面点击 **🛡 安装看门狗** 可配置并安装该服务。安装对话框中可勾选 **同时配置内核级死机检测**，复用同一次 sudo 密码一并完成；还可设定 **机器中途休息**（每周定时优雅重启，见下节）；另提供 **🔄 测试硬重启** 按钮与 **♻️ 关闭内核检测** 入口。

### 内核级死机检测

安装看门狗时可一并开启内核级死机检测（写入 `/etc/sysctl.d/95-servertoolbox-hang.conf`，可随时通过「♻️ 关闭内核检测」移除）：

- `softlockup_panic` / `hardlockup_panic` / `panic_on_oops`：CPU 锁死（硬锁死走 NMI，关中断也能检测）或内核 oops 时触发 panic，配合 `kernel.panic = 10` 在 10 秒后自动重启。
- `kdump`：崩溃时由 crash kernel 抓取 vmcore 与内核日志到 `/var/crash/`（`crashkernel` 内存已在启动参数中预留）。
- `hung_task_panic`（默认关闭）：进程 D 状态卡死超过 120s 也自动重启；对磁盘/IO 卡死最有效，但可能因合法的大 I/O 误重启，需要时再勾选。

> 说明：内核级检测依赖内核自身的检测线程仍能运行；若整机彻底冻结（连 NMI 都无法进入），仍需硬件看门狗或外置断电复位。

### 机器中途休息（每周定时优雅重启）

安装看门狗时可勾选 **机器中途休息（每周定时优雅重启）**：选定每周的休息日（周一…周日，可多选）与一个时间段（`HH:mm → HH:mm`），看门狗会在**进入时间段的第一个检测周期**自动重启本机，用于在低峰时段让机器"休息"。

- 走优雅重启（`systemctl reboot`），15 秒未生效则回退内核级 Magic SysRq 硬重启。
- 每个休息日最多触发一次：执行日期写入配置的 `rest_last_run`，重启后守护进程随开机自启，不会在同一个窗口里循环重启。
- 起始时间晚于结束时间表示**跨午夜窗口**（如 22:00 → 02:00，归属起始日）。
- 配置项位于 `~/.config/ServerToolbox/health.toml`：`rest_enabled`、`rest_days`（ISO 星期，1=周一 … 7=周日）、`rest_start`、`rest_end`、`rest_last_run`。

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
