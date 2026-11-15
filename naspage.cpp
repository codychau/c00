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
#include <QStandardPaths>
#include <QApplication>
#include <QScrollArea>
#include <QFrame>
#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QDirIterator>
#include <QDateTime>
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

    // ── 参数区：导出服务 / 扩展服务 两个标签页 ──
    // 刻意分成两页而不是堆在一起：扩展服务（影视索引）失败或还没装
    // 依赖时，不该连带让人以为导出也用不了。两页失败域隔离。
    //
    // 标签页自己就是分组，不再另套 QGroupBox：外面那层框的标题和第一个
    // 标签同名，同一个词在相邻两级重复一次是噪声而不是信息。
    m_tabs = new QTabWidget();
    // 按内容高度，不吸收页面多余空间。否则它会吃掉画板下方的全部富余
    // 高度，把画板顶到中间，看起来就是机箱上方一大片空白。
    //
    // 光设 Fixed 策略不够：maximumHeight 默认是 QWIDGETSIZE_MAX，布局有
    // 富余时照样把标签页拉高（实测拉到 651，内容只要 253），多出来的高度
    // 会被摊给页内控件，一行提示文字能被撑到几百像素。画板那边踩过同一个
    // 坑，结论一样 —— 得同时把 maximumHeight 收到 sizeHint。
    m_tabs->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_svcTab = new QWidget();
    auto *outer = new QVBoxLayout(m_svcTab);
    outer->setContentsMargins(0, 8, 0, 0);

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
    // 这一行只有提示文字，不给约束的话布局会把富余竖直高度摊给它，
    // 一行 17px 的提示实测被撑到 297px，在带边框的标签页里就是一片空白。
    m_modeHint->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_modeHint->setMaximumHeight(m_modeHint->sizeHint().height() + 6);
    m_modeHint->setProperty("role", "mute");
    modeRow->addWidget(m_modeHint, 1);
    outer->addLayout(modeRow);

    // iSCSI 那套行整体装进一个容器，好和 SMB 那套互斥显隐。
    m_iscsiBox = new QWidget();
    // 只是给一组控件做显隐的容器，不该吸收页面富余高度；
    // 不设 Fixed 的话它会被拉到内容高度的两倍以上。
    m_iscsiBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
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
    // 只是给一组控件做显隐的容器，不该吸收页面富余高度；
    // 不设 Fixed 的话它会被拉到内容高度的两倍以上。
    m_smbBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *sform = new QFormLayout(m_smbBox);
    sform->setContentsMargins(0, 0, 0, 0);

    m_smbName = new QLineEdit();
    m_smbName->setPlaceholderText("nas-share");
    m_smbName->setToolTip("\\\\主机\\<共享名>。不能含 \\ / * ? \" < > | :，不能叫 IPC$ 或 HOMES。");
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

    m_tabs->addTab(m_svcTab, "文件共享服务");
    // raidfs 放在导出与扩展之间：它是导出的另一种出口（同样是把卷交出去），
    // 不是可选扩展。放错位置会让人以为它是电视墙那类可选能力。
    m_fsTab = buildRaidFsTab();
    m_tabs->addTab(m_fsTab, "RAID 挂载目录");
    m_tabs->addTab(buildExtensionsTab(), "扩展服务");
    // 收口高度：maximumHeight 默认是 QWIDGETSIZE_MAX，光设 Fixed 策略挡不住
    // 布局拉伸（实测标签页被拉到 651 而内容只要 253，多出来的高度摊给页内
    // 控件，一行提示文字被撑成几百像素）。画板那边踩过同一个坑。
    //
    // 不设高度上限：标签页取最宽那页的高度（扩展服务页比导出页高），
    layout->addWidget(m_tabs);

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
    // 与「② 导出 iSCSI」并列：同一个卷的两种出口，不是先后步骤，所以两个都叫 ②
    // 且必须挨着放 —— 中间插进"撤销导出"会把这组并列关系切断。
    m_mountFsBtn = new QPushButton("② 挂载到目录");
    m_mountFsBtn->setToolTip("用 nasrs 做 RAID0 后挂成 /mnt 下的目录，供影视服务扫描。");
    m_mountFsBtn->setEnabled(false);
    connect(m_mountFsBtn, &QPushButton::clicked, this, &NasPage::onMountFsClicked);
    btnRow->addWidget(m_mountFsBtn);

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
    // 挂载目录预填：按卷名推一个 /mnt/<卷名>。只在空的时候填，不覆盖用户输入。
    // 用 defaultMountPoint 而不是自己拼串：它保证返回值为空或必然合法，
    // 预填一个非法路径等于在用户没动手之前就埋雷。
    if (m_mountPoint && m_mountPoint->text().isEmpty()) {
        const QString suggest = Nas::defaultMountPoint(m_volName->text().trimmed());
        if (!suggest.isEmpty()) m_mountPoint->setText(suggest);
    }
    refreshSmbWarning();
    refreshRaidFsStatus();
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
    // 挂目录走的是同一个卷，门槛一致；不给它单独放宽，
    // 否则会出现"阵列还没建成就挂目录"这种半截状态。
    if (m_createBtn && !m_currentIqn.isEmpty())
        m_mountFsBtn->setEnabled(true);
    setStatus(success ? "完成" : "中途失败，后面的步骤已跳过", !success);
    refresh();
}

void NasPage::setStatus(const QString &text, bool bad)
{
    m_status->setText(text);
    m_status->setStyleSheet(bad ? ("color:" + Theme::bad().name()) : QString());
}

// ── RAID 挂载目录 ──────────────────────────────────────────
//
// 用户要的出口：同一个 nasrs 引擎做 RAID0，但不导成 iSCSI，而是挂成一个
// /mnt 下的目录，让影视服务直接扫。iSCSI 那条走到块设备就结束了，
// Jellyfin 这类服务拿不到文件，所以才需要这一页。
//
// 这一页最要紧的是别让人以为能直接用。nasrs 现在只算布局就退出（退出码
// 3），没有任何导出通路。所以状态徽标写的不是"环境未就绪"，而是"引擎未
// 实现"—— 两者的处置动作完全不同，写成前者会把人引去装软件，白费功夫。
QWidget *NasPage::buildRaidFsTab()
{
    auto *w = new QWidget();
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(0, 8, 0, 0);

    auto *intro = new QLabel("同样是 nasrs 做 RAID0，差别只在出口：不导成 iSCSI，而是挂成一个目录，影视服务能直接扫。");
    // 上面那句只说差别，不承诺能用；下一行的徽标才说现在到底能不能。
    intro->setWordWrap(true);
    intro->setProperty("role", "mute");
    v->addWidget(intro);

    auto *card = new QGroupBox("挂载目录");
    auto *form = new QFormLayout(card);

    m_mountPoint = new QLineEdit();
    m_mountPoint->setPlaceholderText("/mnt/raid");
    m_mountPoint->setToolTip("只接受 /mnt 的直接子目录。挂到会遮内容：原目录里的东西在挂载期间看不见，卸载后回来。");
    form->addRow("挂载到", m_mountPoint);

    auto *scope = new QLabel("限定在 /mnt 的直接子目录：这一层不会盖住系统运行需要的目录，也不会挂到已有分区的挂载点里面去（那样分区一旦没挂，数据会写进根盘的同名目录，极难发现）。");
    scope->setWordWrap(true);
    scope->setProperty("role", "mute");
    form->addRow("", scope);

    m_fsPill = new QLabel("检查中…");
    m_fsPill->setProperty("role", "pill-warn");
    // 刻意不设 role="pill"：QSS 里那条规则会和稍后的行内 setStyleSheet 打架，
    // 实测文字整块消失只剩色块。电视墙那三个徽标同样是"不设 role、样式走行内"。
    m_fsDeps = new QLabel();
    m_fsDeps->setWordWrap(true);
    m_fsDeps->setTextFormat(Qt::RichText);
    m_fsDeps->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *pillRow = new QHBoxLayout();
    pillRow->addWidget(m_fsPill);
    pillRow->addWidget(m_fsDeps, 1);
    form->addRow("依赖", pillRow);

    m_fsNote = new QLabel();
    m_fsNote->setWordWrap(true);
    m_fsNote->setTextFormat(Qt::RichText);
    m_fsNote->setProperty("role", "note");
    v->addWidget(card);
    v->addWidget(m_fsNote);

    auto *row = new QHBoxLayout();
    row->addStretch(1);
    m_umountBtn = new QPushButton("卸载该目录");
    m_umountBtn->setToolTip("umount 挂载点。目录里原有的内容会重新显现。");
    // 一开始没有可撤的东西，先禁用；导出成功后才放开（见 onMountFsClicked）
    m_umountBtn->setEnabled(false);
    connect(m_umountBtn, &QPushButton::clicked, this, &NasPage::onUmountFsClicked);
    row->addWidget(m_umountBtn);
    v->addLayout(row);

    v->addStretch(1);
    refreshRaidFsStatus();
    return w;
}


// 把"能不能挂"讲清楚。刻意分成两层说，合成一句就会误导人：
//   环境层：/dev/fuse、fusermount3、nasrs 二进制在不在 —— 这些缺了能装；
//   实现层：nasrs 目前根本没有导出通路，只算布局就退出（退出码 3）。
// 只报环境层会让人去装软件，装完照样不能用。所以未实现时徽标就写未实现。
void NasPage::refreshRaidFsStatus()
{
    if (!m_fsPill) return;

    struct Row { const char *name; bool has; const char *why; };
    const QList<Row> rows = {
        { "/dev/fuse",    m_env.fuseDevice,  "内核 FUSE 支持" },
        { "fusermount3",  m_env.fuseMount,   "非 root 挂载助手（apt install fuse3）" },
        { "nasrs",        m_env.nasrsBinary, "RAID 引擎" },
    };
    // 与电视墙那页同一套画法：逐项 ✓/✗ 上色，缺什么一目了然。
    // 不用 <ul>/<li> —— QLabel 的富文本支持很窄，列表标签不会生效，
    // 只会把项目符号原样显示出来（这里踩过：整页出现一堆 •）。
    QString html;
    for (const Row &r : rows) {
        const QString col = r.has ? Theme::ok().name() : Theme::bad().name();
        html += "<span style=\"color:" + col + "\"> " +
                (r.has ? QString("✓ ") : QString("✗ ")) +
                QString::fromLatin1(r.name) +
                " <span style=\"color:" + Theme::textMute().name() + "\">· " +
                QString::fromUtf8(r.why) + "</span></span>";
    }
    m_fsDeps->setText(html);

    // 徽标只写状态本身，不写颜色以外的信息；未实现这条优先级最高，
    // 而且**不随环境是否齐备改措辞** —— 环境齐备也不代表能用。
    m_fsPill->setText(QString::fromUtf8("引擎未实现"));
    // 徽标走主题 role，不写行内 setStyleSheet：颜色只在一个地方定义。
    // 之前写的是 alpha(warn,36) 底 + warn 字，实测文字整块看不见 ——
    // 同色系半透明底被压成实心后与文字同色。用 role 就没这个坑。
    m_fsPill->setProperty("role", "pill-warn");
    QStringList notes;
    notes << QString::fromUtf8(
        "<b>nasrs 还没有导出能力</b>：当前版本只计算条带布局、打印容量就退出"
        "（退出码 3）。本页生成的命令要等引擎补上导出之后才跑得通。");
    if (!m_env.fuseDevice || !m_env.fuseMount || !m_env.nasrsBinary)
        notes << QString::fromUtf8("另外本机还缺上面打 ✗ 的环境依赖，装上也不代表能用。");
    notes << QString::fromUtf8(
        "挂载目录现在就能填：路径是否合法、会不会遮住现有内容，这些与引擎无关，"
        "现在就校验并报错。");
    m_fsNote->setText(notes.join("<br>"));
}
void NasPage::onMountFsClicked()
{
    const QString mp = m_mountPoint->text().trimmed();

    // 1) 路径合法性（纯字符串，不碰磁盘）
    const QString mpErr = Nas::validateMountPoint(mp);
    if (!mpErr.isEmpty()) { setStatus("挂载目录：" + mpErr, true); return; }

    // 2) 与现有挂载的关系。这一步必须在生成命令之前：挂到已挂载的目录上
    //    会直接失败，而挂到"位于某个分区挂载点之内"的目录则会把数据写到
    //    意料之外的盘上 —— 后者不报错，属于最难发现的那类问题。
    const QString conflict = Nas::mountPointConflict(mp, m_sysMounts);
    if (!conflict.isEmpty()) { setStatus("挂载目录：" + conflict, true); return; }

    // 3) 目录若非空，挂载会把它遮住。这里读一次真实目录：遮内容不是损坏，
    //    卸载就恢复，所以是"要让人知道"而不是"拒绝"。
    QDir probe(mp);
    if (probe.exists()) {
        const QStringList entries =
            probe.entryList(QDir::AllEntries | QDir::Hidden | QDir::System);
        const QString busy = Nas::mountPointBusy(mp, entries);
        if (!busy.isEmpty() &&
            QMessageBox::warning(this, "目录已有内容",
                QString("%1\n\n确定要挂上去吗？（卸载后这些会重新显现）").arg(busy),
                QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel)
                != QMessageBox::Ok) {
            setStatus("已取消：不挂到非空目录", true);
            return;
        }
    }

    // 4) 成员要够。RAID0 少于 2 个成员 nasrs 自己会拒，这里先挡下来并说清原因，
    //    免得用户看到一句英文报错。
    const Nas::Plan plan = currentPlan();
    if (plan.members.size() < 2) {
        setStatus("先在上方点选至少 2 个分区，RAID0 需要 2 个以上成员", true);
        return;
    }

    const auto steps = m_ctl.planRaidfsMount(plan, mp);
    if (steps.isEmpty()) { setStatus("命令生成失败：参数不合法", true); return; }

    // 引擎没实现，所以这里不走 confirmSteps 真的执行 —— 那是骗人。
    // 把命令逐条列出来给人看，并明说现在跑不通：既让流程可审，也不假装能用。
    // 引擎没实现，所以这里不走 confirmSteps 真的执行 —— 那是骗人。
    // 改成把命令逐条列出来给人看并明说跑不通：流程可审，但不假装能用。
    QStringList lines;
    for (const NasController::Step &s : steps)
        lines << QString("# %1\n    %2 %3").arg(s.describe, s.program, s.args.join(" "));
    setStatus(QString::fromUtf8("命令已生成（%1 步），但 nasrs 尚未实现导出，不会执行。")
                  .arg(steps.size()), true);
    QMessageBox::information(this, QString::fromUtf8("RAID 挂载目录"),
        QString::fromUtf8("引擎还未实现，以下命令不会执行，仅供核对：") + "\n\n"
        + lines.join("\n"));
}

// 卸载。只撤我们自己挂的那个目录，且必须由用户点：
// umount 一个正在被影视服务读取的目录会让对方的 IO 报错。
void NasPage::onUmountFsClicked()
{
    const QString mp = m_mountPoint->text().trimmed();
    const QString mpErr = Nas::validateMountPoint(mp);
    if (!mpErr.isEmpty()) { setStatus("挂载目录：" + mpErr, true); return; }

    const auto steps = m_ctl.planRaidfsUnmount(mp);
    if (steps.isEmpty()) { setStatus("卸载目标不合法", true); return; }
    const auto toRun = confirmSteps(steps, "卸载挂载目录");
    if (toRun.isEmpty()) return;
    m_ctl.run(toRun);
}



// ── 扩展服务页 ────────────────────────────────────────────
//
// v1 的「电视墙」只做只读索引：扫目录、认视频文件、读已有 nfo。
// 不重命名、不删除、不改任何媒体文件，也不写任何用户目录。
// 刮削的在线部分本机不可用（TMDB 不可达、omdb 需 API key、无 ffmpeg），
// 所以这里一律如实标注来源，绝不显示编造的标题或封面。

QWidget *NasPage::buildExtensionsTab()
{
    auto *w = new QWidget();
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(0, 8, 0, 0);

    auto *intro = new QLabel(
        "扩展服务是可选能力，关掉不影响导出。缺的依赖会直接说缺，"
        "不显示假的运行状态。");
    // QLabel 默认 Qt::AutoText：纯文本里写尖括号标签会原样显示。这段本来只要强调，去掉标签最省事。
    intro->setProperty("role", "mute");
    intro->setWordWrap(true);
    v->addWidget(intro);

    // ── 电视墙 ──
    auto *tv = new QGroupBox("电视墙 · 影视库只读索引");
    auto *tvv = new QVBoxLayout(tv);

    auto *desc = new QLabel(
        "扫描所选分区的挂载点，列出影视文件：按文件名推断标题/年份/季集，"
        "有 nfo 时读 nfo 里的真实元数据。只读，不重命名、不删除、不移动任何文件。");
    desc->setWordWrap(true);
    desc->setProperty("role", "mute");
    tvv->addWidget(desc);

    auto *meta = new QHBoxLayout();
    m_tvRoot = new QLabel("（未选分区，先在上方点选）");
    m_tvRoot->setProperty("role", "mute");
    m_tvCount = new QLabel("已索引 0 项");
    m_tvCount->setProperty("role", "mute");
    m_tvPill = new QLabel("未索引");
    m_tvPill->setProperty("role", "pill");
    meta->addWidget(m_tvRoot, 1);
    meta->addWidget(m_tvCount);
    meta->addSpacing(8);
    meta->addWidget(m_tvPill);
    tvv->addLayout(meta);

    // 依赖自检：本机实测缺什么就写什么。这栏存在的意义是让人不必
    // 猜「为什么没有封面」—— 不说清楚就会被当成程序坏了。
    m_tvDeps = new QLabel();
    m_tvDeps->setWordWrap(true);
    m_tvDeps->setTextFormat(Qt::RichText);
    m_tvDeps->setTextFormat(Qt::RichText);
    tvv->addWidget(m_tvDeps);

    m_tvTable = new QTableWidget(0, 4);
    m_tvTable->setHorizontalHeaderLabels({"标题 / 年份", "季集", "元数据来源", "文件名"});
    m_tvTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tvTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tvTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_tvTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_tvTable->verticalHeader()->setVisible(false);
    m_tvTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tvTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    // 不设固定高度：给 stretch 让它吸收富余空间，这样扩展页的整体高度
    // 会随窗口收缩，导出页那边同高的空白也就跟着变小。写死 190 会让
    // 扩展页始终占 700px，标签页同高后导出页只剩一片空白。
    m_tvTable->setMinimumHeight(120);
    tvv->addWidget(m_tvTable, 1);

    m_tvNote = new QLabel();
    m_tvNote->setWordWrap(true);
    m_tvNote->setTextFormat(Qt::RichText);
    m_tvNote->setProperty("role", "note");
    m_tvNote->setText(
        "尚未扫描。扫描只读文件内容与文件名，不写入任何位置。");
    tvv->addWidget(m_tvNote);

    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    m_tvScan = new QPushButton("扫描所选分区");
    m_tvScan->setToolTip("遍历所选分区挂载点下的影视文件。只读。");
    btnRow->addWidget(m_tvScan);
    tvv->addLayout(btnRow);
    connect(m_tvScan, &QPushButton::clicked, this, &NasPage::onTvScanClicked);

    v->addWidget(tv);

    // ── 接外部媒体服务：明确「不实现」，并给出真实安装路径 ──
    auto *jf = new QGroupBox("接外部媒体服务（Jellyfin / Emby）");
    auto *jfv = new QVBoxLayout(jf);
    auto *jfd = new QLabel(
        "本工具不实现 Jellyfin 的 REST API —— 那套协议由 Jellyfin 自己实现，"
        "自研等于重写一个媒体服务器。这里只把影视目录通过 SMB 导出来，"
        "交给真正的媒体服务去建库和刮削。");
    jfd->setWordWrap(true);
    jfd->setProperty("role", "mute");
    jfv->addWidget(jfd);

    auto *cmd = new QLabel(
        "<pre style=\"margin:0\">sudo apt install jellyfin      # 或官方 docker 镜像\n"
        "sudo systemctl enable --now jellyfin\n"
        "# 然后把媒体库路径指到本工具导出的 SMB 共享</pre>");
    cmd->setTextFormat(Qt::RichText);
    cmd->setTextInteractionFlags(Qt::TextSelectableByMouse);
    cmd->setProperty("role", "code");
    jfv->addWidget(cmd);
    v->addWidget(jf);

    v->addStretch(1);
    refreshTvDeps();
    // 滚动包一层。不包的话扩展页内容多高就占多高（实测 700px），而标签页
    // 必须同高，导出页（258px）下面就只剩一片空白。包了之后窗口给多高用
    // 多高，超出才出滚动条 —— 标签页高度不再被最高那页顶起来。
    //
    // 不给导出页硬塞内容：没有真实信息可放时，留白比编一块面板好。
    auto *scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(w);
    scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    return scroll;
}

// 依赖自检。写死成实测结论前先探测一遍，避免哪天装了还在喊缺。
void NasPage::refreshTvDeps()
{
    struct Row { const char *name; bool has; const char *why; };
    const auto have = [](const char *exe) {
        return !QStandardPaths::findExecutable(QString::fromLatin1(exe)).isEmpty();
    };
    const QList<Row> rows = {
        { "ffmpeg",  have("ffmpeg"),  "缩略图/时长" },
        { "ffprobe", have("ffprobe"), "容器与流信息" },
        // 在线源是运行期才知道的，这里只说明状态，不下结论说永久不可用
        { "TMDB",    false, "在线刮削（本机实测不可达）" },
        { "omdb key", false, "实测返回 No API key provided" },
    };
    QString html = "<span style=\"color:" + Theme::textMute().name() + "\">依赖：</span>";
    for (const Row &r : rows) {
        const QString col = r.has ? Theme::ok().name() : Theme::bad().name();
        html += "<span style=\"color:" + col + "\"> " +
                (r.has ? QString("✓ ") : QString("✗ ")) +
                QString::fromLatin1(r.name) +
                " <span style=\"color:" + Theme::textMute().name() + "\">· " +
                QString::fromUtf8(r.why) + "</span></span>";
    }
    m_tvDeps->setText(html);
}

void NasPage::onTvScanClicked()
{
    const QVector<QString> members = m_board->members();
    if (members.isEmpty()) {
        setStatus("先在上方点选一个分区，电视墙才知道扫哪里");
        return;
    }
    // 只扫成员分区自己的挂载点，不去遍历整块盘，更不去碰 /。
    QStringList roots;
    for (const QString &dev : members) {
        for (const NasBoardWidget::Disk &d : m_disks) {
            if (d.device == dev && !d.mountpoint.isEmpty() && d.mountpoint != "/")
                roots << d.mountpoint;
        }
    }
    if (roots.isEmpty()) {
        setStatus("所选分区没有挂载点，无法扫描");
        return;
    }
    m_tvRoot->setText(roots.join("、"));
    m_tvPill->setText("扫描中…");
    m_tvPill->setProperty("role", "pill-warn");
    QApplication::processEvents();

    struct Row { QString title, se, src, file, size; };
    QList<Row> rows;
    qint64 visited = 0;
    // 两个上限都要能被区分出来：扫到一半停手 和 真的扫完了，
    // 对用户是两个完全不同的结论。混报会让缺文件的分区看起来像没影视。
    bool truncatedWalk = false;
    bool truncatedRows = false;
    constexpr qint64 kMaxVisit = 60000;   // 再大会让界面假死几分钟
    for (const QString &rootDir : roots) {
        QDirIterator it(rootDir, QDir::Files,
                        QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
        while (it.hasNext()) {
            const QString path = it.next();
            if (++visited > kMaxVisit) { truncatedWalk = true; break; }
            const QFileInfo fi(path);
            const qint64 sz = fi.size();
            if (!Nas::isVideoFile(path, sz)) continue;   // .ts 歧义在模型层处理

            Nas::NameGuess g = Nas::guessFromFileName(fi.fileName());
            // 同目录同名 .nfo 才认，避免把别的条目的元数据张冠李戴
            QString nfoPath = fi.absolutePath() + "/" + fi.completeBaseName() + ".nfo";
            QFile nf(nfoPath);
            Nas::NfoInfo n;
            if (nf.open(QIODevice::ReadOnly | QIODevice::Text))
            {
                // Qt6 的 readAll() 没有 maxSize 重载。nfo 正常只有几 KB，
                // 但扫描的是用户目录，得防一个超大文件把界面冻住。
                if (nf.size() <= (1 << 20))
                    n = Nas::parseNfo(QString::fromUtf8(nf.readAll()));
            }

            Row r;
            if (n.ok) {
                r.title = n.title;
                if (n.year) r.title += QString(" (%1)").arg(n.year);
                r.se = (n.season || n.episode)
                     ? QString("S%1E%2").arg(n.season, 2, 10, QChar('0'))
                                        .arg(n.episode, 2, 10, QChar('0'))
                     : QString("—");
                r.src = "本地 nfo";
            } else {
                r.title = g.title.isEmpty() ? QString("（无法从文件名判断）") : g.title;
                if (g.year) r.title += QString(" (%1)").arg(g.year);
                r.se = (g.season || g.episode)
                     ? QString("S%1E%2").arg(g.season, 2, 10, QChar('0'))
                                        .arg(g.episode, 2, 10, QChar('0'))
                     : QString("—");
                // 来源如实标：把推断包装成"已刮削"是骗人
                r.src = g.title.isEmpty() ? "未识别" : "文件名推断";
            }
            r.file = fi.fileName() + " · " + Nas::humanSize(sz);
            rows << r;
            if (rows.size() >= 400) { truncatedRows = true; break; }
        }
    }

    m_tvTable->setRowCount(rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        m_tvTable->setItem(i, 0, new QTableWidgetItem(rows[i].title));
        m_tvTable->setItem(i, 1, new QTableWidgetItem(rows[i].se));
        m_tvTable->setItem(i, 2, new QTableWidgetItem(rows[i].src));
        m_tvTable->setItem(i, 3, new QTableWidgetItem(rows[i].file));
    }
    m_tvCount->setText(QString("已索引 %1 项").arg(rows.size()));
    m_tvScanned = true;
    m_tvPill->setText(rows.isEmpty() ? "无影视文件" : "已索引");
    m_tvPill->setProperty("role", rows.isEmpty() ? "pill" : "pill-ok");

    // 未验证的部分写明白。这条说明的价值在于防止"看起来像刮削好了"。
    m_tvNote->setText(
        "标 <b>文件名推断</b> 的条目未经任何在线校验：本机 TMDB 不可达、"
        "omdb 需要 API key。标题与年份可能与实际不符；季集判断不了时显示 "
        "<b>—</b>，不编造。若装了 ffmpeg，可补缩略图与时长 —— 当前未接。");
    if (rows.isEmpty() && !truncatedWalk && !truncatedRows) {
        // 空结果就说空结果。继续挂着「文件名推断不可信」那段，
        // 会让人以为有条目被藏起来了。
        m_tvNote->setText(QString::fromUtf8(
            "所选分区下没有找到影视文件。若影视放在别的目录，"
            "把那个目录所在的分区选上再扫。"));
        setStatus(QString::fromUtf8(
            "电视墙：所选分区下没有影视文件（只读，未改动任何文件）"));
    } else if (truncatedWalk || truncatedRows) {
        // 说清楚没扫完，而不是给一个看起来完整的数字
        QString why = truncatedWalk
            ? QString("已遍历 %1 个条目后停止（上限 %2），结果不完整").arg(visited).arg(kMaxVisit)
            : QString("结果已达 %1 条上限，可能还有更多未列出").arg(rows.size());
        m_tvPill->setText("结果不完整");
        m_tvPill->setProperty("role", "pill-warn");
        m_tvNote->setText(m_tvNote->text() + "<br><b>" + why + "</b>");
        setStatus("电视墙扫描被上限打断：" + why);
    } else {
        setStatus(QString("电视墙扫描完成：%1 项（只读，未改动任何文件）").arg(rows.size()));
    }
}
