#include <QApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QEvent>
#include <QStackedWidget>
#include <QTabBar>

#include "mainwindow.h"
#include "theme.h"
#include "glasspanel.h"
#include "statuspage.h"
#include "scenepage.h"
#include "storagepage.h"
#include "entertainmentpage.h"
#include "desktoppage.h"
#include "aipage.h"
#include "servicepage.h"
#include "aboutpage.h"
#include "vmpage.h"
#include "logpage.h"
#include "mountpage.h"

// 侧栏宽度对应 demo 01 的 grid-template-columns: 208px 1fr
static constexpr int kSidebarWidth = 208;
// 各页面正文用 kTopbarInset 让出这段高度（见 *page.cpp）
static constexpr int kTopbarHeight = 52;

// 一条"导航行"。
//
// 没有直接用 QTabWidget：它的侧栏只能靠 QTabBar::RoundedWest 实现，
// 而 West 形状下 Qt 会把标签文字旋转 90°，中文竖排不可读。
// 这里每个页面一个 checkable QPushButton（文字天然水平），
// 组内用 QButtonGroup 保证互斥单选，跨组再手动取消同组外的选中项。
// NavItem 同时充当 QStackedWidget 的 index 记录，切换时不依赖文本匹配。
class MainWindow::NavItem : public QPushButton
{
public:
    NavItem(const QString &icon, const QString &text, int stackIndex, QWidget *parent = nullptr)
        : QPushButton(icon + "  " + text, parent), m_stackIndex(stackIndex)
    {
        setObjectName("navItem");
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    int stackIndex() const { return m_stackIndex; }

private:
    int m_stackIndex;
};

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("家用服务器工具箱");
    resize(1040, 800);

    auto *root = new QWidget(this);
    auto *rootLay = new QHBoxLayout(root);
    rootLay->setContentsMargins(0, 0, 0, 0);
    rootLay->setSpacing(0);

    // ── 侧栏 ──
    auto *side = new QWidget(root);
    side->setObjectName("sidebar");
    side->setFixedWidth(kSidebarWidth);
    auto *sideLay = new QVBoxLayout(side);
    sideLay->setContentsMargins(10, 14, 10, 12);
    sideLay->setSpacing(2);

    // 品牌区（demo 01 的 .brand：渐变方块 + 主副标题）
    auto *brandRow = new QHBoxLayout();
    brandRow->setSpacing(10);
    auto *mark = new QLabel("🛠");
    mark->setObjectName("brandMark");
    mark->setAlignment(Qt::AlignCenter);
    mark->setFixedSize(34, 34);
    brandRow->addWidget(mark);
    auto *brandText = new QVBoxLayout();
    brandText->setSpacing(1);
    auto *brandName = new QLabel("家用服务器工具箱");
    brandName->setObjectName("brandName");
    auto *brandSub = new QLabel("Home Server Toolbox");
    brandSub->setObjectName("brandSub");
    brandText->addWidget(brandName);
    brandText->addWidget(brandSub);
    brandRow->addLayout(brandText);
    brandRow->addStretch();
    sideLay->addLayout(brandRow);
    sideLay->addSpacing(8);

    m_navLay = new QVBoxLayout();
    m_navLay->setSpacing(2);
    sideLay->addLayout(m_navLay);
    sideLay->addStretch(1);   // 导航贴顶，剩余空间留在底部

    // ── 主区：页面堆栈铺满，顶栏浮在其上 ──
    // 顶栏要模糊，就必须有内容在它背后（backdrop 的本义）。
    // 所以这里不用 QVBoxLayout 把顶栏和页面上下排，而是让堆栈占满整个主区，
    // 顶栏作为它的兄弟控件浮在上层，靠 resizeEvent 保持贴顶通栏。
    auto *mainCol = new QWidget(root);
    mainCol->setObjectName("mainCol");
    m_mainCol = mainCol;
    // 不用布局管理 mainCol 的两个孩子：布局会把顶栏和堆栈排成上下两块，
    // 玻璃背后就没有内容了。这里让它们重叠，几何在 resizeEvent 里算。
    m_pages = new QStackedWidget(mainCol);
    // 页面内容（主板 + 概览右列）需要约 1000px；不给最小宽的话
    // 堆栈会被压窄，右列溢出（各页 QScrollArea 已有横向滚动兜底）
    mainCol->setMinimumWidth(1000);

    m_topbar = new GlassPanel(mainCol);   // 后创建 ⇒ 叠在堆栈之上
    m_topbar->setBackdropSource(m_pages);
    m_topbar->setFixedHeight(kTopbarHeight);
    m_topbar->setTint(Theme::alpha(QColor("#ffffff"), 12));
    // 主色辉光：左浓右淡，呼应 demo 01 顶栏的发光质感
    m_topbar->setAmbientGlow(Theme::alpha(Theme::accent(), 26),
                             Theme::alpha(Theme::accent2(), 8));

    auto *topLay = new QHBoxLayout(m_topbar);
    topLay->setContentsMargins(20, 11, 20, 11);
    topLay->setSpacing(10);
    m_title = new QLabel("系统状态仪表");
    m_title->setObjectName("pageTitle");
    m_subtitle = new QLabel();
    m_subtitle->setObjectName("pageSub");
    topLay->addWidget(m_title);
    topLay->addWidget(m_subtitle);
    topLay->addStretch();

    rootLay->addWidget(side);
    rootLay->addWidget(mainCol, 1);
    setCentralWidget(root);
    mainCol->installEventFilter(this);   // 容器尺寸一变就重排顶栏与堆栈

    // ── 注册页面（顺序即侧栏顺序）──
    auto *vmPage = new VMPage(this);
    addNav("监控", "📊", "状态仪表", new StatusPage(this),     "每 8 秒自动刷新");
    addNav("",     "🎬", "场景设定", new ScenePage(vmPage, this), "定时任务与自动化场景");
    addNav("",     "💾", "存储管理", new StoragePage(this),   "磁盘与挂载点");
    addNav("管理", "🖥️", "虚机管理", vmPage,                  "VirtualBox 与 QEMU/KVM");
    addNav("",     "📂", "文件挂载", new MountPage(this),      "SMB / NFS 挂载管理");
    addNav("",     "⚙️", "服务控制", new ServicePage(this),    "systemd 服务");
    addNav("",     "📋", "日志管理", new LogPage(this),        "应用与系统日志");
    addNav("系统", "🎮", "娱乐选项", new EntertainmentPage(this), "音频与媒体输出");
    addNav("",     "🎨", "个性化桌面", new DesktopPage(this),    "壁纸与界面偏好");
    addNav("",     "🤖", "AI 选项", new AIPage(this),          "模型与推理服务");
    addNav("",     "ℹ️", "关于", new AboutPage(this),          "版本与系统信息");

    m_pages->setCurrentIndex(0);
    if (!m_items.isEmpty()) {
        m_items.first()->setChecked(true);
        if (m_title) m_title->setText(m_items.first()->property("plainText").toString());
    }
}

void MainWindow::relayoutMainCol()
{
    if (!m_mainCol || !m_pages || !m_topbar) return;
    const QRect a = m_mainCol->rect();
    m_pages->setGeometry(a);                    // 铺满，玻璃背后才有真实内容
    m_topbar->setGeometry(a.left(), a.top(), a.width(), kTopbarHeight);
    m_topbar->raise();
    m_topbar->revalidate();                     // 区域变了，模糊缓存作废
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // mainCol 的尺寸由外层布局决定，这里跟不靠窗口 resizeEvent，
    // 直接盯住容器本身（首帧与任何一次重排都会触发）
    if (watched == m_mainCol && event->type() == QEvent::Resize)
        relayoutMainCol();
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::addNav(const QString &group, const QString &icon,
                        const QString &text, QWidget *page, const QString &sub)
{
    if (!group.isEmpty()) {
        auto *sep = new QLabel(group, nullptr);
        sep->setObjectName("navSep");
        m_navLay->addSpacing(8);
        m_navLay->addWidget(sep);
    }

    const int stackIndex = m_pages->count();
    m_pages->addWidget(page);

    auto *item = new NavItem(icon, text, stackIndex);
    m_navLay->addWidget(item);
    m_items.append(item);

    connect(item, &QPushButton::clicked, this, [this, item]() {
        // 跨组互斥：清掉其它条目的高亮
        for (NavItem *other : m_items)
            if (other != item) other->setChecked(false);
        item->setChecked(true);
        m_pages->setCurrentIndex(item->stackIndex());
        if (m_title) m_title->setText(item->property("plainText").toString());
        if (m_subtitle) m_subtitle->setText(item->property("subText").toString());
        // 切页后玻璃背后的内容换了，缓存必须作废
        if (m_topbar) m_topbar->revalidate();
    });
    // 顶栏标题用纯文字（不带 emoji），副标题按页给出；预先存好避免每次切换再算
    item->setProperty("plainText", text);
    item->setProperty("subText", sub);
}
