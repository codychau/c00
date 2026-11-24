#include "naspage.h"
#include "theme.h"
#include "adminhelper.h"

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
#include <QAbstractButton>
#include <QStandardPaths>
#include <QApplication>
#include <QScrollArea>
#include <QFrame>
#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QDirIterator>
#include <QDateTime>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextCursor>
#include <QTimer>
#include <QStyle>
#include <algorithm>

static constexpr int kTopbarInset = 52;   // 与 MainWindow 顶栏一致，正文自行让出

// 页顶通栏按钮的文案。抽成常量是因为装完/失败/取消都要把文案**还原**成
// 一模一样的一句，散在三处的字面串迟早会写成三个不同的版本。
static const QString kInstallBtnText = QString::fromUtf8("点击此按钮一键安装依赖的组件");
// 装包的兜底超时。取 20 分钟：镜像慢时 apt 装 samba 能到十几分钟，
// 再长就属于"网络或仓库出问题了"，继续挂着只会让人以为程序死了。
static constexpr int kInstallTimeoutMs = 20 * 60 * 1000;

// 镜像的定时同步节拍。5 分钟是权衡后的值：再短就会和用户正在做的拷贝抢盘，
// 再长就失去"副本基本是新的"这个意义。写死不做参数：可调就多一个要解释的东西。
static constexpr int kMirrorAutoMs = 5 * 60 * 1000;

// 下拉框下标 → 引擎。三处要用（判定分区可选性、组 Plan、算"还缺什么"），
// 抽成函数而不是每处各写一个三元：漏一处就会出现"分区按镜像的规则挑好了、
// 建阵列时却按 RAID0 生成命令"——两套判据各说各话，是最难查的那类错。
static Nas::Engine engineFromIndex(int i)
{
    switch (i) {
    case 1:  return Nas::Engine::Raid0Rs;
    case 2:  return Nas::Engine::MirrorRs;
    default: return Nas::Engine::Mdadm;
    }
}

// 换 role 属性后必须重新 polish。QSS 只在 style 感知到属性变化时才重算配色，
// 光 setProperty 会让控件留着旧配色 —— 表现是"徽标文字变成'已一致'了，
// 颜色还红着"。本页别处的 role 都是构造时设一次，所以没碰到这个问题。
static void setRole(QWidget *w, const char *role)
{
    if (!w) return;
    if (w->property("role").toByteArray() == role) return;   // 没变就别白折腾一遍 polish
    w->setProperty("role", role);
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

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

    // ── 页顶通栏：一键安装依赖的组件 ──
    // 摆在这里而不是塞进某一格里，是因为"依赖没装"是这台机器上最常见的
    // 第一道坎，而它原先藏在「挂载目录」那一格的角落里，没人会发现。
    // 颜色走 Theme 的 install 角色（橘黄），不在页面里写死十六进制。
    // 宽度：跟着正文走，占满整行（左右各让出布局的 10px 内缩）。
    m_installAllBtn = new QPushButton(kInstallBtnText);
    m_installAllBtn->setProperty("role", "install");
    m_installAllBtn->setToolTip(QString::fromUtf8("检测缺失组件，用系统自带的包管理器安装。"));
    connect(m_installAllBtn, &QPushButton::clicked, this, &NasPage::onInstallAllDepsClicked);
    layout->addWidget(m_installAllBtn);

    // 安装日志：平时不占地方，点下去才现身。装包动辄几十行输出，塞进单行
    // 状态栏等于没给 —— 装失败的人最需要看到的是包管理器的原话。
    m_installLog = new QPlainTextEdit();
    m_installLog->setReadOnly(true);
    m_installLog->setProperty("role", "mono");
    m_installLog->setFixedHeight(150);
    // 只留最近 400 行：apt 的进度输出能刷上千行，不设上限长跑会一直吃内存
    m_installLog->setMaximumBlockCount(400);
    m_installLog->setVisible(false);
    layout->addWidget(m_installLog);

    // 兜底超时。装包卡在镜像上是常态，没有这道闸，按钮会永远停在"安装中"，
    // 而用户唯一能做的就是关掉重开。
    m_installTimer = new QTimer(this);
    m_installTimer->setSingleShot(true);
    connect(m_installTimer, &QTimer::timeout, this, [this]() {
        if (!m_installProc || m_installProc->state() == QProcess::NotRunning) return;
        m_installTimedOut = true;
        appendInstallLog(QString::fromUtf8("!! 超过 20 分钟没有结束，已中止。"
                                           "通常是镜像源太慢或不可达，可稍后重试。"));
        m_installProc->kill();
    });

    // ── 第一行：引擎选择 + 环境提示 ──
    auto *topRow = new QHBoxLayout();
    topRow->addWidget(new QLabel("实现"));
    m_engine = new QComboBox();
    m_engine->addItem("mdadm（内核 RAID，稳）");
    m_engine->addItem("nasrs RAID0（条带 over 文件，不占分区）");
    m_engine->addItem("nasrs 镜像 RAID1（文件存两份，读其中一份）");
    // 三条路线的可用性差别很大，下拉框里就要说清：RAID0 那条缺导出通路（现在
    // 只能算布局），镜像这条普通权限就能跑通。把两条并列成"看着都能用"是骗人。
    m_engine->setItemData(0, "内核 mdadm 建阵列，稳，但要 root 与 mdadm 包。", Qt::ToolTipRole);
    m_engine->setItemData(1, "自研引擎把两个大文件拼成块设备。"
                             "导出层（loop/NBD + LIO）尚未实现，现在只能算布局。", Qt::ToolTipRole);
    m_engine->setItemData(2, "自研引擎把主目录完整拷到副本目录，访问者读其中一份。"
                             "不要 root、不要内核模块，现在就能用；可用容量是一份。", Qt::ToolTipRole);
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

    // ── 阵列参数区：这一格说的是"盘怎么拼"，不是"卷怎么交出去" ──
    // 位置紧贴画板，因为它的输入就是画板上点的那几个盘位。
    // 以前「名称」「条带粒度」住在 m_iscsiBox 里（那个框是为 iSCSI 整组显隐
    // 造的），三条路线却都要用「名称」（镜像的副本目录名由它推），于是选镜像
    // 时只能"整框显示 + 逐行藏"，导出页于是亮着一个满是 iSCSI 字样的框，
    // 而 SMB 的共享名/共享路径就在它下面 —— 看着就像 RAID 的参数属于 SMB 配置。
    // 框先占位，内容按各自的初始化顺序在后面填进来（m_mirrorBox 要比
    // m_smbPath 晚建，见 refreshMirrorBox）。
    m_arrayBox = new QWidget();
    m_arrayBox->setObjectName("arrayBox");        // 探针按名字找它
    // 只是容器，不给它吸收页面富余高度（画板和标签页都栽过这个坑）。
    m_arrayBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *arrayV = new QVBoxLayout(m_arrayBox);
    arrayV->setContentsMargins(0, 8, 0, 0);
    m_arrayForm = new QFormLayout();
    m_arrayForm->setContentsMargins(0, 0, 0, 0);
    arrayV->addLayout(m_arrayForm);
    layout->addWidget(m_arrayBox);
    m_arrayBox->setVisible(false);   // 0 个成员，还没到拼阵列的时候

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
    m_modeMount = new QRadioButton("RAID 挂载目录");
    m_modeMount->setToolTip("nasrs 做 RAID0 后挂成 /mnt 下的目录，影视服务能直接扫。不导 iSCSI。");
    m_modeIscsi->setChecked(true);       // 默认 iSCSI：与既有行为一致，不擅自改默认
    m_modeGroup = new QButtonGroup(this);
    m_modeGroup->addButton(m_modeIscsi);
    m_modeGroup->addButton(m_modeSmb);
    m_modeGroup->addButton(m_modeMount);
    modeRow->addWidget(m_modeIscsi);
    modeRow->addWidget(m_modeSmb);
    modeRow->addWidget(m_modeMount);
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
    m_arrayForm->addRow("名称", m_volName);
    // 副本目录名是由卷名推的，所以名字一改，镜像格与共享路径就得跟着重算。
    // 少了这条，"先点分区、后填名字"这个最常见的顺序会让两条路径标签一直停在
    // 「（先在上方点选第一个分区）」不动 —— 页面显示的路径和真去建的那个不是
    // 同一个东西，比不显示更糟。（镜像页探针抓的就是它）
    connect(m_volName, &QLineEdit::textChanged, this, [this]() { refreshMirrorBox(); });

    m_chunk = new QSpinBox();
    m_chunk->setRange(4, 16384);
    m_chunk->setValue(512);
    m_chunk->setSuffix(" KiB");
    m_chunk->setToolTip("条带粒度。大文件顺序读：越大越好；小块随机写：越小越好。");
    m_arrayForm->addRow("条带粒度", m_chunk);   // 镜像不条带，整行在 syncExportMode 里收掉

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

    // 挂载目录出口：与 iSCSI / SMB 框互斥显隐，由 buildRaidFsBox 造内容。
    // 只给一组控件做显隐的容器，策略要 Fixed，否则吸收页面富余高度。
    m_fsBox = buildRaidFsBox();
    m_fsBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    outer->addWidget(m_fsBox);
    m_fsBox->setVisible(false);   // 默认 iSCSI，挂载框先藏起来

    // 软件镜像（自研 RAID1）的参数：两份目录、读哪一份、同步状态、定时器。
    // 这些全是"盘怎么拼"的事，归上方阵列区，不进导出页 —— 导出页只留共享名、
    // 共享路径、可写这三样共享自己的事。它和 m_fsBox 的语义完全不同
    //（那边写"引擎未实现"，这边写"副本落后多少"），所以仍是两个框。
    m_mirrorBox = buildMirrorBox();
    m_mirrorBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    arrayV->addWidget(m_mirrorBox);
    m_mirrorBox->setVisible(false);

    m_smbWarn = new QLabel();
    m_smbWarn->setWordWrap(true);
    m_smbWarn->setTextFormat(Qt::RichText);
    outer->addWidget(m_smbWarn);

    // 「装包补不了的」前置条件：configfs 没挂、内核里没有 target 模块、
    // nasrs 引擎不在。它们和"缺组件"是两类事，所以不在右上角那条里，
    // 而是放在真正会被它们挡住的导出页底部，缺了才露出来。
    m_blockNote = new QLabel();
    m_blockNote->setObjectName("blockNote");   // 探针按名字找它：它平时是空的
    m_blockNote->setWordWrap(true);
    m_blockNote->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_blockNote->setProperty("role", "warn");
    m_blockNote->setVisible(false);
    outer->addWidget(m_blockNote);

    m_smbBox->setVisible(false);   // 初始 0 个成员，只显示 iSCSI

    // 挂载目录不再是独立标签：它和 SMB 一样是"把卷交出去"的一种出口，
    // 归到导出页里当第三个单选。这里只剩「导出」与「扩展」两页。
    m_tabs->addTab(m_svcTab, "文件共享服务");
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
    // 模式互斥切换：只刷新显隐与警告，不重算成员。
    // 用 buttonToggled 而不是 idClicked —— 实测程序化 setChecked 时
    // idClicked 一次都不发（toggled=1、buttonToggled=2 才有）。之前自动切
    // SMB / 挂载失效回落都靠别处顺带调 syncExportMode 兜住，一旦没有就
    // 漏刷新：出口框残留、② 按钮文案停在旧出口。buttonToggled 两种来源都覆盖。
    // syncExportMode 内部还会 setChecked，会再触发本槽 —— 但那只会重算成
    // 同一状态，收敛不循环；为稳妥加一层再入保护。
    connect(m_modeGroup, &QButtonGroup::buttonToggled, this, [this](QAbstractButton *, bool on){
        if (on && !m_syncingExportMode)
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
    // 出口三选一都走同一个「②」按钮（文案随选中的出口变），不再单列挂载按钮。

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
    const Nas::Engine e = engineFromIndex(index);
    m_board->setEngine(e);
    // 换引擎 = 换"能不能用"的判据，必须重新判定，
    // 否则界面上还停留在上一个引擎的可选性
    refresh();
    // 三条路线的危险程度不同，这里只切视图：
    //  mdadm  会抹掉所选分区的文件系统（mdadm --create 写 superblock）
    //  nasrs  只在分区已有的文件系统里放东西（RAID0 放一个大文件，镜像放一个目录），
    //         不碰分区本身
    // 卡口数量：RAID0 至少 2 个成员才有意义，镜像必须正好 2 个（一主一备）。
    updateSummary();
}

// 装包补不了的几样前置条件。每条都带上"那你自己该怎么补"，因为这条链的
// 尽头没有按钮可点 —— 不给处置办法的红字只会让人去搜错误。
//
// 按当前出口过滤：configfs 与内核 target 模块是 LIO/iSCSI 才需要的，
// 选 SMB 或挂载目录时摆出来就是无关噪音；nasrs 引擎只在选了 nasrs 引擎时相关。
QStringList NasPage::nonPackageBlockers() const
{
    QStringList b;
    const bool iscsi = !m_modeSmb || m_modeIscsi->isChecked();
    if (iscsi && !m_env.configfsMounted)
        b << QString::fromUtf8("configfs 未挂载 —— sudo mount -t configfs configfs /sys/kernel/config");
    if (iscsi && !m_env.kernelModules)
        b << QString::fromUtf8("内核目录里没找到 target_core_mod 模块文件"
                               "（若内核已把 LIO 编进去，这条可以忽略）");
    if (!m_env.nasrsBinary && m_engine && engineFromIndex(m_engine->currentIndex()) != Nas::Engine::Mdadm)
        b << QString::fromUtf8("nasrs 引擎不在程序旁边 —— 它随本程序一起发布，不是发行版的包");
    return b;
}

void NasPage::refreshBlockNote()
{
    if (!m_blockNote) return;
    const QStringList b = nonPackageBlockers();
    if (b.isEmpty()) { m_blockNote->setVisible(false); return; }
    m_blockNote->setText(QString::fromUtf8
        ("下面这几项一键安装管不了（不是缺软件包），用到 iSCSI 导出前得先处理：\n· ")
        + b.join("\n· "));
    m_blockNote->setVisible(true);
}

// 把"环境探测结果"翻译成界面上的两处提示：右上角的缺失标签 + 页顶通栏按钮。
// 单独成函数只为了让它们**必须**同一次算完 —— 这两处说的是同一件事（还缺
// 什么组件），分两处更新就会出现在中间状态下互相打脸的机会。
// 探针也走这一条（见 setEnvForTest），线上和测试看到的是同一段代码。
void NasPage::syncEnvAndInstallUi()
{
    // 右上角只说一件事：还缺哪些**组件**（一键安装能装的那种）。
    // 判据与按钮同源，所以装完必然一起消失。早先这里还混着 configfs 和
    // 内核模块，结果"包装齐了、按钮没了、红字还挂着"，看着就像程序坏了 ——
    // 那两类现在归导出页底部那条说明（见 refreshBlockNote）。
    const QStringList lack = m_env.missingDepLabels;
    if (lack.isEmpty()) {
        m_envLabel->setVisible(false);
    } else {
        QString hint = "缺少：" + lack.join("、");
        // 认不出包管理器时通栏按钮不存在（它无能为力），这句"缺什么"就成了
        // 唯一的信号，得在这里说清"该自己装"，不然用户会以为少了个本该有的按钮。
        if (m_env.pkg == Nas::Pkg::None)
            hint += QString::fromUtf8("（系统里没有 dnf / apt-get / pacman，得自己装）");
        m_envLabel->setText(hint);
        m_envLabel->setStyleSheet("color:" + Theme::bad().name());
        m_envLabel->setVisible(true);
    }

    refreshBlockNote();

    // 通栏按钮：只在该出现的时候出现。装齐了就收起来（用户要的"装好就消失"），
    // 下次任何一次刷新（页面上的「刷新」、换引擎、重开程序）若又缺了会自己回来，
    // 所以这不是"永久消失"，只是"没事别占地方"。
    //
    // 判据只有"还有没有包可装"，不含 configfs / 内核模块 —— 那两样包管理器
    // 补不了，按钮对它们无能为力，留着只会让人点了个空。
    //
    // 装到一半不许隐藏：进程还活着的时候按钮消失，比留着更让人慌。
    if (m_installAllBtn) {
        const bool busy = m_installProc && m_installProc->state() != QProcess::NotRunning;
        const bool canInstall = m_env.pkg != Nas::Pkg::None && m_env.hasMissingDeps();
        m_installAllBtn->setVisible(canInstall || busy);
        if (busy) {
            m_installAllBtn->setToolTip(QString::fromUtf8("安装进行中，日志在下方…"));
        } else if (canInstall) {
            m_installAllBtn->setToolTip(
                QString::fromUtf8("包管理器：%1\n缺：%2\n"
                                  "点一下先看确认框（列出会跑哪几条命令），再输 sudo 密码。")
                    .arg(Nas::pkgManagerName(m_env.pkg),
                         m_env.missingDepLabels.join(QString::fromUtf8("、"))));
        }
    }
}

// 仅供离线探针：塞进一个假的环境结果，然后走**与线上完全相同**的那段同步
// 代码。刻意不复用探针自己的判断，否则测的是探针而不是页面。
void NasPage::setEnvForTest(const NasController::Env &env)
{
    m_env = env;
    syncEnvAndInstallUi();
}

// 仅供离线探针：见 naspage.h 上的说明。这里不调 refresh()，因为 refresh 会
// 重跑 lsblk 把灌进去的盘覆盖掉；要重算的只有画板与出口区这两件事。
void NasPage::setDisksForTest(const QVector<NasBoardWidget::Disk> &disks)
{
    m_disks = disks;
    m_sysMounts.clear();
    for (const NasBoardWidget::Disk &d : disks)
        if (!d.mountpoint.isEmpty()) m_sysMounts << d.mountpoint;
    m_board->setDisks(m_disks);
    updateSummary();
    syncExportMode(m_board->members().size());
}

void NasPage::refresh()
{
    m_env = m_ctl.probe();
    syncEnvAndInstallUi();

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
        const Nas::Engine eng = engineFromIndex(m_engine->currentIndex());
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
    plan.engine = engineFromIndex(m_engine->currentIndex());
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
        } else if (plan.engine == Nas::Engine::MirrorRs) {
            // 镜像目录同样落在挂载点里，由模型层拼（它顺手处理了卷名清洗与
            // "挂载点为空就返回空串"）。这里不自己拼字符串：拼法有两份，
            // 迟早和创建/同步那两处对不上，而对不上的表现是"同步的是另一个目录"。
            m.kind = Nas::MemberKind::MirrorDir;
            for (const NasBoardWidget::Disk &d : m_disks)
                if (d.device == dev) m.filePath = Nas::mirrorDirPath(d.mountpoint, plan.volumeName);
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
    // 本函数内部会 setChecked，触发 buttonToggled -> 又回到这里。加一层
    // 再入保护，保证一次用户/成员变化只重算一遍。
    if (m_syncingExportMode) return;
    m_syncingExportMode = true;
    struct G { bool &f; ~G(){ f = false; } } guard{ m_syncingExportMode };

    // 镜像引擎先单独算：它给的是**目录**，不是块设备，所以三条出口里只剩
    // SMB 一条是说得通的（iSCSI 没有块设备可导，"挂载目录"那条是给 nasrs
    // RAID0 卷的）。放在成员数规则之前，是为了让下面那几条规则保持原样 ——
    // 它们说的是"条带/SMB 不跨盘"这套 RAID0 的事实，混进镜像就全变了味。
    const bool mirror = engineFromIndex(m_engine->currentIndex()) == Nas::Engine::MirrorRs;

    // 三个出口的合法成员数各不相同，这是本函数唯一真正的规则来源：
    //   iSCSI  ≥1 —— 块级，单盘也能导（只是没条带）
    //   SMB    =1 —— 文件级，Samba 不跨盘条带
    //   挂载   ≥2 —— 这条出口的卖点就是"多盘 RAID0 挂成一个目录"，
    //                少于 2 个成员 nasrs 自己会拒，这里先挡下来
    const bool smbAllowed   = mirror ? true : (memberCount == 1);
    const bool mountAllowed = mirror ? false : (memberCount >= 2);
    const bool wasMount = m_modeMount->isChecked();

    m_modeIscsi->setEnabled(!mirror);
    m_modeSmb->setEnabled(smbAllowed);
    m_modeMount->setEnabled(mountAllowed);

    // 成员数变化可能让当前选中的出口失效，此时回落到本引擎下总是合法的那条：
    // 镜像回 SMB（唯一出口），其余回 iSCSI。
    const auto fallback = [this, mirror] {
        if (mirror) m_modeSmb->setChecked(true);
        else        m_modeIscsi->setChecked(true);
    };
    if (mirror && !m_modeSmb->isChecked()) fallback();
    if (!smbAllowed && m_modeSmb->isChecked())   fallback();
    if (!mountAllowed && m_modeMount->isChecked()) fallback();

    // 只有"刚刚才允许"时主动切过去，不做每敲一下都跳：
    //   掉到 1 盘 -> 建议 SMB；升到 ≥2 盘且原本是 iSCSI -> 建议挂载。
    // 用户手动选过别的就不抢（droppedToOne 那套同理）。镜像不参与这套
    // 建议：它没有得选，抢着切只会把用户刚看懂的界面搬走。
    const bool roseToMulti = (memberCount >= 2 && m_lastMemberCount < 2);
    const bool droppedToOne = (memberCount == 1 && m_lastMemberCount > 1);
    if (!mirror && smbAllowed && droppedToOne && !m_modeSmb->isChecked()) {
        m_modeSmb->setChecked(true);
        setStatus("只剩 1 个分区，已切到 SMB 文件共享（可手动切回 iSCSI）");
    } else if (!mirror && mountAllowed && roseToMulti && !wasMount && m_modeIscsi->isChecked()) {
        m_modeMount->setChecked(true);
        setStatus("已选多个分区，可挂成一个目录给影视服务扫（可切回 iSCSI）");
    }

    // 提示语按当前成员数说明"还能选哪些"，而不是只说选中的那个
    if (mirror)
        m_modeHint->setText(QString::fromUtf8(
            "镜像引擎：出口只有 SMB —— 它给的是目录，不是块设备。"
            "两份目录与「访问哪一份」在上方阵列区"));
    else if (memberCount == 0)
        m_modeHint->setText("先在上方点选分区：1 个可走 SMB，≥2 个可做 RAID（iSCSI 或挂目录）");
    else if (memberCount == 1)
        m_modeHint->setText("单个分区：SMB（文件级，最常用）或 iSCSI（块级）；挂载目录需 ≥2 盘");
    else
        m_modeHint->setText(QString("已选 %1 个分区：iSCSI RAID 或挂成 /mnt 目录；SMB 不跨盘条带")
                                .arg(memberCount));

    // 三选一显隐 + 按钮文案
    const bool smb   = m_modeSmb->isChecked();
    const bool mount = m_modeMount->isChecked();
    m_mirrorBox->setVisible(mirror);
    // 阵列区（名称 / 条带 / 镜像那两目录与同步）：真在拼阵列时才出现。
    // 镜像永远要（两份目录和名字是它的定义）；其余引擎只有 ≥2 个成员才有
    // 阵列 —— 单分区是直接导出那个分区，没有"拼"这一步，就不该有名字栏。
    m_arrayBox->setVisible(mirror || memberCount >= 2);
    // 条带粒度对镜像没有意义：它不条带，可用容量就是一份。
    m_arrayForm->setRowVisible(1, !mirror);
    // iSCSI 框现在真的只有 iSCSI 的事了（IQN / 监听地址 / 匿名 ACL）。
    // 这里不再需要那个 "|| mirror" 的例外和逐行藏：例外存在的唯一理由
    //（要给它留个填名字的地方）已经跟着「名称」搬走了。
    m_iscsiBox->setVisible(!smb && !mount && !mirror);
    m_smbBox->setVisible(smb);
    m_smbWarn->setVisible(smb);
    m_fsBox->setVisible(mount);
    // 那条"装包补不了的"说明按出口过滤（configfs / target 模块只有 iSCSI 用得着），
    // 所以换出口就要重算一次，不能等下一次整页刷新。
    refreshBlockNote();
    m_exportBtn->setText(smb   ? "② 导出 SMB 共享"
                         : mount ? "② 挂载到目录"
                                 : "② 导出 iSCSI");
    // 「②」的可用条件：有成员就允许点。之前唯一的开关藏在 onAllFinished 里、
    // 判据是"已经成功导过 iSCSI"，于是纯 SMB 与纯镜像的用法永远点不动它 ——
    // 那两个出口连 IQN 都没有。缺什么（Samba 没装、目录不存在、IQN 非法）
    // 由各自的出口给出具体错误，比把按钮一直灰着说得更清楚。
    m_exportBtn->setEnabled(memberCount >= 1);

    // SMB 表单预填：只在没被用户改过时填默认值，别覆盖用户输入
    if (smb && !mirror) {
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
    // 挂载目录预填：按卷名推 /mnt/<卷名>，只在空时填，不覆盖用户输入。
    // 用 defaultMountPoint：它保证返回值为空或必然合法，预填非法路径等于埋雷。
    if (mount && m_mountPoint->text().isEmpty()) {
        const QString suggest = Nas::defaultMountPoint(m_volName->text().trimmed());
        if (!suggest.isEmpty()) m_mountPoint->setText(suggest);
    }
    refreshSmbWarning();
    refreshRaidFsStatus();
    refreshMirrorBox();
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
    //
    // 镜像的成员数是定义的一部分（一主一备），不是偏好：1 个没有副本、
    // 3 个以上语义未定（谁是主、坏了顶哪份），所以按钮在这里就卡住，
    // 而不是等用户填完名字、按过确认之后才告诉他数量不对。
    bool canCreate = info.members >= 1 && !plan.volumeName.isEmpty();
    if (plan.engine == Nas::Engine::MirrorRs)
        canCreate = info.members == Nas::kMirrorMembers && !plan.volumeName.isEmpty();
    m_createBtn->setEnabled(canCreate);
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
        // 这里进的是 QMessageBox 的纯文本，写 **加粗** 只会让用户看见两颗星，
        // 反而把这句最要紧的话弄脏了。
        text += "\n\n其中含「破坏性」操作，可能不可逆。";
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
    // 建阵列要的是**引擎本身**。configfs 和内核 target 模块是导出 iSCSI 才
    // 需要的，原先一并挡在这里，会出现"两个按钮都点不动，理由却是 iSCSI 的
    // 前置条件"这种八竿子打不着的提示。
    if (plan.engine == Nas::Engine::Mdadm && !m_env.mdadm) {
        setStatus("缺 mdadm，建不了内核阵列（用页顶的一键安装）", true);
        return;
    }
    if (plan.engine == Nas::Engine::Raid0Rs && (!m_env.nasrsBinary || !m_env.losetup)) {
        setStatus("nasrs 引擎或 losetup 不在 —— 引擎随本程序一起发布，losetup 属 util-linux", true);
        return;
    }

    m_pendingConfirmName = plan.volumeName;

    QVector<NasController::Step> steps;
    if (plan.engine == Nas::Engine::MirrorRs) {
        // 镜像这条路在这里只做一件事：建两个目录。它不抹任何东西，所以
        // 上面那些"会抹分区"的守卫一条都不适用（mdadm 的、losetup 的都不是）。
        // 守卫用的是 mirrorDirs —— 和「同步」「查询」同一套，三处不会各说各话。
        QString p, s, why;
        if (!mirrorDirs(&p, &s, &why)) { setStatus(why, true); return; }
        steps = m_ctl.planMirrorCreate(plan);
        if (steps.isEmpty()) {
            setStatus("生成建目录命令失败（卷名或路径不合法）", true);
            return;
        }
        const QVector<NasController::Step> go =
            confirmSteps(steps, QString::fromUtf8("创建镜像阵列（建两个副本目录）"));
        if (go.isEmpty()) return;
        m_ctl.run(go);
        return;
    }

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
    // 出口三选一，跟着当前单选走。三个出口共用同一个「②」按钮，
    // 按钮文案已由 syncExportMode 改好，这里只负责分发。
    if (m_modeSmb->isChecked())        exportSmb();
    else if (m_modeMount->isChecked()) onMountFsClicked();
    else                               exportIscsi();
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

    // targetcli 不在就注定失败（modprobe 那几步倒是能过），在这里说清楚比
    // 让进程 FailedToStart 好。**不**在这里挡 configfs：python-rtslib 启动时
    // 会自己把 /sys/kernel/config 挂上，挡在门口反而拦住了一本来能成的导出。
    // configfs / target 模块的情况由导出页那条说明提示，不挡路。
    if (!m_env.targetCli) {
        setStatus("缺 targetcli，导不了 iSCSI（用页顶的一键安装）", true);
        return;
    }

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
    const RunKind kind = m_pendingRun;
    m_pendingRun = RunKind::None;

    if (success && !m_currentIqn.isEmpty())
        m_teardownBtn->setEnabled(true);
    // 注意这里**不再**是「②」的唯一开关：判据是 m_currentIqn 非空，而 SMB
    // 与镜像这条流压根没有 IQN，于是原来的写法让 ② 在纯 SMB 用法下永远是灰的
    // —— 只有先成功导一次 iSCSI，② 才会亮。真正的开关在 syncExportMode：
    // 有成员就允许点，具体缺什么由 exportSmb / exportIscsi 自己给出逐条错误。
    if (m_createBtn && !m_currentIqn.isEmpty())
        m_exportBtn->setEnabled(true);

    if (kind == RunKind::MirrorSync) {
        if (success) {
            // 同步进程自己说"拷完了"不算数，回查一次两份的差值再说"已一致"。
            // 多扫一遍目录的代价换来的是：这句话不是承诺，是实测。
            setMirrorPill(QString::fromUtf8("核对中…"), "pill-warn");
            m_mirrorState->setText(QString::fromUtf8("同步进程已结束，正在回查两份是否一致…"));
            QString p, s, why;
            if (mirrorDirs(&p, &s, &why))
                QTimer::singleShot(0, this, [this, p, s]() {
                    applyMirrorStatus(m_ctl.queryMirrorStatus(p, s));
                });
        } else {
            setMirrorPill(QString::fromUtf8("同步失败"), "pill-warn");
            m_mirrorState->setText(QString::fromUtf8("同步没有跑完，副本还是旧的"));
        }
    }

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
QWidget *NasPage::buildRaidFsBox()
{
    // 这是导出页里的一个出口框，不是独立标签页。所以不再自带整页标题和
    // 尾部 stretch —— 那些是标签页才需要的；出口框只负责"挂载目录"这块
    // 表单本身，显隐由 syncExportMode 统一管（与 m_iscsiBox/m_smbBox 一致）。
    auto *box = new QWidget();
    auto *v = new QVBoxLayout(box);
    v->setContentsMargins(0, 8, 0, 0);

    m_mountPoint = new QLineEdit();
    m_mountPoint->setPlaceholderText("/mnt/raid");
    m_mountPoint->setToolTip("只接受 /mnt 的直接子目录。挂到会遮内容：原目录里的东西在挂载期间看不见，卸载后回来。");

    auto *scope = new QLabel("限定在 /mnt 的直接子目录：这一层不会盖住系统运行需要的目录，也不会挂到已有分区的挂载点里面去（那样分区一旦没挂，数据会写进根盘的同名目录，极难发现）。");
    scope->setWordWrap(true);
    scope->setProperty("role", "mute");

    auto *form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->addRow("挂载到", m_mountPoint);
    form->addRow("", scope);

    m_fsPill = new QLabel("检查中…");
    m_fsPill->setProperty("role", "pill-warn");
    m_fsDeps = new QLabel();
    m_fsDeps->setWordWrap(true);
    m_fsDeps->setTextFormat(Qt::RichText);
    m_fsDeps->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *pillRow = new QHBoxLayout();
    pillRow->setContentsMargins(0, 0, 0, 0);
    pillRow->addWidget(m_fsPill);
    pillRow->addWidget(m_fsDeps, 1);
    // 一键安装按钮：默认隐藏。是否显示完全由 refreshRaidFsStatus 按
    // Env::canInstallFuse() 决定（apt 系 + apt 在 + fusermount3 缺）。
    // 装的是 fusermount3 这个挂载助手，不碰 /dev/fuse（那是内核模块，apt 帮不上）。
    m_installFuseBtn = new QPushButton("一键安装 fuse");
    m_installFuseBtn->setToolTip("用系统包管理器安装 fuse3（提供 fusermount3）。需要管理员授权。");
    m_installFuseBtn->setVisible(false);
    connect(m_installFuseBtn, &QPushButton::clicked, this, &NasPage::onInstallFuseClicked);
    pillRow->addWidget(m_installFuseBtn);
    form->addRow("依赖", pillRow);
    v->addLayout(form);

    m_fsNote = new QLabel();
    m_fsNote->setWordWrap(true);
    m_fsNote->setTextFormat(Qt::RichText);
    m_fsNote->setProperty("role", "note");
    v->addWidget(m_fsNote);

    // 卸载入口内联在这里、且排在说明之后：它是"挂上之后"才成立的撤销动作，
    // 跟着出口走，不放进底部按钮行（那里是跨出口的 ①/② 主流程，混在一起会
    // 让人误以为卸载和导出是同一层级的步骤）。
    auto *row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    row->addStretch(1);
    m_umountBtn = new QPushButton("卸载该目录");
    m_umountBtn->setToolTip("umount 挂载点。目录里原有的内容会重新显现。");
    m_umountBtn->setEnabled(false);
    connect(m_umountBtn, &QPushButton::clicked, this, &NasPage::onUmountFsClicked);
    row->addWidget(m_umountBtn);
    v->addLayout(row);

    refreshRaidFsStatus();
    return box;
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

    // 安装按钮：三条件全真才露脸（Env::canInstallFuse 已经合并判定）。
    // 装好之后 probe() 会重跑、fuseMount 变真，这里下次刷新就自动收起 ——
    // 不需要单独写"用完就消失"的逻辑，可见性永远跟着真实环境走。
    if (m_installFuseBtn)
        m_installFuseBtn->setVisible(m_env.canInstallFuse());
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

// 一键装 fuse。这里再判一次 canInstallFuse 不是冗余：按钮虽然只在为真时才
// 显示，但从显示到点击之间环境可能变（比如用户自己在别处装了）。守卫放这，
// 保证真正跑 pkexec 的入口永远先过一遍条件。
void NasPage::onInstallFuseClicked()
{
    if (!m_env.canInstallFuse()) {
        setStatus("当前不适合一键安装（非 apt 系，或缺少 apt，或 fuse 已就绪）", true);
        return;
    }
    const auto steps = m_ctl.planInstallFuse();
    const auto go = confirmSteps(steps, "安装 fuse3");
    if (go.isEmpty()) return;
    m_ctl.run(go);
}

// ── 软件镜像（nasrs RAID1）──────────────────────────────────
//
// 这一格是本页少数"点下去真的会动数据、但不要 root"的地方。措辞因此全部
// 围绕两个数字：差几个文件、差多少字节。不写"已实现/未实现"那种工程话
// （raidfs 那格才需要写那个），也不写"同步成功"这种还没跑就先承诺的话。
//
// 三个决定，改界面之前先看：
//   · 路径只显示不给输入：这个值会进 nasrs 的 argv，而那条命令带删除能力。
//   · 顺序 = 用户在面板上点分区的先后：先点的是主副本（可写），后点的是副本。
//     不提供"交换"按钮，因为重排成员会影响 RAID0 的条带序号，一个控件两件事。
//   · 访问副本网时共享强制只读：副本会被下一次同步覆盖，访客写进去的东西
//     会在下一轮消失 —— 那比不给写危险得多。

QWidget *NasPage::buildMirrorBox()
{
    auto *box = new QWidget();
    auto *v = new QVBoxLayout(box);
    v->setContentsMargins(0, 8, 0, 0);

    auto *form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);

    m_mirrorPrimary = new QLabel();
    m_mirrorPrimary->setObjectName("mirrorPrimary");   // 探针按名字找它
    m_mirrorPrimary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_mirrorSecondary = new QLabel();
    m_mirrorSecondary->setObjectName("mirrorSecondary");
    m_mirrorSecondary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(QString::fromUtf8("主副本（写这里）"), m_mirrorPrimary);
    form->addRow(QString::fromUtf8("副本（同步过去）"), m_mirrorSecondary);

    m_mirrorAccess = new QComboBox();
    m_mirrorAccess->setObjectName("mirrorAccess");
    m_mirrorAccess->addItem(QString::fromUtf8("主副本 —— 默认，读写都在它上面"), 0);
    m_mirrorAccess->addItem(QString::fromUtf8("副本网 —— 主盘故障时切过来顶住"), 1);
    m_mirrorAccess->setToolTip(QString::fromUtf8(
        "这一项决定 SMB 共享把哪个目录交给访问者。\n"
        "副本网是上一次同步时的快照：切过去之后，这期间主副本上的新写入看不见。"));
    connect(m_mirrorAccess, &QComboBox::currentIndexChanged,
            this, &NasPage::onMirrorAccessChanged);
    form->addRow(QString::fromUtf8("访问哪一份"), m_mirrorAccess);

    m_mirrorPill = new QLabel(QString::fromUtf8("没查过"));
    m_mirrorPill->setObjectName("mirrorPill");
    m_mirrorPill->setProperty("role", "pill-warn");
    m_mirrorState = new QLabel(QString::fromUtf8("点右边那个按钮，看看两份差多少"));
    m_mirrorState->setObjectName("mirrorState");
    m_mirrorState->setWordWrap(true);
    m_mirrorState->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_mirrorState->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_mirrorCheckBtn = new QPushButton(QString::fromUtf8("查副本落后多少"));
    m_mirrorCheckBtn->setObjectName("mirrorCheckBtn");
    m_mirrorCheckBtn->setToolTip(QString::fromUtf8(
        "遍历两个目录逐项比对（nasrs mirror --report）。只读，不写任何文件；"
        "文件很多时要几十秒。"));
    connect(m_mirrorCheckBtn, &QPushButton::clicked, this, &NasPage::onMirrorCheckClicked);
    auto *stateRow = new QHBoxLayout();
    stateRow->setContentsMargins(0, 0, 0, 0);
    stateRow->addWidget(m_mirrorPill);
    stateRow->addWidget(m_mirrorState, 1);
    stateRow->addWidget(m_mirrorCheckBtn);
    form->addRow(QString::fromUtf8("同步状态"), stateRow);

    m_mirrorDeleteExtras = new QCheckBox(
        QString::fromUtf8("同步时删掉副本上「主网没有」的文件（默认不删）"));
    m_mirrorDeleteExtras->setToolTip(QString::fromUtf8(
        "默认关：主网误删的东西一旦同步过去，两份就都没了。\n"
        "保持关闭，多出来的旧文件只占空间，不伤人。"));
    // 勾了删除就顺手关掉定时：没人盯着的时候让机器自动删文件，
    // 出事的时候你不会知道，等知道的时候已经找不回来了。
    connect(m_mirrorDeleteExtras, &QCheckBox::toggled, this, [this](bool on) {
        if (on && m_mirrorAuto && m_mirrorAuto->isChecked()) {
            m_mirrorAuto->setChecked(false);
            setStatus("已关掉每 5 分钟自动同步：定时同步不做删除", true);
        }
    });

    m_mirrorAuto = new QCheckBox(QString::fromUtf8("每 5 分钟自动同步一次（只增不删）"));
    m_mirrorAuto->setToolTip(QString::fromUtf8(
        "到点就跑一轮同步，只拷新增与改过的文件，绝不删副本上的东西。\n"
        "上一轮没跑完会跳过本次，不会排第二个进程。"));
    connect(m_mirrorAuto, &QCheckBox::toggled, this, &NasPage::onMirrorAutoToggled);

    form->addRow(QString(), m_mirrorDeleteExtras);
    form->addRow(QString(), m_mirrorAuto);
    v->addLayout(form);

    // 成员组合的问题（不是 2 个、或两块盘是同一块）用红字，且只在有问题时出现。
    // 常驻说明走 setStatus，不占这条 —— 常驻的红字看两天就不看了。
    m_mirrorWarn = new QLabel();
    m_mirrorWarn->setObjectName("mirrorWarn");
    m_mirrorWarn->setWordWrap(true);
    m_mirrorWarn->setTextFormat(Qt::RichText);
    m_mirrorWarn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_mirrorWarn->setProperty("role", "bad");
    m_mirrorWarn->setVisible(false);
    v->addWidget(m_mirrorWarn);

    // 同步按钮排最后：它是"上面都填好之后"才成立的动作，和 SMB 那格里的
    // 「卸载该目录」同一层级，不放进底部的 ①/② 主流程行。
    auto *row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    row->addStretch(1);
    m_mirrorSyncBtn = new QPushButton(QString::fromUtf8("同步到副本"));
    m_mirrorSyncBtn->setObjectName("mirrorSyncBtn");
    m_mirrorSyncBtn->setToolTip(QString::fromUtf8(
        "把主副本完整拷到副本目录（只增不删，除非勾了上面那项）。\n"
        "普通用户权限就够，不要 root。"));
    connect(m_mirrorSyncBtn, &QPushButton::clicked, this, &NasPage::onMirrorSyncClicked);
    row->addWidget(m_mirrorSyncBtn);
    v->addLayout(row);

    m_mirrorTimer = new QTimer(this);
    m_mirrorTimer->setInterval(kMirrorAutoMs);
    connect(m_mirrorTimer, &QTimer::timeout, this, [this]() {
        // 上一轮还在跑就跳过这一次，而不是再排一个进程：两个 nasrs 同时往
        // 同一个副本目录写，会互相把对方正在写的临时文件当成陌生文件。
        if (m_ctl.busy()) return;
        QString p, s, why;
        if (!mirrorDirs(&p, &s, &why)) {
            m_mirrorTimer->stop();
            setStatus("定时同步已停：" + why, true);
            return;
        }
        const auto steps = m_ctl.planMirrorSync(p, s, false);   // 定时：永远只增不删
        if (steps.isEmpty()) {
            m_mirrorTimer->stop();
            setStatus("定时同步已停：生成命令失败（目录或引擎路径不合法）", true);
            return;
        }
        setMirrorPill(QString::fromUtf8("同步中…"), "pill-warn");
        m_pendingRun = RunKind::MirrorSync;
        m_ctl.run(steps);
    });

    refreshMirrorBox();
    return box;
}

void NasPage::setMirrorPill(const QString &text, const char *role)
{
    if (!m_mirrorPill) return;
    m_mirrorPill->setText(text);
    ::setRole(m_mirrorPill, role);
}

// 算出并校验两个副本目录。三个入口（同步、查询、定时器）共用同一套守卫，
// 免得其中一处漏了"目录正好等于挂载点"这种检查，另两处挡住了。
// why 一定带回给用户看的那句话 —— 返回 false 却不说话，按钮就成了哑弹。
bool NasPage::mirrorDirs(QString *primary, QString *secondary, QString *why) const
{
    const Nas::Plan plan = currentPlan();
    if (plan.engine != Nas::Engine::MirrorRs) {
        *why = QString::fromUtf8("当前不是镜像引擎");
        return false;
    }
    QVector<QString> devs;
    for (const Nas::Member &m : plan.members) devs << m.device;

    const QString warn = Nas::mirrorMembersWarning(devs);
    if (!warn.isEmpty()) { *why = warn; return false; }

    const QString p = plan.members.at(0).filePath;
    const QString s = plan.members.at(1).filePath;
    if (p.isEmpty() || s.isEmpty()) {
        *why = QString::fromUtf8("有成员所在分区没有挂载点：副本目录没有落脚处。"
                                 "先把它挂载上再刷新。");
        return false;
    }
    const QString pErr = Nas::validateMirrorDir(p);
    if (!pErr.isEmpty()) { *why = QString::fromUtf8("主副本目录：%1").arg(pErr); return false; }
    const QString sErr = Nas::validateMirrorDir(s);
    if (!sErr.isEmpty()) { *why = QString::fromUtf8("副本目录：%1").arg(sErr); return false; }

    // 与真实挂载表比一次。validateMirrorDir 只看形状，看不出
    // "这个路径恰好就是某个分区的挂载点" —— 而那种情况下删除多余文件
    // 清掉的是整个副本分区上原有的东西。
    QStringList mounts;
    for (const NasBoardWidget::Disk &d : m_disks)
        if (!d.mountpoint.isEmpty()) mounts << d.mountpoint;
    const QString pc = Nas::mirrorDirConflict(p, mounts);
    if (!pc.isEmpty()) { *why = QString::fromUtf8("主副本目录：%1").arg(pc); return false; }
    const QString sc = Nas::mirrorDirConflict(s, mounts);
    if (!sc.isEmpty()) { *why = QString::fromUtf8("副本目录：%1").arg(sc); return false; }

    if (!m_env.nasrsBinary) {
        *why = QString::fromUtf8("nasrs 引擎不在程序旁边 —— 它随本程序一起发布，不是发行版的包");
        return false;
    }
    *primary = p;
    *secondary = s;
    return true;
}

void NasPage::refreshMirrorBox()
{
    if (!m_mirrorPrimary) return;
    const bool mirror = engineFromIndex(m_engine->currentIndex()) == Nas::Engine::MirrorRs;
    if (!mirror) {
        // 换走引擎就把定时器关掉：一个看不见的进程还在往目录里写东西，
        // 是这一页最不该出现的事。
        if (m_mirrorAuto && m_mirrorAuto->isChecked()) m_mirrorAuto->setChecked(false);
        m_smbPath->setReadOnly(false);
        m_smbWrite->setEnabled(true);
        return;
    }

    const Nas::Plan plan = currentPlan();
    QString devs;
    auto labelFor = [&plan](int i) -> QString {
        if (i >= plan.members.size()) return QString();
        return plan.members.at(i).device;
    };
    const QString p = plan.members.size() > 0 ? plan.members.at(0).filePath : QString();
    const QString s = plan.members.size() > 1 ? plan.members.at(1).filePath : QString();

    // 两条路径长得几乎一样（都是 …/<卷名>.nas-mirror），只报路径分不清谁是谁，
    // 所以各自带上它所在的那个分区名。
    m_mirrorPrimary->setText(p.isEmpty()
        ? QString::fromUtf8("（先在上方点选第一个分区）") : p + "　· " + labelFor(0));
    m_mirrorSecondary->setText(s.isEmpty()
        ? QString::fromUtf8("（再点选第二个分区）") : s + "　· " + labelFor(1));
    Q_UNUSED(devs);

    QVector<QString> devList;
    for (const Nas::Member &m : plan.members) devList << m.device;
    const QString warn = Nas::mirrorMembersWarning(devList);
    m_mirrorWarn->setText(warn);
    m_mirrorWarn->setVisible(!warn.isEmpty());

    m_mirrorSyncBtn->setEnabled(warn.isEmpty() && !m_ctl.busy());
    m_mirrorCheckBtn->setEnabled(warn.isEmpty() && !m_ctl.busy());

    // 访问哪一份 → 决定共享路径。这里覆盖 m_smbPath 是有意的：镜像模式下
    // 那个框是只读的，被覆盖的只能是程序自己填的值，不会盖掉用户的手输。
    const bool accessSecondary = (m_mirrorAccess->currentIndex() == 1);
    const QString acc = accessSecondary ? s : p;
    m_smbPath->setReadOnly(true);
    if (!acc.isEmpty() && m_smbPath->text() != acc) m_smbPath->setText(acc);
    // 副本网强制只读：下一次同步会把主副本整份盖回来，
    // 访客写进去的东西会在下一轮无声消失 —— 那比根本不给写更危险。
    if (accessSecondary) {
        if (m_smbWrite->isChecked()) m_smbWrite->setChecked(false);
        m_smbWrite->setEnabled(false);
    } else {
        m_smbWrite->setEnabled(true);
    }
}

void NasPage::onMirrorAccessChanged(int idx)
{
    Q_UNUSED(idx)
    refreshMirrorBox();
    // 这句话就是"访问哪一份"这个下拉框的全部后果，放在这里说而不是写进
    // tooltip：tooltip 要鼠标停上去才看得到，而这里选错一次的代价是
    // "别人读到的是昨天的文件"，得让他当场看见。
    setStatus(Nas::mirrorRoleNote(m_mirrorAccess->currentIndex() == 0));
}

void NasPage::onMirrorAutoToggled(bool on)
{
    if (!m_mirrorTimer) return;
    if (on) {
        if (m_mirrorDeleteExtras->isChecked()) {
            // 这里拒绝而不是默默改成"不删"：勾了删除又开定时，说明用户想要的
            // 是"自动镜像站"，而那个组合本页面不提供给任何人自动执行。
            setStatus("定时同步不做删除。要用定时同步请先取消上面那个删除选项", true);
            m_mirrorAuto->setChecked(false);
            return;
        }
        m_mirrorTimer->start();
        setStatus(QString::fromUtf8("每 5 分钟自动同步一次（只增不删）已开启"));
    } else {
        m_mirrorTimer->stop();
        setStatus("定时同步已关闭");
    }
}

void NasPage::onMirrorSyncClicked()
{
    QString p, s, why;
    if (!mirrorDirs(&p, &s, &why)) { setStatus(why, true); return; }

    const bool del = m_mirrorDeleteExtras->isChecked();
    const auto steps = m_ctl.planMirrorSync(p, s, del);
    if (steps.isEmpty()) {
        setStatus("生成同步命令失败（目录名不合法，或引擎不在程序旁边）", true);
        return;
    }

    QVector<NasController::Step> go;
    if (del) {
        // 会删文件 → 走完整确认（逐条列命令 + 手输卷名），和建阵列同一档。
        m_pendingConfirmName = m_volName->text().trimmed();
        go = confirmSteps(steps, QString::fromUtf8("同步到副本（含删除多余文件）"));
    } else {
        // 只增不删不弹框：这一步不会让任何文件消失。每次同步都弹一框，
        // 用户学会的唯一一件事就是看都不看按回车 —— 那会把真要他看的那几次也一起废掉。
        go = steps;
    }
    if (go.isEmpty()) return;

    setMirrorPill(QString::fromUtf8("同步中…"), "pill-warn");
    m_mirrorState->setText(QString::fromUtf8("正在拷贝…文件多时要等一会儿，别关页面"));
    m_pendingRun = RunKind::MirrorSync;
    m_ctl.run(go);
}

void NasPage::onMirrorCheckClicked()
{
    QString p, s, why;
    if (!mirrorDirs(&p, &s, &why)) { setStatus(why, true); return; }

    setMirrorPill(QString::fromUtf8("查询中…"), "pill-warn");
    m_mirrorState->setText(QString::fromUtf8("正在遍历两个目录…文件多时要几十秒"));
    // 用 singleShot(0) 而不是就地查询：状态文字得先画出来，界面才有反应。
    // 查询本身是同步遍历磁盘的（见 queryMirrorStatus 的超时说明），
    // 就地跑的话这两句永远看不到 —— 用户只会觉得按钮点了没动静。
    QTimer::singleShot(0, this, [this, p, s]() {
        applyMirrorStatus(m_ctl.queryMirrorStatus(p, s));
    });
}

void NasPage::applyMirrorStatus(const NasController::MirrorStatus &st)
{
    m_mirrorState->setText(st.summaryLine());
    if (!st.parsed) {
        setMirrorPill(QString::fromUtf8("读不到"), "pill-warn");
        setStatus(st.error.isEmpty() ? QString::fromUtf8("读不到副本状态") : st.error, true);
        return;
    }
    if (!st.ok) {
        setMirrorPill(QString::fromUtf8("%1 个文件失败").arg(st.failed), "pill-warn");
    } else if (st.inSync()) {
        setMirrorPill(QString::fromUtf8("已一致"), "pill-ok");
    } else {
        setMirrorPill(QString::fromUtf8("落后 %1 个").arg(st.lagFiles), "pill-warn");
    }
    setStatus(st.summaryLine(), !st.ok);
    if (!st.messages.isEmpty())
        m_mirrorState->setText(st.summaryLine() + "\n· " + st.messages.join("\n· "));
}

// ── 页顶通栏：一键安装依赖的组件（dnf / apt-get / pacman）─────
//
// 为什么这一条走 sudo + 密码框，而本页其余 root 步骤走 pkexec：
// pkexec 只给一个授权对话框，装包那几十行输出（哪个在下、哪个装不上）没有
// 落点，失败了也只剩"退出码 1"一句。sudo -S 把进程留在我们自己手上，输出能
// 直接流进页面里的日志框。两条都是提权，没有另起第三套 —— 免密判定与
// "密码错了再问一次"的判据都复用 AdminHelper。
void NasPage::onInstallAllDepsClicked()
{
    if (m_installProc && m_installProc->state() != QProcess::NotRunning) {
        setStatus("上一次安装还在跑，等它结束再点", true);
        return;
    }
    // 重探一次再用：从页面刷到现在，用户可能自己装了包，也可能刚接上网络。
    // 要紧的是「确认框里看到的缺失项」和「真去装的那批」来自同一次探测。
    refresh();

    if (m_env.pkg == Nas::Pkg::None) {
        setStatus("系统里没有 dnf / apt-get / pacman，不能一键安装。缺："
                      + m_env.missingDepLabels.join("、"), true);
        return;
    }
    if (!m_env.hasMissingDeps()) {
        setStatus("依赖已齐备，不需要装", false);
        return;
    }

    const NasController::InstallPlan plan = m_ctl.planInstallDeps(m_env);
    if (!plan.valid()) {
        if (!plan.blocked.isEmpty())
            setStatus(QString("%1 在 %2 的官方仓库里没有对应的包，得手动解决")
                          .arg(plan.blocked.join("、"), plan.pmName), true);
        else
            setStatus("依赖清单没通过包名白名单，已中止（不该发生，请把本页日志发出来）", true);
        return;
    }
    m_installPlan = plan;

    // 确认框：把「缺什么 → 装哪个包」和「会跑哪几条命令」都摊开。
    // 装包会改整个系统的状态，看过命令再点确认才算真确认。
    QString text = QString("将用 %1 安装 %2 个组件（需要管理员权限）：\n")
                       .arg(plan.pmName).arg(plan.packages.size());
    for (const QString &r : plan.rows)
        text += "  · " + r + "\n";
    text += "\n会执行的命令（set -e：任一条失败就停）：\n";
    for (const QString &c : plan.commands)
        text += "  # " + c + "\n";
    if (!plan.blocked.isEmpty())
        text += "\n这几项它装不了（官方仓库没有对应的包，要自己想办法）：\n  · "
                + plan.blocked.join("\n  · ") + "\n";
    text += "\n会改动系统：装上软件包及其依赖。不碰磁盘、不改配置文件、不动已有的共享。";
    text += "\n装不上时会把包管理器的原话完整贴在页面日志里，不会假装成功。";

    if (QMessageBox::question(this, "一键安装依赖", text,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes) {
        setStatus("已取消，什么都没装", false);
        return;
    }

    m_installAttempts = 0;
    askInstallPassword();
}

// 取 sudo 密码。免密（sudo -n 直接过）就不问 —— 已经能免密的人还弹框，
// 只会教会用户"随手回车"，那比不弹更危险。
void NasPage::askInstallPassword()
{
    QString pw;
    if (!AdminHelper::passwordlessSudoAvailable()) {
        bool ok = false;
        const QString prompt = m_installAttempts == 0
            ? QString("请输入管理员密码（sudo）：")
            : QString("密码不对，再输一次：");
        pw = QInputDialog::getText(this, "管理员权限", prompt,
                                   QLineEdit::Password, QString(), &ok);
        if (!ok) { setStatus("已取消，什么都没装", false); return; }
    }
    startInstall(pw);
}

void NasPage::startInstall(const QString &password)
{
    m_installTimedOut = false;
    m_installLog->setVisible(true);
    m_installLog->clear();
    appendInstallLog(QString("# 用 %1 安装：%2")
                         .arg(m_installPlan.pmName, m_installPlan.packages.join(" ")));
    for (const QString &c : m_installPlan.commands)
        appendInstallLog("$ " + c);

    if (!m_installProc) {
        m_installProc = new QProcess(this);
        // 合流：apt/dnf 的进度和报错本来就交错出现，分成两栏就看不出先后。
        m_installProc->setProcessChannelMode(QProcess::MergedChannels);
        connect(m_installProc, &QProcess::readyReadStandardOutput,
                this, &NasPage::onInstallOutput);
        connect(m_installProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &NasPage::onInstallFinished);
        connect(m_installProc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
            if (e != QProcess::FailedToStart) return;
            m_installTimer->stop();
            m_installAllBtn->setEnabled(true);
            m_installAllBtn->setText(kInstallBtnText);
            appendInstallLog("!! 无法启动 sudo —— 系统里可能没装 sudo 命令。");
            setStatus("无法启动提权进程（sudo 不在？）", true);
        });
    }

    // 免密就走 -n：不加 -S，sudo 就不会去读 stdin，也不会等不到密码挂在那儿。
    if (password.isEmpty()) {
        m_installProc->start("sudo", { "-n", "bash", "-c", m_installPlan.script });
    } else {
        // -p "" 让 sudo 别打印提示语，否则那句"请输入密码"会混进日志第一行，
        // 看起来像是我们输出给用户的字。
        m_installProc->start("sudo", { "-S", "-p", "", "bash", "-c", m_installPlan.script });
        m_installProc->write((password + "\n").toUtf8());
        // 密码只经这一次 stdin：不落盘、不进日志、不存成成员变量
    }
    m_installProc->closeWriteChannel();

    m_installAllBtn->setEnabled(false);
    m_installAllBtn->setText("安装中…（输出见下方日志）");
    setStatus("安装中…");
    m_installTimer->start(kInstallTimeoutMs);
}

void NasPage::appendInstallLog(const QString &chunk)
{
    // \r 也算换行：包管理器用回车刷新同一行进度，不切开的话整个下载过程会
    // 变成一条几万字符的"单行日志"，在框里根本翻不动。
    static const QRegularExpression kNewline(QStringLiteral("[\\r\\n]"));
    const QStringList lines = chunk.split(kNewline, Qt::SkipEmptyParts);
    for (const QString &l : lines)
        m_installLog->appendPlainText(l.trimmed());
    m_installLog->verticalScrollBar()->setValue(m_installLog->verticalScrollBar()->maximum());
}

void NasPage::onInstallOutput()
{
    if (!m_installProc) return;
    const QString chunk = QString::fromUtf8(m_installProc->readAllStandardOutput());
    if (chunk.isEmpty()) return;
    appendInstallLog(chunk);
    // 状态行报最后一行非空输出：包管理器的进度就靠它说话，
    // 比"安装中…"三个字有用。
    static const QRegularExpression kNewline(QStringLiteral("[\\r\\n]"));
    const QStringList lines = chunk.split(kNewline, Qt::SkipEmptyParts);
    if (!lines.isEmpty())
        setStatus(QString("安装中… %1").arg(lines.last().trimmed()));
}

void NasPage::onInstallFinished(int exitCode, QProcess::ExitStatus status)
{
    m_installTimer->stop();
    m_installAllBtn->setEnabled(true);
    m_installAllBtn->setText(kInstallBtnText);

    const bool normal = (status == QProcess::NormalExit);
    if (normal && exitCode == 0) {
        appendInstallLog("# 完成");
        refresh();     // 重新探测：装完就该看到缺的少了几项
        if (m_env.missingDeps.isEmpty())
            setStatus("依赖已齐备", false);
        else
            setStatus("装完了，仍缺：" + m_env.missingDepLabels.join("、") + "（见下方日志）", true);
        return;
    }

    if (m_installTimedOut) { setStatus("安装超时，已中止", true); return; }

    // 密码错：只给重来一次的机会。判据是 AdminHelper 那张英文串表 —— sudo
    // 被本地化成中文时这里认不出来，表现是"不重试、直接报错"，而日志里仍是
    // sudo 的原话，不会把人引向错误结论。
    if (m_installAttempts < 1 && AdminHelper::isAuthFailure(m_installLog->toPlainText())) {
        ++m_installAttempts;
        setStatus("sudo 密码不对，再输一次", true);
        askInstallPassword();
        return;
    }

    if (!normal) { setStatus("安装进程异常结束（被信号杀死？）", true); return; }
    setStatus(QString("安装失败（退出码 %1），完整输出见下方日志").arg(exitCode), true);
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
