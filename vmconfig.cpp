#include "vmconfig.h"

#include <QDir>
#include <QStandardPaths>

#include <toml++/toml.h>

// ── 默认配置文件路径 ──
QString VMConfigManager::defaultConfigPath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return dir + "/vms.toml";
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
            if (auto *v = vmTbl->get("disk"))   vm.disk   = QString::fromStdString(v->value_or(""));
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
            // 系统盘接口: 优先 disk_interface; 旧配置用 virtio_disk(bool) 迁移
            if (auto *v = vmTbl->get("disk_interface")) {
                QString s = QString::fromStdString(v->value_or(""));
                if (s == "sata" || s == "ide" || s == "virtio") vm.diskInterface = s;
            } else if (auto *v = vmTbl->get("virtio_disk")) {
                vm.diskInterface = v->value_or(true) ? "virtio" : "ide";
            }
            if (auto *v = vmTbl->get("vga"))        vm.vga      = QString::fromStdString(v->value_or("virtio"));
            if (auto *v = vmTbl->get("nic_model"))  vm.nicModel = QString::fromStdString(v->value_or("virtio-net-pci"));

            // VirtualBox 专属 (旧配置缺省时回退默认)
            if (auto *v = vmTbl->get("vm_type"))         vm.vmType      = QString::fromStdString(v->value_or("qemu"));
            if (auto *v = vmTbl->get("ostype")) {
                // VirtualBox ostype ID 无空格 (Windows7/Windows10...), 旧配置若存了界面名则归一
                QString o = QString::fromStdString(v->value_or("Windows7"));
                o.remove(' ');
                vm.ostype = o;
            }
            if (auto *v = vmTbl->get("disk_format")) {
                QString df = QString::fromStdString(v->value_or("vdi")).toLower();
                if (df.contains("vmdk"))      vm.diskFormat = "vmdk";
                else if (df.contains("vhdx")) vm.diskFormat = "vhdx";
                else                          vm.diskFormat = "vdi";  // VDI (VirtualBox 原生) 等
            }
            if (auto *v = vmTbl->get("storage_backend")) {
                QString sb = QString::fromStdString(v->value_or("sata")).toLower();
                vm.storageBackend = sb.contains("ide") ? "ide" : "sata";  // "IDE (...)" / "SATA (...)"
            }
            if (auto *v = vmTbl->get("vbox_disk_size"))  vm.vboxDiskSize= v->value_or(20480);
            if (auto *v = vmTbl->get("vbox_acpi"))       vm.vboxAcpi   = v->value_or(true);
            if (auto *v = vmTbl->get("vbox_ioapic"))     vm.vboxIoapic = v->value_or(true);
            if (auto *v = vmTbl->get("vbox_nic_type"))   vm.vboxNicType = QString::fromStdString(v->value_or("nat"));
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

            // 数据盘
            if (auto *v = vmTbl->get("data_disks")) {
                if (auto *arr = v->as_array()) {
                    for (auto &el : *arr) {
                        auto *dt = el.as_table();
                        if (!dt) continue;
                        VMDataDisk dd;
                        if (auto *pv = dt->get("path"))   dd.path   = QString::fromStdString(pv->value_or(""));
                        if (auto *pv = dt->get("format")) dd.format = QString::fromStdString(pv->value_or("qcow2"));
                        if (auto *pv = dt->get("cache"))  dd.cache  = QString::fromStdString(pv->value_or("none"));
                        if (auto *pv = dt->get("aio"))    dd.aio    = QString::fromStdString(pv->value_or("native"));
                        if (!dd.path.isEmpty())
                            vm.dataDisks.append(dd);
                    }
                }
            }

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
        tbl.emplace("disk",   vm.disk.toStdString());
        tbl.emplace("iso",    vm.iso.toStdString());
        tbl.emplace("vnc",    vm.vnc);
        tbl.emplace("qemu_binary", vm.qemuBinary.toStdString());
        tbl.emplace("ramfb",  vm.ramfb);

        // 结构化字段
        tbl.emplace("net",        vm.net.toStdString());
        tbl.emplace("machine",    vm.machine.toStdString());
        tbl.emplace("cpu_type",   vm.cpuType.toStdString());
        tbl.emplace("kvm",        vm.kvm);
        tbl.emplace("disk_interface", vm.diskInterface.toStdString());
        tbl.emplace("vga",        vm.vga.toStdString());
        tbl.emplace("nic_model",  vm.nicModel.toStdString());
        tbl.emplace("vm_type",        vm.vmType.toStdString());
        tbl.emplace("ostype",         vm.ostype.toStdString());
        tbl.emplace("disk_format",    vm.diskFormat.toStdString());
        tbl.emplace("storage_backend", vm.storageBackend.toStdString());
        tbl.emplace("vbox_disk_size", vm.vboxDiskSize);
        tbl.emplace("vbox_acpi",      vm.vboxAcpi);
        tbl.emplace("vbox_ioapic",    vm.vboxIoapic);
        tbl.emplace("vbox_nic_type",  vm.vboxNicType.toStdString());
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

        // 数据盘
        toml::array disksArr;
        for (const auto &dd : vm.dataDisks) {
            toml::table dt;
            dt.emplace("path",   dd.path.toStdString());
            dt.emplace("format", dd.format.toStdString());
            dt.emplace("cache",  dd.cache.toStdString());
            dt.emplace("aio",    dd.aio.toStdString());
            disksArr.push_back(std::move(dt));
        }
        tbl.emplace("data_disks", std::move(disksArr));

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
