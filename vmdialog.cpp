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
#include <QDir>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSignalBlocker>

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
//  创建磁盘映像对话框（QEMU 走 qemu-img, VirtualBox 走 VBoxManage createmedium）
// ═══════════════════════════════════════════════════════════════════
static bool createDiskImageDialog(QWidget *parent, const QString &vmType, QString &outPath)
{
    const bool vbox = (vmType == "virtualbox");

    QDialog dlg(parent);
    dlg.setWindowTitle(vbox ? "创建磁盘映像 (VirtualBox)" : "创建磁盘映像");
    dlg.resize(500, 220);

    auto *layout = new QVBoxLayout(&dlg);
    auto *form   = new QFormLayout();

    // 保存路径
    auto *pathEdit = new QLineEdit();
    pathEdit->setPlaceholderText(vbox ? "/path/to/disk.vdi" : "/path/to/disk.qcow2");
    auto *pathBrowseBtn = new QPushButton("选择路径…");
    auto *pathRow = new QHBoxLayout();
    pathRow->addWidget(pathEdit, 1);
    pathRow->addWidget(pathBrowseBtn);
    QObject::connect(pathBrowseBtn, &QPushButton::clicked, [&]() {
        QString path = QFileDialog::getSaveFileName(&dlg, "创建磁盘映像",
            QDir::homePath(),
            "磁盘映像 (*.qcow2 *.raw *.img *.vdi *.vmdk *.vhdx);;所有文件 (*)");
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
    if (vbox) {
        fmtCombo->addItem("VDI (VirtualBox 原生)", "vdi");
        fmtCombo->addItem("VMDK (兼容 VMware)",    "vmdk");
        fmtCombo->addItem("VHDX (Hyper-V)",        "vhdx");
    } else {
        fmtCombo->addItem("qcow2", "qcow2");
        fmtCombo->addItem("raw",   "raw");
    }
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

        QString fmt   = fmtCombo->currentData().toString();
        int     size  = sizeSpin->value();
        int     sizeMB = size * (unitCombo->currentData().toString() == "T" ? 1048576
                              : unitCombo->currentData().toString() == "G" ? 1024 : 1);

        // 如果路径没有扩展名，自动补全
        if (!path.contains('.'))
            path += "." + fmt;

        // 检查后端工具是否存在
        const QString tool = vbox ? "VBoxManage" : "qemu-img";
        QString toolPath = QStandardPaths::findExecutable(tool);
        if (toolPath.isEmpty()) {
            QMessageBox::critical(&dlg, "错误",
                QString("未找到 %1，请先安装:\nsudo pacman -S %2")
                    .arg(tool).arg(vbox ? "virtualbox" : "qemu-img"));
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

        // 执行创建
        QProcess proc;
        if (vbox)
            proc.start(toolPath, {"createmedium", "disk", "--filename", path,
                                   "--size", QString::number(sizeMB),
                                   "--format", fmt.toUpper()});
        else
            proc.start(toolPath, {"create", "-f", fmt, path, QString("%1%2").arg(size).arg(unitCombo->currentData().toString())});

        bool finished = proc.waitForFinished(120000);  // 最多等 2 分钟

        if (!finished) {
            // 超时 — 进程卡住了
            proc.kill();
            proc.waitForFinished(2000);
            QMessageBox::critical(&dlg, "创建超时",
                QString("%1 创建磁盘超时，进程已强制终止。\n"
                        "请检查磁盘是否已存在或被其他进程占用。").arg(tool));
            return;
        }

        if (proc.exitCode() != 0) {
            QString err = QString::fromUtf8(proc.readAllStandardError());
            QMessageBox::critical(&dlg, "创建失败",
                QString("%1 创建磁盘失败:\n").arg(tool) + err);
            return;
        }

        outPath = path;
        dlg.accept();
    });

    QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    return dlg.exec() == QDialog::Accepted;
}

// ═══════════════════════════════════════════════════════════════════
//  磁盘编辑对话框 (系统盘与数据盘共用)
// ═══════════════════════════════════════════════════════════════════
static bool editDiskDialog(QWidget *parent, VMDataDisk &dd, bool isNew, const QString &vmType)
{
    const bool vbox = (vmType == "virtualbox");

    QDialog dlg(parent);
    dlg.setWindowTitle(isNew ? "添加磁盘" : "编辑磁盘");
    dlg.resize(560, 260);

    auto *layout = new QVBoxLayout(&dlg);
    auto *form   = new QFormLayout();

    auto *fmtCombo = new QComboBox();
    fmtCombo->addItem("qcow2", "qcow2");
    fmtCombo->addItem("raw",   "raw");
    fmtCombo->addItem("vmdk",  "vmdk");
    fmtCombo->addItem("vdi",   "vdi");
    fmtCombo->addItem("vhdx",  "vhdx");
    int fIdx = fmtCombo->findData(dd.format);
    if (fIdx >= 0) fmtCombo->setCurrentIndex(fIdx);
    else if (isNew && vbox) fmtCombo->setCurrentIndex(fmtCombo->findData("vdi"));
    fmtCombo->setToolTip("qcow2 / raw 用于 QEMU; vdi / vmdk / vhdx 用于 VirtualBox\n(老格式 img 归一为 raw)");

    auto *pathEdit = new QLineEdit(dd.path);
    pathEdit->setPlaceholderText("磁盘映像路径 (.qcow2 / .raw / .vdi / .vmdk / .vhdx)");
    auto *browseBtn = new QPushButton("浏览…");
    auto *createBtn = new QPushButton(vbox ? "创建磁盘…" : "创建映像…");
    createBtn->setToolTip(vbox ? "调用 VBoxManage 创建新的磁盘映像"
                               : "调用 qemu-img 创建新的磁盘映像");
    auto *pathRow   = new QHBoxLayout();
    pathRow->addWidget(pathEdit, 1);
    pathRow->addWidget(browseBtn);
    pathRow->addWidget(createBtn);
    QObject::connect(browseBtn, &QPushButton::clicked, [&]() {
        QString path = QFileDialog::getOpenFileName(&dlg, "选择磁盘",
            pathEdit->text().isEmpty() ? QDir::homePath() : pathEdit->text(),
            "磁盘映像 (*.qcow2 *.raw *.img *.vdi *.vmdk *.vhdx);;所有文件 (*)");
        if (!path.isEmpty()) pathEdit->setText(path);
    });
    QObject::connect(createBtn, &QPushButton::clicked, [&]() {
        QString newPath;
        if (createDiskImageDialog(&dlg, vmType, newPath)) {
            pathEdit->setText(newPath);
            // 自动匹配格式
            QString ext = QFileInfo(newPath).suffix().toLower();
            int idx = fmtCombo->findData(ext);
            if (idx >= 0) fmtCombo->setCurrentIndex(idx);
        }
    });
    form->addRow("路径:", pathRow);

    form->addRow("格式:", fmtCombo);

    auto *sysCb = new QCheckBox("系统盘 (启动盘)");
    sysCb->setChecked(dd.system);
    sysCb->setToolTip("勾选后此盘作为启动盘, 位于列表首位; 每个虚机最多一个系统盘");
    form->addRow("", sysCb);

    auto *ifaceCombo = new QComboBox();
    ifaceCombo->addItem("virtio (高性能, 需装驱动)", "virtio");
    ifaceCombo->addItem("SATA (AHCI)", "sata");
    ifaceCombo->addItem("IDE", "ide");
    int iIdx = ifaceCombo->findData(dd.iface);
    if (iIdx >= 0) ifaceCombo->setCurrentIndex(iIdx);
    ifaceCombo->setToolTip(
        "这块盘接在哪个控制器上 (系统盘与数据盘都可选).\n"
        "IDE: 原生 IDE 控制器, Win98/2000/XP/7 免驱动即认盘(推荐旧 OS)\n"
        "SATA: AHCI 控制器 (q35 上自动建 ich9-ahci), Win7 SP1+ 原生支持\n"
        "virtio: 性能最高, 但需装入对应驱动 (Win10/11 自带, 老系统需手动装)\n"
        "VirtualBox 忽略此项.");
    form->addRow(dd.system ? "系统盘接口:" : "接口:", ifaceCombo);

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
    dd.format = fmtCombo->currentData().toString();
    dd.system = sysCb->isChecked();
    dd.iface  = ifaceCombo->currentData().toString();
    dd.cache  = cacheCombo->currentData().toString();
    dd.aio    = aioCombo->currentData().toString();
    return !dd.path.isEmpty();
}

// ── 磁盘条目 ↔ 列表项 (显示文本 / 编码数据) ──
static QListWidgetItem *makeDiskItem(const VMDataDisk &dd)
{
    QString name = QFileInfo(dd.path).fileName();
    QString text = dd.system
        ? QString("★ 系统盘  %1  [%2 | if:%3 | cache:%4 | aio:%5]")
              .arg(name, dd.format, dd.iface, dd.cache, dd.aio)
        : QString("%1  [%2 | if:%3 | cache:%4 | aio:%5]").arg(name, dd.format, dd.iface, dd.cache, dd.aio);

    auto *item = new QListWidgetItem(text);
    item->setData(Qt::UserRole,
        QString("%1|%2|%3|%4|%5|%6").arg(dd.path, dd.format, dd.cache, dd.aio, dd.iface)
                                    .arg(dd.system ? "1" : "0"));
    QString tip = QString("路径:%1\n格式:%2\n接口:%3\n缓存:%4\nAIO:%5")
                      .arg(dd.path, dd.format, dd.iface, dd.cache, dd.aio);
    if (dd.system) tip += "\n系统盘 (启动盘)";
    item->setToolTip(tip);
    return item;
}

static VMDataDisk diskFromItem(QListWidgetItem *item)
{
    QStringList parts = item->data(Qt::UserRole).toString().split('|');
    VMDataDisk dd;
    if (parts.size() >= 1) dd.path   = parts[0];
    if (parts.size() >= 2) dd.format = parts[1];
    if (parts.size() >= 3) dd.cache  = parts[2];
    if (parts.size() >= 4) dd.aio    = parts[3];
    if (parts.size() >= 5) dd.iface  = parts[4];
    if (parts.size() >= 6) dd.system = (parts[5] == "1");
    return dd;
}

// ═══════════════════════════════════════════════════════════════════
//  USB 直通: 宿主机扫描 + 条目编辑
//  列表项 data: "host|vid:pid|描述" 或 "evdev|/dev/input/by-id/…"
// ═══════════════════════════════════════════════════════════════════

// 用 lsusb 列出宿主机 USB 设备, 返回 "vid:pid|描述"
static QStringList scanUsbDevices()
{
    QStringList out;
    QProcess p;
    p.start("lsusb", {});
    if (!p.waitForFinished(4000)) { p.kill(); p.waitForFinished(500); return out; }
    static const QRegularExpression re("ID ([0-9a-f]{4}:[0-9a-f]{4})\\s+(.+)$",
                                       QRegularExpression::CaseInsensitiveOption);
    for (const QString &line : QString::fromUtf8(p.readAllStandardOutput()).split('\n')) {
        const auto m = re.match(line.trimmed());
        if (!m.hasMatch()) continue;
        out << QString("%1|%2").arg(m.captured(1), m.captured(2).trimmed());
    }
    return out;
}

// 扫描 /dev/input/by-id 下的 evdev 事件设备
static QStringList scanEvdevPaths()
{
    QStringList out;
    const QString dir = "/dev/input/by-id";
    const QStringList entries = QDir(dir).entryList(QDir::Files, QDir::Name);
    for (const QString &f : entries)
        if (f.contains("event")) out << dir + "/" + f;
    return out;
}

static QString usbItemText(const QString &data)
{
    const QStringList parts = data.split('|');
    if (parts.value(0) == "host")
        return QString("🖳 USB  %1  %2").arg(parts.value(1), parts.value(2));
    return QString("⌨ evdev  %1").arg(parts.value(1));
}

static bool editUsbDeviceDialog(QWidget *parent, QString &outData)
{
    QDialog dlg(parent);
    dlg.setWindowTitle("添加 USB 直通条目");
    dlg.resize(560, 220);

    auto *layout = new QVBoxLayout(&dlg);
    auto *hostRadio = new QRadioButton("物理 USB 设备 (按 vid:pid 整体透传)");
    auto *evRadio   = new QRadioButton("evdev 输入设备 (物理键鼠直接进虚机)");
    hostRadio->setChecked(true);
    hostRadio->setToolTip("透传后宿主机失去该设备; 需要上方勾选 xhci 控制器.");
    evRadio->setToolTip("走 virtio-input-host-pci, 读 /dev/input/*, 需要以 root 运行 QEMU.");
    layout->addWidget(hostRadio);
    layout->addWidget(evRadio);

    auto *vidEdit = new QLineEdit();
    vidEdit->setPlaceholderText("例: 046d:c53f  (lsusb 的 ID 字段)");
    auto *scanUsbBtn = new QPushButton("🔍 从 lsusb 选择");
    auto *vidRow = new QHBoxLayout();
    vidRow->addWidget(new QLabel("VID:PID"));
    vidRow->addWidget(vidEdit, 1);
    vidRow->addWidget(scanUsbBtn);
    layout->addLayout(vidRow);

    auto *evCombo = new QComboBox();
    evCombo->setEditable(true);
    const QStringList evs = scanEvdevPaths();
    for (const QString &e : evs) evCombo->addItem(e);
    evCombo->setToolTip("扫描自 /dev/input/by-id (只列 event 设备)");
    layout->addWidget(evCombo);

    QObject::connect(hostRadio, &QRadioButton::toggled, &dlg,
                     [vidEdit, evCombo, scanUsbBtn](bool on) {
        vidEdit->setVisible(on);
        scanUsbBtn->setVisible(on);
        evCombo->setVisible(!on);
    });

    QObject::connect(scanUsbBtn, &QPushButton::clicked, &dlg, [vidEdit]() {
        const QStringList devs = scanUsbDevices();
        if (devs.isEmpty()) {
            QMessageBox::warning(vidEdit->parentWidget(), "提示", "lsusb 未取到设备");
            return;
        }
        QStringList items;
        for (const QString &d : devs) {
            const QStringList p = d.split('|');
            items << QString("%1  %2").arg(p.value(0), p.value(1));
        }
        bool ok = false;
        const QString picked = QInputDialog::getItem(vidEdit->parentWidget(),
            "选择 USB 设备", "设备:", items, 0, false, &ok);
        if (ok && !picked.isEmpty()) {
            const QString vid = picked.section(' ', 0, 0);
            const QString desc = picked.section(' ', 1);
            vidEdit->setText(vid);
            vidEdit->setToolTip(desc);
        }
    });

    auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(btns);

    if (dlg.exec() != QDialog::Accepted) return false;

    if (hostRadio->isChecked()) {
        const QString vp = vidEdit->text().trimmed().toLower();
        static const QRegularExpression re("^(?:0x)?([0-9a-f]{4}):(?:0x)?([0-9a-f]{4})$");
        const auto m = re.match(vp);
        if (!m.hasMatch()) {
            QMessageBox::warning(&dlg, "提示", "格式应为 vid:pid, 如 046d:c53f");
            return false;
        }
        outData = QString("host|%1:%2|%3").arg(m.captured(1), m.captured(2),
                                               vidEdit->toolTip());
        return true;
    }
    const QString path = evCombo->currentText().trimmed();
    if (path.isEmpty()) {
        QMessageBox::warning(&dlg, "提示", "请选择 evdev 路径");
        return false;
    }
    outData = "evdev|" + path;
    return true;
}

// ═══════════════════════════════════════════════════════════════════
//  updateVmTypeUI — 按虚拟机类型显隐对应控件
// ═══════════════════════════════════════════════════════════════════
void VMDialog::updateVmTypeUI()
{
    const bool vbox = (m_vmTypeCombo->currentData() == "virtualbox");

    // 类型专属的整组: 隐藏时移出布局 —— Qt 的隐藏控件仍计入布局最小高度,
    // 不移出的话窗口最小高度被 VirtualBox 组撑到 1200+px, 压不进 1080 屏幕。
    auto syncGroup = [this](QGroupBox *g, bool show) {
        g->setVisible(show);
        if (show) {
            const auto it = m_groupSlot.find(g);
            if (it != m_groupSlot.end() && it->first->indexOf(g) < 0)
                it->first->insertWidget(qMin(it->second, it->first->count()), g);
            return;
        }
        for (auto *col : { m_leftCol, m_midCol, m_rightCol }) {
            const int idx = col->indexOf(g);
            if (idx >= 0) { m_groupSlot.insert(g, { col, idx }); col->removeWidget(g); return; }
        }
    };
    syncGroup(m_vboxGpuGroup, vbox);
    syncGroup(m_vboxGroup,   vbox);

    // 网络设置组: 按类型显示对应页
    m_netGroup->setVisible(true);
    m_netPageQemu->setVisible(!vbox);
    m_netPageVbox->setVisible(vbox);

    // QEMU 高级控件: VirtualBox 时隐藏 (架构/机器/CPU/KVM/VGA/VNC)
    m_qemuCombo->setVisible(!vbox);
    m_machineCombo->setVisible(!vbox);
    m_cpuTypeCombo->setVisible(!vbox);
    m_kvmCb->setVisible(!vbox);
    m_vgaCombo->setVisible(!vbox);
    m_vncSpin->setVisible(!vbox);

    // 高级参数组 (右列): VirtualBox 时整组隐藏，窗口更短
    syncGroup(m_advGroup, !vbox);

    // 额外参数组是 QEMU 专属，VirtualBox 时整组隐藏（窗口更短）
    syncGroup(m_extraGroup, !vbox);

    // QEMU 专属: UEFI 固件 / 音频 / USB 直通 / Root 钩子 / 无显示 / 直通增强
    syncGroup(m_hptGroup,    !vbox);   // VirtualBox 忽略 PCI 直通与 hugepages
    syncGroup(m_uefiGroup,   !vbox);
    syncGroup(m_audioGroup,  !vbox);
    syncGroup(m_usbGroup,    !vbox);
    syncGroup(m_rootHookGroup, !vbox);
    m_displayNoneCb->setVisible(!vbox);
    m_runRootCb->setVisible(!vbox);
    m_vfioMultiCb->setVisible(!vbox);
    m_hole64Row->setVisible(!vbox);

    // 端口转发组: VirtualBox 时压缩高度并隐藏「编辑」按钮 (表变矮, 窗口更短)
    m_portGroup->setTitle(vbox ? "端口转发 (NAT)" : "端口转发 (user 模式)");
    m_portTable->setMinimumHeight(vbox ? 50 : 110);
    m_portTable->setMaximumHeight(vbox ? 90 : QWIDGETSIZE_MAX);
    m_portEditBtn->setVisible(!vbox);

    // VirtualBox: 桥接模式时才显示桥接网卡输入
    if (m_vboxNicTypeCombo && m_vboxBridgeEdit)
        m_vboxBridgeEdit->setVisible(vbox
            && m_vboxNicTypeCombo->currentData().toString() == "bridged");
}

VMDialog::VMDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("编辑虚机配置");
    resize(1240, 940);

    auto *outer = new QVBoxLayout(this);

    // ── 三列主体 (左: 基本/磁盘/钩子 · 中: 网络/直通/端口 · 右: 显示/固件/音频/USB) ──
    auto *cols = new QHBoxLayout();

    // ════════ 左列 ════════
    m_leftCol = new QVBoxLayout();
    auto *leftCol = m_leftCol;

    // ════════ 中列 ════════
    m_midCol = new QVBoxLayout();
    auto *midCol = m_midCol;

    // ════════ 右列 ════════
    m_rightCol = new QVBoxLayout();
    auto *rightCol = m_rightCol;

    auto *basicGroup = new QGroupBox("基本参数");
    auto *basicForm  = new QFormLayout(basicGroup);

    // 虚拟机类型 (QEMU / VirtualBox)
    m_vmTypeCombo = new QComboBox();
    m_vmTypeCombo->addItem("QEMU (KVM 加速, 高性能)", "qemu");
    m_vmTypeCombo->addItem("VirtualBox (自带窗口)", "virtualbox");
    m_vmTypeCombo->setCurrentIndex(0);
    m_vmTypeCombo->setToolTip("QEMU: KVM 硬件加速、精细调优\nVirtualBox: 开箱即用、自带窗口，适合老系统");
    basicForm->addRow("虚拟机类型:", m_vmTypeCombo);

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

    auto *isoRow = new QHBoxLayout();
    m_isoEdit = new QLineEdit();
    m_isoEdit->setPlaceholderText("安装 ISO (可选)");
    isoRow->addWidget(m_isoEdit, 1);
    m_isoBtn = new QPushButton("浏览…");
    isoRow->addWidget(m_isoBtn);
    basicForm->addRow("ISO:", isoRow);

    leftCol->addWidget(basicGroup);

    // ── 显卡设置 ── (VirtualBox 专属, 置于网络设置上方)
    m_vboxGpuGroup = new QGroupBox("显卡设置");
    auto *gpuForm  = new QFormLayout(m_vboxGpuGroup);

    m_vboxGpuCombo = new QComboBox();
    m_vboxGpuCombo->addItem("VBoxSVGA (VMSVGA, 默认, 支持 3D)", "vmsvga");
    m_vboxGpuCombo->addItem("VBoxVGA (老 OS 兼容, 不支持 3D)", "vboxvga");
    m_vboxGpuCombo->addItem("VBoxGBA (无头服务器)", "vboxgba");
    m_vboxGpuCombo->setCurrentIndex(m_vboxGpuCombo->findData("vmsvga"));
    m_vboxGpuCombo->setToolTip(
        "显卡控制器 (--graphicscontroller).\n"
        "VBoxSVGA: 现代系统默认, 唯一支持 3D 加速的控制器.\n"
        "VBoxVGA: 老 OS (Win98/XP) 免驱动兼容, 不支持 3D.\n"
        "VBoxGBA: 无头模式用, 无 3D.\n"
        "勾选 3D 加速后强制锁定为 VBoxSVGA.");
    gpuForm->addRow("显卡控制器:", m_vboxGpuCombo);

    m_vboxVramSpin = new QSpinBox();
    m_vboxVramSpin->setRange(64, 128);
    m_vboxVramSpin->setValue(128);
    m_vboxVramSpin->setSingleStep(8);
    m_vboxVramSpin->setSuffix(" MB");
    m_vboxVramSpin->setToolTip(
        "显卡显存. 未启用 3D 加速时 64~128 MB;\n"
        "启用 3D 加速后最高 256 MB.\n"
        "现代系统建议 128 MB; 老 OS (Win98/XP) 用 64 MB 即可.");
    gpuForm->addRow("显存:", m_vboxVramSpin);

    m_vbox3dCb = new QCheckBox("启用 3D 加速");
    m_vbox3dCb->setChecked(false);
    m_vbox3dCb->setToolTip(
        "VirtualBox 3D 加速 (--accelerate3d on), 需安装 Guest Additions.\n"
        "老 OS (Win98/XP) 与 3D 加速兼容性差, 建议关闭.\n"
        "勾选后显存上限放宽到 256 MB.");
    gpuForm->addRow("", m_vbox3dCb);

    // 3D 加速开关 → 显存上限 128 / 256, 显卡控制器强制 VBoxSVGA
    connect(m_vbox3dCb, &QCheckBox::toggled, this, [this](bool on) {
        m_vboxVramSpin->setMaximum(on ? 256 : 128);
        if (m_vboxVramSpin->value() > m_vboxVramSpin->maximum())
            m_vboxVramSpin->setValue(m_vboxVramSpin->maximum());
        m_vboxGpuCombo->setEnabled(!on);
        if (on)
            m_vboxGpuCombo->setCurrentIndex(m_vboxGpuCombo->findData("vmsvga"));
    });

    m_vboxGpuGroup->setVisible(false);  // 默认隐藏, 非 VirtualBox 时不显示
    midCol->addWidget(m_vboxGpuGroup);

    // ── 网络设置 ── 统一组: 按虚拟机类型显示 QEMU / VirtualBox 页
    m_netGroup = new QGroupBox("网络设置");
    auto *netOuter = new QVBoxLayout(m_netGroup);
    netOuter->setContentsMargins(9, 9, 9, 9);
    netOuter->setSpacing(0);

    // QEMU 页
    m_netPageQemu = new QWidget();
    auto *qemuNetForm = new QFormLayout(m_netPageQemu);
    qemuNetForm->setContentsMargins(0, 0, 0, 0);

    m_netCombo = new QComboBox();
    m_netCombo->addItem("user (NAT)",   "user");
    m_netCombo->addItem("bridge (桥接)", "bridge");
    m_netCombo->addItem("tap 设备",      "tap");
    m_netCombo->setToolTip(
        "QEMU 网络模式.\n"
        "user (NAT): 用户态网络, 出站可用; 入站靠下方「端口转发」(hostfwd).\n"
        "bridge: 桥接到宿主机网桥, 与局域网同网段.\n"
        "tap: 直接使用预先创建好的 tap 设备.");
    qemuNetForm->addRow("网络模式:", m_netCombo);

    m_nicCombo = new QComboBox();
    m_nicCombo->setEditable(true);
    m_nicCombo->addItem("virtio-net-pci");
    m_nicCombo->addItem("e1000");
    m_nicCombo->addItem("rtl8139");
    m_nicCombo->addItem("ne2k_pci");
    m_nicCombo->addItem("virtio-net-ccw");
    m_nicCombo->setToolTip(
        "QEMU 网卡型号.\n"
        "virtio-net-pci: 性能最好, Windows 需装 virtio 驱动.\n"
        "e1000: Intel 千兆, 多数系统免驱动.\n"
        "rtl8139 / ne2k_pci: 老系统兼容用.");
    qemuNetForm->addRow("网卡型号:", m_nicCombo);

    // VirtualBox 页
    m_netPageVbox = new QWidget();
    auto *vboxNetForm = new QFormLayout(m_netPageVbox);
    vboxNetForm->setContentsMargins(0, 0, 0, 0);

    m_vboxNicTypeCombo = new QComboBox();
    m_vboxNicTypeCombo->addItem("NAT", "nat");
    m_vboxNicTypeCombo->addItem("桥接 (Bridged)", "bridged");
    m_vboxNicTypeCombo->setCurrentIndex(m_vboxNicTypeCombo->findData("nat"));
    m_vboxNicTypeCombo->setToolTip(
        "VirtualBox 网络模式.\n"
        "NAT: 宿主机做 NAT, 虚机可出站; 端口转发用下方「端口转发」表 (写入 --natpf1).\n"
        "桥接: 虚机直接接到宿主机网卡, 与局域网同网段.");
    vboxNetForm->addRow("网络模式:", m_vboxNicTypeCombo);

    m_vboxNicModelCombo = new QComboBox();
    m_vboxNicModelCombo->addItem("Intel PRO/1000 (82545EM, 默认)", "82545EM");
    m_vboxNicModelCombo->addItem("Intel PRO/100 (82540EM)", "82540EM");
    m_vboxNicModelCombo->addItem("PCNet (Am79C973, 老 OS 免驱动)", "Am79C973");
    m_vboxNicModelCombo->addItem("Virtio-NET (高性能, 需装驱动)", "virtio");
    m_vboxNicModelCombo->addItem("USB 网卡 (usbnet)", "usbnet");
    m_vboxNicModelCombo->setCurrentIndex(m_vboxNicModelCombo->findData("82545EM"));
    m_vboxNicModelCombo->setToolTip(
        "网卡型号 (--nic-type1).\n"
        "82545EM: 千兆, Win2000+ 原生支持.\n"
        "82540EM: 百兆, Win98/2000 免驱动.\n"
        "Am79C973: PCNet, 极老 OS (Win3.1/95/98) 免驱动.\n"
        "virtio: 高性能, Linux 原生, Windows 需装驱动.");
    vboxNetForm->addRow("网卡型号:", m_vboxNicModelCombo);

    m_vboxBridgeEdit = new QLineEdit();
    m_vboxBridgeEdit->setPlaceholderText("宿主机网卡名, 如 eth0");
    vboxNetForm->addRow("桥接网卡:", m_vboxBridgeEdit);

    netOuter->addWidget(m_netPageQemu);
    netOuter->addWidget(m_netPageVbox);
    // 注意: 本组 (含 QEMU/VBox 两个子页) 由下方 rightCol/midCol 决定归属, 此处只建控件

    // ── VirtualBox 专属设置 ── (按虚拟机类型显示/隐藏)
    m_vboxGroup = new QGroupBox("VirtualBox 设置");
    auto *vboxForm  = new QFormLayout(m_vboxGroup);

    m_ostypeCombo = new QComboBox();
    m_ostypeCombo->setEditable(false);
    m_ostypeCombo->addItem("Windows 3.1", "Windows31");
    m_ostypeCombo->addItem("Windows 95", "Windows95");
    m_ostypeCombo->addItem("Windows 98", "Windows98");
    m_ostypeCombo->addItem("Windows ME", "WindowsMe");
    m_ostypeCombo->addItem("Windows NT 4", "WindowsNT4");
    m_ostypeCombo->addItem("Windows 2000", "Windows2000");
    m_ostypeCombo->addItem("Windows XP (32-bit)", "WindowsXP");
    m_ostypeCombo->addItem("Windows XP (64-bit)", "WindowsXP_64");
    m_ostypeCombo->addItem("Windows 7 (32-bit)", "Windows7");
    m_ostypeCombo->addItem("Windows 7 (64-bit)", "Windows7_64");
    m_ostypeCombo->addItem("Windows 8.1 (32-bit)", "Windows81");
    m_ostypeCombo->addItem("Windows 8.1 (64-bit)", "Windows81_64");
    m_ostypeCombo->addItem("Windows 10 (32-bit)", "Windows10");
    m_ostypeCombo->addItem("Windows 10 (64-bit)", "Windows10_64");
    m_ostypeCombo->addItem("Windows 11 (64-bit)", "Windows11_64");
    m_ostypeCombo->setCurrentIndex(m_ostypeCombo->findData("Windows7_64"));
    m_ostypeCombo->setToolTip(
        "目标操作系统 (VirtualBox 的 ostype 预设).\n"
        "64 位镜像必须选带 (64-bit) 的条目, 否则 VM 不开 long mode, 64 位安装程序无法启动.\n"
        "旧 OS (Win98/XP) 也可运行, 但安装镜像需走 SATA 控制器.");
    vboxForm->addRow("目标 OS:", m_ostypeCombo);

    m_storageBackendCombo = new QComboBox();
    m_storageBackendCombo->addItem("SATA (默认, 通用)", "sata");
    m_storageBackendCombo->addItem("IDE (老 OS, 7.2.6 会回退 SATA)", "ide");
    m_storageBackendCombo->setCurrentIndex(0);
    m_storageBackendCombo->setToolTip(
        "磁盘控制器.\n"
        "注: VirtualBox 7.2.x 的 CLI 中 IDE 控制器 storageattach 失效,\n"
        "无论选哪个都会自动回退到 SATA.");
    vboxForm->addRow("存储后端:", m_storageBackendCombo);

    m_vboxDiskSizeSpin = new QSpinBox();
    m_vboxDiskSizeSpin->setRange(16, 10485760);
    m_vboxDiskSizeSpin->setValue(20480);
    m_vboxDiskSizeSpin->setSuffix(" MB");
    m_vboxDiskSizeSpin->setSingleStep(512);
    m_vboxDiskSizeSpin->setToolTip("磁盘条目指向的文件不存在时, 按此大小自动创建 (格式取该条目自己的格式)");
    vboxForm->addRow("建盘大小:", m_vboxDiskSizeSpin);

    m_vboxAcpiCb = new QCheckBox("启用 ACPI");
    m_vboxAcpiCb->setChecked(true);
    m_vboxAcpiCb->setToolTip("Win7 及更新系统必须开启, 否则安装时报 'BIOS is not ACPI compliant'.\n极老 OS (Win98/DOS) 可能需要关闭.");
    vboxForm->addRow("", m_vboxAcpiCb);

    m_vboxIoapicCb = new QCheckBox("启用 IOAPIC");
    m_vboxIoapicCb->setChecked(true);
    m_vboxIoapicCb->setToolTip("多核/中断控制器, 通常与 ACPI 一起开启.");
    vboxForm->addRow("", m_vboxIoapicCb);

    m_vboxUefiCb = new QCheckBox("启用 UEFI 引导");
    m_vboxUefiCb->setChecked(true);
    m_vboxUefiCb->setToolTip(
        "VirtualBox 固件: UEFI (--firmware efi) 或 BIOS.\n"
        "UEFI: 现代系统需要 (Win10/11, GRUB-UEFI, 大于 2TB 的启动盘).\n"
        "BIOS: 老 OS (Win98/XP/7 32-bit) 免驱动即引导.\n"
        "注意: 已装好的系统换固件后通常无法启动, 只在新建或重装时改.");
    vboxForm->addRow("", m_vboxUefiCb);

    m_vboxRdpCb = new QCheckBox("启用 RDP 远程控制 (VRDE)");
    m_vboxRdpCb->setChecked(false);
    m_vboxRdpCb->setToolTip(
        "VirtualBox 内置的 RDP 服务器 (VRDE). 开启后可用 RDP 客户端连虚机画面,\n"
        "适合无头 / 远程管理. 只在虚机运行时监听.\n"
        "宿主机需装 RDP 客户端: xfreerdp / rdesktop / remmina / krdc.");
    vboxForm->addRow("", m_vboxRdpCb);

    m_vboxRdpPortSpin = new QSpinBox();
    m_vboxRdpPortSpin->setRange(1, 65535);
    m_vboxRdpPortSpin->setValue(3389);
    m_vboxRdpPortSpin->setToolTip("RDP 监听端口 (默认 3389). 端口被占用时 VirtualBox 会自动换端口.");
    m_vboxRdpPortSpin->setEnabled(false);
    vboxForm->addRow("RDP 端口:", m_vboxRdpPortSpin);
    connect(m_vboxRdpCb, &QCheckBox::toggled, this, [this](bool on) {
        m_vboxRdpPortSpin->setEnabled(on);
    });

    m_vboxGroup->setVisible(false);  // 默认隐藏, 非 VirtualBox 时不显示
    midCol->addWidget(m_vboxGroup);

    // ── 磁盘 (系统盘 + 数据盘 统一列表) ──
    auto *ddGroup = new QGroupBox("磁盘");
    auto *ddLayout = new QVBoxLayout(ddGroup);
    auto *ddHint = new QLabel("★ 系统盘 = 启动盘 (自动置于列表首位), 其余为附加数据盘:");
    ddHint->setStyleSheet("color: #888; font-size: 11px;");
    ddLayout->addWidget(ddHint);

    m_diskList = new QListWidget();
    m_diskList->setAlternatingRowColors(true);
    m_diskList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_diskList->setMinimumHeight(90);
    ddLayout->addWidget(m_diskList);

    auto *ddBtnRow = new QHBoxLayout();
    m_diskAddBtn = new QPushButton("➕ 添加");
    m_diskEditBtn = new QPushButton("✏️ 编辑");
    m_diskRemoveBtn = new QPushButton("🗑️ 移除");
    ddBtnRow->addWidget(m_diskAddBtn);
    ddBtnRow->addWidget(m_diskEditBtn);
    ddBtnRow->addWidget(m_diskRemoveBtn);
    ddBtnRow->addStretch();
    ddLayout->addLayout(ddBtnRow);
    leftCol->addWidget(ddGroup);

    // ── 硬件直通 ──
    m_hptGroup = new QGroupBox("硬件直通");
    auto *hptGroup = m_hptGroup;
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

    // ── 直通增强: 同 slot 多 function + q35 hole64 ──
    m_vfioMultiCb = new QCheckBox("多函数 multifunction=on");
    m_vfioMultiCb->setToolTip(
        "同一物理 slot 的多个 function (如 显卡 + 显卡音频) 必须落在同一个虚拟 slot.\n"
        "勾选后每个 vfio-pci 设备都带 multifunction=on, 并分配显式 id.\n"
        "左侧 PCI 列表的顺序 = 命令行顺序 = function 递增顺序\n"
        "(例: 06:00.0 在前, 06:00.1 在后).");
    hptLayout->addWidget(m_vfioMultiCb);

    m_hole64Row = new QWidget();
    auto *hole64RowLay = new QHBoxLayout(m_hole64Row);
    hole64RowLay->setContentsMargins(0, 0, 0, 0);
    hole64RowLay->addWidget(new QLabel("PCI hole64:"));
    m_hole64Combo = new QComboBox();
    m_hole64Combo->setEditable(true);
    m_hole64Combo->addItem("留空 (不设)");
    m_hole64Combo->addItem("32G");
    m_hole64Combo->addItem("64G");
    m_hole64Combo->addItem("256G");
    m_hole64Combo->addItem("512G");
    m_hole64Combo->addItem("1024G");
    m_hole64Combo->setToolTip(
        "q35-pcihost.pci-hole64-size: 给设备内存映射预留的地址空间.\n"
        "显卡直通需要足够大的 hole64 才能映射显存 BAR (RTX 4060 用 512G 稳).\n"
        "仅 q35 系机器生效; pc / virt 机器不会输出该参数.");
    hole64RowLay->addWidget(m_hole64Combo, 1);
    hptLayout->addWidget(m_hole64Row);

    midCol->addWidget(m_hptGroup);

    // ── 音频 (QEMU audiodev) ──
    m_audioGroup = new QGroupBox("音频 (QEMU)");
    auto *audioForm = new QFormLayout(m_audioGroup);

    m_audioBackendCombo = new QComboBox();
    m_audioBackendCombo->addItem("不启用 (none)",     "none");
    m_audioBackendCombo->addItem("PulseAudio (pa)",   "pa");
    m_audioBackendCombo->addItem("PipeWire (pipewire)", "pipewire");
    m_audioBackendCombo->addItem("ALSA (alsa)",       "alsa");
    m_audioBackendCombo->addItem("OSS (oss)",         "oss");
    m_audioBackendCombo->addItem("Sndio (sndio)",     "sndio");
    m_audioBackendCombo->setCurrentIndex(m_audioBackendCombo->findData("none"));
    m_audioBackendCombo->setToolTip(
        "-audiodev 后端.\n"
        "pa: 走 PulseAudio / pipewire-pulse. 程序会自动带上 XDG_RUNTIME_DIR\n"
        "与 PULSE_SERVER, 并在缺少 pulse/pid 时补一次 (root 客户端需要它).\n"
        "显卡直通的 GPU 音频函数交给虚机后, 主机侧声卡消失, 用 HDA 虚声卡给虚机出声.");
    audioForm->addRow("后端:", m_audioBackendCombo);

    m_audioModelCombo = new QComboBox();
    m_audioModelCombo->addItem("HDA duplex (hda-duplex, 全双工)", "hda-duplex");
    m_audioModelCombo->addItem("HDA micro (hda-micro, 只出声)",   "hda-micro");
    m_audioModelCombo->setCurrentIndex(m_audioModelCombo->findData("hda-duplex"));
    m_audioModelCombo->setToolTip(
        "选后端为 none 时此项不起作用.\n"
        "勾选后自动输出: -audiodev <后端>,id=au0 -device ich9-intel-hda\n"
        "                        -device <hda-duplex|hda-micro>,audiodev=au0");
    audioForm->addRow("虚声卡:", m_audioModelCombo);

    leftCol->addWidget(m_audioGroup);

    // ── USB 直通 ──
    m_usbGroup = new QGroupBox("USB 直通 (QEMU)");
    auto *usbLayout = new QVBoxLayout(m_usbGroup);

    m_usbXhciCb = new QCheckBox("启用 xhci 控制器 (qemu-xhci)");
    m_usbXhciCb->setToolTip("物理 USB 设备与虚拟 usb-kbd/tablet 都要挂在 xhci 总线上.\n"
                            "勾选列表里的 usb-host 条目时若未勾选此项, 程序会自动补一条 xhci.");
    usbLayout->addWidget(m_usbXhciCb);

    m_usbKbdTabletCb = new QCheckBox("虚拟 usb-kbd + usb-tablet (远程注入用)");
    m_usbKbdTabletCb->setToolTip("在主机没有物理键盘透传时, 给虚机一套虚拟键鼠,\n"
                                 "可用 VNC / 远程工具注入输入.");
    usbLayout->addWidget(m_usbKbdTabletCb);

    m_usbList = new QListWidget();
    m_usbList->setAlternatingRowColors(true);
    m_usbList->setMinimumHeight(70);
    m_usbList->setToolTip(
        "🖳 = 按 vid:pid 整体透传物理 USB 设备 (透传后宿主机失去该设备)\n"
        "⌨ = evdev 输入设备走 virtio-input-host-pci (读 /dev/input/*, 需 root 运行 QEMU)");
    usbLayout->addWidget(m_usbList);

    auto *usbBtnRow = new QHBoxLayout();
    m_usbAddBtn    = new QPushButton("➕ 添加");
    m_usbRemoveBtn = new QPushButton("🗑️ 移除");
    m_usbScanBtn   = new QPushButton("🔍 扫描宿主机");
    m_usbScanBtn->setToolTip("用 lsusb 列出可整体透传的 USB 设备, 并扫描 /dev/input/by-id 下的输入设备,\n"
                             "多选后加入列表.");
    usbBtnRow->addWidget(m_usbAddBtn);
    usbBtnRow->addWidget(m_usbRemoveBtn);
    usbBtnRow->addWidget(m_usbScanBtn);
    usbBtnRow->addStretch();
    usbLayout->addLayout(usbBtnRow);

    rightCol->addWidget(m_usbGroup);

    // ── Root 钩子 (GPU 切换脚本) ──
    m_rootHookGroup = new QGroupBox("Root 钩子 (GPU 切换脚本)");
    auto *hookLayout = new QVBoxLayout(m_rootHookGroup);

    auto *hookWarn = new QLabel(
        "⚠️ 启动前脚本会停止 greetd 并释放显卡 → 当前桌面黑屏；\n"
        "退出后脚本回绑 nvidia 并重启 greetd。经 sudo 执行，会弹密码框。");
    hookWarn->setStyleSheet("color: #b35421; font-size: 11px;");
    hookWarn->setWordWrap(true);
    hookLayout->addWidget(hookWarn);

    hookLayout->addWidget(new QLabel("启动前 (root):"));
    m_preRootEdit = new QPlainTextEdit();
    m_preRootEdit->setPlaceholderText("例如: 绑定 vfio-pci + 停合成器");
    m_preRootEdit->setMaximumHeight(80);
    hookLayout->addWidget(m_preRootEdit);

    hookLayout->addWidget(new QLabel("退出后 (root):"));
    m_postRootEdit = new QPlainTextEdit();
    m_postRootEdit->setPlaceholderText("例如: 回绑 nvidia + 重启 greetd");
    m_postRootEdit->setMaximumHeight(80);
    hookLayout->addWidget(m_postRootEdit);

    m_rootLoadBtn = new QPushButton("📄 载入 run-win10-pt.sh 的绑定/归还段");
    m_rootLoadBtn->setToolTip("把 ~/bin/run-win10-pt.sh 里的 gpu_to_vfio / gpu_to_host 两段填入上方两格。\n"
                              "BDF 以 06:00.0 / 06:00.1 为默认，需在 PCI 列表里选你自己的设备。");
    hookLayout->addWidget(m_rootLoadBtn);

    rightCol->addWidget(m_rootHookGroup);

    // ── 额外参数 ──
    m_extraGroup = new QGroupBox("额外 QEMU 参数");
    auto *extraLayout = new QVBoxLayout(m_extraGroup);
    m_extraEdit = new QPlainTextEdit();
    m_extraEdit->setPlaceholderText("以上控件未覆盖的参数写在这里");
    m_extraEdit->setMaximumHeight(70);
    extraLayout->addWidget(m_extraEdit);
    midCol->addWidget(m_extraGroup);

    m_advGroup = new QGroupBox("高级参数");
    auto *advForm  = new QFormLayout(m_advGroup);

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

    // 网络模式/网卡型号已移到上方「网络设置」组

    // VNC
    m_vncSpin = new QSpinBox();
    m_vncSpin->setRange(-1, 99);
    m_vncSpin->setValue(-1);
    m_vncSpin->setSpecialValueText("禁用");
    m_vncSpin->setToolTip("显示编号 (0~99)，VNC 端口 = 5900 + 编号");
    advForm->addRow("VNC 显示编号 (0~99):", m_vncSpin);

    // 无显示 (纯显卡直通场景)
    m_displayNoneCb = new QCheckBox("无显示 (-display none)");
    m_displayNoneCb->setToolTip(
        "整卡直通的虚机没有虚拟显卡: 需要 VGA 选 none + 勾选本项.\n"
        "与 VNC 互斥 —— 勾选后 VNC 置为「禁用」; VNC 编号 ≥ 0 时本项不可勾选.\n"
        "虚机画面改由直通显卡输出到物理显示器 (需要下方 Root 钩子先把显卡交给虚机).");
    advForm->addRow("", m_displayNoneCb);
    m_runRootCb = new QCheckBox("以 root 运行 QEMU (sudo)");
    m_runRootCb->setToolTip(
        "PCI 直通 (/dev/vfio/*) 与 evdev 透传 (/dev/input/*) 都需要 root.\n"
        "勾选后程序用 sudo 启动 qemu-system-*:\n"
        "  - 已有免密 sudo (启动失败时的「QEMU 权限配置」会写入 sudoers) → sudo -n;\n"
        "  - 否则弹一次密码框, 经 sudo -S 传入.\n"
        "单显卡直通的机器必须勾选此项, 并配合下方 Root 钩子先把显卡绑到 vfio-pci.");
    advForm->addRow("", m_runRootCb);

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

    m_waitMountCb = new QCheckBox("尝试主动等待所有挂载点成功再启动");
    m_waitMountCb->setToolTip(
        "勾选后，启动虚机前会检查存储管理中是否有未就绪的挂载点，"
        "若有则主动尝试挂载。适用于硬盘在开机时无法立即挂载的场景。");
    autoLayout->addWidget(m_waitMountCb);

    leftCol->addWidget(autoGroup);

    // 网络设置放到右列 (借用端口转发组缩短后腾出的空间)
    midCol->addWidget(m_netGroup);

    // ── UEFI 固件 (QEMU, pflash) ──
    m_uefiGroup = new QGroupBox("UEFI 固件 (QEMU)");
    auto *uefiLayout = new QVBoxLayout(m_uefiGroup);

    m_uefiCb = new QCheckBox("启用 UEFI (pflash CODE + VARS)");
    m_uefiCb->setToolTip(
        "用两条 pflash 盘代替 QEMU 内置 BIOS:\n"
        "  -drive if=pflash,format=raw,readonly=on,file=<CODE>\n"
        "  -drive if=pflash,format=raw,file=<VARS>\n"
        "CODE 只读 (可共用), VARS 必须可写且每台虚机一份 —— 共享 VARS\n"
        "会互相污染启动项. 已装好的系统换固件通常无法启动, 只在新建/重装时改.");
    uefiLayout->addWidget(m_uefiCb);

    auto *uefiForm = new QFormLayout();
    auto *codeRow = new QHBoxLayout();
    m_uefiCodeEdit = new QLineEdit("/usr/share/OVMF/x64/OVMF_CODE.4m.fd");
    auto *codeBtn = new QPushButton("浏览…");
    codeRow->addWidget(m_uefiCodeEdit, 1);
    codeRow->addWidget(codeBtn);
    uefiForm->addRow("CODE:", codeRow);

    auto *varsRow = new QHBoxLayout();
    m_uefiVarsEdit = new QLineEdit("/usr/share/OVMF/x64/OVMF_VARS.4m.fd");
    auto *varsBtn = new QPushButton("浏览…");
    varsRow->addWidget(m_uefiVarsEdit, 1);
    varsRow->addWidget(varsBtn);
    uefiForm->addRow("VARS:", varsRow);
    uefiLayout->addLayout(uefiForm);

    m_uefiCodeEdit->setEnabled(false);
    m_uefiVarsEdit->setEnabled(false);
    connect(m_uefiCb, &QCheckBox::toggled, this, [this](bool on) {
        m_uefiCodeEdit->setEnabled(on);
        m_uefiVarsEdit->setEnabled(on);
    });

    connect(codeBtn, &QPushButton::clicked, this, [this]() {
        const QString p = QFileDialog::getOpenFileName(this, "选择 OVMF CODE",
            m_uefiCodeEdit->text().isEmpty() ? "/usr/share/OVMF" : m_uefiCodeEdit->text(),
            "固件映像 (*.fd *.rom *.bin);;所有文件 (*)");
        if (!p.isEmpty()) m_uefiCodeEdit->setText(p);
    });
    connect(varsBtn, &QPushButton::clicked, this, [this]() {
        const QString p = QFileDialog::getOpenFileName(this, "选择 OVMF VARS",
            m_uefiVarsEdit->text().isEmpty() ? QDir::homePath() : m_uefiVarsEdit->text(),
            "固件映像 (*.fd *.rom *.bin);;所有文件 (*)");
        if (!p.isEmpty()) m_uefiVarsEdit->setText(p);
    });

    rightCol->addWidget(m_advGroup);
    leftCol->addWidget(m_uefiGroup);

    // ── 端口转发 ──
    m_portGroup = new QGroupBox("端口转发 (user 模式)");
    auto *portLayout = new QVBoxLayout(m_portGroup);

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
    midCol->addWidget(m_portGroup);

    // 列内顺序微调 (控件创建较晚, 这里按职责重排):
    //  左列: 基本参数 → 磁盘 → 启动管理 → UEFI 固件 → 音频
    //  中列: VirtualBox 设置 → 硬件直通 → 网络设置 → 端口转发 → 额外参数
    //  右列: 高级参数(显示/root 运行) → USB 直通 → Root 钩子
    // 注意: Qt 的 insertWidget 对已在布局中的控件会「再插一份」(重复项撑高度),
    // 所以重排必须先 removeWidget 再插到目标位置。
    auto moveGroup = [](QVBoxLayout *col, QGroupBox *g, int idx) {
        col->removeWidget(g);
        col->insertWidget(qMin(idx, col->count()), g);
    };
    moveGroup(leftCol, autoGroup, 2);
    moveGroup(leftCol, m_uefiGroup, 3);
    moveGroup(rightCol, m_advGroup, 0);

    // 各列按内容高度排列, 不被拉伸成大外框
    leftCol->addStretch(1);
    midCol->addStretch(1);
    rightCol->addStretch(1);

    cols->addLayout(leftCol, 1);
    cols->addLayout(midCol, 1);
    cols->addLayout(rightCol, 1);
    outer->addLayout(cols);

    // ── 确认/取消 ──
    auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(btns, &QDialogButtonBox::accepted, this, [this]() {
        if (m_nameEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, "提示", "请输入虚机名称");
            return;
        }

        // UEFI 固件路径校验: CODE 只读可共用, VARS 必须可写且每台 VM 独立
        if (m_uefiCb->isChecked()) {
            const QString code = m_uefiCodeEdit->text().trimmed();
            const QString vars = m_uefiVarsEdit->text().trimmed();
            if (code.isEmpty() || vars.isEmpty()) {
                QMessageBox::warning(this, "提示",
                    "启用 UEFI 时 CODE 与 VARS 路径都不能为空。\n"
                    "CODE 通常用 /usr/share/OVMF/x64/OVMF_CODE.4m.fd, VARS 用每台虚机自己的副本。");
                return;
            }
            if (!QFileInfo(code).exists()) {
                QMessageBox::warning(this, "提示", QString("CODE 文件不存在:\n%1").arg(code));
                return;
            }
            const QFileInfo vi(vars);
            if (vi.exists() && !vi.isWritable()) {
                QMessageBox::warning(this, "提示",
                    QString("VARS 文件不可写:\n%1\n\nVARS 必须可写 (建议复制一份到 "
                            "~/.config/ServerToolbox/ovmf/<虚机名>-vars.fd 后指向它)")
                        .arg(vars));
                return;
            }
            if (vi.absoluteFilePath() == "/usr/share/OVMF/x64/OVMF_VARS.4m.fd") {
                auto ret = QMessageBox::question(this, "提示",
                    "VARS 指向系统全局文件, 与其他虚机共享会互相污染启动项。\n仍要保存吗？",
                    QMessageBox::Yes | QMessageBox::No);
                if (ret != QMessageBox::Yes) return;
            }
        }

        // 无磁盘且无 ISO → 无法引导
        if (m_diskList->count() == 0 && m_isoEdit->text().trimmed().isEmpty()) {
            auto ret = QMessageBox::question(this, "提示",
                "未添加任何磁盘且未选择 ISO，虚机将无法引导。\n仍要保存吗？",
                QMessageBox::Yes | QMessageBox::No);
            if (ret != QMessageBox::Yes)
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

    // 虚拟机类型切换 → 显隐 VirtualBox 专属控件 / QEMU 高级控件
    connect(m_vmTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateVmTypeUI(); });

    // 网络模式切换 → 桥接网卡输入框显隐
    connect(m_vboxNicTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateVmTypeUI(); });

    // ── 无显示 ↔ VNC 互斥 ──
    connect(m_displayNoneCb, &QCheckBox::toggled, this, [this](bool on) {
        if (on) {
            QSignalBlocker b(m_vncSpin);
            m_vncSpin->setValue(-1);
            m_vncSpin->setEnabled(false);
            // 纯显卡直通场景: 顺带把 VGA 切到 none
            int noneIdx = m_vgaCombo->findData("none");
            if (noneIdx >= 0) m_vgaCombo->setCurrentIndex(noneIdx);
        } else {
            m_vncSpin->setEnabled(true);
        }
    });
    connect(m_vncSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
        if (v >= 0 && m_displayNoneCb->isChecked()) {
            QSignalBlocker b(m_displayNoneCb);
            m_displayNoneCb->setChecked(false);
            m_vncSpin->setEnabled(true);
        }
    });

    // ── USB 直通: 添加 / 移除 / 扫描 ──
    connect(m_usbAddBtn, &QPushButton::clicked, this, [this]() {
        QString data;
        if (editUsbDeviceDialog(this, data)) {
            auto *item = new QListWidgetItem(usbItemText(data));
            item->setData(Qt::UserRole, data);
            item->setToolTip(data);
            m_usbList->addItem(item);
        }
    });
    connect(m_usbRemoveBtn, &QPushButton::clicked, this, [this]() {
        int row = m_usbList->currentRow();
        if (row >= 0) delete m_usbList->takeItem(row);
    });
    connect(m_usbScanBtn, &QPushButton::clicked, this, [this]() {
        // 候选: lsusb 的 vid:pid + /dev/input/by-id 的 evdev 设备
        QListWidget *pick = new QListWidget();
        pick->setSelectionMode(QAbstractItemView::ExtendedSelection);
        for (const QString &d : scanUsbDevices()) {
            const QStringList p = d.split('|');
            auto *it = new QListWidgetItem(QString("🖳 %1  %2").arg(p.value(0), p.value(1)));
            it->setData(Qt::UserRole, QString("host|%1|%2").arg(p.value(0), p.value(1)));
            pick->addItem(it);
        }
        for (const QString &e : scanEvdevPaths()) {
            auto *it = new QListWidgetItem(QString("⌨ %1").arg(e));
            it->setData(Qt::UserRole, QString("evdev|%1").arg(e));
            pick->addItem(it);
        }
        if (pick->count() == 0) {
            delete pick;
            QMessageBox::information(this, "扫描", "未发现可透传的设备 (lsusb / /dev/input/by-id 为空)");
            return;
        }
        QDialog dlg(this);
        dlg.setWindowTitle("选择要透传的设备 (可多选)");
        dlg.resize(560, 340);
        auto *lay = new QVBoxLayout(&dlg);
        auto *hint = new QLabel("🖳 = 物理 USB 设备 (整体透传, 宿主机失去该设备)\n"
                                "⌨ = evdev 输入设备 (需 root 运行 QEMU):");
        hint->setStyleSheet("color: #888; font-size: 11px;");
        lay->addWidget(hint);
        lay->addWidget(pick);
        auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        lay->addWidget(btns);
        if (dlg.exec() == QDialog::Accepted) {
            for (auto *it : pick->selectedItems()) {
                const QString data = it->data(Qt::UserRole).toString();
                auto *newItem = new QListWidgetItem(usbItemText(data));
                newItem->setData(Qt::UserRole, data);
                newItem->setToolTip(data);
                m_usbList->addItem(newItem);
            }
            // 扫到物理 USB 设备时顺手勾上 xhci (usb-host 必须挂总线)
            bool hasHost = false;
            for (int i = 0; i < m_usbList->count(); ++i)
                if (m_usbList->item(i)->data(Qt::UserRole).toString().startsWith("host|"))
                    hasHost = true;
            if (hasHost) m_usbXhciCb->setChecked(true);
        }
        pick->setParent(nullptr);
        pick->deleteLater();
    });

    // ── Root 钩子: 载入 run-win10-pt.sh 的绑定 / 归还段 ──
    connect(m_rootLoadBtn, &QPushButton::clicked, this, [this]() {
        m_preRootEdit->setPlainText(R"HOOK(# 停合成器 (桌面黑屏) → 绑定 vfio-pci → 关闭该设备 runtime 挂起
ln -sf /dev/null /run/systemd/system/greetd.service 2>/dev/null
systemctl stop greetd 2>/dev/null
sleep 2
modprobe vfio-pci disable_idle_d3=1 2>/dev/null
p=/sys/module/vfio_pci/parameters/disable_idle_d3
[ -w "$p" ] && echo 1 > "$p" 2>/dev/null
for d in 0000:06:00.0 0000:06:00.1; do
    cur=$(basename "$(readlink /sys/bus/pci/devices/$d/driver 2>/dev/null)" 2>/dev/null)
    [ -n "$cur" ] && [ "$cur" != unknown ] && echo $d > /sys/bus/pci/drivers/$cur/unbind 2>/dev/null
    echo vfio-pci > /sys/bus/pci/devices/$d/driver_override
    echo $d > /sys/bus/pci/drivers/vfio-pci/bind 2>/dev/null
    echo on > /sys/bus/pci/devices/$d/power/control 2>/dev/null
done)HOOK");
        m_postRootEdit->setPlainText(R"HOOK(# 回绑 nvidia (GBM 后端需要) + 显卡音频回绑, 然后重启 greetd
for d in 0000:06:00.0 0000:06:00.1; do
    echo $d > /sys/bus/pci/drivers/vfio-pci/unbind 2>/dev/null
    echo '' > /sys/bus/pci/devices/$d/driver_override
done
modprobe nvidia_drm 2>/dev/null
echo nvidia > /sys/bus/pci/devices/0000:06:00.0/driver_override
echo 0000:06:00.0 > /sys/bus/pci/drivers/nvidia/bind 2>/dev/null
echo '' > /sys/bus/pci/devices/0000:06:00.0/driver_override
echo 0000:06:00.1 > /sys/bus/pci/drivers/snd_hda_intel/bind 2>/dev/null
rm -f /run/systemd/system/greetd.service 2>/dev/null
systemctl daemon-reload 2>/dev/null
systemctl start greetd 2>/dev/null)HOOK");
        QMessageBox::information(this, "已载入",
            "已填入显卡占用/归还脚本 (BDF 为 06:00.0 / 06:00.1)。\n"
            "换硬件时请自行改 BDF, 并确认 PCI 列表里的设备与之一致。");
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

    // 磁盘操作 (系统盘 + 数据盘 统一列表)
    connect(m_diskAddBtn, &QPushButton::clicked, this, [this]() {
        VMDataDisk dd;
        dd.system = (m_diskList->count() == 0);  // 第一块盘默认是系统盘
        if (editDiskDialog(this, dd, true, m_vmTypeCombo->currentData().toString()))
            m_diskList->addItem(makeDiskItem(dd));
    });
    connect(m_diskEditBtn, &QPushButton::clicked, this, [this]() {
        int row = m_diskList->currentRow();
        if (row < 0) return;
        VMDataDisk dd = diskFromItem(m_diskList->item(row));
        if (editDiskDialog(this, dd, false, m_vmTypeCombo->currentData().toString())) {
            delete m_diskList->takeItem(row);
            m_diskList->insertItem(row, makeDiskItem(dd));
            m_diskList->setCurrentRow(row);
        }
    });
    connect(m_diskRemoveBtn, &QPushButton::clicked, this, [this]() {
        int row = m_diskList->currentRow();
        if (row >= 0) delete m_diskList->takeItem(row);
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

    // 初始化显隐 (默认 QEMU → 隐藏 VirtualBox 设置组)
    updateVmTypeUI();
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
    m_isoEdit->setText(vm.iso);

    // 虚拟机类型 + VirtualBox 专属字段 (下拉一律用 findData 按 data 定位, 不能用 setCurrentText 传 ID)
    if (m_vmTypeCombo->findData(vm.vmType) >= 0)
        m_vmTypeCombo->setCurrentIndex(m_vmTypeCombo->findData(vm.vmType));
    if (m_ostypeCombo) {
        int oIdx = m_ostypeCombo->findData(vm.ostype);
        if (oIdx >= 0) m_ostypeCombo->setCurrentIndex(oIdx);
    }
    if (m_storageBackendCombo) {
        int sbIdx = m_storageBackendCombo->findData(vm.storageBackend);
        if (sbIdx >= 0) m_storageBackendCombo->setCurrentIndex(sbIdx);
    }
    if (m_vboxDiskSizeSpin)      m_vboxDiskSizeSpin->setValue(vm.vboxDiskSize);
    if (m_vboxAcpiCb)            m_vboxAcpiCb->setChecked(vm.vboxAcpi);
    if (m_vboxIoapicCb)          m_vboxIoapicCb->setChecked(vm.vboxIoapic);
    if (m_vboxUefiCb)            m_vboxUefiCb->setChecked(vm.vboxUefi);
    if (m_vbox3dCb)              m_vbox3dCb->setChecked(vm.vbox3d);
    if (m_vboxGpuCombo) {
        int gcIdx = m_vboxGpuCombo->findData(vm.graphicsController);
        if (gcIdx >= 0) m_vboxGpuCombo->setCurrentIndex(gcIdx);
        // 3D 加速时强制 VBoxSVGA
        m_vboxGpuCombo->setEnabled(!vm.vbox3d);
        if (vm.vbox3d)
            m_vboxGpuCombo->setCurrentIndex(m_vboxGpuCombo->findData("vmsvga"));
    }
    if (m_vboxVramSpin) {
        m_vboxVramSpin->setMaximum(vm.vbox3d ? 256 : 128);
        m_vboxVramSpin->setValue(qBound(64, vm.vboxVram, m_vboxVramSpin->maximum()));
    }
    if (m_vboxRdpCb)             m_vboxRdpCb->setChecked(vm.vboxRdp);
    if (m_vboxRdpPortSpin)       m_vboxRdpPortSpin->setValue(vm.vboxRdpPort);
    if (m_vboxNicTypeCombo) {
        int ntIdx = m_vboxNicTypeCombo->findData(vm.vboxNicType);
        if (ntIdx >= 0) m_vboxNicTypeCombo->setCurrentIndex(ntIdx);
    }
    if (m_vboxNicModelCombo) {
        int nmIdx = m_vboxNicModelCombo->findData(vm.vboxNicModel);
        if (nmIdx >= 0) m_vboxNicModelCombo->setCurrentIndex(nmIdx);
    }
    if (m_vboxBridgeEdit)        m_vboxBridgeEdit->setText(vm.vboxBridgeDev);

    // QEMU 固件 (UEFI)
    m_uefiCb->setChecked(vm.uefi);
    m_uefiCodeEdit->setText(vm.uefiCode);
    m_uefiVarsEdit->setText(vm.uefiVars);
    m_uefiCodeEdit->setEnabled(vm.uefi);
    m_uefiVarsEdit->setEnabled(vm.uefi);

    // 直通增强: multifunction + hole64
    m_vfioMultiCb->setChecked(vm.vfioMultifunction);
    if (vm.pciHole64.isEmpty()) {
        m_hole64Combo->setCurrentIndex(0);        // 「留空 (不设)」
    } else {
        int hIdx = m_hole64Combo->findText(vm.pciHole64);
        if (hIdx >= 0) m_hole64Combo->setCurrentIndex(hIdx);
        else m_hole64Combo->setCurrentText(vm.pciHole64);
    }

    // 显示: 无显示 + VNC 互斥 (先设 VNC, 再设勾选, 用 blocker 避免联动打架)
    {
        QSignalBlocker bv(m_vncSpin);
        m_vncSpin->setValue(vm.vnc);
        m_vncSpin->setEnabled(!(vm.displayNone && vm.vnc < 0));
        QSignalBlocker bd(m_displayNoneCb);
        m_displayNoneCb->setChecked(vm.displayNone);
    m_runRootCb->setChecked(vm.qemuRunRoot);
    }

    // 音频
    int abIdx = m_audioBackendCombo->findData(vm.audioBackend);
    if (abIdx >= 0) m_audioBackendCombo->setCurrentIndex(abIdx);
    int amIdx = m_audioModelCombo->findData(vm.audioModel);
    if (amIdx >= 0) m_audioModelCombo->setCurrentIndex(amIdx);

    // USB 直通
    m_usbXhciCb->setChecked(vm.usbXhci);
    m_usbKbdTabletCb->setChecked(vm.usbKbdTablet);
    m_usbList->clear();
    for (const auto &vp : vm.usbHostVendors) {
        const QString data = QString("host|%1|").arg(vp);
        auto *it = new QListWidgetItem(usbItemText(data));
        it->setData(Qt::UserRole, data);
        it->setToolTip(data);
        m_usbList->addItem(it);
    }
    for (const auto &ev : vm.usbEvdevPaths) {
        const QString data = QString("evdev|%1").arg(ev);
        auto *it = new QListWidgetItem(usbItemText(data));
        it->setData(Qt::UserRole, data);
        it->setToolTip(data);
        m_usbList->addItem(it);
    }

    // Root 钩子
    m_preRootEdit->setPlainText(vm.preStartRoot);
    m_postRootEdit->setPlainText(vm.postStopRoot);

    // 按当前类型刷新显隐
    updateVmTypeUI();

    // 磁盘列表 (系统盘 + 数据盘)
    m_diskList->clear();
    for (const auto &dd : vm.disks)
        m_diskList->addItem(makeDiskItem(dd));

    // 自动启动
    m_autoStartCb->setChecked(vm.autoStart);
    m_waitMountCb->setChecked(vm.waitMount);

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
    vm.vmType      = m_vmTypeCombo->currentData().toString();
    vm.ostype      = m_ostypeCombo->currentData().toString();
    vm.storageBackend = m_storageBackendCombo->currentData().toString();
    vm.vboxDiskSize = m_vboxDiskSizeSpin->value();
    vm.vboxAcpi     = m_vboxAcpiCb->isChecked();
    vm.vboxIoapic   = m_vboxIoapicCb->isChecked();
    vm.vboxUefi     = m_vboxUefiCb->isChecked();
    vm.vboxVram     = m_vboxVramSpin->value();
    vm.vbox3d       = m_vbox3dCb->isChecked();
    // 3D 加速时强制 VBoxSVGA (与界面锁定逻辑一致, 双保险)
    vm.graphicsController = vm.vbox3d ? "vmsvga"
                                       : m_vboxGpuCombo->currentData().toString();
    vm.vboxRdp      = m_vboxRdpCb->isChecked();
    vm.vboxRdpPort  = m_vboxRdpPortSpin->value();
    vm.vboxNicType  = m_vboxNicTypeCombo ? m_vboxNicTypeCombo->currentData().toString() : "nat";
    vm.vboxNicModel = m_vboxNicModelCombo->currentData().toString();
    vm.vboxBridgeDev = m_vboxBridgeEdit ? m_vboxBridgeEdit->text().trimmed() : "";

    // QEMU 固件 / 直通增强 / 显示 / 音频 / USB / Root 钩子
    vm.uefi      = m_uefiCb->isChecked();
    vm.uefiCode  = m_uefiCodeEdit->text().trimmed();
    vm.uefiVars  = m_uefiVarsEdit->text().trimmed();
    vm.vfioMultifunction = m_vfioMultiCb->isChecked();
    const QString h64 = m_hole64Combo->currentText().trimmed();
    vm.pciHole64 = (h64 == "留空 (不设)") ? QString() : h64;
    vm.displayNone = m_displayNoneCb->isChecked();
    vm.qemuRunRoot = m_runRootCb->isChecked();
    vm.audioBackend = m_audioBackendCombo->currentData().toString();
    vm.audioModel   = m_audioModelCombo->currentData().toString();
    vm.usbXhci      = m_usbXhciCb->isChecked();
    vm.usbKbdTablet = m_usbKbdTabletCb->isChecked();
    vm.usbHostVendors.clear();
    vm.usbEvdevPaths.clear();
    for (int i = 0; i < m_usbList->count(); ++i) {
        const QStringList p = m_usbList->item(i)->data(Qt::UserRole).toString().split('|');
        if (p.value(0) == "host" && !p.value(1).isEmpty()) vm.usbHostVendors << p.value(1);
        else if (p.value(0) == "evdev" && !p.value(1).isEmpty()) vm.usbEvdevPaths << p.value(1);
    }
    vm.preStartRoot  = m_preRootEdit->toPlainText().trimmed();
    vm.postStopRoot  = m_postRootEdit->toPlainText().trimmed();
    vm.autoStart  = m_autoStartCb->isChecked();
    vm.waitMount  = m_waitMountCb->isChecked();

    // 端口转发
    vm.portForwards.clear();
    for (int i = 0; i < m_portTable->rowCount(); ++i) {
        PortFwd pf;
        pf.hostPort  = m_portTable->item(i, 0)->text().toInt();
        pf.guestPort = m_portTable->item(i, 1)->text().toInt();
        pf.protocol  = m_portTable->item(i, 2)->text().toLower();
        vm.portForwards.append(pf);
    }

    // 磁盘列表 (系统盘 + 数据盘)
    vm.disks.clear();
    for (int i = 0; i < m_diskList->count(); ++i)
        vm.disks.append(diskFromItem(m_diskList->item(i)));
    vm.normalizeDisks();  // 系统盘唯一且置于首位 (index=0)

    // 硬件直通
    vm.hugepages = m_hugepagesCb->isChecked();
    for (int i = 0; i < m_pciList->count(); ++i)
        vm.pciDevices.append(m_pciList->item(i)->text());

    vm.extra = m_extraEdit->toPlainText().trimmed();

    return vm;
}
