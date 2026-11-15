#include "qemuargs.h"

#include <QRegularExpression>

// ═══════════════════════════════════════════════════════════════════
//  BDF / vid:pid 归一
// ═══════════════════════════════════════════════════════════════════

QString normalizeBDF(const QString &bdf)
{
    QString addr = bdf.trimmed();
    if (!addr.contains(':')) return QString();
    if (addr.count(':') == 1 && !addr.startsWith("0000:"))
        addr = "0000:" + addr;
    return addr;
}

QString usbHostArg(const QString &vidpid)
{
    // 接受 "046d:c53f" / "0x046d:0xc53f" (大小写不敏感)
    QString s = vidpid.trimmed().toLower();
    s.remove("0x");
    const QStringList parts = s.split(':', Qt::SkipEmptyParts);
    if (parts.size() != 2) return QString();
    static const QRegularExpression hex4("^[0-9a-f]{4}$");
    if (!hex4.match(parts[0]).hasMatch() || !hex4.match(parts[1]).hasMatch())
        return QString();
    return QString("vendorid=0x%1,productid=0x%2").arg(parts[0], parts[1]);
}

// ═══════════════════════════════════════════════════════════════════
//  buildQemuArgs — 由 VMConfig 生成 QEMU 参数
//  旧配置 (新字段全为缺省) 的输出与重构前逐字节一致
// ═══════════════════════════════════════════════════════════════════
QStringList buildQemuArgs(const VMConfig &vm, const QString &sockPath)
{
    QStringList args;

    args << "-name" << vm.name;
    args << "-smp" << QString::number(vm.cpu);
    args << "-m" << QString::number(vm.memory);

    QString machineStr = vm.machine;
    if (vm.kvm) machineStr += ",accel=kvm";
    args << "-machine" << machineStr;
    args << "-cpu" << vm.cpuType;

    // 显卡直通需要大 hole64 映射 BAR; 仅 q35 系机器有此属性
    if (!vm.pciHole64.isEmpty() && vm.machine.startsWith("q35")) {
        args << "-global" << QString("q35-pcihost.pci-hole64-size=%1").arg(vm.pciHole64.trimmed());
    }

    // QMP 管理接口（用于关机/电源管理）
    args << "-qmp" << QString("unix:%1,server=on,wait=off").arg(sockPath);

    // UEFI 固件: code 只读 + vars 可写 (每台 VM 独立 vars, 共享会污染启动项)
    if (vm.uefi) {
        if (!vm.uefiCode.trimmed().isEmpty())
            args << "-drive" << QString("if=pflash,format=raw,readonly=on,file=%1").arg(vm.uefiCode.trimmed());
        if (!vm.uefiVars.trimmed().isEmpty())
            args << "-drive" << QString("if=pflash,format=raw,file=%1").arg(vm.uefiVars.trimmed());
    }

    // 磁盘列表: 每块盘用条目自己的接口 (virtio / sata / ide)
    int diskIdx = 0;
    for (const auto &dd : vm.disks) {
        if (dd.path.isEmpty()) continue;
        QString if_ = dd.iface;
        if (if_ != "sata" && if_ != "ide") if_ = "virtio";
        args << "-drive" << QString("id=drive%1,file=%2,format=%3,if=%4,index=%5,cache=%6,aio=%7")
            .arg(diskIdx).arg(dd.path, dd.format).arg(if_).arg(diskIdx).arg(dd.cache, dd.aio);
        ++diskIdx;
    }

    if (!vm.iso.isEmpty())
        args << "-cdrom" << vm.iso;

    if (vm.net == "user") {
        QString netdev = "user,id=net0";
        for (const auto &pf : vm.portForwards)
            netdev += QString(",hostfwd=%1::%2-:%3").arg(pf.protocol).arg(pf.hostPort).arg(pf.guestPort);
        args << "-netdev" << netdev;
        args << "-device" << QString("%1,netdev=net0").arg(vm.nicModel);
    } else if (vm.net == "bridge") {
        args << "-netdev" << "bridge,id=net0";
        args << "-device" << QString("%1,netdev=net0").arg(vm.nicModel);
    } else if (vm.net == "tap") {
        args << "-netdev" << "tap,id=net0";
        args << "-device" << QString("%1,netdev=net0").arg(vm.nicModel);
    }

    args << "-vga" << vm.vga;

    // 显示: 无虚拟显卡的纯直通 VM 用 -display none; 与 VNC 互斥
    if (vm.displayNone && vm.vnc < 0) {
        args << "-display" << "none";
    } else if (vm.vnc >= 0) {
        // vnc 存的是显示编号 (0~99)，直接传给 QEMU
        args << "-vnc" << QString(":%1").arg(vm.vnc);
    }

    // PCI 直通 (列表顺序 = 命令行顺序 = function 递增顺序)
    for (int i = 0; i < vm.pciDevices.size(); ++i) {
        const QString addr = normalizeBDF(vm.pciDevices[i]);
        if (addr.isEmpty()) continue;
        if (vm.vfioMultifunction) {
            // 同 slot 多 function: 需要显式 id + multifunction=on
            args << "-device" << QString("vfio-pci,host=%1,id=vfio%2,multifunction=on")
                        .arg(addr).arg(i);
        } else {
            args << "-device" << QString("vfio-pci,host=%1").arg(addr);
        }
    }

    if (vm.ramfb)
        args << "-device" << "ramfb";

    if (vm.hugepages) {
        args << "-object"
             << QString("memory-backend-file,id=mem,size=%1M,mem-path=/dev/hugepages,share=on")
                        .arg(vm.memory)
             << "-numa" << "node,memdev=mem";
    }

    // 音频: audiodev 后端 + Intel HDA 控制器 + duplex/micro 声卡
    if (vm.audioBackend != "none" && !vm.audioBackend.isEmpty()) {
        args << "-audiodev" << QString("%1,id=au0").arg(vm.audioBackend);
        args << "-device" << "ich9-intel-hda";
        args << "-device" << QString("%1,audiodev=au0").arg(vm.audioModel);
    }

    // USB 直通: xhci 控制器 + 虚拟键鼠 + 物理 USB 设备 + evdev 输入设备
    const bool needXhci = vm.usbXhci || vm.usbKbdTablet || !vm.usbHostVendors.isEmpty();
    if (needXhci) {
        args << "-device" << "qemu-xhci,id=xh0";
        if (vm.usbKbdTablet) {
            args << "-device" << "usb-kbd,bus=xh0.0";
            args << "-device" << "usb-tablet,bus=xh0.0";
        }
        for (const auto &vp : vm.usbHostVendors) {
            const QString frag = usbHostArg(vp);
            if (frag.isEmpty()) continue;
            args << "-device" << QString("%1,bus=xh0.0").arg(frag);
        }
    }
    // evdev 透传是 PCI 设备, 不挂 xhci 总线
    for (const auto &ev : vm.usbEvdevPaths) {
        const QString p = ev.trimmed();
        if (p.isEmpty()) continue;
        args << "-device" << QString("virtio-input-host-pci,evdev=%1").arg(p);
    }

    if (!vm.extra.isEmpty())
        args << vm.extra.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);

    return args;
}
