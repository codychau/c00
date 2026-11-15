#include "vmpage.h"
#include "vmdialog.h"
#include "virtualbox.h"
#include "qemuargs.h"
#include "adminhelper.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QInputDialog>
#include <QLineEdit>
#include <QDir>
#include <QRegularExpression>
#include <QFileInfo>
#include <QFile>
#include <QSet>
#include <QColor>
#include <QBrush>
#include <QStandardPaths>
#include <QDateTime>
#include <QPointer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QTextStream>

#include "logger.h"

#include <unistd.h>

// 与 MainWindow 顶栏高度一致：主窗口的页面堆栈铺满整块主区，顶栏以浮层
// 形式盖在内容上做毛玻璃，因此正文要自行让出这段顶部空间。
static constexpr int kTopbarInset = 52;

// 列索引
enum Col { ColName = 0, ColCPU, ColMem, ColDisk, ColNet, ColVNC, ColStatus, ColExtra, ColCount };

VMPage::VMPage(QWidget *parent)
    : QWidget(parent), m_mgr(VMConfigManager::defaultConfigPath())
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, kTopbarInset, 16, 16);

    auto *title = new QLabel("🖥️  虚机管理");
    QFont f = title->font(); f.setPointSize(14); f.setBold(true);
    title->setFont(f);
    layout->addWidget(title);

    // ── 表格（8 列，含状态）──
    m_table = new QTableWidget(0, ColCount);
    m_table->setHorizontalHeaderLabels({
        "名称", "CPU", "内存", "磁盘", "网络", "远程", "状态", "额外参数"
    });
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->verticalHeader()->hide();
    m_table->setAlternatingRowColors(true);

    connect(m_table, &QTableWidget::cellDoubleClicked,
            this, [this](int row, int) { editVM(row); });
    connect(m_table, &QTableWidget::itemSelectionChanged,
            this, &VMPage::onSelectionChanged);

    layout->addWidget(m_table);

    // ── 按钮行 ──
    auto *btnRow = new QHBoxLayout();

    m_status = new QLabel("就绪");
    m_status->setProperty("role", "mute");
    btnRow->addWidget(m_status, 1);

    m_addBtn = new QPushButton("➕ 新建");
    connect(m_addBtn, &QPushButton::clicked, this, &VMPage::addVM);
    btnRow->addWidget(m_addBtn);

    m_editBtn = new QPushButton("✏️ 编辑");
    connect(m_editBtn, &QPushButton::clicked, this, [this]() {
        int row = m_table->currentRow();
        if (row >= 0) editVM(row);
    });
    btnRow->addWidget(m_editBtn);

    m_deleteBtn = new QPushButton("🗑️ 删除");
    connect(m_deleteBtn, &QPushButton::clicked, this, &VMPage::deleteVM);
    btnRow->addWidget(m_deleteBtn);

    m_startBtn = new QPushButton("▶️ 启动");
    m_startBtn->setMinimumWidth(100);
    connect(m_startBtn, &QPushButton::clicked, this, [this]() {
        int row = m_table->currentRow();
        if (row < 0) return;
        auto *item = m_table->item(row, ColStatus);
        if (item && item->text().contains("运行中"))
            stopVM();
        else
            startVM();
    });
    btnRow->addWidget(m_startBtn);

    m_vncBtn = new QPushButton("🖥️ 远程连接");
    connect(m_vncBtn, &QPushButton::clicked, this, &VMPage::connectVNC);
    btnRow->addWidget(m_vncBtn);

    layout->addLayout(btnRow);

    // ── 检测远程连接客户端 (QEMU 用 VNC, VirtualBox 用 RDP) ──
    m_vncViewer = findVNCViewer();
    m_rdpClient = findRDPClient();
    if (m_vncViewer.isEmpty() && m_rdpClient.isEmpty()) {
        m_vncBtn->setToolTip("未找到远程连接客户端 (VNC: tigervnc / gtk-vnc; RDP: xfreerdp / rdesktop / remmina)");
        m_vncBtn->setEnabled(false);
    } else {
        QString tip = "连接: ";
        if (!m_vncViewer.isEmpty()) tip += QString("VNC → %1").arg(m_vncViewer);
        if (!m_rdpClient.isEmpty()) tip += (tip.endsWith(' ') ? " / " : "") + QString("RDP → %1").arg(m_rdpClient);
        m_vncBtn->setToolTip(tip);
    }

    // ── 加载配置 ──
    if (!m_mgr.load()) {
        m_status->setText("⚠️ 配置文件读取失败");
    }
    refreshTable();

    // ── 定时刷新状态（每 5 秒）──
    m_statusTimer = new QTimer(this);
    connect(m_statusTimer, &QTimer::timeout, this, &VMPage::refreshStatus);
    m_statusTimer->start(5000);
    refreshStatus();

    // ── 等状态刷新后，自动启动标记了 autoStart 的虚机 ──
    QTimer::singleShot(1000, this, &VMPage::startAutoStartVMs);
}

QString VMPage::findVNCViewer() const
{
    QStringList candidates = {"vncviewer", "Xtigervnc", "gvncviewer", "vinagre", "krdc", "remmina"};
    for (const auto &candidate : candidates) {
        QString path = QStandardPaths::findExecutable(candidate);
        if (!path.isEmpty()) return path;
    }
    return {};
}

QString VMPage::findRDPClient() const
{
    QStringList candidates = {"xfreerdp", "xfreerdp3", "wlfreerdp", "rdesktop", "remmina", "krdc"};
    for (const auto &candidate : candidates) {
        QString path = QStandardPaths::findExecutable(candidate);
        if (!path.isEmpty()) return path;
    }
    return {};
}

void VMPage::onSelectionChanged()
{
    updateStartButton();
}

void VMPage::updateStartButton()
{
    int row = m_table->currentRow();
    if (row < 0) {
        m_startBtn->setText("▶️ 启动");
        return;
    }
    auto *item = m_table->item(row, ColStatus);
    bool running = item && item->text().contains("运行中");
    m_startBtn->setText(running ? "⏹️ 停止" : "▶️ 启动");
}

void VMPage::refreshTable()
{
    const auto &vms = m_mgr.vms();
    m_table->setRowCount(vms.size());

    for (int i = 0; i < vms.size(); ++i) {
        const auto &vm = vms[i];

        m_table->setItem(i, ColName, new QTableWidgetItem(vm.name));
        m_table->setItem(i, ColCPU,  new QTableWidgetItem(QString::number(vm.cpu)));
        m_table->setItem(i, ColMem, new QTableWidgetItem(
            vm.memory >= 1024
                ? QString("%1 GB").arg(vm.memory / 1024.0, 0, 'f', 1)
                : QString("%1 MB").arg(vm.memory)));
        m_table->setItem(i, ColDisk, new QTableWidgetItem(
            QFileInfo(vm.systemDiskPath()).fileName()));

        m_table->setItem(i, ColNet, new QTableWidgetItem(vm.net));

        // 远程连接列: QEMU 显示 VNC 端口, VirtualBox 显示 RDP 端口
        QString remote = (vm.vmType == "virtualbox")
            ? (vm.vboxRdp ? QString("RDP %1").arg(vm.vboxRdpPort) : "RDP 禁用")
            : (vm.vnc < 0 ? "禁用" : QString("VNC %1").arg(5900 + vm.vnc));
        m_table->setItem(i, ColVNC, new QTableWidgetItem(remote));

        auto *statusItem = new QTableWidgetItem("⋯ 检测中");
        statusItem->setForeground(QColor("#9ca3af"));
        m_table->setItem(i, ColStatus, statusItem);

        QStringList extras;
        if (!vm.extra.isEmpty()) {
            extras << (vm.extra.left(28).replace('\n', ' ')
                      + (vm.extra.length() > 28 ? "…" : ""));
        }
        int dataCount = vm.disks.size() - (vm.systemDiskIndex() >= 0 ? 1 : 0);
        if (dataCount > 0)
            extras << QString("💾%1").arg(dataCount);
        if (!vm.pciDevices.isEmpty())
            extras << QString("🖧%1").arg(vm.pciDevices.size());
        if (vm.vfioMultifunction && !vm.pciDevices.isEmpty())
            extras << "🖧mf";
        if (!vm.pciHole64.isEmpty())
            extras << QString("⬔%1").arg(vm.pciHole64);
        const int usbCount = vm.usbHostVendors.size() + vm.usbEvdevPaths.size();
        if (usbCount > 0)
            extras << QString("🖳%1").arg(usbCount);
        if (vm.audioBackend != "none")
            extras << QString("♪%1").arg(vm.audioBackend);
        if (vm.uefi)
            extras << "⊙UEFI";
        if (vm.displayNone)
            extras << "🖵none";
        if (vm.qemuRunRoot)
            extras << "🔒root";
        if (vm.ramfb)
            extras << "🖵ramfb";
        if (vm.hugepages)
            extras << "🖧H";
        if (vm.autoStart)
            extras << "🔄auto";
        m_table->setItem(i, ColExtra, new QTableWidgetItem(
            extras.isEmpty() ? "" : extras.join(" | ")));
    }

    m_status->setText(QString("共 %1 个虚机").arg(vms.size()));
    updateStartButton();
}

// 判断某个 QEMU 进程行（ps -ef 输出）是否属于给定虚机
static bool qemuLineForVM(const QString &line, const QString &vmName, const QString &disk)
{
    // 1) 精确匹配 -name 参数后面的值，避免子串误命中（如 vm 名 "win7" 误匹配到 "win7x"）
    QStringList fields = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    for (int i = 0; i + 1 < fields.size(); ++i) {
        if (fields[i] == "-name" && fields[i + 1] == vmName)
            return true;
    }

    // 2) 系统盘路径命中（磁盘为空时跳过 ——
    //    否则 "".contains() 会命中所有进程，让尚未装系统的 ISO 启动型虚机被误判为“运行中”）
    if (!disk.isEmpty() && line.contains(disk))
        return true;

    return false;
}

void VMPage::refreshStatus()
{
    if (m_mgr.vms().isEmpty()) return;

    QProcess ps;
    ps.start("ps", {"-ef"});
    ps.waitForFinished(3000);
    QStringList allLines = QString::fromUtf8(ps.readAllStandardOutput()).split('\n');

    QStringList qemuLines;
    for (const auto &line : allLines) {
        if (line.contains("qemu-system-"))
            qemuLines.append(line);
    }

    bool prevRunning = false;
    int curRow = m_table->currentRow();
    if (curRow >= 0) {
        auto *item = m_table->item(curRow, ColStatus);
        prevRunning = item && item->text().contains("运行中");
    }

    // VirtualBox 运行状态: 一次 list runningvms 搞定, 不逐个 showvminfo
    // (showvminfo 会为运行中的机器建立会话, 短时间占住配置锁, 可能让
    //  VirtualBox GUI 打开设置时报「正在被其他进程修改」)
    const QSet<QString> vboxRunning =
        (m_mgr.vms().isEmpty() ? QSet<QString>() : VirtualBox::runningVMs());

    for (int i = 0; i < m_mgr.vms().size(); ++i) {
        const auto &vm = m_mgr.vms()[i];
        bool running = false;

        if (vm.vmType == "virtualbox") {
            running = vboxRunning.contains(vm.name);
        } else {
            for (const auto &ql : qemuLines) {
                if (qemuLineForVM(ql, vm.name, vm.systemDiskPath())) {
                    running = true;
                    break;
                }
            }
        }

        auto *item = m_table->item(i, ColStatus);
        if (!item) continue;

        if (running) {
            item->setText(vm.runAsService ? "✅ 运行中 (服务)" : "✅ 运行中");
            item->setForeground(QColor("#22c55e"));
        } else {
            item->setText("⏹️ 已停止");
            item->setForeground(QColor("#9ca3af"));
            // 进程已退出，清理跟踪
            if (m_runningProcs.contains(vm.name)) {
                m_runningProcs[vm.name]->deleteLater();
                m_runningProcs.remove(vm.name);
            }
        }
    }

    // 状态变化时更新按钮文字
    bool nowRunning = false;
    if (curRow >= 0) {
        auto *item = m_table->item(curRow, ColStatus);
        nowRunning = item && item->text().contains("运行中");
    }
    if (prevRunning != nowRunning)
        updateStartButton();
}

QString VMPage::qmpSocketPath(const QString &vmName) const
{
    // 用临时目录存放 QMP socket，避免名称冲突
    static QTemporaryDir tmpDir;
    QString safeName = vmName;
    safeName.replace(QRegularExpression("[^\\w\\-]"), "_");
    return (tmpDir.isValid() ? tmpDir.path() : "/tmp")
           + "/qmp_" + safeName + ".sock";
}

void VMPage::addVM()
{
    VMDialog dlg(this);
    if (dlg.exec() == QDialog::Accepted) {
        m_mgr.addVM(dlg.vmConfig());
        saveConfig();
        refreshTable();
        refreshStatus();
    }
}

void VMPage::editVM(int row)
{
    if (row < 0 || row >= m_mgr.vms().size()) return;

    VMDialog dlg(this);
    dlg.setVMConfig(m_mgr.vms()[row]);
    dlg.setWindowTitle("编辑虚机 — " + m_mgr.vms()[row].name);
    if (dlg.exec() == QDialog::Accepted) {
        m_mgr.updateVM(row, dlg.vmConfig());
        saveConfig();
        refreshTable();
        refreshStatus();
    }
}

void VMPage::deleteVM()
{
    int row = m_table->currentRow();
    if (row < 0 || row >= m_mgr.vms().size()) return;

    auto *statusItem = m_table->item(row, ColStatus);
    bool running = statusItem && statusItem->text().contains("运行中");

    if (running) {
        auto ret = QMessageBox::warning(this, "虚机运行中",
            QString("「%1」正在运行，确定要删除配置？\n（不会停止正在运行的 QEMU 进程）")
            .arg(m_mgr.vms()[row].name),
            QMessageBox::Yes | QMessageBox::No);
        if (ret != QMessageBox::Yes) return;
    } else {
        auto ret = QMessageBox::question(this, "确认删除",
            QString("确定删除虚机「%1」？").arg(m_mgr.vms()[row].name),
            QMessageBox::Yes | QMessageBox::No);
        if (ret != QMessageBox::Yes) return;
    }

    m_mgr.removeVM(row);
    saveConfig();
    refreshTable();
    refreshStatus();
}

// ── 启动 ──

// 检查 /etc/fstab 中是否还有 `mount -a` 需要处理的未就绪挂载点。
// 全部已挂载时返回 false，此时无需触发 pkexec 提权。
static bool fstabHasPendingMounts()
{
    // 1) 收集当前已挂载的挂载点
    QSet<QString> mounted;
    QFile mnt("/proc/self/mounts");
    if (mnt.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!mnt.atEnd()) {
            const QString line = QString::fromUtf8(mnt.readLine()).trimmed();
            if (line.isEmpty()) continue;
            const QStringList f = line.split(' ', Qt::SkipEmptyParts);
            if (f.size() < 2) continue;
            QString mp = f[1];
            // mount 表中的空格等转义字符还原
            mp.replace("\\040", " ").replace("\\011", "\t")
              .replace("\\012", "\n").replace("\\134", "\\");
            const QFileInfo info(mp);
            if (info.exists()) mp = info.canonicalFilePath();
            mounted.insert(mp);
        }
    }

    // 2) 遍历 fstab，凡是 mount -a 仍会尝试挂载的条目都算“未就绪”
    QFile fstab("/etc/fstab");
    if (!fstab.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    const QRegularExpression ws("\\s+");
    while (!fstab.atEnd()) {
        const QString line = QString::fromUtf8(fstab.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        const QStringList f = line.split(ws, Qt::SkipEmptyParts);
        if (f.size() < 4) continue;
        const QString &target = f[1];
        const QString &type   = f[2];
        const QString &opts   = f[3];

        if (type == "swap") continue;              // swap 不参与挂载点检查
        if (opts.split(',').contains("noauto")) continue; // mount -a 不会处理

        QString mp = target;
        const QFileInfo info(target);
        if (info.exists()) mp = info.canonicalFilePath();
        if (!mounted.contains(mp))
            return true;                           // 还有未挂载的
    }
    return false;
}

void VMPage::startVM()
{
    const int row = m_table->currentRow();
    if (row < 0) return;
    startVMAt(row, false);
}

bool VMPage::startVMByName(const QString &name, bool quiet)
{
    if (name.isEmpty())
        return false;

    for (int i = 0; i < m_mgr.vms().size(); ++i) {
        if (m_mgr.vms()[i].name == name) {
            m_table->setCurrentCell(i, 0);
            return startVMAt(i, quiet);
        }
    }

    Logger::log("VM", QString("场景请求启动虚机，但配置中不存在：%1").arg(name));
    return false;
}

bool VMPage::startVMAt(int row, bool quiet)
{
    if (row < 0 || row >= m_mgr.vms().size()) return false;

    auto *statusItem = m_table->item(row, ColStatus);
    if (statusItem && statusItem->text().contains("运行中")) {
        if (!quiet) {
            QMessageBox::information(this, "提示",
                QString("「%1」已在运行中").arg(m_mgr.vms()[row].name));
        }
        return false;
    }

    const auto &vm = m_mgr.vms()[row];

    // ── VirtualBox 分支 ──
    if (vm.vmType == "virtualbox")
        return startVBoxVM(vm, quiet);

    // ── 等待挂载点（可选）──
    if (vm.waitMount) {
        m_status->setText("🔄 正在检查 fstab 中未就绪的挂载点…");
        QCoreApplication::processEvents();

        // 只有确实存在未挂载的条目时才提权执行 mount -a；
        // 全部就绪时直接跳过，避免每次都弹出授权框
        if (fstabHasPendingMounts()) {
            m_status->setText("🔄 检测到未就绪的挂载点，正在尝试挂载…");
            QCoreApplication::processEvents();

            // mount -a 自动挂载 fstab 中尚未挂载的设备
            QProcess mountProc;
            mountProc.start("pkexec", {"mount", "-a"});
            if (mountProc.waitForFinished(30000)) {
                if (mountProc.exitCode() == 0) {
                    QString mountOut = QString::fromUtf8(mountProc.readAllStandardOutput()).trimmed();
                    if (!mountOut.isEmpty())
                        Logger::log("VM", QString("mount -a 输出: %1").arg(mountOut.left(500)));
                    Logger::log("VM", "✅ 等待挂载点完成");
                } else {
                    QString err = QString::fromUtf8(mountProc.readAllStandardError()).trimmed();
                    Logger::log("VM", QString("⚠️ mount -a 结果: exit=%1, err=%2")
                        .arg(mountProc.exitCode()).arg(err.left(500)));
                }
            } else {
                Logger::log("VM", "⏰ mount -a 超时");
            }
        } else {
            Logger::log("VM", "✅ fstab 中所有挂载点已就绪，跳过提权挂载");
        }
        m_status->setText("✅ 挂载点检查完成，准备启动虚机…");
        QCoreApplication::processEvents();
    }


    // ── 以服务文件执行: 钩子与 QEMU 都归 systemd 管 ──
    // 程序不 fork 子进程, 所以「停 greetd 把桌面干掉、顺带把程序也杀了」这条链
    // 断掉了: 服务跑在系统层, 归还脚本挂在 ExecStopPost 上由 systemd 保证执行。
    if (vm.runAsService) {
        const QString sock = serviceSocketPath(vm.name);
        const QStringList sargs = buildQemuArgs(vm, sock);
        return startVMAsService(vm, sargs, sock, quiet);
    }

    // ── Root 钩子: 启动前执行 (单显卡直通: 停合成器 + 绑定 vfio-pci) ──
    // 失败则不启动 QEMU, 避免把没有显卡的桌面直接交给虚机.
    if (!vm.preStartRoot.trimmed().isEmpty()) {
        m_status->setText("🔒 正在执行启动前 root 脚本（显卡直通切换）…");
        QCoreApplication::processEvents();
        QString hookOut, hookErr;
        if (!AdminHelper::runAsRootScript(vm.preStartRoot, this, &hookOut, &hookErr)) {
            Logger::log("VM", QString("❌ 启动前 root 脚本失败: %1").arg(hookErr.left(500)));
            if (!quiet)
                QMessageBox::warning(this, "启动前脚本失败",
                    QString("启动前 root 脚本未成功，虚机「%1」未启动。\n\n%2")
                        .arg(vm.name, hookErr.left(500)));
            return false;
        }
        Logger::log("VM", QString("✅ 启动前 root 脚本完成: %1").arg(hookOut.left(300)));
    }

    // QMP 管理接口（用于关机/电源管理）
    const QString sockPath = qmpSocketPath(vm.name);

    // 参数生成 —— 纯逻辑, 见 qemuargs.cpp (可用 tools/qemuargs-test 静态核对)
    const QStringList args = buildQemuArgs(vm, sockPath);

    QString cmdLine = vm.qemuBinary + " " + args.join(' ');
    Logger::log("VM", QString("启动虚机: %1").arg(vm.name));
    Logger::log("VM", cmdLine.left(500));

    auto *proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);

    // 音频后端走 PulseAudio/pipewire: 需要用户会话的 runtime dir 与 socket;
    // root 运行 QEMU 时尤其明显 (pipewire-pulse 不建经典 pidfile, 见下方兜底).
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString rtDir = QString("/run/user/%1").arg(getuid());
    if (env.value("XDG_RUNTIME_DIR").isEmpty())
        env.insert("XDG_RUNTIME_DIR", rtDir);
    if (vm.audioBackend == "pa" || vm.audioBackend == "pipewire") {
        if (env.value("PULSE_SERVER").isEmpty())
            env.insert("PULSE_SERVER", QString("unix:%1/pulse/native").arg(rtDir));
        const QString pidFile = rtDir + "/pulse/pid";
        if (!QFile::exists(pidFile)) {
            QProcess pg;
            pg.start("pgrep", {"-u", QString::number(getuid()), "pipewire-pulse"});
            pg.waitForFinished(2000);
            QString pid = QString::fromUtf8(pg.readAllStandardOutput()).trimmed().split('\n').value(0);
            if (pid.isEmpty()) pid = "1";
            QFile f(pidFile);
            if (f.open(QIODevice::WriteOnly)) { f.write(pid.toUtf8()); f.close(); }
        }
    }
    proc->setProcessEnvironment(env);

    // 以 root 运行 QEMU (sudo): PCI 直通需要 /dev/vfio/*, evdev 透传需要 /dev/input/*
    QString runBin = vm.qemuBinary;
    QStringList runArgs = args;
    QString sudoPw;
    const bool pwless = AdminHelper::passwordlessSudoAvailable();
    if (vm.qemuRunRoot) {
        if (pwless) {
            runArgs = QStringList{"-n"} + QStringList{vm.qemuBinary} + args;
        } else {
            bool ok = false;
            sudoPw = QInputDialog::getText(this, "以 root 运行 QEMU", "请输入管理员密码：",
                                           QLineEdit::Password, QString(), &ok);
            if (!ok || sudoPw.isEmpty()) {
                if (!quiet)
                    QMessageBox::warning(this, "已取消", "未提供密码，虚机未启动。");
                delete proc;
                return false;
            }
            runArgs = QStringList{"-S", "-p", ""} + QStringList{vm.qemuBinary} + args;
        }
        runBin = "sudo";
        Logger::log("VM", QString("以 root 运行 QEMU (%1)").arg(pwless ? "免密 sudo" : "sudo -S"));
    }

    proc->start(runBin, runArgs);
    if (vm.qemuRunRoot && !pwless) proc->write((sudoPw + "\n").toUtf8());

    Logger::log("VM", QString("实际命令: %1").arg(runBin + " " + runArgs.join(' ')).left(500));

    if (!proc->waitForStarted(3000)) {
        QString err = proc->errorString();
        Logger::log("VM", QString("❌ 启动失败: %1 — %2").arg(vm.name, err));
        if (!quiet) {
            QMessageBox::warning(this, "启动失败",
                QString("无法启动 QEMU:\n%1").arg(err));
        }
        delete proc;
        return false;
    }

    auto startedAt = QDateTime::currentSecsSinceEpoch();
    Logger::log("VM", QString("✅ %1 已启动 (PID: %2)").arg(vm.name).arg(proc->processId()));
    m_status->setText(QString("▶️ %1 已启动 (PID: %2)").arg(vm.name).arg(proc->processId()));

    m_runningProcs[vm.name] = proc;
    updateStartButton();

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, proc, name = vm.name, startedAt, quiet, postHook = vm.postStopRoot](int exitCode, QProcess::ExitStatus status) {
        auto elapsed = QDateTime::currentSecsSinceEpoch() - startedAt;
        QString stderr = QString::fromUtf8(proc->readAll()).trimmed();

        if (status == QProcess::CrashExit)
            Logger::log("VM", QString("💥 %1 异常退出").arg(name));
        else
            Logger::log("VM", QString("⏹️ %1 正常退出 (exit: %2, 运行 %3s)")
                .arg(name).arg(exitCode).arg(elapsed));

        if (elapsed < 5 && exitCode != 0) {
            QString errMsg = stderr.isEmpty() ? "虚机启动后立即退出" : stderr.left(2000);
            Logger::log("VM", QString("❌ %1 启动失败:\n%2").arg(name, errMsg));
            
            bool hasPermissionError = checkPermissionError(stderr);

            if (!quiet) {
                // 延迟弹窗，避免在信号处理器内直接弹模态框重入事件循环导致崩溃
                QTimer::singleShot(0, this, [this, name, exitCode, errMsg, hasPermissionError]() {
                    if (hasPermissionError) {
                        QMessageBox msgBox(this);
                        msgBox.setWindowTitle("虚机启动失败 - 权限问题");
                        msgBox.setIcon(QMessageBox::Warning);
                        msgBox.setText(QString("虚机「%1」启动失败，可能是映像文件权限问题。\n\nQEMU 输出:\n%2")
                            .arg(name).arg(errMsg.left(500)));
                        msgBox.setInformativeText("是否配置 QEMU sudo 免密权限以解决此问题？\n\n"
        "勾选「以服务文件执行」的虚机还需要 systemctl start/stop vm-* 与 daemon-reload 免密, "
        "本配置一并写入 (写单元文件仍会弹密码, 因为那条要给 bash 配免密 root, 不该做)。");
                        msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
                        msgBox.setDefaultButton(QMessageBox::Yes);

                        if (msgBox.exec() == QMessageBox::Yes) {
                            configureQemuSudoers(name);
                        }
                    } else {
                        QMessageBox::critical(this, "虚机启动失败",
                            QString("%1\n\n退出码: %2\n\nQEMU 输出:\n%3")
                                .arg(name).arg(exitCode).arg(errMsg));
                    }
                });
            }
        }

        // 退出后 root 钩子 (回绑显卡 + 重启合成器). 延迟执行避免模态框重入.
        if (!postHook.trimmed().isEmpty()) {
            QTimer::singleShot(0, this, [this, name, postHook]() {
                QString hookOut, hookErr;
                if (AdminHelper::runAsRootScript(postHook, this, &hookOut, &hookErr))
                    Logger::log("VM", QString("✅ %1 退出后 root 脚本完成: %2").arg(name, hookOut.left(300)));
                else
                    Logger::log("VM", QString("⚠️ %1 退出后 root 脚本失败: %2").arg(name, hookErr.left(500)));
            });
        }

        m_runningProcs.remove(name);
        proc->deleteLater();
        m_status->setText(QString("⏹️ %1 已停止").arg(name));
        refreshStatus();
    });

    return true;
}

// ── 服务化启动: 写单元文件 + systemctl start ──

bool VMPage::startVMAsService(const VMConfig &vm, const QStringList &args,
                              const QString &sockPath, bool quiet)
{
    const QString unit     = serviceUnitName(vm.name);
    const QString unitPath = "/etc/systemd/system/" + unit;
    const QString text     = buildSystemdUnit(vm, args, sockPath, getuid());

    // heredoc 定界符不能出现在单元内容里
    QString delim = "ST_UNIT_EOF";
    while (text.contains(delim)) delim += "X";

    // 三个文件一次写完: 单元 + 启动前钩子脚本 + 退出后钩子脚本
    const QString prePath  = serviceScriptPath(vm.name, false);
    const QString postPath = serviceScriptPath(vm.name, true);
    const QString script = QString(
        "mkdir -p /var/lib/ServerToolbox\n"
        "cat > %1 <<'ST_PRE_EOF'\n%2\nST_PRE_EOF\n"
        "cat > %3 <<'ST_POST_EOF'\n%4\nST_POST_EOF\n"
        "chmod 700 %1 %3\n"
        "cat > %5 <<'%6'\n%7%6\n"
        "chmod 644 %5\n"
        "systemctl daemon-reload\n"
        "systemctl start %8\n"
        "systemctl is-active %8"
        + (vm.autoStart ? "\nsystemctl enable %8" : QString()))
        .arg(prePath, vm.preStartRoot.trimmed() + "\n",
             postPath, vm.postStopRoot.trimmed() + "\n",
             unitPath, delim, text, unit);

    m_status->setText("📄 正在写入并启动服务 " + unit);
    QCoreApplication::processEvents();

    QString out, err;
    if (!AdminHelper::runAsRootScript(script, this, &out, &err)) {
        Logger::log("VM", QString("❌ 服务 %1 启动失败: %2").arg(unit, err.left(600)));
        // 把 journal 尾巴抓出来, 便于定位是参数错还是权限错
        QProcess j;
        j.start("journalctl", {"-u", unit, "-n", "40", "--no-pager"});
        if (j.waitForFinished(5000)) {
            const QString jout = QString::fromUtf8(j.readAllStandardOutput()).trimmed();
            if (!jout.isEmpty())
                Logger::log("VM", QString("journal: %1").arg(jout.right(1200)));
        }
        if (!quiet)
            QMessageBox::warning(this, "服务启动失败",
                QString("服务 %1 未能启动。\n\n%2\n\n(日志里可看 journalctl -u %1)")
                    .arg(unit, err.left(600)));
        return false;
    }

    Logger::log("VM", QString("✅ 单元文件已写入: %1").arg(unitPath));
    Logger::log("VM", QString("✅ 服务 %1 已启动, 状态: %2").arg(unit, out.simplified().left(120)));
    m_status->setText(QString("▶️ %1 由 systemd 服务启动 (%2)").arg(vm.name, unit));
    refreshStatus();
    updateStartButton();
    return true;
}

// ── VirtualBox 启动 ──

bool VMPage::startVBoxVM(const VMConfig &vm, bool quiet)
{
    // 表格里「运行中」每 5 秒才刷一次, 可能滞后 (刚点过启动 / VBox 窗口已开).
    // 这里先做一次权威查询 (list runningvms, 不占配置锁), 避免重复启动被
    // VirtualBox 以会话锁拒绝后误报「启动失败」.
    if (VirtualBox::isRunning(vm.name)) {
        refreshStatus();
        updateStartButton();
        if (!quiet)
            QMessageBox::information(this, "提示",
                QString("「%1」已在运行中").arg(vm.name));
        return false;
    }

    QString err;
    // 创建/配置 (若 VM 尚未在 VirtualBox 注册)
    if (!VirtualBox::prepare(vm, &err)) {
        Logger::log("VirtualBox", QString("❌ 准备失败 %1: %2").arg(vm.name, err));
        if (!quiet)
            QMessageBox::warning(this, "VirtualBox 启动失败",
                QString("「%1」初始化失败:\n%2").arg(vm.name, err));
        return false;
    }

    if (!VirtualBox::start(vm.name, /*headless=*/false)) {
        if (!quiet)
            QMessageBox::warning(this, "VirtualBox 启动失败",
                QString("「%1」启动失败:\nVirtualBox 拒绝了启动请求 "
                        "(虚机可能正被其他会话占用或配置异常).\n\n详细信息见「日志」页.")
                    .arg(vm.name));
        return false;
    }

    m_status->setText(QString("▶️ %1 已启动 (VirtualBox 窗口)").arg(vm.name));
    // 立即规范化表格状态，使“运行中”文案可被停止逻辑识别
    refreshStatus();
    updateStartButton();
    return true;
}

// ── 停止 ──

void VMPage::stopVM()
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_mgr.vms().size()) return;

    const QString vmName = m_mgr.vms()[row].name;
    auto *statusItem = m_table->item(row, ColStatus);
    if (!statusItem || !statusItem->text().contains("运行中")) return;

    // 弹出选择
    auto *menu = new QMessageBox(this);
    menu->setWindowTitle("停止虚机");
    menu->setText(QString("选择停止方式: %1").arg(vmName));
    menu->setIcon(QMessageBox::Question);

    auto *btnShutdown = menu->addButton("🛑 正常关机", QMessageBox::AcceptRole);
    auto *btnKill     = menu->addButton("⚡ 极速停止", QMessageBox::DestructiveRole);
    auto *btnCancel   = menu->addButton("取消", QMessageBox::RejectRole);
    menu->setDefaultButton(btnShutdown);
    menu->exec();

    if (menu->clickedButton() == btnCancel)
        return;

    stopVMAt(row, menu->clickedButton() == btnKill, false);
}

bool VMPage::stopVMByName(const QString &name, bool force, bool quiet)
{
    if (name.isEmpty())
        return false;

    for (int i = 0; i < m_mgr.vms().size(); ++i) {
        if (m_mgr.vms()[i].name == name) {
            m_table->setCurrentCell(i, 0);
            return stopVMAt(i, force, quiet);
        }
    }

    Logger::log("VM", QString("场景请求停止虚机，但配置中不存在：%1").arg(name));
    return false;
}

bool VMPage::stopVMAt(int row, bool force, bool quiet)
{
    if (row < 0 || row >= m_mgr.vms().size()) return false;

    const auto &vm = m_mgr.vms()[row];
    auto *statusItem = m_table->item(row, ColStatus);
    if (!statusItem || !statusItem->text().contains("运行中")) return false;

    // ── VirtualBox 分支 ──
    if (vm.vmType == "virtualbox")
        return stopVBoxVM(vm, force, quiet);

    // ── 服务分支 ──
    // 正常关机走 QMP quit: QEMU 退出后 systemd 触发 ExecStopPost, 归还脚本必然执行。
    // 极速停止走 systemctl stop (需要 root), 同样触发 ExecStopPost。
    if (vm.runAsService) {
        const QString unit = serviceUnitName(vm.name);
        const QString sock = serviceSocketPath(vm.name);

        if (!force) {
            Logger::log("VM", QString("服务模式正常关机: %1 (QMP quit → ExecStopPost 执行归还脚本)").arg(vm.name));
            sendQemuPowerdown(vm.name, sock);
            // 等服务转为非 active (最多 ~30s), 避免状态列滞后
            for (int i = 0; i < 15; ++i) {
                QProcess p;
                p.start("systemctl", {"is-active", unit});
                if (!p.waitForFinished(2000)) { p.kill(); break; }
                if (QString::fromUtf8(p.readAllStandardOutput()).trimmed() != "active") break;
                QCoreApplication::processEvents();
            }
        } else {
            Logger::log("VM", QString("服务模式极速停止: %1").arg(unit));
            QString o, e;
            if (AdminHelper::passwordlessSudoAvailable())
                QProcess::execute("sudo", {"-n", "systemctl", "stop", unit});
            else if (!AdminHelper::runAsRootScript("systemctl stop " + unit, this, &o, &e)) {
                Logger::log("VM", QString("❌ systemctl stop 失败: %1").arg(e.left(400)));
                if (!quiet)
                    QMessageBox::warning(this, "停止失败",
                        QString("停止服务 %1 失败:\n%2").arg(unit, e.left(400)));
                return false;
            }
        }
        refreshStatus();
        updateStartButton();
        return true;
    }

    if (!force) {
        Logger::log("VM", QString("正常关机: %1").arg(vm.name));
        sendQemuPowerdown(vm.name, qmpSocketPath(vm.name));

        // 如果进程还在跟踪，等待最多 15 秒后强行终止
        if (m_runningProcs.contains(vm.name)) {
            QProcess *p = m_runningProcs[vm.name];
            if (quiet) {
                // 定时任务不能阻塞界面：15 秒后仍在运行则强制终止
                QPointer<QProcess> guard(p);
                const QString vmName = vm.name;
                QTimer::singleShot(15000, this, [guard, vmName]() {
                    if (guard && guard->state() != QProcess::NotRunning) {
                        Logger::log("VM", QString("关机超时，强行终止: %1").arg(vmName));
                        guard->kill();
                    }
                });
            } else if (!p->waitForFinished(15000)) {
                Logger::log("VM", QString("超时，强行终止: %1").arg(vm.name));
                p->kill();
                p->waitForFinished(2000);
            }
        }
    } else {
        Logger::log("VM", QString("极速停止: %1").arg(vm.name));
        bool killed = false;

        // 优先从跟踪表杀
        if (m_runningProcs.contains(vm.name)) {
            QProcess *p = m_runningProcs[vm.name];
            if (p->state() != QProcess::NotRunning) {
                Logger::log("VM", QString("杀掉跟踪的 QEMU 进程 (PID %1)").arg(p->processId()));
                p->kill();
                p->waitForFinished(2000);
                killed = true;
            }
            m_runningProcs.remove(vm.name);
            p->deleteLater();
        }

        // 不在跟踪表中，用 ps 查找并杀掉 QEMU 进程
        if (!killed) {
            QProcess psProc;
            psProc.start("ps", {"-ef"});
            if (psProc.waitForFinished(3000)) {
                QString output = QString::fromUtf8(psProc.readAllStandardOutput());
                for (const auto &line : output.split('\n')) {
                    if (line.contains("qemu-system-") && line.contains(vm.name) && line.contains("-name")) {
                        QStringList fields = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
                        if (fields.size() >= 2) {
                            int pid = fields[1].toInt();
                            if (pid > 0) {
                                Logger::log("VM", QString("杀掉 QEMU PID %1 (虚机 %2)").arg(pid).arg(vm.name));
                                QProcess::execute("kill", {"-9", QString::number(pid)});
                                killed = true;
                            }
                        }
                    }
                }
            }
        }

        if (!killed) {
            QString warn = QString("未找到虚机「%1」的 QEMU 进程，可能已退出").arg(vm.name);
            Logger::log("VM", warn);
            if (!quiet)
                QMessageBox::warning(this, "极速停止", warn);
        }
    }

    refreshStatus();
    return true;
}

// ── VirtualBox 停止 ──

bool VMPage::stopVBoxVM(const VMConfig &vm, bool force, bool quiet)
{
    if (!VirtualBox::stop(vm.name, force)) {
        if (!quiet)
            QMessageBox::warning(this, "VirtualBox 停止失败",
                QString("「%1」无法发送停止命令，可能已退出").arg(vm.name));
        return false;
    }
    // VirtualBox 无 QProcess 跟踪: 立即刷新状态，由轮询确认真实运行态
    m_status->setText(QString("⏹️ %1 已发送停止命令 (%2)")
        .arg(vm.name, force ? "强制" : "正常关机"));
    refreshStatus();
    return true;
}

bool VMPage::isVMRunning(const QString &name) const
{
    for (int i = 0; i < m_mgr.vms().size(); ++i) {
        if (m_mgr.vms()[i].name != name)
            continue;
        auto *item = m_table->item(i, ColStatus);
        return item && item->text().contains("运行中");
    }
    return false;
}

void VMPage::sendQemuPowerdown(const QString &vmName, const QString &sockPath)
{
    auto *socket = new QLocalSocket(this);
    socket->connectToServer(sockPath, QIODevice::ReadWrite);

    if (!socket->waitForConnected(2000)) {
        Logger::log("VM", QString("QMP socket 连接失败: %1").arg(sockPath));
        socket->deleteLater();
        return;
    }

    // 1. 读取 QEMU 发送的 QMP 问候
    if (!socket->waitForReadyRead(2000)) {
        Logger::log("VM", "QMP 读取问候超时");
        socket->disconnectFromServer();
        socket->deleteLater();
        return;
    }
    QByteArray greeting = socket->readAll();
    Logger::log("VM", QString("QMP 问候: %1").arg(QString::fromUtf8(greeting.left(200))));

    // 2. 发送 qmp_capabilities 握手
    QByteArray capCmd = "{\"execute\":\"qmp_capabilities\"}\n";
    socket->write(capCmd);
    if (!socket->waitForBytesWritten(1000)) {
        Logger::log("VM", "QMP 写入 capabilities 失败");
        socket->disconnectFromServer();
        socket->deleteLater();
        return;
    }
    if (!socket->waitForReadyRead(2000)) {
        Logger::log("VM", "QMP 读取 capabilities 响应超时");
        socket->disconnectFromServer();
        socket->deleteLater();
        return;
    }
    QByteArray capResp = socket->readAll();
    Logger::log("VM", QString("QMP capabilities 响应: %1").arg(QString::fromUtf8(capResp.left(200))));

    // 3. 发送 system_powerdown
    QByteArray pdCmd = "{\"execute\":\"system_powerdown\",\"id\":\"pd1\"}\n";
    socket->write(pdCmd);
    if (!socket->waitForBytesWritten(1000)) {
        Logger::log("VM", "QMP 写入 system_powerdown 失败");
    } else {
        if (socket->waitForReadyRead(2000)) {
            QByteArray pdResp = socket->readAll();
            Logger::log("VM", QString("QMP system_powerdown 响应: %1").arg(QString::fromUtf8(pdResp.left(200))));
        }
    }

    socket->disconnectFromServer();
    socket->deleteLater();
}

// ── 自动启动 ──

void VMPage::startAutoStartVMs()
{
    const auto &vms = m_mgr.vms();
    for (int i = 0; i < vms.size(); ++i) {
        if (!vms[i].autoStart) continue;
        auto *statusItem = m_table->item(i, ColStatus);
        if (statusItem && statusItem->text().contains("运行中")) continue;
        m_table->setCurrentCell(i, 0);
        startVM();
    }
}

// ── 远程连接 (QEMU: VNC / VirtualBox: RDP) ──

void VMPage::connectVNC()
{
    int row = m_table->currentRow();
    if (row < 0 || row >= m_mgr.vms().size()) return;

    const auto &vm = m_mgr.vms()[row];

    auto *statusItem = m_table->item(row, ColStatus);
    if (!statusItem || !statusItem->text().contains("运行中")) {
        QMessageBox::information(this, "提示", "该虚机未运行，请先启动");
        return;
    }

    if (vm.vmType == "virtualbox") {
        // VirtualBox 走内置 RDP 服务器 (VRDE), 只有 VM 运行时才监听
        if (!vm.vboxRdp) {
            QMessageBox::information(this, "提示",
                "该虚机未启用 RDP (VRDE)，请在配置里勾选「启用 RDP 远程控制」后重启虚机");
            return;
        }
        if (m_rdpClient.isEmpty()) {
            QMessageBox::warning(this, "提示",
                "未找到 RDP 客户端。\n请安装其一: xfreerdp / rdesktop / remmina / krdc");
            return;
        }
        QString target = QString("localhost:%1").arg(vm.vboxRdpPort);
        QStringList args;
        if (m_rdpClient.contains("xfreerdp"))
            args << QString("/v:%1").arg(target) << "/dynamic-resolution";
        else if (m_rdpClient.contains("rdesktop"))
            args << "-g" << "1280x800" << target;
        else if (m_rdpClient.contains("remmina"))
            args << "-c" << QString("rdp://%1").arg(target);
        else if (m_rdpClient.contains("krdc"))
            args << QString("rdp://%1").arg(target);
        else
            args << target;

        Logger::log("VM", QString("RDP 连接 %1 → %2").arg(vm.name, target));
        if (!QProcess::startDetached(m_rdpClient, args)) {
            QMessageBox::warning(this, "启动失败",
                QString("无法启动 RDP 客户端:\n%1").arg(m_rdpClient));
        }
        return;
    }

    if (vm.vnc < 0) {
        QMessageBox::information(this, "提示", "该虚机未配置 VNC");
        return;
    }

    // VNC 端口 = 5900 + 显示编号
    int vncPort = 5900 + vm.vnc;
    QString target = QString("localhost:%1").arg(vncPort);
    QString viewer = m_vncViewer;

    QStringList args;
    if (viewer.contains("remmina"))
        args << "-c" << QString("vnc://%1").arg(target);
    else
        args << target;

    if (!QProcess::startDetached(viewer, args)) {
        QMessageBox::warning(this, "启动失败",
            QString("无法启动 VNC 客户端:\n%1").arg(viewer));
    }
}

bool VMPage::checkPermissionError(const QString &stderr)
{
    QString lowerErr = stderr.toLower();
    if (lowerErr.contains("permission denied") ||
        lowerErr.contains("access denied") ||
        lowerErr.contains("无法打开磁盘映像") ||
        lowerErr.contains("cannot open disk image") ||
        lowerErr.contains("cannot open image") ||
        lowerErr.contains("operation not permitted") ||
        lowerErr.contains("no such file or directory")) {
        return true;
    }
    return false;
}

void VMPage::configureQemuSudoers(const QString &vmName)
{
    QMessageBox msgBox(this);
    msgBox.setWindowTitle("QEMU 权限配置");
    msgBox.setIcon(QMessageBox::Information);
    msgBox.setText("检测到 QEMU 可能因映像文件权限问题启动失败。\n\n"
                   "为了解决这个问题，需要为 QEMU 命令添加 sudo 免密权限。\n\n"
                   "系统将弹出系统认证窗口，请输入您的密码以配置 sudoers 文件。\n\n"
                   "配置后将允许以下命令免密执行：\n"
                   "- qemu-system-x86_64\n"
                   "- qemu-system-aarch64\n"
                   "- qemu-system-i386\n"
                   "- qemu-img");
    msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    msgBox.setDefaultButton(QMessageBox::Yes);

    if (msgBox.exec() != QMessageBox::Yes) {
        return;
    }

    QString sudoersContent = "# VM Manager QEMU sudoers configuration\n";
    sudoersContent += "# Generated by server-toolbox\n\n";

    QProcess currentUserProc;
    currentUserProc.start("whoami", {});
    if (currentUserProc.waitForFinished(2000)) {
        QString currentUser = QString::fromUtf8(currentUserProc.readAllStandardOutput()).trimmed();
        if (!currentUser.isEmpty()) {
            sudoersContent += QString("%1 ALL=(ALL) NOPASSWD: /usr/bin/qemu-system-x86_64*, /usr/bin/qemu-system-aarch64*, /usr/bin/qemu-system-i386*, /usr/bin/qemu-img*\n").arg(currentUser);
        }
    }

    sudoersContent += "%sudo ALL=(ALL) NOPASSWD: /usr/bin/qemu-system-x86_64*, /usr/bin/qemu-system-aarch64*, /usr/bin/qemu-system-i386*, /usr/bin/qemu-img*\n";
    sudoersContent += "%wheel ALL=(ALL) NOPASSWD: /usr/bin/qemu-system-x86_64*, /usr/bin/qemu-system-aarch64*, /usr/bin/qemu-system-i386*, /usr/bin/qemu-img*\n";
    // 服务模式: 启停与 daemon-reload 免密 (写单元文件仍走密码框 —— 那条需要 bash -c,
    // 给 bash 配免密等于给整个系统配免密 root, 不该做)
    sudoersContent += "%sudo ALL=(ALL) NOPASSWD: /usr/bin/systemctl start vm-*, /usr/bin/systemctl stop vm-*, /usr/bin/systemctl daemon-reload\n";
    sudoersContent += "%wheel ALL=(ALL) NOPASSWD: /usr/bin/systemctl start vm-*, /usr/bin/systemctl stop vm-*, /usr/bin/systemctl daemon-reload\n";

    QString sudoersFile = "/etc/sudoers.d/vm-manager-qemu";

    QProcess pkexecProc;
    QString script = QString(
        "echo '%1' > '%2' && "
        "chmod 0440 '%2' && "
        "visudo -cf '%2' > /dev/null 2>&1 || echo 'visudo check failed'"
    ).arg(sudoersContent.replace("'", "'\\''")).arg(sudoersFile);

    pkexecProc.start("pkexec", {"bash", "-c", script});
    if (pkexecProc.waitForFinished(10000)) {
        QString output = QString::fromUtf8(pkexecProc.readAllStandardError()).trimmed();
        if (pkexecProc.exitCode() == 0 && !output.contains("visudo check failed")) {
            QMessageBox::information(this, "配置成功",
                "QEMU sudoers 配置已成功完成。\n\n"
                "请重新启动虚拟机。");
            Logger::log("VM", QString("✅ QEMU sudoers 配置成功: %1").arg(sudoersFile));
        } else {
            QMessageBox::warning(this, "配置失败",
                "sudoers 配置失败或语法检查未通过。\n\n"
                "错误信息:\n" + (output.isEmpty() ? "未知错误" : output));
            Logger::log("VM", QString("❌ QEMU sudoers 配置失败: exit=%1, err=%2")
                .arg(pkexecProc.exitCode()).arg(output));
        }
    } else {
        QMessageBox::warning(this, "配置超时",
            "sudoers 配置超时或用户取消了认证。\n\n"
            "请手动配置 sudoers 文件：\n"
            "/etc/sudoers.d/vm-manager-qemu");
        Logger::log("VM", "⏰ QEMU sudoers 配置超时或取消");
    }
}

void VMPage::saveConfig()
{
    if (!m_mgr.save()) {
        QMessageBox::warning(this, "保存失败",
            "无法写入配置文件:\n" + m_mgr.configPath());
    }
}
