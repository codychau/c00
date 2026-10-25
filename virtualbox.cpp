#include "virtualbox.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QDateTime>
#include <QSet>
#include <QHash>

#include "logger.h"

// ── 工具函数 ──

// 同步执行一条 VBoxManage 命令. 成功返回 0; err 携带失败原因.
static int runVBox(const QStringList &args, QString *err)
{
    QProcess proc;
    proc.start("VBoxManage", args);
    if (!proc.waitForStarted(5000)) {
        if (err) *err = QString("无法启动 VBoxManage: %1").arg(proc.errorString());
        return -1;
    }
    proc.waitForFinished(180000);  // 创建/建盘可能较慢
    QString text = QString::fromUtf8(proc.readAllStandardOutput())
                 + QString::fromUtf8(proc.readAllStandardError());
    int rc = proc.exitCode();
    // 总是填充输出: vmExists/isRunning/addController 需要成功时的 stdout,
    // 失败时调用方也可从同一参数取错误信息。
    if (err) *err = text.trimmed();
    return rc;
}

// showvminfo --machinereadable 的解析结果
struct VboxLayout {
    QHash<QString, int> portcounts;  // 控制器名 → 当前端口数
    QSet<QString> media;             // 已挂载的媒体路径
    QStringList natpfNames;          // NAT 端口转发规则名 (Forwarding(N)="name,proto,,host,,guest")
};

// 前置声明 (定义在文件下方)
static VboxLayout readLayout(const QString &vmName);
static int addController(const QString &vmName, const QString &ctrlName,
                         const QString &type, int portcount,
                         const VboxLayout &lay, QString *err);
static int applySettings(const VMConfig &vm, QString *err);
static void applyNatpf(const VMConfig &vm);
static bool attachDisks(const VMConfig &vm, QString *err);

// VirtualBox 7.x 中 storageattach 的 IDE 控制器存在 CLI 缺陷
// (HDD/DVD 一律报 "Device not specified"), 故统一使用 SATA 控制器.
// 这里保留 backend 字段以兼容用户偏好, 但实际走 SATA.
static QString resolveBackend(const QString &backend)
{
    if (backend.toLower() == "ide")
        Logger::log("VirtualBox", "⚠️ IDE 控制器在本版 VBoxManage 不可用, 已回退 SATA");
    return "sata";
}

// ── 公有接口 ──

bool VirtualBox::vmExists(const QString &name)
{
    QString out;
    if (runVBox({"list", "vms"}, &out) != 0) return false;
    // 行格式: "name" {uuid}
    QRegularExpression re("^\\s*\"" + QRegularExpression::escape(name) + "\"\\s",
                          QRegularExpression::MultilineOption);
    return re.match(out).hasMatch();
}

bool VirtualBox::isRunning(const QString &name)
{
    QString out;
    if (runVBox({"showvminfo", name}, &out) != 0) return false;
    QRegularExpression re("^State:\\s+running", QRegularExpression::MultilineOption);
    return re.match(out).hasMatch();
}

bool VirtualBox::prepare(const VMConfig &vm, QString *err)
{
    const QString name = vm.name;

    if (vmExists(name)) {
        // 已存在: 回写核心设置, 并补挂尚未挂载的磁盘 (已挂载的条目跳过)
        // 注意: applySettings 返回 int (0=成功), 必须显式比较, 不能直接 return
        if (applySettings(vm, err) != 0) return false;
        applyNatpf(vm);
        return attachDisks(vm, err);
    }

    // ── 新建 ──
    // 1) createvm (若 .vbox 残留未注册, 自动 registervm 复用而非报错)
    QString createOut;
    int rc = runVBox({"createvm", "--name", name, "--ostype", vm.ostype, "--register"}, &createOut);
    if (rc != 0 && createOut.contains("invalid guest OS type", Qt::CaseInsensitive)) {
        // 旧配置里的 ostype 可能是界面名或无效 ID (如 Windows11): 回退通用 64 位 Windows
        Logger::log("VirtualBox", QString("⚠️ ostype '%1' 无效, 回退 WindowsNT_64").arg(vm.ostype));
        rc = runVBox({"createvm", "--name", name, "--ostype", "WindowsNT_64", "--register"}, &createOut);
    }
    if (rc != 0) {
        if (createOut.contains("already exists", Qt::CaseInsensitive)) {
            QString vboxFile = QDir::homePath() + "/VirtualBox VMs/" + name + "/" + name + ".vbox";
            if (runVBox({"registervm", vboxFile}, err) != 0) {
                if (err) *err = "复用残留 VM 配置失败: " + *err;
                return false;
            }
            Logger::log("VirtualBox", QString("♻️ 复用已存在的 VM 配置: %1").arg(vboxFile));
        } else {
            if (err) *err = createOut;
            return false;
        }
    }

    // 2) 磁盘列表 (系统盘 + 数据盘) 与 ISO; 控制器按需创建
    if (!attachDisks(vm, err))
        return false;

    // 3) 核心设置
    if (int rc = applySettings(vm, err); rc != 0)
        return false;

    // 4) NAT 端口转发
    applyNatpf(vm);

    Logger::log("VirtualBox", QString("✅ %1 已创建并配置 (ostype=%2)").arg(name, vm.ostype));
    return true;
}

bool VirtualBox::start(const QString &name, bool headless)
{
    QString err;
    QStringList args = {"startvm", name, "--type", headless ? "headless" : "gui"};
    if (int rc = runVBox(args, &err); rc != 0) {
        Logger::log("VirtualBox", QString("❌ 启动失败 %1: %2").arg(name, err));
        return false;
    }
    Logger::log("VirtualBox", QString("✅ %1 已启动 (headless=%2)").arg(name).arg(headless ? "true" : "false"));
    return true;
}

bool VirtualBox::stop(const QString &name, bool force)
{
    QString err;
    QStringList args = {"controlvm", name, force ? "poweroff" : "shutdown"};
    if (int rc = runVBox(args, &err); rc != 0) {
        Logger::log("VirtualBox", QString("❌ 停止失败 %1: %2").arg(name, err));
        return false;
    }
    Logger::log("VirtualBox", QString("⏹️ %1 已发送停止命令 (force=%2)").arg(name).arg(force ? "true" : "false"));
    return true;
}

// ═══════════════════════════════════════════════════════════════════
//  内部辅助
// ═══════════════════════════════════════════════════════════════════

// VirtualBox 建盘格式: qcow2 / img 不被支持, 归一为 VDI
static QString vboxFormat(const QString &fmt)
{
    QString f = fmt.toUpper();
    if (f == "VDI" || f == "VMDK" || f == "VHDX" || f == "RAW" || f == "QED")
        return f;
    return "VDI";
}

// 解析 showvminfo --machinereadable: 控制器名/端口数 + 已挂载媒体路径
static VboxLayout readLayout(const QString &vmName)
{
    VboxLayout lay;
    QString out;
    if (runVBox({"showvminfo", vmName, "--machinereadable"}, &out) != 0)
        return lay;

    for (int i = 0; i < 8; ++i) {
        QString idx = QString::number(i);
        QRegularExpression nameRe("storagecontrollername" + idx + "=\"([^\"]*)\"");
        auto nm = nameRe.match(out);
        if (!nm.hasMatch()) continue;
        QRegularExpression pcRe("storagecontrollerportcount" + idx + "=\"(\\d+)\"");
        auto pc = pcRe.match(out);
        lay.portcounts.insert(nm.captured(1), pc.hasMatch() ? pc.captured(1).toInt() : 0);
    }

    // 已挂载媒体: 键格式 "<控制器名>-<端口>-<设备>"="路径" (空位是 "none")
    for (const auto &ctrl : lay.portcounts.keys()) {
        QRegularExpression medRe("\"" + QRegularExpression::escape(ctrl)
                                 + "-\\d+-\\d+\"=\"([^\"]+)\"");
        auto it = medRe.globalMatch(out);
        while (it.hasNext()) {
            QString p = it.next().captured(1);
            if (p != "none" && !p.isEmpty())
                lay.media.insert(p);
        }
    }

    // NAT 端口转发规则: Forwarding(N)="name,proto,,hostPort,,guestPort"
    QRegularExpression pfRe("Forwarding\\(\\d+\\)=\"([^\"]+)\"");
    auto pit = pfRe.globalMatch(out);
    while (pit.hasNext())
        lay.natpfNames << pit.next().captured(1).split(',').first();

    return lay;
}

// 挂载磁盘列表: 系统盘 → SATA 端口 0, 其余数据盘依次占后续端口, ISO 光驱占最后一个端口.
// 文件不存在时按 vboxDiskSize 建盘. 已挂载的路径跳过, 因此对已存在的 VM 可安全重复调用.
// 系统盘失败返回 false; 数据盘失败仅告警.
static bool attachDisks(const VMConfig &vm, QString *err)
{
    const QString name = vm.name;
    const QString backend = resolveBackend(vm.storageBackend);

    // 端口: 磁盘 0..N-1, 光驱 N (VBox 7.2 的 SATA 控制器最多 30 端口)
    const int ports = qMax(2, vm.disks.size() + (vm.iso.isEmpty() ? 0 : 1));

    const VboxLayout lay = readLayout(name);
    if (addController(name, "SATA", backend, ports, lay, err) != 0) {
        if (err) *err = "添加 SATA 控制器失败: " + *err;
        return false;
    }

    int port = 0;
    for (int i = 0; i < vm.disks.size(); ++i) {
        const VMDataDisk &dd = vm.disks[i];
        if (dd.path.isEmpty()) continue;

        QDir().mkpath(QFileInfo(dd.path).absolutePath());
        if (!QFile::exists(dd.path)) {
            QString out;
            if (runVBox({"createmedium", "disk", "--filename", dd.path,
                         "--size", QString::number(vm.vboxDiskSize),
                         "--format", vboxFormat(dd.format)}, &out) != 0) {
                if (dd.system) {
                    if (err) *err = "创建系统盘失败: " + out;
                    return false;
                }
                Logger::log("VirtualBox", QString("⚠️ 数据盘 %1 创建失败: %2").arg(dd.path, out));
                ++port;
                continue;
            }
        }

        if (lay.media.contains(dd.path)) { ++port; continue; }  // 已挂载, 跳过
        QString out;
        if (runVBox({"storageattach", name, "--storagectl", "SATA",
                     "--port", QString::number(port), "--type", "hdd",
                     "--medium", dd.path}, &out) != 0) {
            if (dd.system) {
                if (err) *err = "挂载系统盘失败: " + out;
                return false;
            }
            Logger::log("VirtualBox", QString("⚠️ 数据盘 %1 挂载失败: %2").arg(dd.path, out));
        }
        ++port;
    }

    // ISO 光驱
    if (!vm.iso.isEmpty() && !lay.media.contains(vm.iso)) {
        QString out;
        if (runVBox({"storageattach", name, "--storagectl", "SATA",
                     "--port", QString::number(port), "--type", "dvddrive",
                     "--medium", vm.iso}, &out) != 0) {
            // ISO 可能是占位路径 (尚未选择): 仅告警, 不阻断启动
            Logger::log("VirtualBox", QString("⚠️ ISO 挂载失败 (%1), 可能尚未选择镜像").arg(vm.iso));
        }
    }

    return true;
}

// 确保控制器存在且端口够用: 不存在则新建, 端口不足则扩容
static int addController(const QString &vmName, const QString &ctrlName,
                         const QString &type, int portcount,
                         const VboxLayout &lay, QString *err)
{
    auto it = lay.portcounts.constFind(ctrlName);
    if (it != lay.portcounts.constEnd()) {
        if (it.value() >= portcount) return 0;  // 已存在且端口够用
        return runVBox({"storagectl", vmName, "--name", ctrlName,
                        "--portcount", QString::number(portcount)}, err);
    }
    return runVBox({"storagectl", vmName, "--name", ctrlName, "--add", type,
                    "--portcount", QString::number(portcount)}, err);
}

static int applySettings(const VMConfig &vm, QString *err)
{
    QStringList a = {"modifyvm", vm.name};
    a << "--acpi" << (vm.vboxAcpi ? "on" : "off");
    a << "--ioapic" << (vm.vboxIoapic ? "on" : "off");
    a << "--firmware" << (vm.vboxUefi ? "efi" : "bios");
    a << "--vrde" << (vm.vboxRdp ? "on" : "off");
    if (vm.vboxRdp)
        a << "--vrde-port" << QString::number(vm.vboxRdpPort);
    a << "--cpus" << QString::number(vm.cpu);
    a << "--memory" << QString::number(vm.memory);
    a << "--os-type" << vm.ostype;
    a << "--graphicscontroller" << "vmsvga";
    a << "--vram" << "128";

    // 64 位: 仅 ostype 含 "64" 时开启
    if (vm.ostype.contains("64"))
        a << "--x86-long-mode" << "on";

    // 网络
    if (vm.vboxNicType == "bridged") {
        a << "--nic1" << "bridged";
        if (!vm.vboxBridgeDev.isEmpty())
            a << "--bridge1" << vm.vboxBridgeDev;
    } else {
        a << "--nic1" << "nat";
    }
    if (!vm.vboxNicModel.isEmpty())
        a << "--nic-type1" << vm.vboxNicModel;

    return runVBox(a, err);
}

// NAT 端口转发: 先删现有规则, 再按配置重建. bridged 模式无 NAT, 清空即可.
// 规则名用 pf<序号>, 保证重复 prepare 时幂等.
static void applyNatpf(const VMConfig &vm)
{
    const QString name = vm.name;
    const VboxLayout lay = readLayout(name);

    for (const auto &ruleName : lay.natpfNames)
        runVBox({"modifyvm", name, "--natpf1", "delete", ruleName}, nullptr);

    if (vm.vboxNicType != "nat")
        return;

    for (int i = 0; i < vm.portForwards.size(); ++i) {
        const auto &pf = vm.portForwards[i];
        if (pf.hostPort <= 0 || pf.guestPort <= 0) continue;
        QString proto = (pf.protocol.toLower() == "udp") ? "udp" : "tcp";
        QString rule  = QString("pf%1,%2,,%3,,%4").arg(i).arg(proto).arg(pf.hostPort).arg(pf.guestPort);
        QString out;
        if (runVBox({"modifyvm", name, "--natpf1", rule}, &out) != 0)
            Logger::log("VirtualBox", QString("⚠️ 端口转发 %1 写入失败: %2").arg(rule, out));
    }
}
