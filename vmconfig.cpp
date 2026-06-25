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
            if (auto *v = vmTbl->get("virtio_disk")) vm.virtioDisk = v->value_or(true);
            if (auto *v = vmTbl->get("vga"))        vm.vga      = QString::fromStdString(v->value_or("virtio"));
            if (auto *v = vmTbl->get("nic_model"))  vm.nicModel = QString::fromStdString(v->value_or("virtio-net-pci"));

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
        tbl.emplace("virtio_disk", vm.virtioDisk);
        tbl.emplace("vga",        vm.vga.toStdString());
        tbl.emplace("nic_model",  vm.nicModel.toStdString());

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
