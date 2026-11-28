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
    case 1:  return Nas::Engine::MergerFs;
    case 2:  return Nas::Engine::MirrorRs;
    default: return Nas::Engine::Mdadm;
    }
}

// 策略下拉的下标 → MergerPolicy。与 engineFromIndex 同一套理由：三处要用
// （组 Plan、生成 mergerfs argv、状态栏那句后果），各写一遍三元迟早会写成
// "分区按 mfs 选的、命令按 eol 生成"。
static Nas::MergerPolicy policyFromIndex(int i)
{
    switch (i) {
    case 1:  return Nas::MergerPolicy::Ff;
    case 2:  return Nas::MergerPolicy::Lfs;
    default: return Nas::MergerPolicy::Mfs;
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
    // 整页套一层 QScrollArea。理由：这一页的内容只会越来越多（引擎、盘位画板、
    // 阵列参数、同盘告警、导出三选一、mergerfs 的说明与依赖表…），而页面高度是
    // 窗口给的固定值 —— 不滚动的话，窗口一矮，QFormLayout 的行就被压成一条缝，
    // 输入框只剩几个像素高（用户报的就是这个）。套上之后高度跟着内容走，窗口再
    // 矮也只是出现滚动条，控件本身的高度不再被挤。
    // 与 StoragePage 同一套路：顶层 QVBoxLayout + 可滚动容器；topbar 的内缩留在
    // 外框上，滚动区域从它下面开始（滚动内容不会钻到顶栏底下）。
    auto *pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(0, kTopbarInset, 0, 0);
    pageLayout->setSpacing(0);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    // 只要竖条，横条关掉。关得掉的依据是量出来的，不是猜的：这页内容的最小
    // 宽度是 876px（最宽的是「实现 + 缺件提示」那一行），而窗口最小宽度被
    // mainCol 的 1000px 顶着 —— 视口再怎么窄也窄不过 989px（1000 减竖条），
    // 内容始终放得下。也就是说横条在这页是一条永远不会出现的条：留着不但没用，
    // 还会在窗口被外部压窄时突然冒出来、白吃掉十几像素高度。
    // 代价要说清：没有横条兜底之后，万一将来某一行把内容顶得比视口宽，右侧就是
    // 被裁掉（不是省略号）。所以缺件提示那种会随机器变长的文字都挂了完整 tooltip。
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *content = new QWidget();
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(10, 0, 10, 8);
    layout->setSpacing(8);
    scroll->setWidget(content);
    pageLayout->addWidget(scroll, 1);

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
    m_engine->addItem("mergerfs 合并目录（第三方 FUSE）");
    m_engine->addItem("nasrs 镜像 RAID1（文件存两份，读其中一份）");
    // 三条路线的可用性差别很大，下拉框里就要说清。名字也不能骗人：mergerfs
    // 不是 RAID0 —— 一个文件整个落在一个分区上，没有条带，坏盘丢那块盘上的
    // 文件（不是整卷）。原先这一格写着"自研 RAID0 条带"，而那条自研路线的
    // 导出层从来没实现（算完布局就退出码 3），现在整条撤掉了。
    m_engine->setItemData(0, "内核 mdadm 建阵列，稳，但要 root 与 mdadm 包。", Qt::ToolTipRole);
    m_engine->setItemData(1, "第三方 mergerfs：把几个分区已有的挂载点并成一个目录。"
                             "普通用户权限，不建文件、不碰分区、不抹数据。"
                             "可用容量 = 各分区空闲之和。没有条带，也没有冗余：坏一块盘，"
                             "丢的是那块盘上的文件。需要 mergerfs 与 /dev/fuse。", Qt::ToolTipRole);
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
    m_modeMount->setToolTip("mergerfs 把选中的分区并成 /mnt 下的目录，影视服务能直接扫。不导 iSCSI。");
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
    connect(m_volName, &QLineEdit::textChanged, this, [this]() {
        refreshMirrorBox();
        // mergerfs 的合并挂载点与 SMB 共享路径同样由卷名推出来，少了这条，
        // "先点分区、后填名字"这个顺序会让两条路径标签一直停在占位文案，
        // 页面显示的路径和真去挂的那个不是同一个东西。
        refreshMergerBox();
    });

    m_chunk = new QSpinBox();
    m_chunk->setRange(4, 16384);
    m_chunk->setValue(512);
    m_chunk->setSuffix(" KiB");
    m_chunk->setToolTip("条带粒度。大文件顺序读：越大越好；小块随机写：越小越好。");
    m_arrayForm->addRow("条带粒度", m_chunk);   // 只对 mdadm 有意义，行按引擎显隐

    // mergerfs 没有条带，所以"条带粒度"这一格对它整行收掉（上面那行是 mdadm 的）。
    // 它唯一真正需要你选的参数是新文件落到哪个分区 —— 这就是策略。
    // 单独一行而不是复用条带那行：两条路线的语义不同，共用一行迟早写成
    // "同一格在不同引擎下是两种东西"，那种界面没人能预期。
    m_policy = new QComboBox();
    m_policy->setObjectName("mergerPolicy");
    m_policy->addItem(Nas::mergerPolicyLabel(Nas::MergerPolicy::Mfs));
    m_policy->addItem(Nas::mergerPolicyLabel(Nas::MergerPolicy::Ff));
    m_policy->addItem(Nas::mergerPolicyLabel(Nas::MergerPolicy::Lfs));
    m_policy->setItemData(0, Nas::mergerPolicyNote(Nas::MergerPolicy::Mfs), Qt::ToolTipRole);
    m_policy->setItemData(1, Nas::mergerPolicyNote(Nas::MergerPolicy::Ff), Qt::ToolTipRole);
    m_policy->setItemData(2, Nas::mergerPolicyNote(Nas::MergerPolicy::Lfs), Qt::ToolTipRole);
    m_policy->setToolTip(Nas::mergerPolicyNote(Nas::MergerPolicy::Mfs));
    connect(m_policy, &QComboBox::currentIndexChanged, this, [this](int) {
        refreshMergerBox();
        // 策略改变的是"新文件写到哪块盘"，这是选完必须当场看见的后果，
        // 不是 tooltip 里的一句话（tooltip 要鼠标停上去才看得到）。
        setStatus(Nas::mergerPolicyNote(policyFromIndex(m_policy->currentIndex())));
    });
    m_arrayForm->addRow("写入策略", m_policy);

    // 成员组合的问题（数量不对、两块盘是同一块）用红字，且只在有问题时出现。
    // 这一行**挂在阵列区**而不是镜像那格：镜像与 mergerfs 都可能撞上"选了两块盘、
    // 其实是一块盘上的两个分区"，分成两行会让用户在一行确认过之后以为另一行也
    // 确认过了。常驻说明走 setStatus，不占这条 —— 常驻的红字看两天就不看了。
    m_membersWarn = new QLabel();
    m_membersWarn->setObjectName("membersWarn");
    m_membersWarn->setWordWrap(true);
    m_membersWarn->setTextFormat(Qt::RichText);
    m_membersWarn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_membersWarn->setProperty("role", "bad");
    m_membersWarn->setVisible(false);

    // 同盘那条红字右边一个「知道了」：它把"零冗余"从硬性拦截变成**用户显式接受**。
    // 这条不是结构问题（数据是好的），是安全告警 —— 拿两个分区当同一块盘上的
    // 备份目录是正当用法，防误删有用、防坏盘没用；mergerfs 同理，同一块盘上的
    // 大目录照样能装片子，只是没有加速也没有冗余。硬性挡死等于替用户决定他
    // 不需要这个用法，而他只是想要一个目录。
    // 数量那条**不给**「知道了」：谁是主、坏了顶哪份都没定义，
    // 点一下不会让语义变清楚。
    m_membersAckBtn = new QPushButton(QString::fromUtf8("知道了"));
    m_membersAckBtn->setObjectName("membersAckBtn");
    m_membersAckBtn->setToolTip(QString::fromUtf8(
        "把「同一块盘」这件事确认下来，同步、查副本、挂载随之放行。\n"
        "记住的是这一组分区 —— 换了分区组合就要重新确认，不是一次性设置。"));
    m_membersAckBtn->setVisible(false);

    auto *warnRowW = new QWidget();
    auto *warnRow = new QHBoxLayout(warnRowW);
    warnRow->setContentsMargins(0, 0, 0, 0);
    warnRow->addWidget(m_membersWarn, 1);
    warnRow->addWidget(m_membersAckBtn);
    m_arrayForm->addRow(QString(), warnRowW);
    connect(m_membersAckBtn, &QPushButton::clicked, this, &NasPage::onMembersAckSameDisk);

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

    // mergerfs（第三方 FUSE 合并目录）的参数：哪几个分区的挂载点被并起来、
    // 并成哪个目录。写入策略那一行在阵列表单里（与条带粒度同层、按引擎显隐）。
    m_mergerBox = buildMergerBox();
    m_mergerBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    arrayV->addWidget(m_mergerBox);
    m_mergerBox->setVisible(false);

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
    //  nasrs  只在分区已有的文件系统里放东西（镜像放一个目录），mergerfs 更干脆：
    //         不碰分区本身
    // 卡口数量：mergerfs 至少 2 个成员才有意义，镜像必须正好 2 个（一主一备）。
    updateSummary();
}

// 装包补不了的几样前置条件。每条都带上"那你自己该怎么补"，因为这条链的
// 尽头没有按钮可点 —— 不给处置办法的红字只会让人去搜错误。
//
// 按当前出口过滤：configfs 与内核 target 模块是 LIO/iSCSI 才需要的，
// 选 SMB 或挂载目录时摆出来就是无关噪音；nasrs 只服务镜像那格，所以只在那格相关。
// mergerfs **不在这里**：它是发行版的包（apt 家有），走页顶的一键安装那条链，
// 混进来会变成"按钮说装不了、红字又叫你自己补"，而你自己补不了包名。
QStringList NasPage::nonPackageBlockers() const
{
    QStringList b;
    const bool iscsi = !m_modeSmb || m_modeIscsi->isChecked();
    if (iscsi && !m_env.configfsMounted)
        b << QString::fromUtf8("configfs 未挂载 —— sudo mount -t configfs configfs /sys/kernel/config");
    if (iscsi && !m_env.kernelModules)
        b << QString::fromUtf8("内核目录里没找到 target_core_mod 模块文件"
                               "（若内核已把 LIO 编进去，这条可以忽略）");
    if (m_engine && engineFromIndex(m_engine->currentIndex()) == Nas::Engine::MirrorRs
        && !m_env.nasrsBinary)
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
    const QStringList installable = Nas::packagesToInstall(m_env.pkg, m_env.missingDeps);
    // mergerfs 在这两家没有包，但能从源码编（见 Nas::mergerfsSourceInstallable）。
    // 按钮的判据要把它算进"装得动"，否则 dnf 系永远看不见这个按钮 —— 而那边
    // 恰恰是唯一需要它的地方。
    const bool canBuildMerger = Nas::mergerfsSourceInstallable(m_env.pkg, m_env.missingDeps);
    if (lack.isEmpty()) {
        m_envLabel->setVisible(false);
    } else {
        QString hint = "缺少：" + lack.join("、");
        // 认不出包管理器时通栏按钮不存在（它无能为力），这句"缺什么"就成了
        // 唯一的信号，得在这里说清"该自己装"，不然用户会以为少了个本该有的按钮。
        if (m_env.pkg == Nas::Pkg::None)
            hint += QString::fromUtf8("（系统里没有 dnf / apt-get / pacman，得自己装）");
        else if (installable.isEmpty() && !canBuildMerger)
            // 表里有这一项、这家仓库里却没有对应的包（mergerfs 在 dnf/pacman 系
            // 就是这种）：按钮点了也不会有确认框，只有一行红字容易被当成"程序卡了"。
            // 这里把"按钮帮不上"写在同一个标签里，不留"看着像坏了"的观感。
            hint += QString::fromUtf8("（%1 的官方仓库里没有这些的包，页顶按钮帮不上，要自己装）")
                        .arg(Nas::pkgManagerName(m_env.pkg));
        m_envLabel->setText(hint);
        // 整页的横向滚动条已关（见构造函数）：万一这句被列到比窗口还宽，右侧是
        // 直接裁掉、不是省略号，鼠标停一下能读全。正常情况下这句不会长到那么宽。
        m_envLabel->setToolTip(hint);
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
        // 判据是"还有没有**这家能装的**包"，不是"有没有缺的东西"。
        // 早先写的是后者，后果是：mergerfs 在 dnf/pacman 系永远缺、永远没有
        // 对应的包，按钮永远摆在那儿，点下去只有一行红字 —— 用户读成"程序没反应"。
        const bool canInstall = m_env.pkg != Nas::Pkg::None
                                && (!installable.isEmpty() || canBuildMerger);
        m_installAllBtn->setVisible(canInstall || busy);
        if (busy) {
            m_installAllBtn->setToolTip(QString::fromUtf8("安装进行中，日志在下方…"));
        } else if (canInstall) {
            QString tip = QString::fromUtf8("包管理器：%1\n缺：%2\n"
                                            "点一下先看确认框（列出会跑哪几条命令），再输 sudo 密码。")
                              .arg(Nas::pkgManagerName(m_env.pkg),
                                   m_env.missingDepLabels.join(QString::fromUtf8("、")));
            if (canBuildMerger && installable.isEmpty())
                tip += QString::fromUtf8("\nmergerfs 这家没有现成的包：会从源码编（自带 libfuse，"
                                         "要 g++/make，几分钟）。");
            m_installAllBtn->setToolTip(tip);
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
    // 超过 8 路的条带任一盘故障即整卷失效，收益早就不对了。
    // 话里不写"RAID0"：这一格现在是 mergerfs，它没有条带，任一盘故障只丢
    // 那块盘上的文件。写 RAID0 会让用户以为 mergerfs 也是"坏一块全完"。
    if (m.size() >= 8) {
        setStatus("最多 8 个成员；盘再多也不建议继续加", true);
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
    plan.mergerPolicy = policyFromIndex(m_policy->currentIndex());
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
        if (plan.engine == Nas::Engine::MergerFs) {
            // mergerfs 的 source 就是分区**已有的挂载点**：不建文件、不碰分区。
            // 没挂载就没有 source —— 这也是这一路线对"已挂载"的判定与 mdadm
            // 完全相反的原因（见 nasmodel.cpp::judge）。
            m.kind = Nas::MemberKind::SourceDir;
            for (const NasBoardWidget::Disk &d : m_disks)
                if (d.device == dev) m.filePath = d.mountpoint;
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

    // 三条路线的出口各不相同，先把引擎算出来再谈成员数：
    //   mdadm    块设备 → iSCSI；单分区也能直接共享它的挂载点
    //   mergerfs 目录 → 只有 SMB 与"挂到 /mnt 的目录"，没有块设备可导 iSCSI
    //   镜像     目录 → 只有 SMB
    const Nas::Engine eng = engineFromIndex(m_engine->currentIndex());
    const bool mirror = eng == Nas::Engine::MirrorRs;
    const bool merger = eng == Nas::Engine::MergerFs;

    // 三个出口的合法成员数各不相同，这是本函数唯一真正的规则来源：
    //   iSCSI  ≥1 且只属于 mdadm —— 块级，单盘也能导（只是没条带）
    //   SMB    mdadm 要正好 1；两条目录路线恒真 —— 文件级，Samba 不跨盘条带
    //   挂载   ≥2 且只有 mergerfs —— 这条出口的卖点就是"多盘并成一个目录"
    const bool iscsiAllowed = !mirror && !merger;
    const bool smbAllowed   = mirror || merger || (memberCount == 1);
    const bool mountAllowed = merger && memberCount >= 2;
    const bool wasMount = m_modeMount->isChecked();

    m_modeIscsi->setEnabled(iscsiAllowed);
    m_modeSmb->setEnabled(smbAllowed);
    m_modeMount->setEnabled(mountAllowed);
    // **点不动的那一格必须自己说为什么**。光置灰不够：主题的置灰只是文字淡一档、
    // 指示圈几乎没变，用户分不清自己在跟规则较劲还是界面坏了 —— 报上来的原话
    // 就是"选了两个分区，只能到 SMB，点了 RAID 挂载没有反应"。真相是那时选着
    // 镜像引擎（它给的是目录，出口只有 SMB），可界面一个字都没说。
    m_modeIscsi->setToolTip(iscsiAllowed
        ? QString::fromUtf8("导出为 iSCSI LUN，发起端拿到的是裸块设备，自己格式化。")
        : QString::fromUtf8(merger
            ? "点不动：mergerfs 交出来的是一个目录，没有块设备，没有 LUN 可导。"
            : "点不动：镜像给的是目录，不是块设备，没有 LUN 可导。"));
    m_modeSmb->setToolTip(smbAllowed
        ? QString::fromUtf8(merger
            ? "用 Samba 共享 mergerfs 合并出来的那个目录（/mnt 下，程序算好）。只读文件，不碰分区数据。"
            : "用 Samba 把该分区的挂载点作为共享目录导出。只读文件，不碰分区数据。")
        : QString::fromUtf8("点不动：Samba 不跨盘条带，SMB 只在正好 1 个分区时可用（现在 %1 个）。"
                            "多盘请走 iSCSI 或挂载目录。").arg(memberCount));
    m_modeMount->setToolTip(mountAllowed
        ? QString::fromUtf8("mergerfs 把选中的分区并成 /mnt 下的一个目录，影视服务能直接扫。"
                            "没有条带，也没有冗余 —— 一个文件整个落在一个分区上。")
        : merger
            ? QString::fromUtf8("点不动：mergerfs 至少 2 个分区才有意义（1 个就是直连单盘，"
                                "现在 %1 个）。").arg(memberCount)
            : mirror
                ? QString::fromUtf8("点不动：这一格是 mergerfs 的。镜像两份读的都是目录本身，"
                                    "出口只有 SMB。")
                : QString::fromUtf8("点不动：把 mdadm 卷挂到 /mnt 这一步本程序没实现。"
                                    "请走 iSCSI，或在别处自己 mount /dev/mdX。"));

    // 成员数或引擎变化可能让当前选中的出口失效，此时回落到本引擎下总是合法的那条：
    // 两条目录路线回 SMB（mergerfs 的 SMB 永远说得通），mdadm 回 iSCSI。
    const auto fallback = [this, eng] {
        if (eng == Nas::Engine::Mdadm) m_modeIscsi->setChecked(true);
        else                           m_modeSmb->setChecked(true);
    };
    if (!iscsiAllowed && m_modeIscsi->isChecked())   fallback();
    if (!smbAllowed && m_modeSmb->isChecked())   fallback();
    if (!mountAllowed && m_modeMount->isChecked()) fallback();

    // 只有"刚刚才允许"时主动切过去，不做每敲一下都跳：
    //   掉到 1 盘 -> 建议 SMB；升到 ≥2 盘 -> mergerfs 建议并成目录。
    // 用户手动选过别的就不抢（droppedToOne 那套同理）。镜像不参与这套
    // 建议：它没有得选，抢着切只会把用户刚看懂的界面搬走。
    const bool roseToMulti = (memberCount >= 2 && m_lastMemberCount < 2);
    const bool droppedToOne = (memberCount == 1 && m_lastMemberCount > 1);
    if (!mirror && smbAllowed && droppedToOne && !m_modeSmb->isChecked()) {
        m_modeSmb->setChecked(true);
        setStatus("只剩 1 个分区，已切到 SMB 文件共享（可手动切回别的出口）");
    } else if (mountAllowed && roseToMulti && !wasMount) {
        m_modeMount->setChecked(true);
        setStatus("已选多个分区，mergerfs 可以把它们并成一个目录给影视服务扫（可切回 SMB）");
    }

    // 提示语按当前引擎与成员数说明"还能选哪些"，而不是只说选中的那个
    if (mirror)
        m_modeHint->setText(QString::fromUtf8(
            "镜像引擎：出口只有 SMB —— 它给的是目录，不是块设备。"
            "两份目录与「访问哪一份」在上方阵列区"));
    else if (merger)
        m_modeHint->setText(QString::fromUtf8(
            "mergerfs：出口是 SMB 或挂成 /mnt 下的目录（它给的是目录，没有块设备可导 iSCSI）。"
            "source 与写入策略在上方阵列区；可用容量 = 各分区空闲之和，没有条带也没有冗余。"));
    else if (memberCount == 0)
        m_modeHint->setText("先在上方点选分区：1 个可走 SMB，≥2 个可做 mdadm RAID（iSCSI）");
    else if (memberCount == 1)
        m_modeHint->setText("单个分区：SMB（文件级，最常用）或 iSCSI（块级）；换 mergerfs 需要 ≥2 个分区");
    else
        m_modeHint->setText(QString("已选 %1 个分区：mdadm 走 iSCSI；要并成一个目录就换 mergerfs")
                                .arg(memberCount));

    // 三选一显隐 + 按钮文案
    const bool smb   = m_modeSmb->isChecked();
    const bool mount = m_modeMount->isChecked();
    m_mirrorBox->setVisible(mirror);
    m_mergerBox->setVisible(merger && memberCount >= 2);
    // 阵列区（名称 / 条带 / 写入策略 / 两份目录与同步）：真在拼阵列时才出现。
    // 镜像永远要（两份目录和名字是它的定义）；其余引擎只有 ≥2 个成员才有
    // 阵列 —— 单分区是直接导出那个分区，没有"拼"这一步，就不该有名字栏。
    m_arrayBox->setVisible(mirror || memberCount >= 2);
    // 条带粒度只对 mdadm 有意义；mergerfs 不条带，它那行是写入策略。
    m_arrayForm->setRowVisible(1, eng == Nas::Engine::Mdadm && memberCount >= 2);
    m_arrayForm->setRowVisible(2, merger && memberCount >= 2);
    // iSCSI 框现在真的只有 iSCSI 的事了（IQN / 监听地址 / 匿名 ACL）。
    // 这里不再需要那个 "|| mirror" 的例外和逐行藏：例外存在的唯一理由
    //（要给它留个填名字的地方）已经跟着「名称」搬走了。
    m_iscsiBox->setVisible(!smb && !mount && !mirror && !merger);
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
    refreshMergerBox();
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
    // mergerfs 的下限是 2：1 个 source 就是直连单盘，没有"合并"这件事。
    bool canCreate = info.members >= 1 && !plan.volumeName.isEmpty();
    if (plan.engine == Nas::Engine::MirrorRs)
        canCreate = info.members == Nas::kMirrorMembers && !plan.volumeName.isEmpty();
    if (plan.engine == Nas::Engine::MergerFs)
        canCreate = info.members >= Nas::kMergerMinMembers && !plan.volumeName.isEmpty();
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

    // mergerfs 要的是**第三方二进制 + FUSE**，与 nasrs 无关（nasrs 只服务镜像）。
    // 这里只报"缺什么"，不猜命令：mergerfs 在 openEuler / Fedora 的仓库里没有
    // （实测 dnf 报 No matching Packages），页顶的一键安装会老实说这家装不了。
    if (plan.engine == Nas::Engine::MergerFs) {
        QStringList srcs;
        QString why;
        if (!mergerDirs(&srcs, mergerMountPoint(), &why)) { setStatus(why, true); return; }
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

    if (plan.engine == Nas::Engine::MergerFs) {
        // ① 在这里只做一件事：建那个挂载目录。不挂任何东西、不碰分区，
        // 所以可撤销，也不需要"手输核对"（confirmSteps 里非破坏性那条只问一次）。
        const QString mp = mergerMountPoint();
        steps = m_ctl.planMergerfsCreate(plan, mp);
        if (steps.isEmpty()) {
            setStatus("生成建目录命令失败（名称或 source 路径不合法）", true);
            return;
        }
        const QVector<NasController::Step> go =
            confirmSteps(steps, QString::fromUtf8("创建 mergerfs 卷（建挂载目录 %1）").arg(mp));
        if (go.isEmpty()) return;
        m_ctl.run(go);
        setStatus(QString::fromUtf8("目录 %1 已建好。下一步「② 挂到目录（mergerfs）」才真把"
                                    "几个分区并成它 —— 挂载会在它上面盖一层，"
                                    "目录里原有的内容会暂时看不见，卸载后回来。").arg(mp));
        return;
    }

    if (plan.engine == Nas::Engine::Mdadm)
        steps = m_ctl.planMdadmCreate(plan, plan.volumeName);

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
    //   mdadm    → /dev/md-<卷名>（mdadm --create 建出来的就是这个名字）
    //   mergerfs → 一个目录，没有块设备，没有 LUN 可导 —— 这里必须明说，
    //              不能拿 source 路径冒充块设备（自研 RAID0 那条曾想这么接 loop）
    //   镜像     → 同上，两份都是目录
    QString blockDev;
    if (plan.engine == Nas::Engine::Mdadm) {
        blockDev = "/dev/md-" + plan.volumeName;
        if (!QFileInfo::exists(blockDev)) {
            setStatus(QString("%1 不存在，先创建阵列").arg(blockDev), true);
            return;
        }
    } else {
        setStatus(plan.engine == Nas::Engine::MergerFs
            ? "mergerfs 交出来的是一个目录，没有块设备可导 iSCSI —— 这一格请走 SMB 或挂载目录"
            : "镜像交出来的是目录，没有块设备可导 iSCSI —— 这一格请走 SMB", true);
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

// ── mergerfs 挂载目录 ──────────────────────────────────────
//
// 用户要的出口：把几个分区并成一个 /mnt 下的目录，让影视服务直接扫。
// iSCSI 那条走到块设备就结束了，Jellyfin 这类服务拿不到文件，所以需要这一格。
//
// 这一格原来是自研 nasrs 的 RAID0，那条的导出层从来没实现（算完布局就退出码 3），
// 所以徽标曾写"引擎未实现"。现在换成第三方 mergerfs，命令真能跑 —— 徽标改写成
// 环境状态，**别再挂"未实现"**：那句话在新的事实上会说谎，也会把能用的东西
// 说成不能用。同时不叫它 RAID0：没有条带、没有冗余，可用容量是各分区之和。
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
    m_umountBtn->setEnabled(false);   // 由 refreshRaidFsStatus 跟着真实状态开
    connect(m_umountBtn, &QPushButton::clicked, this, &NasPage::onUmountFsClicked);
    row->addWidget(m_umountBtn);
    v->addLayout(row);

    refreshRaidFsStatus();
    return box;
}


// 把"能不能挂"讲清楚。这一格现在是 mergerfs（第三方 FUSE 合并目录），
// 自研 RAID0 那条已经整条撤掉，所以这里不再写"引擎未实现" —— 那句话在
// 这一格上挂了很久，它说的事实是真的（nasrs 只算布局、退出码 3），现在
// 事实变了，界面必须跟着改：命令真能跑，缺的就只剩环境。
//
// 环境层三样：mergerfs 二进制、/dev/fuse、fusermount3。前一样靠包管理器，
// 后两样属内核与 fuse 包。mergerfs **在 openEuler / Fedora 的仓库里没有**
// （实测 dnf 报 No matching Packages），所以这一格对 dnf 系只可能显示
// "检测不到"，不能摆一个必然失败的安装按钮 —— 表里那条 dnf 空列表就是为此。
void NasPage::refreshRaidFsStatus()
{
    if (!m_fsPill) return;

    struct Row { const char *name; bool has; const char *why; };
    const QList<Row> rows = {
        { "mergerfs",     m_env.mergerfsBinary, "第三方联合文件系统，本程序只检测、不捆绑" },
        { "/dev/fuse",    m_env.fuseDevice,  "内核 FUSE 支持" },
        { "fusermount3",  m_env.fuseMount,   "非 root 挂载助手（apt install fuse3）" },
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

    // 徽标只写状态本身。mergerfs 缺是"环境缺"，不是"未实现"：这条区别必须
    // 保住 —— 未实现时徽标写未实现，环境齐了也不会变绿。
    if (m_env.readyForMerger()) {
        setRole(m_fsPill, "pill-ok");
        m_fsPill->setText(QString::fromUtf8("mergerfs 已就位"));
    } else {
        setRole(m_fsPill, "pill-warn");
        m_fsPill->setText(m_env.mergerfsBinary
            ? QString::fromUtf8("缺 FUSE 挂载助手")
            : QString::fromUtf8("检测不到 mergerfs"));
    }
    QStringList notes;
    notes << QString::fromUtf8(
        "<b>mergerfs 不是 RAID0</b>：一个文件整个落在一个分区上，没有条带；"
        "可用容量 = 各分区空闲之和；坏一块盘，丢的是那块盘上的文件，不是整卷。");
    if (!m_env.mergerfsBinary)
        notes << QString::fromUtf8(
            "本机没有 mergerfs。页顶的「一键安装」会按系统能力选路：Debian/Ubuntu 的仓库里"
            "有现成的包，直接装；openEuler / Fedora 的仓库里没有（实测 dnf 报 "
            "No matching Packages），Arch 只在 AUR —— 那两家它会**从源码编一个**：mergerfs "
            "的源码自带 vendored libfuse（作者改过的 fork），所以不需要发行版的 fuse3 开发头，"
            "只要 g++、make、curl，编完装到 /usr/local/bin/mergerfs（十几兆的中间产物会清掉）。\n"
            "编完这一格就变成「mergerfs 已就位」——程序按「同目录 → ../lib → /usr/local/bin → "
            "/usr/bin → PATH」找它，装完不用重启程序，刷新一下就认。");
    if (!m_env.fuseDevice)
        notes << QString::fromUtf8(
            "<b>/dev/fuse 不在</b>：那属内核 fuse 模块，装包补不了，"
            "需要先加载 fuse 模块（modprobe fuse）。");
    notes << QString::fromUtf8(
        "挂载目录现在就能填：路径是否合法、会不会遮住现有内容，这些与 mergerfs 无关，"
        "现在就校验并报错。挂载会在目录上盖一层，原有内容暂时看不见，卸载后回来。");
    m_fsNote->setText(notes.join("<br>"));

    // 卸载按钮跟着"这一格在不在、挂载点合不合法"走。它以前在构造时置灰之后
    // 就再没人动过 —— 这一格换成真能跑的 mergerfs 之后，那条灰就说谎了：
    // 用户挂上了，却找不到撤销的地方。
    if (m_umountBtn)
        m_umountBtn->setEnabled(m_env.mergerfsBinary
            && Nas::validateMountPoint(m_mountPoint->text().trimmed()).isEmpty());
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

    // 4) mergerfs 的守卫：source 列表、确认闸（同盘要显式接受）、写入策略、
    //    mergerfs 二进制、/dev/fuse、fusermount3。与 ① 创建共用 mergerDirs，
    //    两处不会各说各话。
    const Nas::Plan plan = currentPlan();
    if (plan.engine != Nas::Engine::MergerFs) {
        setStatus("这一格是 mergerfs 的：换到 mergerfs 引擎再挂。"
                  "mdadm 卷要挂成目录，请走 iSCSI 或在别处自己 mount /dev/mdX", true);
        return;
    }
    QStringList srcs;
    QString why;
    if (!mergerDirs(&srcs, mp, &why)) { setStatus(why, true); return; }

    const auto steps = m_ctl.planMergerfsMount(plan, mp);
    if (steps.isEmpty()) { setStatus("命令生成失败：参数不合法", true); return; }

    // 引擎没实现，所以这里不走 confirmSteps 真的执行 —— 那是骗人。
    // 改成把命令逐条列出来给人看并明说跑不通：流程可审，但不假装能用。
    // 这一条是**真执行**的。自研 RAID0 那条的导出层从来没实现（nasrs 算完布局
    // 就退出码 3），所以这一格原先只列命令、明说不会跑。mergerfs 是现成的
    // 第三方二进制，命令形状清楚，走 confirmSteps 真跑。
    const QVector<NasController::Step> go =
        confirmSteps(steps, QString::fromUtf8("mergerfs：把 %1 个分区并成 %2")
                                  .arg(srcs.size()).arg(mp));
    if (go.isEmpty()) return;
    m_ctl.run(go);
    setStatus(QString::fromUtf8("已挂 %1（策略 %2）。可用 = 各分区空闲之和；"
                                "一个文件整个落在一个分区上，没有条带、没有冗余。")
                  .arg(mp, Nas::mergerPolicyLabel(plan.mergerPolicy)));}

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
// ── mergerfs（第三方 FUSE 合并目录，RAID0 那格现在用它）───────
//
// 三个决定，与镜像那格同一套理由：
//   · source 与合并挂载点只显示、不给输入：这两个值会进 mergerfs 的 argv。
//     给输入框就等于允许把一个 source 写成 / 或某个挂载点本身。
//   · 顺序 = 用户点分区的先后。mergerfs 的 source 列表顺序**有意义**
//     （eol 策略下排第一的先被写满），不是摆设，所以不提供"交换"按钮。
//   · 这一格不写"可用容量 = 各分区之和"之外的承诺：没有条带、没有冗余。
QWidget *NasPage::buildMergerBox()
{
    auto *box = new QWidget();
    auto *v = new QVBoxLayout(box);
    v->setContentsMargins(0, 8, 0, 0);

    auto *form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);

    m_mergerSources = new QLabel();
    m_mergerSources->setObjectName("mergerSources");
    m_mergerSources->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_mergerSources->setWordWrap(true);
    m_mergerMount = new QLabel();
    m_mergerMount->setObjectName("mergerMount");
    m_mergerMount->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(QString::fromUtf8("合并哪些分区"), m_mergerSources);
    form->addRow(QString::fromUtf8("并成一个目录"), m_mergerMount);

    // "这不是 RAID0"写在选它的地方，不藏进 tooltip：坏盘时丢什么，是这一格
    // 唯一真正的风险，必须常驻可见。状态栏那句策略后果说的是新文件落哪，
    // 这句说的是丢什么 —— 两件不同的事，各占一行。
    auto *note = new QLabel(QString::fromUtf8(
        "mergerfs 不是 RAID0：一个文件整个落在一个分区上，没有条带；"
        "坏一块盘，丢的是那块盘上的文件，不是整卷。可用容量 = 各分区空闲之和。"));
    note->setWordWrap(true);
    note->setObjectName("mergerNote");
    note->setProperty("role", "mute");
    v->addLayout(form);
    v->addWidget(note);
    return box;
}

QString NasPage::mergerMountPoint() const
{
    // 只走 defaultMountPoint：它保证返回空串或必然合法的路径，
    // 预填一个非法挂载点等于把雷埋进 argv。
    return Nas::defaultMountPoint(m_volName->text().trimmed());
}

// mergerfs 的 source 列表与合并挂载点，带全部守卫。与 mirrorDirs 同一套形状：
// 返回 false 时 *why 一定是要显示给用户看的那句话（① 创建、② 挂载两处共用）。
bool NasPage::mergerDirs(QStringList *sources, const QString &mountIn, QString *why) const
{
    const Nas::Plan plan = currentPlan();
    QVector<QString> devs;
    for (const Nas::Member &m : plan.members) devs << m.device;
    if (!membersGateOpen(Nas::Engine::MergerFs, devs)) {
        *why = Nas::mergerMembersWarning(devs);
        return false;
    }

    QStringList out;
    for (int i = 0; i < plan.members.size(); ++i) {
        const Nas::Member &m = plan.members.at(i);
        const QString err = Nas::validateMergerSource(m.filePath);
        if (!err.isEmpty()) {
            *why = QString::fromUtf8("第 %1 个 source（%2）：%3")
                        .arg(i + 1).arg(m.device).arg(err);
            return false;
        }
        out << m.filePath;
    }
    const QString conf = Nas::mergerSourcesConflict(out);
    if (!conf.isEmpty()) { *why = conf; return false; }

    const QString mpErr = Nas::validateMountPoint(mountIn);
    if (!mpErr.isEmpty()) { *why = QString::fromUtf8("合并挂载点：%1").arg(mpErr); return false; }
    if (!m_env.mergerfsBinary) {
        *why = QString::fromUtf8("检测不到 mergerfs —— 它是第三方包，本程序只负责检测，"
                                 "不能凭空空挂载。Debian/Ubuntu 的仓库里有 mergerfs，"
                                 "openEuler / Fedora 的仓库里没有（实测 dnf 报 No matching Packages）。");
        return false;
    }
    if (!m_env.fuseDevice) {
        *why = QString::fromUtf8("/dev/fuse 不在 —— 那是内核 fuse 模块，装包补不了，"
                                 "需要先加载 fuse 模块。");
        return false;
    }
    if (!m_env.fuseMount) {
        *why = QString::fromUtf8("缺 fusermount3（挂载 FUSE 卷的助手），可以用页顶的一键安装装。");
        return false;
    }
    // 挂载点原样带回：调用方是 raidfs 那栏（用户可改），这里不改它。
    // 为什么签名里 *sources 是指针、挂载点是 const 引用：挂载点由调用方给、
    // 这里只校验，source 列表才是这里算出来的东西。
    *sources = out;
    return true;
}

void NasPage::refreshMergerBox()
{
    if (!m_mergerSources) return;
    const Nas::Engine eng = engineFromIndex(m_engine->currentIndex());
    if (eng != Nas::Engine::MergerFs) {
        // 换走引擎就把这两条恢复成普通可编辑：mergerfs 模式下 SMB 路径是
        // 程序算出来的只读值，别的模式下它归用户。
        m_smbPath->setReadOnly(false);
        m_smbWrite->setEnabled(true);
        refreshMembersWarn();
        return;
    }

    const Nas::Plan plan = currentPlan();
    QStringList lines;
    for (int i = 0; i < plan.members.size(); ++i) {
        const Nas::Member &m = plan.members.at(i);
        lines << QString::fromUtf8("第 %1 个：%2　%3（空闲 %4）")
                    .arg(i + 1).arg(m.device, m.filePath, Nas::humanSize(m.capacity));
    }
    m_mergerSources->setText(lines.isEmpty()
        ? QString::fromUtf8("（先在上方点选分区，顺序就是 mergerfs 的 source 顺序）")
        : lines.join("\n"));

    const QString mp = mergerMountPoint();
    m_mergerMount->setText(mp.isEmpty()
        ? QString::fromUtf8("（填名称后自动出现 /mnt/<名称>）") : mp);

    // SMB 共享路径 = 合并出来的那个目录。这里覆盖 m_smbPath 是有意的：
    // mergerfs 模式下它是只读的，被覆盖的只能是程序自己填的值。
    const bool smb = m_modeSmb->isChecked();
    m_smbPath->setReadOnly(true);
    if (smb && !mp.isEmpty() && m_smbPath->text() != mp) m_smbPath->setText(mp);
    // 共享名跟卷名走：mergerfs 的共享就是那个合并目录，名字用第一个分区的
    // 挂载点推出来的话，共享名会和目录名对不上，用户在 smb.conf 里找不到自己
    // 刚配的东西。这里只填空的，不覆盖用户改过的名字。
    if (smb && m_smbName->text().isEmpty()) {
        const QString n = plan.volumeName;
        // validateShareName 返回空 = 合法（全页都是这个约定），别写反
        if (!n.isEmpty() && Nas::validateShareName(n).isEmpty()) m_smbName->setText(n);
    }
    refreshMembersWarn();
}

// 成员组合告警行（阵列区那一行）：镜像与 mergerfs 共用，只在这里算一次。
// 放行判据 membersGateOpen 也在这里读，红字与按钮可用性不会各说各话。
void NasPage::refreshMembersWarn()
{
    if (!m_membersWarn) return;
    const Nas::Engine eng = engineFromIndex(m_engine->currentIndex());
    const bool shown = m_arrayBox && m_arrayBox->isVisible()
                       && eng != Nas::Engine::Mdadm;
    if (!shown) {
        m_membersWarn->setVisible(false);
        m_membersAckBtn->setVisible(false);
        return;
    }
    const Nas::Plan plan = currentPlan();
    QVector<QString> devList;
    for (const Nas::Member &m : plan.members) devList << m.device;

    const bool merger = eng == Nas::Engine::MergerFs;
    const QString warn  = merger ? Nas::mergerMembersWarning(devList)
                                 : Nas::mirrorMembersWarning(devList);
    const auto issue    = merger ? Nas::mergerMembersIssue(devList)
                                 : Nas::mirrorMembersIssue(devList);
    const QString pairKey = devList.join(QLatin1Char(','));
    const bool acked = (issue == Nas::MembersIssue::SameDisk && pairKey == m_sameDiskAckKey);

    m_membersWarn->setText(warn);
    m_membersWarn->setVisible(!warn.isEmpty());
    m_membersAckBtn->setVisible(issue == Nas::MembersIssue::SameDisk);
    // 已确认的按钮变成"回执"而不是消失：红字还在，用户能看到放行这件事仍然
    // 生效。让它消失的话，页面看着像"问题自己好了"。
    m_membersAckBtn->setText(acked ? QString::fromUtf8("已确认：同一块盘")
                                   : QString::fromUtf8("知道了"));
    m_membersAckBtn->setEnabled(!acked);
}

// 放行判据只写一份：镜像的按钮、mirrorDirs、定时器、mergerfs 的挂载与创建，
// 全都问它。分开写必漂 —— 之前漂出来过一次：按钮亮了、定时器过了确认闸，
// mirrorDirs 照旧把同盘当硬拒绝，用户看到"已放行"却同步不了。
bool NasPage::membersGateOpen(Nas::Engine engine, const QVector<QString> &devices) const
{
    const auto iss = (engine == Nas::Engine::MergerFs)
        ? Nas::mergerMembersIssue(devices) : Nas::mirrorMembersIssue(devices);
    return iss == Nas::MembersIssue::None
           || (iss == Nas::MembersIssue::SameDisk
               && devices.join(QLatin1Char(',')) == m_sameDiskAckKey);
}

void NasPage::onMembersAckSameDisk()
{
    const Nas::Plan plan = currentPlan();
    const Nas::Engine eng = engineFromIndex(m_engine->currentIndex());
    QVector<QString> devList;
    for (const Nas::Member &m : plan.members) devList << m.device;
    // 只接受同盘这一条。数量不对时按钮本来就不出现，这里再挡一次：
    // 万一将来某个调用路径让按钮在数量不对时可见，确认也不该放行"一主一备
    // 凑不齐""一个分区没有合并意义"这类语义未定的组合。
    const auto iss = (eng == Nas::Engine::MergerFs)
        ? Nas::mergerMembersIssue(devList) : Nas::mirrorMembersIssue(devList);
    if (iss != Nas::MembersIssue::SameDisk) return;
    m_sameDiskAckKey = devList.join(QLatin1Char(','));
    refreshMembersWarn();
    refreshMirrorBox();
    refreshMergerBox();
    setStatus(eng == Nas::Engine::MergerFs
        ? QString::fromUtf8("已确认：这几块分区在同一块盘上，创建与挂载放行。"
                            "目录照做（多一个能装片子的地方），没有加速、没有冗余 —— "
                            "坏盘时这些分区上的文件一起没，这条不会因为确认而变。")
        : QString::fromUtf8("已确认：两份在同一块盘上，同步与查副本放行。"
                            "副本照做（防误删、防手滑覆盖），坏盘时两份一起没 —— "
                            "这条不会因为确认而变。"),
        true);
}

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

    // 成员组合的告警行（红字 +「知道了」）不在这里 —— 它挂在阵列区，镜像与
    // mergerfs 共用一行。见构造函数里的 m_membersWarn。

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
        // 定时这条也要过同一道闸。只查按钮可用性不够：定时器是"没人盯着"
        // 时跑的，那时用户不会看到红字 —— 换分区把确认失效之后，让一个
        // 看不见的进程继续往零冗余的副本写，是这一页最不该出现的事。
        const Nas::Plan gatePlan = currentPlan();
        QVector<QString> devs;
        for (const Nas::Member &m : gatePlan.members) devs << m.device;
        if (!membersGateOpen(Nas::Engine::MirrorRs, devs)) {
            m_mirrorTimer->stop();
            // 用模型层那句话，不在这里另写一套：同盘的情况它带着「点右边的
            // 知道了」，数量的情况它说的是"要正好 2 个" —— 在这儿硬写一句
            // 关于确认的话，遇到数量问题就是答非所问（定时器在 3 个成员时
            // 停，用户看到的却是"确认已失效"）。
            setStatus(QString::fromUtf8("定时同步已停：")
                      + Nas::mirrorMembersWarning(devs), true);
            return;
        }
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

    // 这里用放行判据，不是"有没有警告"：确认过的那对分区就是要让它过。
    // 判据与确认记的是同一对分区，所以换组合自动失效（见 naspage.h 注释）。
    if (!membersGateOpen(Nas::Engine::MirrorRs, devs)) {
        *why = Nas::mirrorMembersWarning(devs);
        return false;
    }

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

    // 告警行与「知道了」在阵列区那一行，由 refreshMembersWarn 统一算（两条路线
    // 共用），这里只读放行判据。
    refreshMembersWarn();
    QVector<QString> devList;
    for (const Nas::Member &m : plan.members) devList << m.device;
    const bool gateOpen = membersGateOpen(Nas::Engine::MirrorRs, devList);
    m_mirrorSyncBtn->setEnabled(gateOpen && !m_ctl.busy());
    m_mirrorCheckBtn->setEnabled(gateOpen && !m_ctl.busy());

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
        const QString msg = !plan.blocked.isEmpty()
            ? QString("%1 在 %2 的官方仓库里没有对应的包，页顶按钮帮不上 —— 要自己装"
                      "（apt 系的机器上这一项有包，那一家的按钮才真能装）")
                  .arg(plan.blocked.join("、"), plan.pmName)
            : "依赖清单没通过包名白名单，已中止（不该发生，请把本页日志发出来）";
        setStatus(msg, true);
        // 这条必须弹窗，不能只写状态栏。这里曾经只写状态栏：点一个大通栏按钮
        // 期待一个确认框，结果只有一行小字，用户读成"程序没反应"（实测用户就是
        // 这么报的）。状态栏在按钮那一行里，容易被当成进度提示。
        QMessageBox::warning(this, "一键安装帮不上", msg
                             + QString("\n\n这一格（mergerfs 合并目录）在检测不到 mergerfs 时"
                                "只会显示「检测不到 mergerfs」，创建与挂载都会停在那一句，"
                                "不会假装成功。"));
        return;
    }
    m_installPlan = plan;

    // 确认框：把「缺什么 → 装哪个包」和「会跑哪几条命令」都摊开。
    // 装包会改整个系统的状态，看过命令再点确认才算真确认。
    QString text = plan.packages.isEmpty()
        ? QString("将从源码编译缺失的组件（需要管理员权限，用 curl + make，不走包管理器）：\n")
        : QString("将用 %1 安装 %2 个组件（需要管理员权限）：\n")
              .arg(plan.pmName).arg(plan.packages.size());
    for (const QString &r : plan.rows)
        text += "  · " + r + "\n";
    text += "\n会执行的命令（set -e：任一条失败就停）：\n";
    for (const QString &c : plan.commands)
        text += "  # " + c + "\n";
    if (!plan.blocked.isEmpty())
        text += "\n这几项它装不了（官方仓库没有对应的包，要自己想办法）：\n  · "
                + plan.blocked.join("\n  · ") + "\n";
    text += plan.packages.isEmpty()
        ? QString("\n会改动系统：从 Debian 的源码包编一个 mergerfs 二进制装到 /usr/local/bin（"
                  "它自带 libfuse，不需要 fuse3 开发头），编译要几分钟，输出会实时贴在下方的日志框里。"
                  "不碰磁盘、不改配置文件、不动已有的共享。")
        : QString("\n会改动系统：装上软件包及其依赖。不碰磁盘、不改配置文件、不动已有的共享。");
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
    appendInstallLog(m_installPlan.packages.isEmpty()
                         ? QString("# 用 %1 从源码编译缺失组件（仓库里没有现成的包）")
                               .arg(m_installPlan.pmName)
                         : QString("# 用 %1 安装：%2")
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
