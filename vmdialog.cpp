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
    ifaceCombo->setEnabled(dd.system);
    ifaceCombo->setToolTip(
        "仅系统盘生效 (数据盘固定 virtio), VirtualBox 忽略此项.\n"
        "IDE: 原生 IDE 控制器, Win98/2000/XP/7 免驱动即认盘(推荐旧 OS)\n"
        "SATA: AHCI 控制器, Win7 SP1+ 原生支持\n"
        "virtio: 性能最高, 但需装入对应驱动 (Win10/11 自带, 老系统需手动装)");
    form->addRow("系统盘接口:", ifaceCombo);
    QObject::connect(sysCb, &QCheckBox::toggled, &dlg, [ifaceCombo](bool on) {
        ifaceCombo->setEnabled(on);
    });

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
        : QString("%1  [%2 | cache:%3 | aio:%4]").arg(name, dd.format, dd.cache, dd.aio);

    auto *item = new QListWidgetItem(text);
    item->setData(Qt::UserRole,
        QString("%1|%2|%3|%4|%5|%6").arg(dd.path, dd.format, dd.cache, dd.aio, dd.iface)
                                    .arg(dd.system ? "1" : "0"));
    QString tip = QString("路径:%1\n格式:%2\n缓存:%3\nAIO:%4").arg(dd.path, dd.format, dd.cache, dd.aio);
    if (dd.system) tip += QString("\n系统盘 (启动盘), 接口:%1").arg(dd.iface);
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
//  updateVmTypeUI — 按虚拟机类型显隐对应控件
// ═══════════════════════════════════════════════════════════════════
void VMDialog::updateVmTypeUI()
{
    const bool vbox = (m_vmTypeCombo->currentData() == "virtualbox");

    // VirtualBox 专属设置组
    m_vboxGroup->setVisible(vbox);

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
    m_advGroup->setVisible(!vbox);

    // 额外参数组是 QEMU 专属，VirtualBox 时整组隐藏（窗口更短）
    m_extraGroup->setVisible(!vbox);

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
    resize(820, 680);

    auto *outer = new QVBoxLayout(this);

    // ── 双列主体 ──
    auto *cols = new QHBoxLayout();

    // ════════ 左列 ════════
    auto *leftCol = new QVBoxLayout();

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
    // 注意: 加到右列 (见下方 rightCol), 不占左列高度

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
    leftCol->addWidget(m_vboxGroup);

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
    m_extraGroup = new QGroupBox("额外 QEMU 参数");
    auto *extraLayout = new QVBoxLayout(m_extraGroup);
    m_extraEdit = new QPlainTextEdit();
    m_extraEdit->setPlaceholderText("以上控件未覆盖的参数写在这里");
    m_extraEdit->setMaximumHeight(70);
    extraLayout->addWidget(m_extraEdit);
    leftCol->addWidget(m_extraGroup);

    cols->addLayout(leftCol, 1);

    // ════════ 右列 ════════
    auto *rightCol = new QVBoxLayout();

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

    rightCol->addWidget(autoGroup);

    // 网络设置放到右列 (借用端口转发组缩短后腾出的空间)
    rightCol->addWidget(m_netGroup);

    rightCol->addWidget(m_advGroup);

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
    rightCol->addWidget(m_portGroup);
    rightCol->addStretch(1);   // 让各组按内容高度排列, 不被拉伸成大外框

    cols->addLayout(rightCol, 1);
    outer->addLayout(cols);

    // ── 确认/取消 ──
    auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(btns, &QDialogButtonBox::accepted, this, [this]() {
        if (m_nameEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, "提示", "请输入虚机名称");
            return;
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
    vm.vboxRdp      = m_vboxRdpCb->isChecked();
    vm.vboxRdpPort  = m_vboxRdpPortSpin->value();
    vm.vboxNicType  = m_vboxNicTypeCombo ? m_vboxNicTypeCombo->currentData().toString() : "nat";
    vm.vboxNicModel = m_vboxNicModelCombo->currentData().toString();
    vm.vboxBridgeDev = m_vboxBridgeEdit ? m_vboxBridgeEdit->text().trimmed() : "";
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
