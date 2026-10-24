#include "virtualbox.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QDateTime>

#include "logger.h"

// ── 工具函数 ──

// 是否属于"极老 OS" (无 ACPI / ioapic / 64 位支持)
static bool isLegacyOS(const QString &ostype)
{
    static const QStringList legacy = {
        "Windows31", "Windows95", "Windows98", "WindowsMe",
        "WindowsNT3x", "WindowsNT4", "MSDOS"
    };
    return legacy.contains(ostype);
}

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
    if (rc != 0 && err) *err = text.trimmed();
    return rc;
}

// 前置声明 (定义在文件下方)
static int addController(const QString &vmName, const QString &name,
                         const QString &type, QString *err);
static int applySettings(const VMConfig &vm, QString *err);

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
        // 已存在: 只回写核心设置 (内存/CPU/ostype/网络/显示). 控制器/磁盘/ISO 保持原样.
        return applySettings(vm, err);
    }

    // ── 新建 ──
    // 1) createvm
    if (int rc = runVBox({"createvm", "--name", name,
                          "--ostype", vm.ostype, "--register"}, err); rc != 0)
        return false;

    // 2) 存储控制器 (SATA; IDE 在本版不可用)
    QString backend = resolveBackend(vm.storageBackend);
    const bool useSata = (backend == "sata");
    if (useSata) {
        if (int rc = addController(name, "SATA", "sata", err); rc != 0) {
            if (err) *err = "添加 SATA 控制器失败: " + *err;
            return false;
        }
    }

    // 3) 磁盘映像: 存在则直接挂载, 否则创建后挂载
    if (!vm.disk.isEmpty()) {
        QDir().mkpath(QFileInfo(vm.disk).absolutePath());
        const bool fileExists = QFile::exists(vm.disk);
        const QString fmt = vm.diskFormat.toUpper();
        if (!fileExists) {
            int rc = runVBox({"createmedium", "disk", "--filename", vm.disk,
                              "--size", QString::number(vm.vboxDiskSize),
                              "--format", fmt}, err);
            if (rc != 0) {
                if (err) *err = "创建磁盘失败: " + *err;
                return false;
            }
        }
        QStringList da = {"storageattach", name};
        if (useSata) {
            da << "--storagectl" << "SATA" << "--port" << "0"
               << "--medium" << vm.disk << "--type" << "hdd";
        }
        if (int rc = runVBox(da, err); rc != 0) {
            if (err) *err = "挂载磁盘失败: " + *err;
            return false;
        }
    }

    // 4) 安装 ISO (光驱)
    if (!vm.iso.isEmpty()) {
        QStringList ia = {"storageattach", name, "--storagectl", "SATA",
                          "--port", "1", "--type", "dvddrive", "--medium", vm.iso};
        if (int rc = runVBox(ia, err); rc != 0) {
            // ISO 可能是占位路径 (尚未选择): 仅告警, 不阻断启动
            Logger::log("VirtualBox", QString("⚠️ ISO 挂载失败 (%1), 可能尚未选择镜像").arg(vm.iso));
        }
    }

    // 5) 核心设置
    if (int rc = applySettings(vm, err); rc != 0)
        return false;

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
    Logger::log("VirtualBox", QString("✅ %1 已启动 (headless=%2)").arg(name, headless));
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
    Logger::log("VirtualBox", QString("⏹️ %1 已发送停止命令 (force=%2)").arg(name, force));
    return true;
}

// ═══════════════════════════════════════════════════════════════════
//  内部辅助
// ═══════════════════════════════════════════════════════════════════

static int addController(const QString &vmName, const QString &name,
                         const QString &type, QString *err)
{
    QStringList exists = {"storagectl", vmName, "--list"};
    QString listOut;
    runVBox(exists, &listOut);  // 忽略错误 (某些版本 --list 需 --details)

    QString lower = listOut.toLower();
    bool has = lower.contains("\"" + name.toLower() + "\"")
            || lower.contains(name.toLower());
    if (has) return 0;  // 已存在

    QStringList add = {"storagectl", vmName, "--name", name, "--add", type};
    return runVBox(add, err);
}

static int applySettings(const VMConfig &vm, QString *err)
{
    QStringList a = {"modifyvm", vm.name};
    a << "--acpi" << (isLegacyOS(vm.ostype) ? "off" : "on");
    a << "--ioapic" << (isLegacyOS(vm.ostype) ? "off" : "on");
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

    return runVBox(a, err);
}
