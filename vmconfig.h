#ifndef VMCONFIG_H
#define VMCONFIG_H

#include <QString>
#include <QVector>
#include <QStringList>

// 数据盘配置
struct VMDataDisk {
    QString path;
    QString format = "qcow2";   // qcow2, raw, img
    QString cache = "none";     // none, writeback, writethrough, unsafe
    QString aio   = "native";   // native, threads, io_uring
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
    QString disk;
    QString iso;
    QString qemuBinary = "qemu-system-x86_64"; // QEMU 二进制
    int     vnc       = -1;     // -1 = 不启用

    // 从 extra 提炼的字段
    QString machine  = "q35";   // q35, pc, pc-i440fx-*
    QString cpuType  = "host";  // host, qemu64, pentium3, cortex-a72
    bool    kvm       = true;    // 启用 KVM 加速
    bool    virtioDisk = true;   // 系统盘使用 virtio（关闭后用默认 IDE/SATA）
    QString vga       = "virtio";// virtio, cirrus, qxl, virtio-gpu, none
    QString net       = "user";  // user, bridge, tap
    QString nicModel  = "virtio-net-pci"; // 网卡模型

    // 端口转发 (user 模式下生效)
    QVector<PortFwd> portForwards;

    // 数据盘列表（除系统盘外的附加磁盘）
    QVector<VMDataDisk> dataDisks;

    // aarch64
    bool ramfb = false;

    // 自动随管理器启动
    bool autoStart = false;

    // 硬件直通
    QStringList pciDevices;     // PCI BDF 地址列表
    bool        hugepages = false;

    // 兜底 — 未被提炼的额外参数
    QString extra;
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
