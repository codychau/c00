#include "vmpage.h"
#include "vmdialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QProcess>
#include <QInputDialog>
#include <QDir>
#include <QRegularExpression>
#include <QFileInfo>
#include <QColor>
#include <QBrush>
#include <QStandardPaths>
#include <QDateTime>
#include <QLocalSocket>
#include <QTemporaryDir>

#include "logger.h"

// 列索引
enum Col { ColName = 0, ColCPU, ColMem, ColDisk, ColNet, ColVNC, ColStatus, ColExtra, ColCount };

VMPage::VMPage(QWidget *parent)
    : QWidget(parent), m_mgr(VMConfigManager::defaultConfigPath())
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);

    auto *title = new QLabel("🖥️  虚机管理");
    QFont f = title->font(); f.setPointSize(14); f.setBold(true);
    title->setFont(f);
    layout->addWidget(title);

    // ── 表格（8 列，含状态）──
    m_table = new QTableWidget(0, ColCount);
    m_table->setHorizontalHeaderLabels({
        "名称", "CPU", "内存", "磁盘", "网络", "VNC", "状态", "额外参数"
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
    m_status->setStyleSheet("color: #888;");
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

    m_vncBtn = new QPushButton("🖥️ VNC 连接");
    connect(m_vncBtn, &QPushButton::clicked, this, &VMPage::connectVNC);
    btnRow->addWidget(m_vncBtn);

    layout->addLayout(btnRow);

    // ── 检测 VNC 客户端 ──
    m_vncViewer = findVNCViewer();
    if (m_vncViewer.isEmpty()) {
        m_vncBtn->setToolTip("未找到 VNC 客户端（已安装 tigervnc 或 gtk-vnc？）");
        m_vncBtn->setEnabled(false);
    } else {
        m_vncBtn->setToolTip(QString("使用 %1 连接").arg(m_vncViewer));
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
    QStringList candidates = {"vncviewer", "gvncviewer", "vinagre", "krdc", "remmina"};
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
            vm.disk.isEmpty() ? "" : QFileInfo(vm.disk).fileName()));

        m_table->setItem(i, ColNet, new QTableWidgetItem(vm.net));

        m_table->setItem(i, ColVNC, new QTableWidgetItem(
            vm.vnc < 0 ? "禁用" : QString::number(5900 + vm.vnc)));

        auto *statusItem = new QTableWidgetItem("⋯ 检测中");
        statusItem->setForeground(QColor("#9ca3af"));
        m_table->setItem(i, ColStatus, statusItem);

        QStringList extras;
        if (!vm.extra.isEmpty()) {
            extras << (vm.extra.left(28).replace('\n', ' ')
                      + (vm.extra.length() > 28 ? "…" : ""));
        }
        if (!vm.dataDisks.isEmpty())
            extras << QString("💾%1").arg(vm.dataDisks.size());
        if (!vm.pciDevices.isEmpty())
            extras << QString("🖧%1").arg(vm.pciDevices.size());
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

    for (int i = 0; i < m_mgr.vms().size(); ++i) {
        const auto &vm = m_mgr.vms()[i];
        bool running = false;
        for (const auto &ql : qemuLines) {
            if (ql.contains(vm.disk) || (ql.contains("-name") && ql.contains(vm.name))) {
                running = true;
                break;
            }
        }

        auto *item = m_table->item(i, ColStatus);
        if (!item) continue;

        if (running) {
            item->setText("✅ 运行中");
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

void VMPage::startVM()
{
    int row = m_table->currentRow();
    if (row < 0 || row >= m_mgr.vms().size()) return;

    auto *statusItem = m_table->item(row, ColStatus);
    if (statusItem && statusItem->text().contains("运行中")) {
        QMessageBox::information(this, "提示",
            QString("「%1」已在运行中").arg(m_mgr.vms()[row].name));
        return;
    }

    const auto &vm = m_mgr.vms()[row];

    QStringList args;
    args << "-name" << vm.name;
    args << "-smp" << QString::number(vm.cpu);
    args << "-m" << QString::number(vm.memory);

    QString machineStr = vm.machine;
    if (vm.kvm) machineStr += ",accel=kvm";
    args << "-machine" << machineStr;
    args << "-cpu" << vm.cpuType;

    // QMP 管理接口（用于关机/电源管理）
    QString sockPath = qmpSocketPath(vm.name);
    args << "-qmp" << QString("unix:%1,server=on,wait=off").arg(sockPath);

    // 系统盘 — 不加 if=virtio 以兼容旧 OS（Win2000/98 无 virtio 驱动）
    if (!vm.disk.isEmpty())
        args << "-drive" << QString("file=%1,format=qcow2").arg(vm.disk);

    for (int di = 0; di < vm.dataDisks.size(); ++di) {
        const auto &dd = vm.dataDisks[di];
        if (dd.path.isEmpty()) continue;
        args << "-drive" << QString("id=drive%1,file=%2,format=%3,if=virtio,index=%4,cache=%5,aio=%6")
            .arg(di + 1).arg(dd.path, dd.format).arg(di + 1).arg(dd.cache, dd.aio);
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

    if (vm.vnc >= 0) {
        // vnc 存的是显示编号 (0~99)，直接传给 QEMU
        args << "-vnc" << QString(":%1").arg(vm.vnc);
    }

    for (const auto &bdf : vm.pciDevices) {
        QString addr = bdf;
        if (!addr.contains(':')) continue;
        if (addr.count(':') == 1 && !addr.startsWith("0000:"))
            addr = "0000:" + addr;
        args << "-device" << QString("vfio-pci,host=%1").arg(addr);
    }

    if (vm.ramfb)
        args << "-device" << "ramfb";

    if (vm.hugepages) {
        args << "-object"
             << QString("memory-backend-file,id=mem,size=%1M,mem-path=/dev/hugepages,share=on")
                    .arg(vm.memory)
             << "-numa" << "node,memdev=mem";
    }

    if (!vm.extra.isEmpty())
        args << vm.extra.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);

    QString cmdLine = vm.qemuBinary + " " + args.join(' ');
    Logger::log("VM", QString("启动虚机: %1").arg(vm.name));
    Logger::log("VM", cmdLine.left(500));

    auto *proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->start(vm.qemuBinary, args);

    if (!proc->waitForStarted(3000)) {
        QString err = proc->errorString();
        Logger::log("VM", QString("❌ 启动失败: %1 — %2").arg(vm.name, err));
        QMessageBox::warning(this, "启动失败",
            QString("无法启动 QEMU:\n%1").arg(err));
        delete proc;
        return;
    }

    auto startedAt = QDateTime::currentSecsSinceEpoch();
    Logger::log("VM", QString("✅ %1 已启动 (PID: %2)").arg(vm.name).arg(proc->processId()));
    m_status->setText(QString("▶️ %1 已启动 (PID: %2)").arg(vm.name).arg(proc->processId()));

    m_runningProcs[vm.name] = proc;
    updateStartButton();

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, proc, name = vm.name, startedAt](int exitCode, QProcess::ExitStatus status) {
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
            QMessageBox::critical(this, "虚机启动失败",
                QString("%1\n\n退出码: %2\n\nQEMU 输出:\n%3").arg(name).arg(exitCode).arg(errMsg));
        }

        m_runningProcs.remove(name);
        proc->deleteLater();
        m_status->setText(QString("⏹️ %1 已停止").arg(name));
        refreshStatus();
    });
}

// ── 停止 ──

void VMPage::stopVM()
{
    int row = m_table->currentRow();
    if (row < 0 || row >= m_mgr.vms().size()) return;

    const auto &vm = m_mgr.vms()[row];
    auto *statusItem = m_table->item(row, ColStatus);
    if (!statusItem || !statusItem->text().contains("运行中")) return;

    // 弹出选择
    auto *menu = new QMessageBox(this);
    menu->setWindowTitle("停止虚机");
    menu->setText(QString("选择停止方式: %1").arg(vm.name));
    menu->setIcon(QMessageBox::Question);

    auto *btnShutdown = menu->addButton("🛑 正常关机", QMessageBox::AcceptRole);
    auto *btnKill     = menu->addButton("⚡ 极速停止", QMessageBox::DestructiveRole);
    auto *btnCancel   = menu->addButton("取消", QMessageBox::RejectRole);
    menu->setDefaultButton(btnShutdown);
    menu->exec();

    if (menu->clickedButton() == btnCancel)
        return;

    if (menu->clickedButton() == btnShutdown) {
        Logger::log("VM", QString("正常关机: %1").arg(vm.name));
        sendQemuPowerdown(vm.name);

        // 如果进程还在跟踪，等待最多 15 秒后强行终止
        if (m_runningProcs.contains(vm.name)) {
            QProcess *p = m_runningProcs[vm.name];
            if (!p->waitForFinished(15000)) {
                Logger::log("VM", QString("超时，强行终止: %1").arg(vm.name));
                p->kill();
                p->waitForFinished(2000);
            }
        }
    } else {
        Logger::log("VM", QString("极速停止: %1").arg(vm.name));
        // 直接杀 QEMU 进程
        if (m_runningProcs.contains(vm.name)) {
            m_runningProcs[vm.name]->kill();
            m_runningProcs[vm.name]->waitForFinished(2000);
        } else {
            // 不在跟踪表中，用 pidof/pkill
            QProcess::execute("pkill", {"-9", "-f",
                QString("qemu.*-name.*%1").arg(QRegularExpression::escape(vm.name))});
        }
    }

    refreshStatus();
}

void VMPage::sendQemuPowerdown(const QString &vmName)
{
    QString sockPath = qmpSocketPath(vmName);

    auto *socket = new QLocalSocket(this);
    socket->connectToServer(sockPath, QIODevice::ReadWrite);

    if (socket->waitForConnected(2000)) {
        // QMP 协议: 先发送 capabilities 握手，再发送 system_powerdown
        QByteArray cmd = R"({"execute":"qmp_capabilities"}
{"execute":"system_powerdown"}
)";
        socket->write(cmd);
        socket->waitForBytesWritten(1000);
        socket->waitForReadyRead(1000);  // 读取响应（不关心内容）
        socket->disconnectFromServer();
    } else {
        Logger::log("VM", QString("QMP socket 连接失败: %1").arg(sockPath));
    }
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

// ── VNC 连接 ──

void VMPage::connectVNC()
{
    int row = m_table->currentRow();
    if (row < 0 || row >= m_mgr.vms().size()) return;

    const auto &vm = m_mgr.vms()[row];
    if (vm.vnc < 0) {
        QMessageBox::information(this, "提示", "该虚机未配置 VNC");
        return;
    }

    auto *statusItem = m_table->item(row, ColStatus);
    if (!statusItem || !statusItem->text().contains("运行中")) {
        QMessageBox::information(this, "提示", "该虚机未运行，请先启动");
        return;
    }

    // VNC 端口 = 5900 + 显示编号
    int vncPort = 5900 + vm.vnc;
    QString target = QString("localhost:%1").arg(vncPort);
    QString viewer = m_vncViewer;

    QStringList args;
    if (viewer.contains("vncviewer"))
        args << target;
    else if (viewer.contains("gvncviewer"))
        args << target;
    else if (viewer.contains("remmina"))
        args << "-c" << QString("vnc://%1").arg(target);
    else
        args << target;

    if (!QProcess::startDetached(viewer, args)) {
        QMessageBox::warning(this, "启动失败",
            QString("无法启动 VNC 客户端:\n%1").arg(viewer));
    }
}

void VMPage::saveConfig()
{
    if (!m_mgr.save()) {
        QMessageBox::warning(this, "保存失败",
            "无法写入配置文件:\n" + m_mgr.configPath());
    }
}
