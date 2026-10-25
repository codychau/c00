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
    QString iface  = "virtio";  // 系统盘接口: virtio, sata, ide (数据盘固定 virtio)
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
    int     vnc       = -1;     // -1 = 不启用

    // 从 extra 提炼的字段
    QString machine  = "q35";   // q35, pc, pc-i440fx-*
    QString cpuType  = "host";  // host, qemu64, pentium3, cortex-a72
    bool    kvm       = true;    // 启用 KVM 加速
    QString vga       = "virtio";// virtio, cirrus, qxl, virtio-gpu, none
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
    // VirtualBox: RDP 远程控制 (VRDE, VirtualBox 内置 RDP 服务器). 仅 VM 运行时监听.
    bool    vboxRdp = false;
    int     vboxRdpPort = 3389;
    // VirtualBox: 网络模式. nat / bridged
    QString vboxNicType = "nat";
    // VirtualBox: 网卡型号 (nic-type1). Am79C970A / Am79C973 / 82540EM / 82543GC / 82545EM / virtio / usbnet
    QString vboxNicModel = "82545EM";
    // VirtualBox: bridged 模式下用的宿主机网卡名 (vboxNicType==bridged 时)
    QString vboxBridgeDev = "";

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
