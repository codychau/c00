#include "vmdialog.h"
#include "pcidialog.h"

#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFileDialog>
#include <QGroupBox>
#include <QLabel>
#include <QHeaderView>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QComboBox>
#include <QDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QProcess>
#include <QIntValidator>
#include <QStandardPaths>

// ═══════════════════════════════════════════════════════════════════
//  端口转发编辑对话框
// ═══════════════════════════════════════════════════════════════════
static bool editPortFwdDialog(QWidget *parent, PortFwd &pf, bool isNew)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(isNew ? "添加端口转发" : "编辑端口转发");
    dlg.resize(360, 160);

    auto *layout = new QVBoxLayout(&dlg);
    auto *form   = new QFormLayout();

    auto *hostSpin = new QSpinBox();
    hostSpin->setRange(1, 65535);
    hostSpin->setValue(pf.hostPort ? pf.hostPort : 8080);
    form->addRow("主机端口:", hostSpin);

    auto *guestSpin = new QSpinBox();
    guestSpin->setRange(1, 65535);
    guestSpin->setValue(pf.guestPort ? pf.guestPort : 80);
    form->addRow("虚机端口:", guestSpin);

    auto *protoCombo = new QComboBox();
    protoCombo->addItem("TCP",  "tcp");
    protoCombo->addItem("UDP",  "udp");
    int pIdx = protoCombo->findData(pf.protocol);
    if (pIdx >= 0) protoCombo->setCurrentIndex(pIdx);
    form->addRow("协议:", protoCombo);

    layout->addLayout(form);

    auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(btns);

    if (dlg.exec() != QDialog::Accepted)
        return false;

    pf.hostPort  = hostSpin->value();
    pf.guestPort = guestSpin->value();
    pf.protocol  = protoCombo->currentData().toString();
    return true;
}

// ═══════════════════════════════════════════════════════════════════
//  创建磁盘映像对话框（调用 qemu-img create）
// ═══════════════════════════════════════════════════════════════════
static bool createDiskImageDialog(QWidget *parent, QString &outPath)
{
    QDialog dlg(parent);
    dlg.setWindowTitle("创建磁盘映像");
    dlg.resize(500, 220);

    auto *layout = new QVBoxLayout(&dlg);
    auto *form   = new QFormLayout();

    // 保存路径
    auto *pathEdit = new QLineEdit();
    pathEdit->setPlaceholderText("/path/to/disk.qcow2");
    auto *pathBrowseBtn = new QPushButton("选择路径…");
    auto *pathRow = new QHBoxLayout();
    pathRow->addWidget(pathEdit, 1);
    pathRow->addWidget(pathBrowseBtn);
    QObject::connect(pathBrowseBtn, &QPushButton::clicked, [&]() {
        QString path = QFileDialog::getSaveFileName(&dlg, "创建磁盘映像",
            QDir::homePath(),
            "磁盘映像 (*.qcow2 *.raw *.img);;所有文件 (*)");
        if (!path.isEmpty()) pathEdit->setText(path);
    });
    form->addRow("保存路径:", pathRow);

    // 大小
    auto *sizeLayout = new QHBoxLayout();
    auto *sizeSpin = new QSpinBox();
    sizeSpin->setRange(1, 1048576);
    sizeSpin->setValue(20);
    sizeSpin->setMinimumWidth(120);
    sizeLayout->addWidget(sizeSpin);
    auto *unitCombo = new QComboBox();
    unitCombo->addItem("GB", "G");
    unitCombo->addItem("MB", "M");
    unitCombo->addItem("TB", "T");
    sizeLayout->addWidget(unitCombo);
    sizeLayout->addStretch();
    form->addRow("大小:", sizeLayout);

    // 格式
    auto *fmtCombo = new QComboBox();
    fmtCombo->addItem("qcow2");
    fmtCombo->addItem("raw");
    form->addRow("格式:", fmtCombo);

    layout->addLayout(form);

    auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto *okBtn = btns->button(QDialogButtonBox::Ok);
    okBtn->setText("创建");
    layout->addWidget(btns);

    // 阻止按钮框自动调用 accept()，由我们手动控制
    QObject::connect(btns, &QDialogButtonBox::accepted, &dlg, [](){});

    QObject::connect(okBtn, &QPushButton::clicked, &dlg, [&]() {
        QString path = pathEdit->text().trimmed();
        if (path.isEmpty()) {
            QMessageBox::warning(&dlg, "提示", "请选择保存路径");
            return;
        }

        QString fmt   = fmtCombo->currentText();
        int     size  = sizeSpin->value();
        QString unit  = unitCombo->currentText();
        QString sizeStr = QString("%1%2").arg(size).arg(unitCombo->currentData().toString());

        // 如果路径没有扩展名，自动补全
        if (!path.contains('.'))
            path += "." + fmt;

        // 检查 qemu-img 是否存在
        QString qemuImg = QStandardPaths::findExecutable("qemu-img");
        if (qemuImg.isEmpty()) {
            QMessageBox::critical(&dlg, "错误",
                "未找到 qemu-img，请先安装:\nsudo pacman -S qemu-img");
            return;
        }

        // 如果文件已存在，确认覆盖
        if (QFileInfo::exists(path)) {
            auto ret = QMessageBox::question(&dlg, "文件已存在",
                QString("%1 已存在，是否覆盖?").arg(path),
                QMessageBox::Yes | QMessageBox::No);
            if (ret != QMessageBox::Yes)
                return;
        }

        // 执行 qemu-img create
        QProcess proc;
        proc.start(qemuImg, {"create", "-f", fmt, path, sizeStr});
        proc.waitForFinished(30000);

        if (proc.exitCode() != 0) {
            QString err = QString::fromUtf8(proc.readAllStandardError());
            QMessageBox::critical(&dlg, "创建失败",
                "qemu-img 创建磁盘失败:\n" + err);
            return;
        }

        outPath = path;
        dlg.accept();
    });

    QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    return dlg.exec() == QDialog::Accepted;
}

// ═══════════════════════════════════════════════════════════════════
//  数据盘编辑对话框
// ═══════════════════════════════════════════════════════════════════
static bool editDataDiskDialog(QWidget *parent, VMDataDisk &dd, bool isNew)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(isNew ? "添加数据盘" : "编辑数据盘");
    dlg.resize(520, 200);

    auto *layout = new QVBoxLayout(&dlg);
    auto *form   = new QFormLayout();

    auto *fmtCombo = new QComboBox();
    fmtCombo->addItem("qcow2");
    fmtCombo->addItem("raw");
    fmtCombo->addItem("img");
    int fIdx = fmtCombo->findText(dd.format, Qt::MatchFixedString);
    if (fIdx >= 0) fmtCombo->setCurrentIndex(fIdx);

    auto *pathEdit = new QLineEdit(dd.path);
    pathEdit->setPlaceholderText("磁盘映像路径 (.qcow2 / .img / .raw)");
    auto *browseBtn = new QPushButton("浏览…");
    auto *createBtn = new QPushButton("创建 qcow2…");
    createBtn->setToolTip("调用 qemu-img 创建新的磁盘映像");
    auto *pathRow   = new QHBoxLayout();
    pathRow->addWidget(pathEdit, 1);
    pathRow->addWidget(browseBtn);
    pathRow->addWidget(createBtn);
    QObject::connect(browseBtn, &QPushButton::clicked, [&]() {
        QString path = QFileDialog::getOpenFileName(&dlg, "选择数据盘",
            pathEdit->text().isEmpty() ? QDir::homePath() : pathEdit->text(),
            "磁盘映像 (*.qcow2 *.raw *.img);;所有文件 (*)");
        if (!path.isEmpty()) pathEdit->setText(path);
    });
    QObject::connect(createBtn, &QPushButton::clicked, [&]() {
        QString newPath;
        if (createDiskImageDialog(&dlg, newPath)) {
            pathEdit->setText(newPath);
            // 自动匹配格式
            if (newPath.endsWith(".raw", Qt::CaseInsensitive) || newPath.endsWith(".img", Qt::CaseInsensitive)) {
                int idx = fmtCombo->findText("raw", Qt::MatchFixedString);
                if (idx >= 0) fmtCombo->setCurrentIndex(idx);
            }
        }
    });
    form->addRow("路径:", pathRow);

    form->addRow("格式:", fmtCombo);

    auto *cacheCombo = new QComboBox();
    cacheCombo->addItem("none",       "none");
    cacheCombo->addItem("writeback",  "writeback");
    cacheCombo->addItem("writethrough", "writethrough");
    cacheCombo->addItem("unsafe",     "unsafe");
    int cIdx = cacheCombo->findData(dd.cache);
    if (cIdx >= 0) cacheCombo->setCurrentIndex(cIdx);
    form->addRow("缓存:", cacheCombo);

    auto *aioCombo = new QComboBox();
    aioCombo->addItem("native",   "native");
    aioCombo->addItem("threads",  "threads");
    aioCombo->addItem("io_uring", "io_uring");
    int aIdx = aioCombo->findData(dd.aio);
    if (aIdx >= 0) aioCombo->setCurrentIndex(aIdx);
    form->addRow("AIO:", aioCombo);

    layout->addLayout(form);

    auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(btns);

    if (dlg.exec() != QDialog::Accepted)
        return false;

    dd.path   = pathEdit->text().trimmed();
    dd.format = fmtCombo->currentText();
    dd.cache  = cacheCombo->currentData().toString();
    dd.aio    = aioCombo->currentData().toString();
    return !dd.path.isEmpty();
}

// ═══════════════════════════════════════════════════════════════════
//  VMDialog — 双列布局
// ═══════════════════════════════════════════════════════════════════

VMDialog::VMDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("编辑虚机配置");
    resize(820, 680);

    auto *outer = new QVBoxLayout(this);

    // ── 双列主体 ──
    auto *cols = new QHBoxLayout();

    // ════════ 左列 ════════
    auto *leftCol = new QVBoxLayout();

    auto *basicGroup = new QGroupBox("基本参数");
    auto *basicForm  = new QFormLayout(basicGroup);

    m_nameEdit = new QLineEdit();
    m_nameEdit->setPlaceholderText("例如: Windows 10");
    basicForm->addRow("名称:", m_nameEdit);

    m_cpuSpin = new QSpinBox();
    m_cpuSpin->setRange(1, 64);
    m_cpuSpin->setValue(2);
    basicForm->addRow("CPU 核心:", m_cpuSpin);

    m_memSpin = new QSpinBox();
    m_memSpin->setRange(128, 1048576);
    m_memSpin->setValue(2048);
    m_memSpin->setSuffix(" MB");
    m_memSpin->setSingleStep(256);
    basicForm->addRow("内存:", m_memSpin);

    auto *diskRow = new QHBoxLayout();
    m_diskEdit = new QLineEdit();
    m_diskEdit->setPlaceholderText("系统盘路径");
    diskRow->addWidget(m_diskEdit, 1);
    m_diskBtn = new QPushButton("浏览…");
    diskRow->addWidget(m_diskBtn);
    basicForm->addRow("系统盘:", diskRow);

    auto *isoRow = new QHBoxLayout();
    m_isoEdit = new QLineEdit();
    m_isoEdit->setPlaceholderText("安装 ISO (可选)");
    isoRow->addWidget(m_isoEdit, 1);
    m_isoBtn = new QPushButton("浏览…");
    isoRow->addWidget(m_isoBtn);
    basicForm->addRow("ISO:", isoRow);

    m_virtioDiskCb = new QCheckBox("系统盘使用 VirtIO 接口（高性能，关闭后用 IDE/SATA 兼容旧 OS）");
    m_virtioDiskCb->setChecked(true);
    m_virtioDiskCb->setToolTip("开启: -drive if=virtio (更快)\n关闭: 默认接口 (IDE/SATA, 兼容 Win2000/98)");
    basicForm->addRow("", m_virtioDiskCb);

    leftCol->addWidget(basicGroup);

    // ── 数据盘 ──
    auto *ddGroup = new QGroupBox("数据盘");
    auto *ddLayout = new QVBoxLayout(ddGroup);
    auto *ddHint = new QLabel("系统盘之外的附加磁盘:");
    ddHint->setStyleSheet("color: #888; font-size: 11px;");
    ddLayout->addWidget(ddHint);

    m_dataDiskList = new QListWidget();
    m_dataDiskList->setAlternatingRowColors(true);
    m_dataDiskList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_dataDiskList->setMinimumHeight(90);
    ddLayout->addWidget(m_dataDiskList);

    auto *ddBtnRow = new QHBoxLayout();
    m_dataDiskAddBtn = new QPushButton("➕ 添加");
    m_dataDiskEditBtn = new QPushButton("✏️ 编辑");
    m_dataDiskRemoveBtn = new QPushButton("🗑️ 移除");
    ddBtnRow->addWidget(m_dataDiskAddBtn);
    ddBtnRow->addWidget(m_dataDiskEditBtn);
    ddBtnRow->addWidget(m_dataDiskRemoveBtn);
    ddBtnRow->addStretch();
    ddLayout->addLayout(ddBtnRow);
    leftCol->addWidget(ddGroup);

    // ── 硬件直通 ──
    auto *hptGroup = new QGroupBox("硬件直通");
    auto *hptLayout = new QVBoxLayout(hptGroup);

    m_pciList = new QListWidget();
    m_pciList->setAlternatingRowColors(true);
    m_pciList->setToolTip("PCI BDF 地址, 如 01:00.0");
    m_pciList->setMinimumHeight(60);
    hptLayout->addWidget(m_pciList);

    auto *pciBtnRow = new QHBoxLayout();
    m_pciAddBtn = new QPushButton("➕ 添加 PCI");
    m_pciRemoveBtn = new QPushButton("🗑️ 移除选中");
    pciBtnRow->addWidget(m_pciAddBtn);
    pciBtnRow->addWidget(m_pciRemoveBtn);
    pciBtnRow->addStretch();
    hptLayout->addLayout(pciBtnRow);

    m_hugepagesCb = new QCheckBox("Hugepages (巨页内存)");
    hptLayout->addWidget(m_hugepagesCb);
    leftCol->addWidget(hptGroup);

    // ── 额外参数 ──
    auto *extraGroup = new QGroupBox("额外 QEMU 参数");
    auto *extraLayout = new QVBoxLayout(extraGroup);
    m_extraEdit = new QPlainTextEdit();
    m_extraEdit->setPlaceholderText("以上控件未覆盖的参数写在这里");
    m_extraEdit->setMaximumHeight(70);
    extraLayout->addWidget(m_extraEdit);
    leftCol->addWidget(extraGroup);

    cols->addLayout(leftCol, 1);

    // ════════ 右列 ════════
    auto *rightCol = new QVBoxLayout();

    auto *advGroup = new QGroupBox("高级参数");
    auto *advForm  = new QFormLayout(advGroup);

    // 架构
    m_qemuCombo = new QComboBox();
    m_qemuCombo->addItem("x86_64",  "qemu-system-x86_64");
    m_qemuCombo->addItem("aarch64", "qemu-system-aarch64");
    m_qemuCombo->addItem("i386",    "qemu-system-i386");
    advForm->addRow("架构:", m_qemuCombo);

    // 机器类型
    m_machineCombo = new QComboBox();
    m_machineCombo->setEditable(true);
    m_machineCombo->addItem("q35");
    m_machineCombo->addItem("pc");
    m_machineCombo->addItem("pc-i440fx-8.2");
    m_machineCombo->addItem("virt");
    advForm->addRow("机器类型:", m_machineCombo);

    // CPU 类型
    m_cpuTypeCombo = new QComboBox();
    m_cpuTypeCombo->setEditable(true);
    m_cpuTypeCombo->addItem("host");
    m_cpuTypeCombo->addItem("qemu64");
    m_cpuTypeCombo->addItem("pentium3");
    m_cpuTypeCombo->addItem("cortex-a72");
    m_cpuTypeCombo->addItem("max");
    advForm->addRow("CPU 类型:", m_cpuTypeCombo);

    // KVM
    m_kvmCb = new QCheckBox("启用 KVM 加速");
    m_kvmCb->setChecked(true);
    advForm->addRow("", m_kvmCb);

    // ── 分隔线 ──
    auto *vgaNicForm = new QFormLayout();

    // VGA
    m_vgaCombo = new QComboBox();
    m_vgaCombo->addItem("virtio",   "virtio");
    m_vgaCombo->addItem("cirrus",   "cirrus");
    m_vgaCombo->addItem("qxl",      "qxl");
    m_vgaCombo->addItem("virtio-gpu", "virtio-gpu");
    m_vgaCombo->addItem("none",     "none");
    advForm->addRow("VGA 类型:", m_vgaCombo);

    // 网络
    m_netCombo = new QComboBox();
    m_netCombo->addItem("user (NAT)",   "user");
    m_netCombo->addItem("bridge (桥接)", "bridge");
    m_netCombo->addItem("tap 设备",      "tap");
    advForm->addRow("网络模式:", m_netCombo);

    // 网卡模型
    m_nicCombo = new QComboBox();
    m_nicCombo->setEditable(true);
    m_nicCombo->addItem("virtio-net-pci");
    m_nicCombo->addItem("e1000");
    m_nicCombo->addItem("rtl8139");
    m_nicCombo->addItem("ne2k_pci");
    m_nicCombo->addItem("virtio-net-ccw");
    advForm->addRow("网卡型号:", m_nicCombo);

    // VNC
    m_vncSpin = new QSpinBox();
    m_vncSpin->setRange(-1, 99);
    m_vncSpin->setValue(-1);
    m_vncSpin->setSpecialValueText("禁用");
    m_vncSpin->setToolTip("显示编号 (0~99)，VNC 端口 = 5900 + 编号");
    advForm->addRow("VNC 显示编号 (0~99):", m_vncSpin);

    // ramfb
    m_ramfbCb = new QCheckBox("ramfb 显示 (aarch64)");
    m_ramfbCb->setVisible(false);
    advForm->addRow("", m_ramfbCb);

    // ── 自动启动 ──
    auto *autoGroup = new QGroupBox("启动管理");
    auto *autoLayout = new QVBoxLayout(autoGroup);
    m_autoStartCb = new QCheckBox("随管理器自动启动");
    m_autoStartCb->setToolTip("应用启动时自动检查并启动此虚机");
    autoLayout->addWidget(m_autoStartCb);
    rightCol->addWidget(autoGroup);

    rightCol->addWidget(advGroup);

    // ── 端口转发 ──
    auto *portGroup = new QGroupBox("端口转发 (user 模式)");
    auto *portLayout = new QVBoxLayout(portGroup);

    m_portTable = new QTableWidget(0, 3);
    m_portTable->setHorizontalHeaderLabels({"主机端口", "虚机端口", "协议"});
    m_portTable->horizontalHeader()->setStretchLastSection(true);
    m_portTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_portTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_portTable->verticalHeader()->hide();
    m_portTable->setMinimumHeight(110);
    portLayout->addWidget(m_portTable);

    auto *portBtnRow = new QHBoxLayout();
    m_portAddBtn    = new QPushButton("➕ 添加");
    m_portEditBtn   = new QPushButton("✏️ 编辑");
    m_portRemoveBtn = new QPushButton("🗑️ 移除");
    portBtnRow->addWidget(m_portAddBtn);
    portBtnRow->addWidget(m_portEditBtn);
    portBtnRow->addWidget(m_portRemoveBtn);
    portBtnRow->addStretch();
    portLayout->addLayout(portBtnRow);
    rightCol->addWidget(portGroup);

    cols->addLayout(rightCol, 1);
    outer->addLayout(cols);

    // ── 确认/取消 ──
    auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(btns, &QDialogButtonBox::accepted, this, [this]() {
        if (m_nameEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, "提示", "请输入虚机名称");
            return;
        }
        accept();
    });
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(btns);

    // ── 信号连接 ──

    // 架构切换 → ramfb 显隐
    connect(m_qemuCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        QString qemu = m_qemuCombo->itemData(idx).toString();
        m_ramfbCb->setVisible(qemu.contains("aarch64"));
        if (qemu.contains("aarch64")) {
            // aarch64 推荐机器类型
            if (m_machineCombo->currentText() == "q35" || m_machineCombo->count() > 0)
                m_machineCombo->setCurrentText("virt");
        }
    });

    // 文件浏览
    connect(m_diskBtn, &QPushButton::clicked, this, [this]() {
        QString path = QFileDialog::getOpenFileName(this, "选择系统盘",
            m_diskEdit->text().isEmpty() ? QDir::homePath() : m_diskEdit->text(),
            "磁盘映像 (*.qcow2 *.raw *.img);;所有文件 (*)");
        if (!path.isEmpty()) m_diskEdit->setText(path);
    });
    connect(m_isoBtn, &QPushButton::clicked, this, [this]() {
        QString path = QFileDialog::getOpenFileName(this, "选择 ISO",
            m_isoEdit->text().isEmpty() ? QDir::homePath() : m_isoEdit->text(),
            "ISO 映像 (*.iso);;所有文件 (*)");
        if (!path.isEmpty()) m_isoEdit->setText(path);
    });

    // 端口转发操作
    connect(m_portAddBtn, &QPushButton::clicked, this, [this]() {
        PortFwd pf;
        if (editPortFwdDialog(this, pf, true)) {
            int row = m_portTable->rowCount();
            m_portTable->insertRow(row);
            m_portTable->setItem(row, 0, new QTableWidgetItem(QString::number(pf.hostPort)));
            m_portTable->setItem(row, 1, new QTableWidgetItem(QString::number(pf.guestPort)));
            m_portTable->setItem(row, 2, new QTableWidgetItem(pf.protocol.toUpper()));
        }
    });
    connect(m_portEditBtn, &QPushButton::clicked, this, [this]() {
        int row = m_portTable->currentRow();
        if (row < 0) return;
        PortFwd pf;
        pf.hostPort  = m_portTable->item(row, 0)->text().toInt();
        pf.guestPort = m_portTable->item(row, 1)->text().toInt();
        pf.protocol  = m_portTable->item(row, 2)->text().toLower();
        if (editPortFwdDialog(this, pf, false)) {
            m_portTable->item(row, 0)->setText(QString::number(pf.hostPort));
            m_portTable->item(row, 1)->setText(QString::number(pf.guestPort));
            m_portTable->item(row, 2)->setText(pf.protocol.toUpper());
        }
    });
    connect(m_portRemoveBtn, &QPushButton::clicked, this, [this]() {
        int row = m_portTable->currentRow();
        if (row >= 0) m_portTable->removeRow(row);
    });

    // 数据盘操作
    connect(m_dataDiskAddBtn, &QPushButton::clicked, this, [this]() {
        VMDataDisk dd;
        if (editDataDiskDialog(this, dd, true)) {
            auto *item = new QListWidgetItem(
                QString("%1  [%2 | cache:%3 | aio:%4]")
                    .arg(QFileInfo(dd.path).fileName(), dd.format, dd.cache, dd.aio));
            item->setData(Qt::UserRole,
                QString("%1|%2|%3|%4").arg(dd.path, dd.format, dd.cache, dd.aio));
            item->setToolTip(QString("路径:%1\n格式:%2\n缓存:%3\nAIO:%4")
                             .arg(dd.path, dd.format, dd.cache, dd.aio));
            m_dataDiskList->addItem(item);
        }
    });
    connect(m_dataDiskEditBtn, &QPushButton::clicked, this, [this]() {
        int row = m_dataDiskList->currentRow();
        if (row < 0) return;
        QStringList parts = m_dataDiskList->item(row)->data(Qt::UserRole).toString().split('|');
        VMDataDisk dd;
        if (parts.size() >= 1) dd.path   = parts[0];
        if (parts.size() >= 2) dd.format = parts[1];
        if (parts.size() >= 3) dd.cache  = parts[2];
        if (parts.size() >= 4) dd.aio    = parts[3];
        if (editDataDiskDialog(this, dd, false)) {
            delete m_dataDiskList->takeItem(row);
            auto *item = new QListWidgetItem(
                QString("%1  [%2 | cache:%3 | aio:%4]")
                    .arg(QFileInfo(dd.path).fileName(), dd.format, dd.cache, dd.aio));
            item->setData(Qt::UserRole,
                QString("%1|%2|%3|%4").arg(dd.path, dd.format, dd.cache, dd.aio));
            item->setToolTip(QString("路径:%1\n格式:%2\n缓存:%3\nAIO:%4")
                             .arg(dd.path, dd.format, dd.cache, dd.aio));
            m_dataDiskList->addItem(item);
        }
    });
    connect(m_dataDiskRemoveBtn, &QPushButton::clicked, this, [this]() {
        int row = m_dataDiskList->currentRow();
        if (row >= 0) delete m_dataDiskList->takeItem(row);
    });

    // ── 硬件直通: PCI ──
    connect(m_pciAddBtn, &QPushButton::clicked, this, [this]() {
        PCIDialog dlg(this);
        QStringList existing;
        for (int i = 0; i < m_pciList->count(); ++i)
            existing << m_pciList->item(i)->text();
        for (const auto &bdf : existing)
            dlg.selectBDF(bdf);
        if (dlg.exec() == QDialog::Accepted) {
            m_pciList->clear();
            for (const auto &bdf : dlg.selectedBDFs())
                m_pciList->addItem(bdf);
        }
    });
    connect(m_pciRemoveBtn, &QPushButton::clicked, this, [this]() {
        int row = m_pciList->currentRow();
        if (row >= 0) delete m_pciList->takeItem(row);
    });
}

// ═══════════════════════════════════════════════════════════════════
//  setVMConfig — 从数据结构填充界面
// ═══════════════════════════════════════════════════════════════════
void VMDialog::setVMConfig(const VMConfig &vm)
{
    // 左列
    m_nameEdit->setText(vm.name);
    m_cpuSpin->setValue(vm.cpu);
    m_memSpin->setValue(vm.memory);
    m_diskEdit->setText(vm.disk);
    m_isoEdit->setText(vm.iso);

    // 数据盘
    m_dataDiskList->clear();
    for (const auto &dd : vm.dataDisks) {
        auto *item = new QListWidgetItem(
            QString("%1  [%2 | cache:%3 | aio:%4]")
                .arg(QFileInfo(dd.path).fileName(), dd.format, dd.cache, dd.aio));
        item->setData(Qt::UserRole,
            QString("%1|%2|%3|%4").arg(dd.path, dd.format, dd.cache, dd.aio));
        item->setToolTip(QString("路径:%1\n格式:%2\n缓存:%3\nAIO:%4")
                         .arg(dd.path, dd.format, dd.cache, dd.aio));
        m_dataDiskList->addItem(item);
    }

    // 自动启动
    m_virtioDiskCb->setChecked(vm.virtioDisk);
    m_autoStartCb->setChecked(vm.autoStart);

    // 硬件直通
    m_pciList->clear();
    for (const auto &bdf : vm.pciDevices)
        m_pciList->addItem(bdf);
    m_hugepagesCb->setChecked(vm.hugepages);

    m_extraEdit->setPlainText(vm.extra);

    // 右列
    int qIdx = m_qemuCombo->findData(vm.qemuBinary);
    if (qIdx >= 0) m_qemuCombo->setCurrentIndex(qIdx);

    m_machineCombo->setCurrentText(vm.machine);
    m_cpuTypeCombo->setCurrentText(vm.cpuType);
    m_kvmCb->setChecked(vm.kvm);

    int vIdx = m_vgaCombo->findData(vm.vga);
    if (vIdx >= 0) m_vgaCombo->setCurrentIndex(vIdx);

    int nIdx = m_netCombo->findData(vm.net);
    if (nIdx >= 0) m_netCombo->setCurrentIndex(nIdx);

    m_nicCombo->setCurrentText(vm.nicModel);
    m_vncSpin->setValue(vm.vnc);
    m_ramfbCb->setChecked(vm.ramfb);
    m_ramfbCb->setVisible(vm.qemuBinary.contains("aarch64"));

    // 端口转发
    m_portTable->setRowCount(0);
    for (const auto &pf : vm.portForwards) {
        int row = m_portTable->rowCount();
        m_portTable->insertRow(row);
        m_portTable->setItem(row, 0, new QTableWidgetItem(QString::number(pf.hostPort)));
        m_portTable->setItem(row, 1, new QTableWidgetItem(QString::number(pf.guestPort)));
        m_portTable->setItem(row, 2, new QTableWidgetItem(pf.protocol.toUpper()));
    }
}

// ═══════════════════════════════════════════════════════════════════
//  vmConfig — 从界面收集数据
// ═══════════════════════════════════════════════════════════════════
VMConfig VMDialog::vmConfig() const
{
    VMConfig vm;
    vm.name   = m_nameEdit->text().trimmed();
    vm.cpu    = m_cpuSpin->value();
    vm.memory = m_memSpin->value();
    vm.disk   = m_diskEdit->text().trimmed();
    vm.iso    = m_isoEdit->text().trimmed();
    vm.vnc    = m_vncSpin->value();

    vm.qemuBinary = m_qemuCombo->currentData().toString();
    vm.machine    = m_machineCombo->currentText();
    vm.cpuType    = m_cpuTypeCombo->currentText();
    vm.kvm        = m_kvmCb->isChecked();
    vm.vga        = m_vgaCombo->currentData().toString();
    vm.net        = m_netCombo->currentData().toString();
    vm.nicModel   = m_nicCombo->currentText();
    vm.ramfb      = m_ramfbCb->isChecked();
    vm.virtioDisk = m_virtioDiskCb->isChecked();
    vm.autoStart  = m_autoStartCb->isChecked();

    // 端口转发
    vm.portForwards.clear();
    for (int i = 0; i < m_portTable->rowCount(); ++i) {
        PortFwd pf;
        pf.hostPort  = m_portTable->item(i, 0)->text().toInt();
        pf.guestPort = m_portTable->item(i, 1)->text().toInt();
        pf.protocol  = m_portTable->item(i, 2)->text().toLower();
        vm.portForwards.append(pf);
    }

    // 数据盘
    vm.dataDisks.clear();
    for (int i = 0; i < m_dataDiskList->count(); ++i) {
        QStringList parts = m_dataDiskList->item(i)->data(Qt::UserRole).toString().split('|');
        if (parts.size() >= 4) {
            VMDataDisk dd;
            dd.path   = parts[0];
            dd.format = parts[1];
            dd.cache  = parts[2];
            dd.aio    = parts[3];
            vm.dataDisks.append(dd);
        }
    }

    // 硬件直通
    vm.hugepages = m_hugepagesCb->isChecked();
    for (int i = 0; i < m_pciList->count(); ++i)
        vm.pciDevices.append(m_pciList->item(i)->text());

    vm.extra = m_extraEdit->toPlainText().trimmed();

    return vm;
}
