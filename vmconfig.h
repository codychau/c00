#ifndef VMCONFIG_H
#define VMCONFIG_H

#include <QString>
#include <QVector>
#include <QStringList>

// 磁盘配置 —— 系统盘与数据盘统一为一个列表:
// 标记 system 的条目是系统盘(启动盘), 其余为附加数据盘.
struct VMDataDisk {
    QString path;
    QString format = "qcow2";   // qcow2, raw, vmdk, vdi, vhdx
    QString cache  = "none";    // none, writeback, writethrough, unsafe
    QString aio    = "native";  // native, threads, io_uring
    QString iface  = "virtio";  // 每块盘各自的接口: virtio, sata, ide (系统盘与数据盘都可选)
    bool    system = false;     // 系统盘 (启动盘)
};

// 端口转发规则
struct PortFwd {
    int     hostPort  = 0;
    int     guestPort = 0;
    QString protocol  = "tcp";  // tcp, udp
};

// 单个虚机配置
struct VMConfig {
    QString name;
    int     cpu       = 2;
    int     memory    = 2048;   // MB
    QString iso;
    QString qemuBinary = "qemu-system-x86_64"; // QEMU 二进制
    // 以 root 运行 QEMU (经 sudo). PCI 直通 (/dev/vfio/*) 与 evdev 透传
    // (/dev/input/*) 都需要 root; 免密 sudo 由「QEMU 权限配置」写入 sudoers.
    bool    qemuRunRoot = false;
    int     vnc       = -1;     // -1 = 不启用

    // 从 extra 提炼的字段
    QString machine  = "q35";   // q35, pc, pc-i440fx-*
    QString cpuType  = "host";  // host, qemu64, pentium3, cortex-a72
    bool    kvm       = true;    // 启用 KVM 加速
    QString vga       = "virtio";// virtio, cirrus, qxl, virtio-gpu, none
    // VirtualBox: 显卡控制器 (--graphicscontroller). VBoxVGA / VMSVGA(即 VBoxSVGA) / VBoxGBA.
    // 勾选 3D 加速时强制 VMSVGA (VirtualBox 唯一支持 3D 的控制器).
    QString graphicsController = "vmsvga";
    QString net       = "user";  // user, bridge, tap
    QString nicModel  = "virtio-net-pci"; // 网卡模型

    // ── 虚拟机类型 / VirtualBox 专属 ──
    //  vmType: qemu (默认) | virtualbox
    QString vmType = "qemu";
    // VirtualBox: 目标 OS 类型 (ostype), 如 Windows98/WindowsXP/Windows7/Windows10
    QString ostype = "Windows7";
    // VirtualBox: 存储控制器后端. sata (默认, 通用). ide 仅历史兼容:
    // 7.2.6 的 CLI 中 IDE 控制器 storageattach 失效, 代码会自动回退 SATA.
    QString storageBackend = "sata";
    // VirtualBox: 新建磁盘时的容量 (MB), 仅建盘时使用
    int     vboxDiskSize = 20480;
    // VirtualBox: ACPI / IOAPIC 开关 (Win7+ 必须 on; 极老 OS 可关)
    bool    vboxAcpi = true;
    bool    vboxIoapic = true;
    // VirtualBox: 固件. true = UEFI (--firmware efi), false = BIOS.
    // 缺省 false 是为了让旧配置里的 VM 保持原样 (已用 BIOS 安装的盘换 UEFI 会无法启动).
    bool    vboxUefi = false;
    // VirtualBox: 显卡. 显存 MB (8~128, VirtualBox 上限 128), 3D 加速开关.
    int     vboxVram = 128;   // 显存 MB: 64~128 (无 3D), 勾选 3D 后最高 256
    bool    vbox3d   = false;
    // VirtualBox: RDP 远程控制 (VRDE, VirtualBox 内置 RDP 服务器). 仅 VM 运行时监听.
    bool    vboxRdp = false;
    int     vboxRdpPort = 3389;
    // VirtualBox: 网络模式. nat / bridged
    QString vboxNicType = "nat";
    // VirtualBox: 网卡型号 (nic-type1). Am79C970A / Am79C973 / 82540EM / 82543GC / 82545EM / virtio / usbnet
    QString vboxNicModel = "82545EM";
    // VirtualBox: bridged 模式下用的宿主机网卡名 (vboxNicType==bridged 时)
    QString vboxBridgeDev = "";

    // ── QEMU 固件 (UEFI / pflash) ──
    // 勾选后用两条 pflash 盘 (code 只读 + vars 可写) 代替 QEMU 内置 BIOS.
    // vars 必须每台 VM 独立, 且文件可写 (共享 vars 会互相污染启动项).
    bool    uefi     = false;
    QString uefiCode = "/usr/share/OVMF/x64/OVMF_CODE.4m.fd";
    QString uefiVars = "/usr/share/OVMF/x64/OVMF_VARS.4m.fd";

    // ── PCI 直通增强 (QEMU) ──
    // multifunction=on: 同一物理 slot 的多个 function (显卡 + 显卡音频) 必须落在
    // 同一 slot, 且命令行顺序 = function 递增顺序 (pciDevices 列表顺序即命令行顺序).
    bool    vfioMultifunction = false;
    // q35-pcihost.pci-hole64-size, 例 "512G"; 空 = 不设置. 仅 q35 系机器生效.
    // 显卡直通需要足够大的 hole64 供大 BAR 映射.
    QString pciHole64 = "";

    // ── 显示 (QEMU) ──
    // 无虚拟显卡的纯直通 VM 需要 -display none; 与 VNC 互斥 (vnc >= 0 时不输出).
    bool    displayNone = false;

    // ── 音频 (QEMU audiodev) ──
    QString audioBackend = "none";   // none, pa, pipewire, alsa, oss, sndio
    QString audioModel   = "hda-duplex"; // hda-duplex, hda-micro

    // ── USB 直通 (QEMU) ──
    bool        usbXhci       = false;  // qemu-xhci 控制器
    bool        usbKbdTablet  = false;  // 虚拟 usb-kbd + usb-tablet (远程注入用)
    QStringList usbHostVendors;         // 每项 "vid:pid", 如 "046d:c53f"
    QStringList usbEvdevPaths;          // evdev 路径, 走 virtio-input-host-pci

    // ── Root 钩子 (QEMU, 经 sudo 执行) ──
    // 用于单显卡直通的 GPU 归还/占用编排: 启动前绑 vfio-pci 并释放合成器,
    // 退出后回绑 nvidia 并重启合成器. 空 = 不执行.
    QString preStartRoot;
    QString postStopRoot;

    // 端口转发 (user 模式下生效)
    QVector<PortFwd> portForwards;

    // 磁盘列表 (系统盘 + 数据盘, 系统盘位于首位)
    QVector<VMDataDisk> disks;

    // aarch64
    bool ramfb = false;

    // 自动随管理器启动
    bool autoStart = false;

    // 尝试主动等待所有挂载点成功再启动
    bool waitMount = false;

    // 硬件直通
    QStringList pciDevices;     // PCI BDF 地址列表
    bool        hugepages = false;

    // 兜底 — 未被提炼的额外参数
    QString extra;

    // 系统盘条目索引 (-1 = 无系统盘)
    int systemDiskIndex() const {
        for (int i = 0; i < disks.size(); ++i)
            if (disks[i].system) return i;
        return -1;
    }

    // 系统盘路径 (无系统盘时为空)
    QString systemDiskPath() const {
        int i = systemDiskIndex();
        return i >= 0 ? disks[i].path : QString();
    }

    // 归一化: 只保留一个系统盘标记, 并把它挪到列表首位 (index=0 即启动盘)
    void normalizeDisks() {
        int sys = systemDiskIndex();
        if (sys > 0) {
            VMDataDisk d = disks[sys];
            disks.removeAt(sys);
            disks.prepend(d);
            sys = 0;
        }
        if (sys < 0) {
            if (!disks.isEmpty()) disks[0].system = true;
            return;
        }
        for (int i = 1; i < disks.size(); ++i)
            disks[i].system = false;
    }
};

// 虚机配置管理器 —— 读写 TOML 文件
class VMConfigManager {
public:
    explicit VMConfigManager(const QString &configPath);

    bool load();                   // 从文件读取
    bool save() const;             // 写入文件

    const QVector<VMConfig> &vms() const { return m_vms; }

    void addVM(const VMConfig &vm);
    void removeVM(int index);
    void updateVM(int index, const VMConfig &vm);

    QString configPath() const { return m_path; }
    static QString defaultConfigPath();

private:
    QString m_path;
    QVector<VMConfig> m_vms;
};

#endif // VMCONFIG_H
