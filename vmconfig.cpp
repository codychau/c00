#include "vmconfig.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

#include <toml++/toml.h>

// ── 默认配置文件路径 ──
QString VMConfigManager::defaultConfigPath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return dir + "/vms.toml";
}

// 归一磁盘格式: 兼容旧配置的界面名 ("VDI (VirtualBox 原生)") 与扩展名
static QString normalizeFormat(const QString &fmt, const QString &path)
{
    QString f = fmt.toLower();
    f.remove(' ');
    if (f.contains("vmdk")) return "vmdk";
    if (f.contains("vhdx")) return "vhdx";
    if (f.contains("vdi"))  return "vdi";
    if (f.contains("qcow2")) return "qcow2";
    if (f == "img" || f == "raw") return "raw";
    if (f.isEmpty()) {
        QString ext = QFileInfo(path).suffix().toLower();
        if (ext == "vmdk" || ext == "vdi" || ext == "vhdx") return ext;
        if (ext == "img") return "raw";
        return "qcow2";
    }
    return f;
}

VMConfigManager::VMConfigManager(const QString &configPath)
    : m_path(configPath.isEmpty() ? defaultConfigPath() : configPath)
{
}

// ── 解析 ──
bool VMConfigManager::load()
{
    m_vms.clear();

    if (!QDir().exists(m_path))
        return true;

    try {
        auto tbl = toml::parse_file(m_path.toStdString());

        auto *vmsArr = tbl.get("vm");
        if (!vmsArr || !vmsArr->is_array()) return true;

        for (auto &elem : *vmsArr->as_array()) {
            auto *vmTbl = elem.as_table();
            if (!vmTbl) continue;

            VMConfig vm;

            if (auto *v = vmTbl->get("name"))   vm.name   = QString::fromStdString(v->value_or(""));
            if (auto *v = vmTbl->get("cpu"))    vm.cpu    = v->value_or(2);
            if (auto *v = vmTbl->get("memory")) vm.memory = v->value_or(2048);
            if (auto *v = vmTbl->get("iso"))    vm.iso    = QString::fromStdString(v->value_or(""));
            if (auto *v = vmTbl->get("vnc"))    vm.vnc    = v->value_or(-1);
            if (auto *v = vmTbl->get("qemu_binary")) vm.qemuBinary = QString::fromStdString(v->value_or("qemu-system-x86_64"));
            if (auto *v = vmTbl->get("ramfb"))  vm.ramfb = v->value_or(false);
            if (auto *v = vmTbl->get("extra"))  vm.extra = QString::fromStdString(v->value_or(""));

            // 提炼后的结构化字段
            if (auto *v = vmTbl->get("net"))   vm.net   = QString::fromStdString(v->value_or("user"));
            if (auto *v = vmTbl->get("machine"))    vm.machine  = QString::fromStdString(v->value_or("q35"));
            if (auto *v = vmTbl->get("cpu_type"))   vm.cpuType  = QString::fromStdString(v->value_or("host"));
            if (auto *v = vmTbl->get("kvm"))        vm.kvm      = v->value_or(true);
            if (auto *v = vmTbl->get("vga"))        vm.vga      = QString::fromStdString(v->value_or("virtio"));
            if (auto *v = vmTbl->get("nic_model"))  vm.nicModel = QString::fromStdString(v->value_or("virtio-net-pci"));

            // VirtualBox 专属 (旧配置缺省时回退默认)
            if (auto *v = vmTbl->get("vm_type"))         vm.vmType      = QString::fromStdString(v->value_or("qemu"));
            if (auto *v = vmTbl->get("ostype")) {
                // VirtualBox ostype ID 无空格 (Windows7/Windows10...), 旧配置若存了界面名则归一
                QString o = QString::fromStdString(v->value_or("Windows7"));
                if (o.contains("(64-bit)", Qt::CaseInsensitive)) {
                    o.remove(QRegularExpression("\\s*\\(64-bit\\)",
                                               QRegularExpression::CaseInsensitiveOption));
                    o += "_64";   // "Windows 7 (64-bit)" → "Windows7_64"
                }
                o.remove(' ');
                if (o == "Windows11") o = "Windows11_64";  // VBox 无 32 位 Windows 11 类型
                vm.ostype = o;
            }
            if (auto *v = vmTbl->get("storage_backend")) {
                QString sb = QString::fromStdString(v->value_or("sata")).toLower();
                vm.storageBackend = sb.contains("ide") ? "ide" : "sata";  // "IDE (...)" / "SATA (...)"
            }
            if (auto *v = vmTbl->get("vbox_disk_size"))  vm.vboxDiskSize= v->value_or(20480);
            if (auto *v = vmTbl->get("vbox_acpi"))       vm.vboxAcpi   = v->value_or(true);
            if (auto *v = vmTbl->get("vbox_ioapic"))     vm.vboxIoapic = v->value_or(true);
            if (auto *v = vmTbl->get("vbox_uefi"))       vm.vboxUefi   = v->value_or(false);
            if (auto *v = vmTbl->get("vbox_rdp"))        vm.vboxRdp    = v->value_or(false);
            if (auto *v = vmTbl->get("vbox_rdp_port"))   vm.vboxRdpPort = v->value_or(3389);
            if (auto *v = vmTbl->get("vbox_nic_type"))   vm.vboxNicType = QString::fromStdString(v->value_or("nat"));
            if (auto *v = vmTbl->get("vbox_nic_model"))  vm.vboxNicModel = QString::fromStdString(v->value_or("82545EM"));
            if (auto *v = vmTbl->get("vbox_bridge_dev")) vm.vboxBridgeDev = QString::fromStdString(v->value_or(""));

            // 端口转发
            if (auto *v = vmTbl->get("port_forwards")) {
                if (auto *arr = v->as_array()) {
                    for (auto &el : *arr) {
                        auto *pt = el.as_table();
                        if (!pt) continue;
                        PortFwd pf;
                        if (auto *pv = pt->get("host"))  pf.hostPort  = pv->value_or(0);
                        if (auto *pv = pt->get("guest")) pf.guestPort = pv->value_or(0);
                        if (auto *pv = pt->get("proto")) pf.protocol  = QString::fromStdString(pv->value_or("tcp"));
                        if (pf.hostPort > 0)
                            vm.portForwards.append(pf);
                    }
                }
            }

            // 磁盘列表 (新格式 disks; 旧格式 data_disks 兼容读取)
            toml::node *diskNode = vmTbl->get("disks");
            if (!diskNode) diskNode = vmTbl->get("data_disks");
            if (diskNode && diskNode->is_array()) {
                for (auto &el : *diskNode->as_array()) {
                    auto *dt = el.as_table();
                    if (!dt) continue;
                    VMDataDisk dd;
                    if (auto *pv = dt->get("path"))   dd.path   = QString::fromStdString(pv->value_or(""));
                    if (auto *pv = dt->get("format")) dd.format = QString::fromStdString(pv->value_or("qcow2"));
                    if (auto *pv = dt->get("cache"))  dd.cache  = QString::fromStdString(pv->value_or("none"));
                    if (auto *pv = dt->get("aio"))    dd.aio    = QString::fromStdString(pv->value_or("native"));
                    if (auto *pv = dt->get("iface"))  dd.iface  = QString::fromStdString(pv->value_or("virtio"));
                    if (auto *pv = dt->get("system")) dd.system = pv->value_or(false);
                    dd.format = normalizeFormat(dd.format, dd.path);
                    if (!dd.path.isEmpty())
                        vm.disks.append(dd);
                }
            }

            // 旧配置迁移: disk(系统盘) + disk_interface / virtio_disk / disk_format → 列表首位
            QString legacyDisk;
            if (auto *v = vmTbl->get("disk")) legacyDisk = QString::fromStdString(v->value_or(""));
            if (!legacyDisk.isEmpty()) {
                VMDataDisk sys;
                sys.path   = legacyDisk;
                sys.system = true;
                QString legacyFmt;
                // disk_format 是 VirtualBox 建盘格式; QEMU 的盘按扩展名推断
                if (vm.vmType == "virtualbox" && vmTbl->get("disk_format"))
                    legacyFmt = QString::fromStdString(vmTbl->get("disk_format")->value_or(""));
                sys.format = normalizeFormat(legacyFmt, legacyDisk);
                if (auto *v = vmTbl->get("disk_interface")) {
                    QString s = QString::fromStdString(v->value_or(""));
                    if (s == "sata" || s == "ide" || s == "virtio") sys.iface = s;
                } else if (auto *v = vmTbl->get("virtio_disk")) {
                    sys.iface = v->value_or(true) ? "virtio" : "ide";
                }
                vm.disks.prepend(sys);
            }
            vm.normalizeDisks();  // 保证系统盘唯一且位于首位

            // 随管理器启动
            if (auto *v = vmTbl->get("auto_start")) vm.autoStart = v->value_or(false);
            if (auto *v = vmTbl->get("wait_mount")) vm.waitMount = v->value_or(false);

            // 硬件直通
            if (auto *v = vmTbl->get("hugepages"))  vm.hugepages = v->value_or(false);
            if (auto *v = vmTbl->get("pci_devices")) {
                if (auto *arr = v->as_array()) {
                    for (auto &el : *arr)
                        vm.pciDevices.append(QString::fromStdString(el.value_or("")));
                }
            }

            m_vms.append(vm);
        }
    } catch (const toml::parse_error &) {
        return false;
    }

    return true;
}

// ── 写出 ──
bool VMConfigManager::save() const
{
    toml::table root;

    for (const auto &vm : m_vms) {
        toml::table tbl;
        tbl.emplace("name",   vm.name.toStdString());
        tbl.emplace("cpu",    vm.cpu);
        tbl.emplace("memory", vm.memory);
        tbl.emplace("iso",    vm.iso.toStdString());
        tbl.emplace("vnc",    vm.vnc);
        tbl.emplace("qemu_binary", vm.qemuBinary.toStdString());
        tbl.emplace("ramfb",  vm.ramfb);

        // 结构化字段
        tbl.emplace("net",        vm.net.toStdString());
        tbl.emplace("machine",    vm.machine.toStdString());
        tbl.emplace("cpu_type",   vm.cpuType.toStdString());
        tbl.emplace("kvm",        vm.kvm);
        tbl.emplace("vga",        vm.vga.toStdString());
        tbl.emplace("nic_model",  vm.nicModel.toStdString());
        tbl.emplace("vm_type",        vm.vmType.toStdString());
        tbl.emplace("ostype",         vm.ostype.toStdString());
        tbl.emplace("storage_backend", vm.storageBackend.toStdString());
        tbl.emplace("vbox_disk_size", vm.vboxDiskSize);
        tbl.emplace("vbox_acpi",      vm.vboxAcpi);
        tbl.emplace("vbox_ioapic",    vm.vboxIoapic);
        tbl.emplace("vbox_uefi",      vm.vboxUefi);
        tbl.emplace("vbox_rdp",       vm.vboxRdp);
        tbl.emplace("vbox_rdp_port",  vm.vboxRdpPort);
        tbl.emplace("vbox_nic_type",  vm.vboxNicType.toStdString());
        tbl.emplace("vbox_nic_model", vm.vboxNicModel.toStdString());
        tbl.emplace("vbox_bridge_dev", vm.vboxBridgeDev.toStdString());

        // 端口转发
        toml::array pfArr;
        for (const auto &pf : vm.portForwards) {
            toml::table pt;
            pt.emplace("host",  pf.hostPort);
            pt.emplace("guest", pf.guestPort);
            pt.emplace("proto", pf.protocol.toStdString());
            pfArr.push_back(std::move(pt));
        }
        tbl.emplace("port_forwards", std::move(pfArr));

        // 随管理器启动
        tbl.emplace("auto_start", vm.autoStart);
        tbl.emplace("wait_mount", vm.waitMount);

        // 磁盘列表 (系统盘 + 数据盘)
        toml::array disksArr;
        for (const auto &dd : vm.disks) {
            toml::table dt;
            dt.emplace("path",   dd.path.toStdString());
            dt.emplace("format", dd.format.toStdString());
            dt.emplace("cache",  dd.cache.toStdString());
            dt.emplace("aio",    dd.aio.toStdString());
            dt.emplace("iface",  dd.iface.toStdString());
            dt.emplace("system", dd.system);
            disksArr.push_back(std::move(dt));
        }
        tbl.emplace("disks", std::move(disksArr));

        // 硬件直通
        tbl.emplace("hugepages", vm.hugepages);
        toml::array pciArr;
        for (const auto &bdf : vm.pciDevices)
            pciArr.push_back(bdf.toStdString());
        tbl.emplace("pci_devices", std::move(pciArr));

        // 兜底额外参数
        tbl.emplace("extra", vm.extra.toStdString());

        if (auto *existing = root.get("vm")) {
            existing->as_array()->push_back(std::move(tbl));
        } else {
            toml::array newArr;
            newArr.push_back(std::move(tbl));
            root.emplace("vm", std::move(newArr));
        }
    }

    QDir dir;
    dir.mkpath(QFileInfo(m_path).absolutePath());

    std::ofstream ofs(m_path.toStdString());
    if (!ofs) return false;
    ofs << root << "\n";
    return true;
}

// ── CRUD ──
void VMConfigManager::addVM(const VMConfig &vm)
{
    m_vms.append(vm);
}

void VMConfigManager::removeVM(int index)
{
    if (index >= 0 && index < m_vms.size())
        m_vms.removeAt(index);
}

void VMConfigManager::updateVM(int index, const VMConfig &vm)
{
    if (index >= 0 && index < m_vms.size())
        m_vms[index] = vm;
}
