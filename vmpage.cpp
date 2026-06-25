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
    connect(m_startBtn, &QPushButton::clicked, this, &VMPage::startVM);
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
    // 优先级: TigerVNC > GTK VNC > 其他
    QStringList candidates = {"vncviewer", "gvncviewer", "vinagre", "krdc", "remmina"};
    for (const auto &candidate : candidates) {
        QString path = QStandardPaths::findExecutable(candidate);
        if (!path.isEmpty()) return path;
    }
    return {};
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
            vm.vnc < 0 ? "禁用" : QString::number(vm.vnc)));

        // 状态列初始文本，稍后由 refreshStatus() 更新
        auto *statusItem = new QTableWidgetItem("⋯ 检测中");
        statusItem->setForeground(QColor("#9ca3af"));
        m_table->setItem(i, ColStatus, statusItem);

        // 额外参数字段: 显示摘要 + 数据盘 + PCI 直通数量
        QStringList extras;
        if (!vm.extra.isEmpty()) {
            extras << (vm.extra.left(28).replace('\n', ' ')
                      + (vm.extra.length() > 28 ? "…" : ""));
        }
        if (!vm.dataDisks.isEmpty()) {
            extras << QString("💾%1").arg(vm.dataDisks.size());
        }
        if (!vm.pciDevices.isEmpty()) {
            extras << QString("🖧%1").arg(vm.pciDevices.size());
        }
        if (vm.ramfb) {
            extras << "🖵ramfb";
        }
        if (vm.hugepages) {
            extras << "🖧H";
        }
        if (vm.autoStart) {
            extras << "🔄auto";
        }
        m_table->setItem(i, ColExtra, new QTableWidgetItem(
            extras.isEmpty() ? "" : extras.join(" | ")));
    }

    m_status->setText(QString("共 %1 个虚机").arg(vms.size()));
}

void VMPage::refreshStatus()
{
    if (m_mgr.vms().isEmpty()) return;

    // 一次 ps 拿到所有 qemu 进程命令行
    QProcess ps;
    ps.start("ps", {"-ef"});
    ps.waitForFinished(3000);
    QStringList allLines = QString::fromUtf8(ps.readAllStandardOutput()).split('\n');

    // 过滤出 QEMU 进程行
    QStringList qemuLines;
    for (const auto &line : allLines) {
        if (line.contains("qemu-system-")) {
            qemuLines.append(line);
        }
    }

    // 逐行匹配虚机
    for (int i = 0; i < m_mgr.vms().size(); ++i) {
        const auto &vm = m_mgr.vms()[i];
        bool running = false;

        for (const auto &ql : qemuLines) {
            // 用磁盘路径匹配最可靠
            if (ql.contains(vm.disk)) {
                running = true;
                break;
            }
            // 退一步用名称匹配
            if (ql.contains("-name") && ql.contains(vm.name)) {
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
        }
    }
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

void VMPage::startVM()
{
    int row = m_table->currentRow();
    if (row < 0 || row >= m_mgr.vms().size()) return;

    // 已经在运行则跳过
    auto *statusItem = m_table->item(row, ColStatus);
    if (statusItem && statusItem->text().contains("运行中")) {
        QMessageBox::information(this, "提示",
            QString("「%1」已在运行中").arg(m_mgr.vms()[row].name));
        return;
    }

    const auto &vm = m_mgr.vms()[row];

    // 构造 QEMU 命令行
    QStringList args;
    args << "-name" << vm.name;
    args << "-smp" << QString::number(vm.cpu);
    args << "-m" << QString::number(vm.memory);

    // 机器类型 + KVM
    QString machineStr = vm.machine;
    if (vm.kvm)
        machineStr += ",accel=kvm";
    args << "-machine" << machineStr;

    // CPU 类型
    args << "-cpu" << vm.cpuType;

    // 系统盘
    if (!vm.disk.isEmpty()) {
        args << "-drive" << QString("file=%1,format=qcow2,if=virtio,index=0,cache=none,aio=native").arg(vm.disk);
    }

    // 数据盘（从 index=1 开始）
    for (int di = 0; di < vm.dataDisks.size(); ++di) {
        const auto &dd = vm.dataDisks[di];
        if (dd.path.isEmpty()) continue;
        args << "-drive" << QString("id=drive%1,file=%2,format=%3,if=virtio,index=%4,cache=%5,aio=%6")
            .arg(di + 1)
            .arg(dd.path, dd.format)
            .arg(di + 1)
            .arg(dd.cache, dd.aio);
    }

    if (!vm.iso.isEmpty()) {
        args << "-cdrom" << vm.iso;
    }

    // ── 网络: user 模式 + 端口转发 ──
    if (vm.net == "user") {
        QString netdev = "user,id=net0";
        for (const auto &pf : vm.portForwards) {
            netdev += QString(",hostfwd=%1::%2-:%3")
                .arg(pf.protocol)
                .arg(pf.hostPort)
                .arg(pf.guestPort);
        }
        args << "-netdev" << netdev;
        args << "-device" << QString("%1,netdev=net0").arg(vm.nicModel);
    } else if (vm.net == "bridge") {
        args << "-netdev" << "bridge,id=net0";
        args << "-device" << QString("%1,netdev=net0").arg(vm.nicModel);
    } else if (vm.net == "tap") {
        args << "-netdev" << "tap,id=net0";
        args << "-device" << QString("%1,netdev=net0").arg(vm.nicModel);
    }

    // VGA
    args << "-vga" << vm.vga;

    if (vm.vnc >= 0) {
        args << "-vnc" << QString(":%1").arg(vm.vnc);
    }

    // ── 硬件直通: PCI 直通 ──
    for (const auto &bdf : vm.pciDevices) {
        QString addr = bdf;
        // 补全短地址为 0000: 前缀
        if (!addr.contains(':')) continue;
        if (addr.count(':') == 1 && !addr.startsWith("0000:"))
            addr = "0000:" + addr;
        args << "-device" << QString("vfio-pci,host=%1").arg(addr);
    }

    // ── ramfb (aarch64 图形输出) ──
    if (vm.ramfb) {
        args << "-device" << "ramfb";
    }

    // ── 硬件直通: Hugepages ──
    if (vm.hugepages) {
        QString memSizeStr = QString::number(vm.memory);
        args << "-object"
             << QString("memory-backend-file,id=mem,size=%1M,mem-path=/dev/hugepages,share=on")
                    .arg(memSizeStr)
             << "-numa" << "node,memdev=mem";
    }

    // 额外参数
    if (!vm.extra.isEmpty()) {
        args << vm.extra.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    }

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
    } else {
        auto startedAt = QDateTime::currentSecsSinceEpoch();
        Logger::log("VM", QString("✅ %1 已启动 (PID: %2)").arg(vm.name).arg(proc->processId()));
        m_status->setText(QString("▶️ %1 已启动 (PID: %2)")
                          .arg(vm.name).arg(proc->processId()));

        // 进程退出时更新状态
        connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this, proc, name = vm.name, startedAt](int exitCode, QProcess::ExitStatus status) {
                    auto elapsed = QDateTime::currentSecsSinceEpoch() - startedAt;
                    QString stderr = QString::fromUtf8(proc->readAll()).trimmed();

                    if (status == QProcess::CrashExit) {
                        Logger::log("VM", QString("💥 %1 异常退出").arg(name));
                    } else {
                        Logger::log("VM", QString("⏹️ %1 正常退出 (exit: %2, 运行 %3s)")
                            .arg(name).arg(exitCode).arg(elapsed));
                    }

                    // 启动后 5 秒内退出且 exit != 0 → 多半是参数错误
                    if (elapsed < 5 && exitCode != 0) {
                        QString errMsg = stderr.isEmpty()
                            ? "虚机启动后立即退出"
                            : stderr.left(2000);
                        Logger::log("VM", QString("❌ %1 启动失败:\n%2").arg(name, errMsg));
                        QMessageBox::critical(this, "虚机启动失败",
                            QString("%1\n\n退出码: %2\n\nQEMU 输出:\n%3")
                                .arg(name).arg(exitCode).arg(errMsg));
                    }

                    m_status->setText(QString("⏹️ %1 已停止").arg(name));
                    refreshStatus();
                    proc->deleteLater();
                });
    }
}

void VMPage::startAutoStartVMs()
{
    const auto &vms = m_mgr.vms();
    for (int i = 0; i < vms.size(); ++i) {
        if (!vms[i].autoStart) continue;

        // 检查是否已在运行
        auto *statusItem = m_table->item(i, ColStatus);
        if (statusItem && statusItem->text().contains("运行中"))
            continue;

        // 临时选中该行并启动
        m_table->setCurrentCell(i, 0);
        startVM();
    }
}

void VMPage::connectVNC()
{
    int row = m_table->currentRow();
    if (row < 0 || row >= m_mgr.vms().size()) return;

    const auto &vm = m_mgr.vms()[row];
    if (vm.vnc < 0) {
        QMessageBox::information(this, "提示", "该虚机未配置 VNC");
        return;
    }

    // 检查是否在运行
    auto *statusItem = m_table->item(row, ColStatus);
    if (!statusItem || !statusItem->text().contains("运行中")) {
        QMessageBox::information(this, "提示", "该虚机未运行，请先启动");
        return;
    }

    QString target = QString("localhost:%1").arg(vm.vnc);
    QString viewer = m_vncViewer;

    // 根据不同的 VNC 客户端构造参数
    QStringList args;
    if (viewer.contains("vncviewer")) {
        args << target;   // TigerVNC: vncviewer localhost:5902
    } else if (viewer.contains("gvncviewer")) {
        args << target;   // gvncviewer localhost:5902
    } else if (viewer.contains("remmina")) {
        args << "-c" << QString("vnc://%1").arg(target);
    } else {
        args << target;   // 默认
    }

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
