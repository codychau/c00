#include "naspage.h"
#include "theme.h"

#include <QProcess>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QMessageBox>
#include <QInputDialog>
#include <QTextStream>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QHostAddress>
#include <QRegularExpression>
#include <QList>
#include <QButtonGroup>
#include <QRadioButton>
#include <algorithm>

static constexpr int kTopbarInset = 52;   // 与 MainWindow 顶栏一致，正文自行让出

// ── 小工具 ──────────────────────────────────────────────────
static QString runCapture(const QString &prog, const QStringList &args)
{
    QProcess p;
    p.start(prog, args);
    if (!p.waitForFinished(4000)) { p.kill(); p.waitForFinished(500); return QString(); }
    return QString::fromUtf8(p.readAllStandardOutput());
}

// 从 lsblk 拿分区列表（含 SIZE / AVAIL 需要的信息）
static QByteArray captureLsblk()
{
    QProcess p;
    // -b 强制 SIZE 用字节：不加的话会拿到 "1.8T" 这种字符串，
    // nasmodel 的 toLongLong 会截成 1，容量判断全错。
    p.start("lsblk", {"-J", "-b", "-o",
                      "NAME,TYPE,FSTYPE,MOUNTPOINT,SIZE,LABEL,UUID"});
    if (!p.waitForFinished(6000)) { p.kill(); p.waitForFinished(500); return QByteArray(); }
    return p.readAllStandardOutput();
}

NasPage::NasPage(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, kTopbarInset, 10, 8);
    layout->setSpacing(8);

    // ── 第一行：引擎选择 + 环境提示 ──
    auto *topRow = new QHBoxLayout();
    topRow->addWidget(new QLabel("实现"));
    m_engine = new QComboBox();
    m_engine->addItem("mdadm（内核 RAID，稳）");
    m_engine->addItem("nasrs RAID0（条带 over 文件，不占分区）");
    connect(m_engine, &QComboBox::currentIndexChanged, this, &NasPage::onEngineChanged);
    topRow->addWidget(m_engine);
    topRow->addStretch(1);
    m_envLabel = new QLabel("检测环境…");
    m_envLabel->setProperty("role", "mute");
    topRow->addWidget(m_envLabel);
    // topRow 固定高度，不参与竖直拉伸。
    // 否则 QComboBox 的 expanding 竖直策略会让这一行(嵌套 HBox)在
    // VBox 里抢走多余空间 —— 实测它 sizeHint 高 32 却被撑到 283，
    // 把画板整块往下推 300px，看起来就像机箱上方一大片空白。
    layout->addLayout(topRow);

    // ── 中间：画板 ──
    m_board = new NasBoardWidget();
    // 不给 stretch：机箱自己定高度，多余竖直空间留给参数区。
    // 之前给 stretch=1，画板吃掉整页剩余高度，
    // 于是 sizeHint 改小也白改，机箱被拉回高个子。
    layout->addWidget(m_board);
    // 面板只报"用户点了哪个盘位"，插入/拔出的语义在这里决定
    connect(m_board, &NasBoardWidget::slotClicked, this, &NasPage::onSlotClicked);
    connect(m_board, &NasBoardWidget::slotRejected, this, &NasPage::onSlotRejected);

    // ── 参数区 ──
    auto *cfg = new QGroupBox("导出参数");
    // 参数框按内容高度，不吸收页面多余空间。否则它会吃掉画板下方的
    // 全部富余高度，把画板顶到中间，看起来就是机箱上方一大片空白。
    cfg->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *outer = new QVBoxLayout(cfg);

    // ── 导出方式：单分区可在 SMB / iSCSI 间选，多盘只能 iSCSI ──
    auto *modeRow = new QHBoxLayout();
    m_modeIscsi = new QRadioButton("iSCSI 块存储");
    m_modeSmb   = new QRadioButton("SMB 文件共享");
    m_modeIscsi->setToolTip("导出为 iSCSI LUN，发起端拿到的是裸块设备，自己格式化。");
    m_modeSmb->setToolTip("用 Samba 把该分区的挂载点作为共享目录导出。只读文件，不碰分区数据。");
    m_modeIscsi->setChecked(true);       // 默认 iSCSI：与既有行为一致，不擅自改默认
    m_modeGroup = new QButtonGroup(this);
    m_modeGroup->addButton(m_modeIscsi);
    m_modeGroup->addButton(m_modeSmb);
    modeRow->addWidget(m_modeIscsi);
    modeRow->addWidget(m_modeSmb);
    m_modeHint = new QLabel();
    m_modeHint->setProperty("role", "mute");
    modeRow->addWidget(m_modeHint, 1);
    outer->addLayout(modeRow);

    // iSCSI 那套行整体装进一个容器，好和 SMB 那套互斥显隐。
    m_iscsiBox = new QWidget();
    auto *form = new QFormLayout(m_iscsiBox);
    form->setContentsMargins(0, 0, 0, 0);
    m_volName = new QLineEdit();
    m_volName->setPlaceholderText("nas-data（创建时要手输核对）");
    form->addRow("名称", m_volName);

    m_chunk = new QSpinBox();
    m_chunk->setRange(4, 16384);
    m_chunk->setValue(512);
    m_chunk->setSuffix(" KiB");
    m_chunk->setToolTip("条带粒度。大文件顺序读：越大越好；小块随机写：越小越好。");
    form->addRow("条带粒度", m_chunk);

    m_iqn = new QLineEdit(Nas::makeIqn("local", "26-11", "01"));
    m_iqn->setToolTip("iqn.<YY-MM>.<反序域名>:<名字>。域名乱填可能与别的 target 撞名。");
    form->addRow("iSCSI IQN", m_iqn);

    // 这一栏**不是**"填了就生效"的开关：LIO 没有 portal 级网段白名单
    // （已查证 rtslib_fb 无此概念）。它只用来算出该把 portal 绑到哪个本机
    // 地址，真正的网段限制必须落到 nftables，见下方 m_portalNote。
    m_portal = new QLineEdit();
    m_portal->setPlaceholderText("本机内网地址，如 192.168.1.10");
    m_portal->setToolTip("portal 绑定地址。留空会让 target 监听 0.0.0.0，等于对全网开放 3260。");
    form->addRow("监听地址", m_portal);

    m_portalNote = new QLabel("网段限制不由这里实现：LIO 只认 initiator IQN。"
                              "要限网段需另配 nftables（页面会给出对应命令）。");
    m_portalNote->setWordWrap(true);
    m_portalNote->setProperty("role", "mute");
    form->addRow("", m_portalNote);

    // 默认**关闭**匿名。之前默认开是错的：demo_mode_write_protect=0 会让
    // 任何能连上 3260 的机器直接读写整个阵列。
    m_anonAcl = new QCheckBox("允许未登记 IQN 的发起端（仅调试，任何能连上端口的主机都能读写）");
    m_anonAcl->setChecked(false);
    form->addRow(QString(), m_anonAcl);
    outer->addWidget(m_iscsiBox);

    // ── SMB 参数（默认隐藏，恰好 1 个分区时才显示）──
    m_smbBox = new QWidget();
    auto *sform = new QFormLayout(m_smbBox);
    sform->setContentsMargins(0, 0, 0, 0);

    m_smbName = new QLineEdit();
    m_smbName->setPlaceholderText("nas-share");
    m_smbName->setToolTip("\\\\主机\<共享名>。不能含 \\ / * ? \" < > | :，不能叫 IPC$ 或 HOMES。");
    sform->addRow("共享名", m_smbName);

    m_smbPath = new QLineEdit();
    m_smbPath->setPlaceholderText("选分区后自动填它的挂载点");
    m_smbPath->setToolTip("被共享的目录。只能绝对路径；命中系统/家目录挂载点会红字警告。");
    sform->addRow("共享路径", m_smbPath);

    m_smbAuth = new QComboBox();
    m_smbAuth->addItem("需账号密码（推荐）", int(Nas::SmbAuth::Password));
    m_smbAuth->addItem("匿名只读", int(Nas::SmbAuth::GuestRO));
    m_smbAuth->addItem("匿名可写（危险）", int(Nas::SmbAuth::GuestRW));
    m_smbAuth->setCurrentIndex(0);
    sform->addRow("访问方式", m_smbAuth);

    m_smbUser = new QLineEdit(Nas::currentUserName());
    m_smbUser->setToolTip("Samba 以这个 Linux 用户的权限读写文件。");
    sform->addRow("映射用户", m_smbUser);

    m_smbWrite = new QCheckBox("允许写入（只读共享请取消）");
    m_smbWrite->setChecked(true);
    sform->addRow("可写", m_smbWrite);
    outer->addWidget(m_smbBox);

    m_smbWarn = new QLabel();
    m_smbWarn->setWordWrap(true);
    m_smbWarn->setTextFormat(Qt::RichText);
    outer->addWidget(m_smbWarn);

    m_smbBox->setVisible(false);   // 初始 0 个成员，只显示 iSCSI
    layout->addWidget(cfg);

    // ── 按钮行 ──
    auto *btnRow = new QHBoxLayout();
    m_status = new QLabel("就绪");
    m_status->setProperty("role", "mute");
    btnRow->addWidget(m_status, 1);

    m_createBtn = new QPushButton("① 创建阵列");
    m_createBtn->setToolTip("会格式化成员分区上的数据，需要逐条确认");
    // 模式互斥切换：只刷新显隐与警告，不重算成员
    connect(m_modeGroup, &QButtonGroup::idClicked, this, [this](int){
        syncExportMode(m_board->members().size());
    });
    connect(m_smbAuth, &QComboBox::currentIndexChanged, this, &NasPage::refreshSmbWarning);
    connect(m_smbPath, &QLineEdit::textChanged, this, &NasPage::refreshSmbWarning);
    connect(m_smbName, &QLineEdit::textChanged, this, &NasPage::refreshSmbWarning);
    connect(m_smbUser, &QLineEdit::textChanged, this, &NasPage::refreshSmbWarning);
    connect(m_smbWrite, &QCheckBox::toggled, this, &NasPage::refreshSmbWarning);
    connect(m_createBtn, &QPushButton::clicked, this, &NasPage::createArray);
    btnRow->addWidget(m_createBtn);

    m_exportBtn = new QPushButton("② 导出 iSCSI");
    m_exportBtn->setEnabled(false);
    connect(m_exportBtn, &QPushButton::clicked, this, &NasPage::onExportClicked);
    btnRow->addWidget(m_exportBtn);

    m_teardownBtn = new QPushButton("撤销导出");
    m_teardownBtn->setEnabled(false);
    connect(m_teardownBtn, &QPushButton::clicked, this, &NasPage::teardownIscsi);
    btnRow->addWidget(m_teardownBtn);

    m_refreshBtn = new QPushButton("刷新");
    connect(m_refreshBtn, &QPushButton::clicked, this, &NasPage::refresh);
    btnRow->addWidget(m_refreshBtn);
    layout->addLayout(btnRow);

    // 页面剩余高度全部沉到底部。之前没有这一句，VBox 会把富余高度
    // 按 stretch 摊给各个 stretch=0 项（实测参数框被撑到 283），
    // 于是画板被推到 y=343，机箱上方出现约 200px 空白。
    layout->addStretch(1);

    // 名称只有非空才允许创建，所以每次按键都要重算
    connect(m_volName, &QLineEdit::textChanged, this, [this](const QString &){ updateSummary(); });

    connect(&m_ctl, &NasController::stepFinished, this, &NasPage::showStepLog);
    connect(&m_ctl, &NasController::allFinished, this, &NasPage::onAllFinished);

    onEngineChanged(m_engine->currentIndex());
    refresh();
}

void NasPage::onEngineChanged(int index)
{
    const Nas::Engine e = (index == 1) ? Nas::Engine::Raid0Rs : Nas::Engine::Mdadm;
    m_board->setEngine(e);
    // 换引擎 = 换"能不能用"的判据，必须重新判定，
    // 否则界面上还停留在上一个引擎的可选性
    refresh();
    // 两条路线的危险程度不同，这里只切视图：
    //  mdadm  会抹掉所选分区的文件系统（mdadm --create 写 superblock）
    //  nasrs  只在分区已有的文件系统里放一个大文件，不碰分区本身
    // 卡口数量两者目前都按 RAID0 给（至少 2 个才有意义）。
    updateSummary();
}

void NasPage::refresh()
{
    m_env = m_ctl.probe();
    QStringList lack;
    if (!m_env.mdadm)         lack << "mdadm";
    if (!m_env.targetCli)     lack << "targetcli";
    if (!m_env.configfsMounted) lack << "configfs 未挂载";
    if (!m_env.kernelModules) lack << "内核 target 模块";
    if (!m_env.nasrsBinary && m_engine->currentIndex() == 1) lack << "nasrs 引擎";

    if (lack.isEmpty()) {
        m_envLabel->setText("环境就绪");
        m_envLabel->setStyleSheet(QString());
    } else {
        m_envLabel->setText("缺少：" + lack.join("、"));
        m_envLabel->setStyleSheet("color:" + Theme::bad().name());
    }

    // 分区列表
    const QByteArray js = captureLsblk();
    const QVector<Nas::Block> tree = Nas::parseLsblk(js);
    const QVector<Nas::Block> all = Nas::flatten(tree);

    QFile mnt("/proc/mounts");
    QString mounts;
    if (mnt.open(QIODevice::ReadOnly | QIODevice::Text))
        mounts = QString::fromUtf8(mnt.readAll());
    const QVector<QString> sysDisks = Nas::systemDiskNames(mounts);

    m_disks.clear();
    // 面板位子是有限的、要像机箱盘位，所以不能把 lsblk 的每一项都摆上去。
    // 整盘若已经有分区被列出，就**不再单独占一个盘位** ——
    // 之前 sda 和 sda1 各占一格，看着像两块盘，且整盘那格永远是灰的，
    // 用户会以为是坏盘。整盘的判决仍在 tooltip 里，只是不占位。
    QSet<QString> parentWithPart;
    for (const Nas::Block &b : all)
        if (b.type == "part") parentWithPart.insert(Nas::parentDiskOf(b.name));

    for (const Nas::Block &b : all) {
        const Nas::Engine eng = (m_engine->currentIndex() == 1)
                                  ? Nas::Engine::Raid0Rs : Nas::Engine::Mdadm;
        const Nas::Verdict v = Nas::judge(b, sysDisks, eng);
        if (b.type != "part" && b.type != "disk") continue;   // loop/rom 由 judge 拒
        if (b.type == "disk" && parentWithPart.contains(b.name)) continue;

        NasBoardWidget::Disk d;
        d.device = b.path;
        d.label = b.name;
        d.fstype = b.fstype;
        d.mountpoint = b.mountpoint;
        d.bytes = b.size;
        d.selectable = v.selectable;
        d.blockReason = v.reason;
        // 系统盘与真故障要分开画：一律画红斜纹会让整机看起来像坏了
        d.systemDisk = (v.risk == Nas::Risk::FileSystemRoot);
        d.faulty     = (v.risk == Nas::Risk::ExistingRaid
                        || v.risk == Nas::Risk::Crypto);

        // 可用容量：挂载中的分区用 QStorageInfo，没挂载的只能退回总容量
        if (!b.mountpoint.isEmpty()) {
            QStorageInfo si(b.mountpoint);
            if (si.isValid() && si.bytesAvailable() > 0)
                d.freeBytes = si.bytesAvailable();
        }
        m_disks.append(d);
    }
    m_sysMounts = Nas::systemMountPoints(all, mounts);
    m_board->setDisks(m_disks);
    updateSummary();
    syncExportMode(m_board->members().size());

    setStatus(QString("已枚举 %1 个设备，其中可选 %2 个")
                  .arg(m_disks.size())
                  .arg(std::count_if(m_disks.cbegin(), m_disks.cend(),
                                     [](const NasBoardWidget::Disk &x) { return x.selectable; })));
}

void NasPage::onSlotClicked(const QString &device, int slotIndex)
{
    Q_UNUSED(slotIndex)
    QVector<QString> m = m_board->members();
    const int i = m.indexOf(device);
    if (i >= 0) {
        // 再点一次 = 拔出。成员顺序就是条带顺序，删中间一个会让后面的
        // 序号整体前移，所以这里必须整体重发而不是就地清空。
        m.removeAt(i);
        m_board->setMembers(m);
        updateSummary();
        syncExportMode(m.size());
        setStatus(QString("%1 已移出阵列（其余盘条带序号已前移）").arg(device));
        return;
    }

    // 上限：mdadm 走内核，理论很宽；这里按常见控制器给 8，
    // 超过 8 路的 RAID0 任一盘故障即整卷失效，收益早就不对了。
    if (m.size() >= 8) {
        setStatus("RAID0 最多 8 个成员；盘再多也不建议继续加", true);
        return;
    }

    m.append(device);
    m_board->setMembers(m);
    updateSummary();
    syncExportMode(m.size());
    setStatus(QString("%1 已加入阵列，条带序号 %2").arg(device).arg(m.size() - 1));
}

void NasPage::setMemberDevicesForTest(const QVector<QString> &devices)
{
    m_board->setMembers(devices);
    updateSummary();
    syncExportMode(devices.size());
}
void NasPage::onSlotRejected(const QString &device, int slotIndex, const QString &why)
{
    Q_UNUSED(slotIndex)
    setStatus(QString("%1 不可用：%2").arg(device, why), true);
}

Nas::Plan NasPage::currentPlan() const
{
    Nas::Plan plan;
    plan.engine = (m_engine->currentIndex() == 1) ? Nas::Engine::Raid0Rs : Nas::Engine::Mdadm;
    plan.stripeChunkKiB = m_chunk->value();
    plan.volumeName = m_volName->text().trimmed();

    // members() 是有序的：下标即条带序号，绝不能在这里排序或去重
    const QVector<QString> chosen = m_board->members();
    for (const QString &dev : chosen) {
        if (dev.isEmpty()) continue;
        Nas::Member m;
        m.device = dev;
        for (const NasBoardWidget::Disk &d : m_disks) {
            if (d.device != dev) continue;
            m.capacity = d.freeBytes > 0 ? d.freeBytes : d.bytes;
            break;
        }
        if (plan.engine == Nas::Engine::Raid0Rs) {
            m.kind = Nas::MemberKind::BackingFile;
            // backing 文件放在该分区当前的挂载点上；没挂载就没有落脚处，
            // 这也是"nasrs 不占分区"的代价：必须有个可用的文件系统。
            for (const NasBoardWidget::Disk &d : m_disks)
                if (d.device == dev) m.filePath = d.mountpoint + "/" + plan.volumeName + ".nas.img";
        } else {
            m.kind = Nas::MemberKind::Partition;
        }
        plan.members.append(m);
    }
    return plan;
}

// 按成员数刷新导出区。核心是那条跳变判断：
// 只有「上一帧 >1 且这一帧 ==1」才自动切到 SMB。
// 写成「当前 ==1 就切」是错的 —— 那样连改引擎下拉框（成员数不变）
// 都会把用户显式选好的 iSCSI 翻成 SMB，等于跟用户抢方向盘。
void NasPage::syncExportMode(int memberCount)
{
    const bool droppedToOne = (memberCount == 1 && m_lastMemberCount > 1);
    const bool smbAllowed = (memberCount == 1);

    m_modeSmb->setEnabled(smbAllowed);
    if (!smbAllowed && m_modeSmb->isChecked())
        m_modeIscsi->setChecked(true);      // 成员数变了，SMB 不再合法 -> 回 iSCSI

    if (smbAllowed) {
        if (droppedToOne && !m_modeSmb->isChecked()) {
            m_modeSmb->setChecked(true);
            setStatus("只剩 1 个分区，已切到 SMB 文件共享（可手动切回 iSCSI）");
        }
        m_modeHint->setText("单个分区：可走 SMB（文件级，最常用）或 iSCSI（块级）");
    } else if (memberCount == 0) {
        m_modeHint->setText("先在上方点选分区。选 1 个可切 SMB，选 ≥2 个做 iSCSI RAID0");
    } else {
        m_modeHint->setText(QString("已选 %1 个分区：多盘只能做 iSCSI RAID（SMB 不跨盘条带）")
                                .arg(memberCount));
    }

    const bool smb = m_modeSmb->isChecked();
    m_iscsiBox->setVisible(!smb);
    m_smbBox->setVisible(smb);
    m_smbWarn->setVisible(smb);
    m_exportBtn->setText(smb ? "② 导出 SMB 共享" : "② 导出 iSCSI");

    // SMB 表单预填：只在没被用户改过时填默认值，别覆盖用户输入
    if (smb) {
        const QString dev = m_board->members().value(0);
        for (const NasBoardWidget::Disk &d : m_disks) {
            if (d.device != dev) continue;
            if (m_smbPath->text().isEmpty())
                m_smbPath->setText(d.mountpoint);
            if (m_smbName->text().isEmpty())
                m_smbName->setText(Nas::suggestShareName(d.mountpoint, dev));
            break;
        }
    }
    refreshSmbWarning();
    m_lastMemberCount = memberCount;
}

// SMB 的红字警告。刻意「不过度报警」：正常配置只说清授予了什么权限，
// 不做每敲一个字都变红那种，否则用户会无视所有警告。
void NasPage::refreshSmbWarning()
{
    if (!m_smbBox || !m_smbBox->isVisible()) { m_smbWarn->clear(); return; }

    const int authIdx = m_smbAuth->currentData().toInt();
    const Nas::SmbAuth auth = Nas::SmbAuth(authIdx);
    const QString path = m_smbPath->text();
    // 用 refresh() 时算好的那一份。这里不重算：
    // 本函数由每次按键触发，重读挂载表既浪费，也可能与
    // 面板的可用性判定给出不同答案。
    const QStringList &sysMounts = m_sysMounts;

    QStringList msgs;
    if (auth == Nas::SmbAuth::GuestRW)
        msgs << "<b>匿名可写</b>：局域网内任何设备无需密码即可写入、删除这里的文件。";
    else if (auth == Nas::SmbAuth::GuestRO)
        msgs << "匿名只读：局域网内任何设备都能浏览并下载，无需密码。";
    else if (m_smbWrite->isChecked())
        msgs << QString("可写共享：拿到账号密码的设备能写入。映射用户 <b>%1</b> "
                        "需已用 smbpasswd -a 设过 Samba 密码。").arg(m_smbUser->text());

    if (Nas::isRiskySharePath(path, sysMounts))
        msgs << QString("路径 <b>%1</b> 命中系统或家目录挂载点，会暴露不该暴露的内容。")
                    .arg(path.isEmpty() ? "(空)" : path);

    const QString nameErr = Nas::validateShareName(m_smbName->text());
    if (!nameErr.isEmpty()) msgs << QString("共享名：%1").arg(nameErr);
    const QString pathErr = Nas::validateSharePath(path);
    if (!pathErr.isEmpty()) msgs << QString("路径：%1").arg(pathErr);

    if (msgs.isEmpty()) { m_smbWarn->clear(); return; }
    m_smbWarn->setText("⚠ " + msgs.join("<br>⚠ "));
    m_smbWarn->setProperty("role", "bad");
}

void NasPage::updateSummary()
{
    const Nas::Plan plan = currentPlan();
    const Nas::ArrayInfo info = Nas::evaluateArray(plan);
    // 容量与告警由画板底部的汇总条显示（drawSummary 里同样调 evaluateArray），
    // 这里不再放第二个 QLabel —— 曾经两个都显示 info.note，同样的话出现两遍。
    m_createBtn->setEnabled(info.members >= 1 && !plan.volumeName.isEmpty());
}

QVector<NasController::Step>
NasPage::confirmSteps(const QVector<NasController::Step> &steps, const QString &title)
{
    if (steps.isEmpty()) return steps;

    // 把每条命令摊开来给用户看。破坏性的排在前面也照实写，
    // 不藏进"详细信息"里 —— 看过 argv 再点确认才算真确认。
    QString text = QString("%1\n\n将依次执行 %2 条命令：\n").arg(title).arg(steps.size());
    int i = 1;
    for (const NasController::Step &s : steps) {
        text += QString("\n%1) %2\n   %3 %4\n")
                    .arg(i++).arg(s.describe, s.program, s.args.join(" "));
    }

    bool hasDestructive = false;
    for (const NasController::Step &s : steps)
        if (s.destructive) hasDestructive = true;

    if (hasDestructive) {
        text += "\n\n其中含**破坏性**操作，可能不可逆。";
        const auto ret = QMessageBox::warning(this, "确认执行", text,
                                              QMessageBox::Yes | QMessageBox::No,
                                              QMessageBox::No);
        if (ret != QMessageBox::Yes) return {};

        // 第二道闸：手输卷名。RAID 建错盘是救不回来的，
        // 让"手滑回车"这一类失误在这里断掉。
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, "再确认一次",
            QString("输入卷名 %1 以确认：").arg(m_pendingConfirmName),
            QLineEdit::Normal, QString(), &ok).trimmed();
        if (!ok || name != m_pendingConfirmName) {
            setStatus("已取消（名称未核对）", true);
            return {};
        }
    } else {
        const auto ret = QMessageBox::question(this, "确认执行", text,
                                               QMessageBox::Yes | QMessageBox::No,
                                               QMessageBox::No);
        if (ret != QMessageBox::Yes) return {};
    }
    return steps;
}

void NasPage::createArray()
{
    const Nas::Plan plan = currentPlan();
    if (plan.members.isEmpty()) { setStatus("先拖线加入至少一个成员", true); return; }
    if (plan.volumeName.isEmpty()) { setStatus("先填名称", true); return; }
    if (!m_env.readyForMdadm()) { setStatus("环境未就绪，见右上角提示", true); return; }

    m_pendingConfirmName = plan.volumeName;

    QVector<NasController::Step> steps;
    if (plan.engine == Nas::Engine::Mdadm)
        steps = m_ctl.planMdadmCreate(plan, plan.volumeName);
    else
        steps = m_ctl.planRaid0RsCreate(plan, plan.volumeName,
                                        Nas::suggestBackingSize({}, 0.95));

    const QVector<NasController::Step> go = confirmSteps(steps, "创建阵列");
    if (go.isEmpty()) return;
    m_ctl.run(go);
}

// 导出按钮一个就够：按当前模式分派，避免并排两个按钮让用户猜哪个有效。
void NasPage::onExportClicked()
{
    if (m_modeSmb->isChecked()) exportSmb();
    else                        exportIscsi();
}

// 导出 SMB 共享。这条路径**不会**碰分区上的数据，风险全在"暴露范围"，
// 所以护栏集中在：环境是否齐备、共享名/路径是否合法、是否命中敏感路径、
// 是否匿名可写。密码永远不在这条链里改（不含 smbpasswd）。
void NasPage::exportSmb()
{
    if (!m_env.readyForSmb()) {
        QStringList need;
        if (!m_env.smbd)       need << "smbd（先装 Samba：apt install samba）";
        if (!m_env.testparm)   need << "testparm（smbclient 包里，用来校验配置）";
        if (!m_env.smbService) need << "smbd.service（systemd 服务单元）";
        setStatus("SMB 环境未就绪，缺少：" + need.join("；"), true);
        return;
    }

    const QString name = m_smbName->text().trimmed();
    const QString path = m_smbPath->text().trimmed();

    const QString nameErr = Nas::validateShareName(name);
    if (!nameErr.isEmpty()) { setStatus("共享名：" + nameErr, true); return; }
    const QString pathErr = Nas::validateSharePath(path);
    if (!pathErr.isEmpty()) { setStatus("路径：" + pathErr, true); return; }

    // 路径必须真的存在且是目录，否则共享出去是个死链接
    QFileInfo fi(path);
    if (!fi.exists())          { setStatus(QString("%1 不存在").arg(path), true); return; }
    if (!fi.isDir())           { setStatus(QString("%1 不是目录").arg(path), true); return; }

    const Nas::SmbAuth auth = Nas::SmbAuth(m_smbAuth->currentData().toInt());
    const QString user = m_smbUser->text().trimmed();
    // 需密码模式必须有映射用户：Samba 无法把匿名请求映射到"没有的用户"
    if (auth == Nas::SmbAuth::Password && user.isEmpty()) {
        setStatus("需密码模式必须指定映射用户", true);
        return;
    }
    if (user.isEmpty() || user == "root") {
        setStatus("映射用户不能为空或 root：那会让访客以 root 权限读写文件", true);
        return;
    }

    // 敏感路径 / 匿名可写：单独一道强确认，不能只靠那条常驻警告。
    const bool risky = Nas::isRiskySharePath(path, m_sysMounts);
    const bool guestRW = (auth == Nas::SmbAuth::GuestRW);
    if (risky || guestRW) {
        QString why;
        if (risky)  why += QString("路径 %1 命中系统或家目录挂载点。\n").arg(path);
        if (guestRW) why += "共享为匿名可写：局域网内任何设备无需密码即可写入和删除。\n";
        const auto r = QMessageBox::warning(this, "高风险共享",
            why + "\n\n确认要把这个目录以这种方式开放到局域网吗？",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (r != QMessageBox::Yes) { setStatus("已取消导出", true); return; }
    }

    Nas::SmbShare sh;
    sh.shareName = name;
    sh.path      = path;
    sh.user      = user.isEmpty() ? QStringLiteral("nobody") : user;
    sh.auth      = auth;
    sh.browseable = true;
    sh.writable  = (auth != Nas::SmbAuth::GuestRO) && m_smbWrite->isChecked();

    const QVector<NasController::Step> steps =
        m_ctl.planSmbExport(name, Nas::makeShareSection(sh));
    if (steps.isEmpty()) { setStatus("生成导出步骤失败：共享名或路径未通过校验", true); return; }

    const QVector<NasController::Step> go =
        confirmSteps(steps, QString("导出 SMB 共享 \\主机\%1  →  %2").arg(name, path));
    if (go.isEmpty()) return;
    m_currentSmbShare = name;
    m_ctl.run(go);
}

void NasPage::removeSmb()
{
    if (m_currentSmbShare.isEmpty()) { setStatus("没有记录本次导出的共享名", true); return; }
    const auto r = QMessageBox::question(this, "撤销 SMB 共享",
        QString("将删除共享 %1 的配置片段并重载 smbd。\n\n"
                "只删配置片段，不碰分区上的文件。").arg(m_currentSmbShare),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (r != QMessageBox::Yes) return;
    const QVector<NasController::Step> go =
        confirmSteps(m_ctl.planSmbRemove(m_currentSmbShare),
                     QString("撤销 SMB 共享 %1").arg(m_currentSmbShare));
    if (go.isEmpty()) return;
    m_ctl.run(go);
}

void NasPage::exportIscsi()
{
    const Nas::Plan plan = currentPlan();
    if (plan.volumeName.isEmpty()) { setStatus("先填名称", true); return; }
    const QString iqn = m_iqn->text().trimmed();
    if (iqn.isEmpty()) { setStatus("IQN 不能为空", true); return; }

    // 监听地址：空 = targetcli 默认建 0.0.0.0:3260 的 portal，等于全网开放。
    // 这里不放行，要求显式填写或显式确认。
    const QString listen = m_portal->text().trimmed();
    QHostAddress addr;
    if (listen.isEmpty()) {
        const auto r = QMessageBox::warning(this, "监听地址为空",
            "未填监听地址时 target 会监听 0.0.0.0:3260，"
            "局域网内（乃至公网，若端口被转发）任何主机都能尝试连接。\n\n"
            "确定要这样吗？",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (r != QMessageBox::Yes) { setStatus("已取消：请填写监听地址", true); return; }
    } else if (!addr.setAddress(listen)) {
        setStatus(QString("%1 不是合法 IP 地址").arg(listen), true);
        return;
    } else if (addr.isLoopback()) {
        setStatus("监听地址不能是回环地址，那样只有本机能连", true);
        return;
    }
    // 导出对象取决于引擎：
    //   mdadm → /dev/md-<卷名>（mdadm --create 建出来的就是这个名字）
    //   nasrs → 由 nasrs serve 挂上的 loop 设备，v1 尚未接入 NBD/loop，
    //           所以这里必须**明说没做**，不能拿 backing 文件路径冒充块设备
    QString blockDev;
    if (plan.engine == Nas::Engine::Mdadm) {
        blockDev = "/dev/md-" + plan.volumeName;
        if (!QFileInfo::exists(blockDev)) {
            setStatus(QString("%1 不存在，先创建阵列").arg(blockDev), true);
            return;
        }
    } else {
        setStatus("nasrs 引擎尚未接入 loop/NBD 导出（见 nasrs/src/main.rs 末尾说明），"
                  "目前只有 --report 可用", true);
        return;
    }

    const auto steps = m_ctl.planIscsiExport(blockDev, iqn, listen, m_anonAcl->isChecked());
    if (steps.isEmpty()) { setStatus("IQN 不合法或设备路径异常，未生成命令", true); return; }

    const auto go = confirmSteps(steps, "导出 iSCSI");
    if (go.isEmpty()) return;
    m_currentIqn = iqn;
    m_currentBlockDev = blockDev;
    m_ctl.run(go);
}

// 撤销按钮的入口：按最近导出的类型分派到 SMB 或 iSCSI。
void NasPage::teardownIscsi()
{
    if (!m_currentSmbShare.isEmpty()) { removeSmb(); return; }
    if (!m_currentIqn.isEmpty())      { teardownIscsiInternal(); return; }
    setStatus("本页没有记录已导出的对象（先导出一次）", true);
}
void NasPage::teardownIscsiInternal()
{
    // SMB 与 iSCSI 共用这一个撤销按钮：按最近一次导出的类型分派。
    // 判据用 m_currentSmbShare 是否非空，而不是看单选按钮 ——
    // 用户可能导出 SMB 后又把单选切回 iSCSI，那时该撤销的仍是 SMB。
    if (!m_currentSmbShare.isEmpty()) { removeSmb(); return; }
    if (!m_currentIqn.isEmpty())      { teardownIscsiInternal(); return; }
    setStatus("本页没有记录已导出的对象（先导出一次）", true);
    const auto steps = m_ctl.planIscsiTeardown(m_currentIqn, m_currentBlockDev);
    if (steps.isEmpty()) { setStatus("IQN 或设备路径异常", true); return; }
    const auto go = confirmSteps(steps, "撤销 iSCSI 导出");
    if (go.isEmpty()) return;
    m_ctl.run(go);
}

void NasPage::showStepLog(int index, bool ok, const QString &output)
{
    Q_UNUSED(output)
    setStatus(QString("步骤 %1：%2").arg(index + 1).arg(ok ? "完成" : "失败"), !ok);
}

void NasPage::onAllFinished(bool success)
{
    if (success && !m_currentIqn.isEmpty())
        m_teardownBtn->setEnabled(true);
    // 创建成功后才允许导出
    if (m_createBtn && !m_currentIqn.isEmpty())
        m_exportBtn->setEnabled(true);
    setStatus(success ? "完成" : "中途失败，后面的步骤已跳过", !success);
    refresh();
}

void NasPage::setStatus(const QString &text, bool bad)
{
    m_status->setText(text);
    m_status->setStyleSheet(bad ? ("color:" + Theme::bad().name()) : QString());
}
