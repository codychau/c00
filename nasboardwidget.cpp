#include "nasboardwidget.h"
#include "theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QTimer>
#include <QToolTip>
#include <QFont>
#include <QFontMetricsF>
#include <QtMath>

// 与 doc/demos/30-nas-2u-panel.html 对应的尺寸比例。
// 数值是相对盘位的分数，这样缩窗口时比例不变。
namespace {

// 托架变扁后这些分数要跟着调：竖托架（高=1.3宽）下 26% 的把手
// 在扁托架（高=0.45宽）上只剩几像素，画出来像条缝。
constexpr qreal kHandleTop   = 0.06;
constexpr qreal kHandleH     = 0.30;   // 扁托架上把手占比调大
constexpr qreal kVentTop     = 0.42;
constexpr qreal kVentH       = 0.34;
constexpr qreal kSidePad     = 0.06;   // 左右留白
constexpr qreal kEarW        = 40.0;   // 挂耳宽（固定像素）
constexpr qreal kCtrlW       = 54.0;   // 左控制区宽（固定像素）
constexpr qreal kGap         = 7.0;

// 2U 机箱的宽高比：19 英寸机架宽 482.6mm，2U 高 88.9mm。
// 这是机箱的定义而不是审美参数，所以画板按它反算高度，
// 不是布局给多少高就用多少高（那会让机箱随窗口变胖）。
constexpr qreal kAspect      = 482.6 / 88.9;   // 约 5.43

// 机箱金属色：故意不用 Theme 的色板。
// Theme 是"UI 表面"的颜色，机箱要的是"金属"的颜色 —— 比 UI 更暗、
// 描边更深，才有"这是个铁盒子"的层次。UI 元素（文字/LED/描边高亮）
// 仍然全部取 Theme 的令牌。
QColor metalHi()   { return QColor(0x1b, 0x23, 0x2d); }
QColor metalMid()  { return QColor(0x15, 0x1c, 0x25); }
QColor metalLo()   { return QColor(0x11, 0x18, 0x20); }
QColor chassisHi() { return QColor(0x16, 0x1d, 0x26); }
QColor chassisLo() { return QColor(0x0c, 0x11, 0x17); }
QColor seam()      { return QColor(0x05, 0x08, 0x0b); }   // 结构缝/描边

} // namespace

// 每行几个盘位：宽的 2U 面板排 6 个最像真机；盘少时自动收窄
int NasBoardWidget::columnsFor(int count)
{
    if (count <= 0)  return 0;
    // 2U 机箱只有两行（真机 12/24 盘位都是 2 行）。
    // 所以 <=6 个盘排一行，其余一律两行 —— 不再出现第三行，
    // 三行就已经不是 2U 了。
    if (count <= 6)  return count;
    if (count <= 10) return 5;
    if (count <= 12) return 6;
    if (count <= 16) return 8;
    return (count + 1) / 2;            // 盘再多也坚持两行

}

NasBoardWidget::NasBoardWidget(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setAcceptDrops(false);
    setToolTipDuration(8000);   // 拒绝原因较长，默认 5s 不够读完

    // 活动灯闪动。真实 HDD 灯就是闪的：稳定的黄灯会被读成"故障"，
    // 所以只要面板上有任何一块活动盘，就让灯闪起来。
    m_blinkTimer = new QTimer(this);
    m_blinkTimer->setInterval(520);
    connect(m_blinkTimer, &QTimer::timeout, this, [this]() {
        m_blinkOn = !m_blinkOn;
        update();
    });
    m_blinkTimer->start();
}

// 高度上限。sizeHint 只是建议，布局有余量时仍会拉伸控件，
// 所以必须同时压住 maximumHeight，机箱才不会随窗口长高。
void NasBoardWidget::applyHeightLimit()
{
    const int ref = width() > 0 ? width() : 760;
    setMaximumHeight(static_cast<int>(idealHeight(ref)));
}

void NasBoardWidget::setDisks(const QVector<Disk> &disks)
{
    m_disks = disks;
    relayout();
    applyHeightLimit();
    update();
}

void NasBoardWidget::setEngine(Nas::Engine e)
{
    if (m_engine == e) return;
    m_engine = e;
    update();   // 只改变 LED/提示语，不影响可选性（可选性由页面重算 setDisks）
}

void NasBoardWidget::setEngineLabel(const QString &text)
{
    m_engineLabel = text;
    update();
}

void NasBoardWidget::setMembers(const QVector<QString> &ordered)
{
    m_members = ordered;
    update();
}

void NasBoardWidget::setBlink(bool on)
{
    if (on && !m_blinkTimer->isActive()) m_blinkTimer->start();
    if (!on) m_blinkTimer->stop();
}

int NasBoardWidget::diskIndexByDevice(const QString &device) const
{
    for (int i = 0; i < m_disks.size(); ++i)
        if (m_disks[i].device == device) return i;
    return -1;
}

int NasBoardWidget::memberOrderOf(const QString &device) const
{
    return m_members.indexOf(device);
}

QRectF NasBoardWidget::slotRectAt(int i) const
{
    return (i >= 0 && i < m_slotRects.size()) ? m_slotRects[i] : QRectF();
}

int NasBoardWidget::slotIndexAt(const QPointF &pos) const
{
    for (int i = 0; i < m_slotRects.size(); ++i)
        if (m_slotRects[i].contains(pos)) return i;
    return -1;
}

// ── 布局 ────────────────────────────────────────────────────
QRectF NasBoardWidget::chassisRect() const
{
    // 宽度用满，高度按 kAspect 反算，再在可用空间里垂直居中。
    // 于是窗口拉高只多出上下留白，机箱本身保持扁的。
    // 之前 chassisRect 直接等于 rect()，布局给多高机箱就长多高，
    // 所以只改 sizeHint 是无效的。
    const qreal w = width() - 2;
    qreal h = w / kAspect;

    // 行数多于两行时（盘很多）按比例装不下，按需放宽。
    // 放宽的是高度上限，不是默认行为：两行时严格按 2U 比例。
    const int cols = columnsFor(m_disks.size());
    const int rows = cols > 0 ? (m_disks.size() + cols - 1) / cols : 1;
    if (rows > 2) h = qMax(h, rows * 52.0 + 24);

    // 窗口实在很矮时用可用高度，别把盘位画到机箱外
    h = qMin(h, static_cast<qreal>(height()) - 2.0);
    h = qMax(h, 96.0);

    // 顶对齐，不居中。居中会把机箱与上方控件（实现下拉框）之间的空白
    // 撑开，看起来像布局坏了 —— 截图里下拉框被挤到机箱正中就是这个原因。
    // 机箱是页面的第一个元素，它就该贴着顶部。
    return QRectF(1, 1, width() - 2, h - 2);
}

void NasBoardWidget::relayout()
{
    m_slotRects.clear();
    const int n = m_disks.size();
    if (n == 0) return;

    const QRectF c = chassisRect();
    const int cols = columnsFor(n);
    const int rows = (n + cols - 1) / cols;

    // 可用区：扣掉左右挂耳与左侧控制区
    const qreal inner = c.left() + kEarW + kCtrlW + kGap;
    const qreal right = c.right() - kEarW;
    const qreal top   = c.top() + 12;
    const qreal bot   = c.bottom() - 12;
    const qreal w     = right - inner;
    const qreal h     = bot - top;

    // ── 高度由机箱比例决定，而不是由托架反推 ────────────────────
    //
    // 之前写的是 bh = min(可用高, bw * 1.30)，等于要求托架必须竖着。
    // 那是错的：2U 的 12 盘位是横扁托架排 2 行（约 107mm 宽 x 40mm 高，
    // 比例 2.7:1），不是竖托架。竖托架把机箱撑成 2.2:1，
    // 而 2U 按定义是 482.6mm / 88.9mm = 5.43:1。
    //
    // 现在反过来：机箱高度由 kAspect 定，托架填满它，
    // 比例自然变扁。"像不像 2U" 主要由这个比例决定，不是靠画得多细。
    const qreal bw = (w - (cols - 1) * kGap) / cols;
    qreal bh = (h - (rows - 1) * kGap) / rows;

    // 托架不能无限扁：扁到 46px 以下，把手和通风网就没地方画了。
    // 这时宁可让机箱比标准 2U 略高，也不能画成不能用的东西。
    const qreal minBayH = 46.0;
    if (bh < minBayH) bh = minBayH;
    // 也不许高过宽：竖托架就不像 2U 了
    const qreal maxBayH = bw * 1.05;
    if (bh > maxBayH) bh = maxBayH;

    const qreal vpad = (h - (rows * bh + (rows - 1) * kGap)) / 2.0;

    for (int i = 0; i < n; ++i) {
        const int row = i / cols, col = i % cols;
        m_slotRects.append(QRectF(inner + col * (bw + kGap),
                                  top + vpad + row * (bh + kGap),
                                  bw, bh));
    }
}

void NasBoardWidget::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    relayout();
    applyHeightLimit();
}

// 画板高度自己说了算：机箱高度按 kAspect 由宽度算出，
// 于是 sizeHint / minimumSizeHint 口径一致，布局无从拉伸它。
// 之前只改 sizeHint 不管用，是因为默认 maximumHeight 是 QWIDGETSIZE_MAX，
// 布局一给空间机箱就长个，这正是太高的来源。
QSize NasBoardWidget::sizeHint() const
{
    // 不能拿 width() 去算：构造时 width() 还是 0，
    // 布局据此定的高度和之后按真实宽度算出来的对不上，就会被拉伸。
    // 这里固定用 760 作参考宽度，真实宽高比由 chassisRect 保证。
    return QSize(760, static_cast<int>(idealHeight(760)));
}

QSize NasBoardWidget::minimumSizeHint() const
{
    return QSize(360, static_cast<int>(idealHeight(360)));
}

qreal NasBoardWidget::idealHeight(int forWidth) const
{
    // 与 chassisRect 用同一套算法，避免两处口径不一致
    qreal h = (forWidth - 2) / kAspect;
    const int cols = columnsFor(m_disks.size());
    const int rows = cols > 0 ? (m_disks.size() + cols - 1) / cols : 1;
    if (rows > 2) h = qMax(h, rows * 52.0 + 24.0);
    return qMax(96.0, h) + 2.0;
}

// ── 绘制 ────────────────────────────────────────────────────
void NasBoardWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    const QRectF c = chassisRect();
    drawChassis(p, c);
    drawControlColumn(p, QRectF(c.left() + kEarW, c.top() + 10,
                                kCtrlW, c.height() - 20));

    for (int i = 0; i < m_disks.size() && i < m_slotRects.size(); ++i)
        drawBay(p, m_disks[i], i, m_slotRects[i]);
}

void NasBoardWidget::drawChassis(QPainter &p, const QRectF &c)
{
    // 机箱本体：上亮下暗，模拟顶盖受光
    QLinearGradient g(c.topLeft(), c.bottomLeft());
    g.setColorAt(0.00, chassisHi());
    g.setColorAt(0.18, QColor(0x0f, 0x15, 0x1c));
    g.setColorAt(1.00, chassisLo());
    p.setPen(QPen(seam(), 1));
    p.setBrush(g);
    p.drawRoundedRect(c, 5, 5);

    // 顶部一条极淡的高光，代替 CSS 的 inset 0 1px 0 rgba(255,255,255,.045)
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 11));
    p.drawRect(QRectF(c.left() + 5, c.top() + 1, c.width() - 10, 1));

    // 挂耳 + 十字螺丝（2U 是 4 颗）
    for (int side = 0; side < 2; ++side) {
        const qreal x = side ? c.right() - kEarW : c.left();
        QRectF ear(x, c.top(), kEarW, c.height());
        QLinearGradient eg(side ? ear.right() : ear.left(), 0,
                           side ? ear.left()  : ear.right(), 0);
        eg.setColorAt(0, QColor(0x11, 0x17, 0x1e));
        eg.setColorAt(1, QColor(0x0c, 0x12, 0x18));
        p.setBrush(eg);
        p.setPen(QPen(seam(), 1));
        p.drawRect(ear);

        for (int k = 0; k < 2; ++k) {
            const QPointF ctr(x + kEarW / 2.0,
                              k ? c.bottom() - 20 : c.top() + 20);
            QRadialGradient rg(ctr, 7, ctr + QPointF(-2.5, -3));
            rg.setColorAt(0.0, QColor(0x4a, 0x5a, 0x6b));
            rg.setColorAt(0.7, QColor(0x1a, 0x23, 0x2c));
            p.setBrush(rg);
            p.setPen(QPen(seam(), 1));
            p.drawEllipse(ctr, 6.5, 6.5);
            p.setPen(QPen(QColor(0x0a, 0x0e, 0x12), 1.4));
            p.drawLine(ctr + QPointF(0, -4), ctr + QPointF(0, 4));
            p.drawLine(ctr + QPointF(-4, 0), ctr + QPointF(4, 0));
        }
    }
}

void NasBoardWidget::drawControlColumn(QPainter &p, const QRectF &area)
{
    // 电源键
    const qreal d = 30;
    QRectF pwr(area.center().x() - d / 2, area.top(), d, d);
    QRadialGradient pg(pwr.center(), d / 2, pwr.center() + QPointF(-3, -4));
    pg.setColorAt(0, QColor(0x24, 0x30, 0x40));
    pg.setColorAt(1, QColor(0x14, 0x1c, 0x26));
    p.setBrush(pg);
    p.setPen(QPen(seam(), 1));
    p.drawEllipse(pwr);
    // 电源符号：一个圆 + 顶部一竖，不依赖 emoji 字体是否覆盖 U+23FB
    p.setPen(QPen(Theme::accent(), 1.4));
    p.setBrush(Qt::NoBrush);
    p.drawArc(pwr.adjusted(8, 8, -8, -8), 70 * 16, 320 * 16);
    p.drawLine(pwr.center() + QPointF(0, -8), pwr.center() + QPointF(0, -1));

    // 三颗指示灯
    struct { const char *name; QColor col; bool blink; } leds[] = {
        { "PWR", Theme::ok(),   false },
        { "HDD", Theme::warn(), true  },
        { "UID", Theme::accent2(), false },
    };
    QFont mono = font();
    mono.setFamily(QStringLiteral("DejaVu Sans Mono"));
    mono.setPointSizeF(7.0);
    mono.setStyleHint(QFont::Monospace);
    p.setFont(mono);

    qreal y = pwr.bottom() + 10;
    for (const auto &L : leds) {
        const bool lit = L.blink ? m_blinkOn : true;
        QRectF bulb(area.left() + 6, y, 7, 7);
        if (lit) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(L.col.red(), L.col.green(), L.col.blue(), 55));
            p.drawEllipse(bulb.adjusted(-2.5, -2.5, 2.5, 2.5));   // 光晕
        }
        p.setBrush(lit ? L.col : QColor(0x1c, 0x25, 0x30));
        p.setPen(QPen(seam(), 1));
        p.drawEllipse(bulb);
        p.setPen(lit ? Theme::textDim() : Theme::textMute());
        p.drawText(bulb.adjusted(12, -3, 40, 3),
                   Qt::AlignLeft | Qt::AlignVCenter, QString::fromLatin1(L.name));
        y += 15;
    }

    // UID 按钮
    QRectF btn(area.left() + 4, y + 5, 34, 14);
    p.setBrush(QColor(0x1b, 0x25, 0x32));
    p.setPen(QPen(seam(), 1));
    p.drawRoundedRect(btn, 3, 3);
    p.setPen(Theme::accent2());
    p.drawText(btn, Qt::AlignCenter, QStringLiteral("UID"));

    // 与盘位区的分隔缝
    p.setPen(QPen(seam(), 1));
    p.drawLine(QPointF(area.right() + 6, area.top() - 4),
               QPointF(area.right() + 6, area.bottom() + 4));
}

void NasBoardWidget::drawBay(QPainter &p, const Disk &d, int slot, const QRectF &r)
{
    const int order = memberOrderOf(d.device);
    const bool member = order >= 0;
    const bool hovered = (slot == m_hover);

    const bool empty = d.bytes <= 0;

    p.save();
    // 压暗分两级：空位最暗；"不可用但没坏"次一级。
    // QSS 没有 opacity，只能在 QPainter 层面做，所以要在这里一次性定好，
    // 后面所有元素继承同一个不透明度。
    if (empty)                     p.setOpacity(0.50);
    else if (!d.selectable && !d.faulty) p.setOpacity(0.66);

    // 面板本体
    QLinearGradient bg(r.topLeft(), r.bottomLeft());
    bg.setColorAt(0.00, metalHi());
    bg.setColorAt(0.55, metalMid());
    bg.setColorAt(1.00, metalLo());

    // 描边颜色即状态。成员盘用主色，比 CSS 的 1px 略粗一点更醒目。
    QColor edge = seam();
    qreal edgeW = 1.0;
    if (member)          { edge = Theme::accent();        edgeW = 1.6; }
    else if (d.inOtherArray) edge = Theme::accent2().darker(140);
    else if (!d.selectable)  edge = Theme::bad().darker(150);

    // 外发光（代替 box-shadow）：先画一圈半透明扩边
    if (member) {
        p.setPen(QPen(QColor(Theme::accent().red(), Theme::accent().green(),
                             Theme::accent().blue(), hovered ? 110 : 70), 3.5));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r.adjusted(-1.5, -1.5, 1.5, 1.5), 4, 4);
    }

    p.setPen(QPen(edge, edgeW));
    p.setBrush(bg);
    p.drawRoundedRect(r, 3, 3);

    drawHandle(p, r);
    drawVent(p, r);
    drawBayLeds(p, r, d, order);

    // 不可选的画法分三档 —— 之前一律画红斜纹，结果本机 7 块盘全不可选时
    // 整个面板红成一片，看起来像机器坏了，反而看不出到底哪块是故障盘。
    //   故障/损坏 → 红斜纹 + 红 LED（最强）
    //   系统盘     → 压暗 + 锁标（长期状态，不该抢注意力）
    //   其它不可用 → 压暗（已挂载、已在阵列等，鼠标悬停看原因）
    if (!d.selectable && !empty) {
        if (d.faulty) {
            drawDiagonalHatch(p, r, QColor(Theme::bad().red(), Theme::bad().green(),
                                           Theme::bad().blue(), 30));
        }
        // 其余不可用情况不画红纹：上面已统一压暗，原因交给 tooltip。
        // 一律画红纹会让整面看起来像机器坏了，反而看不出哪块真故障。
    }

    // 盘号 + 容量
    QFont mono = font();
    mono.setFamily(QStringLiteral("DejaVu Sans Mono"));
    mono.setStyleHint(QFont::Monospace);

    mono.setBold(true);
    mono.setPointSizeF(qMax(6.5, r.height() * 0.075));
    p.setFont(mono);
    QColor slotCol = Theme::textMute();
    if (member)              slotCol = Theme::accent();
    else if (d.inOtherArray) slotCol = Theme::accent2();
    else if (!d.selectable)  slotCol = Theme::bad();
    p.setPen(slotCol);
    {
        const QString slotTxt = QStringLiteral("%1").arg(slot + 1, 2, 10, QChar('0'));
        // 同行左右两端，中间留空：可用宽度接近整块盘位，
        // 而不是被对半切开后两边都放不下。
        const QRectF box(r.left() + r.width() * kSidePad,
                         r.bottom() - r.height() * 0.155,
                         r.width() * (1.0 - 2 * kSidePad), r.height() * 0.135);
        QFontMetricsF fm(mono);
        // 容量占宽更大，所以盘号空间压到 0.40（原来是 0.5，会和容量重叠）
        p.drawText(box, Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(slotTxt, Qt::ElideRight, box.width()));
    }

    // 容量：右下角，靠右对齐。
    // 之前字号只按盘位高度算，"3.64 TiB" 在窄盘位下被**左端裁掉**，
    // 显示成 ".64 TiB" —— 因为靠右对齐、超出部分从左边溢出，
    // 看起来就像数字丢了个首位。所以必须按**文本宽度**自适应缩字号。
    mono.setBold(false);
    p.setPen(Theme::textMute());
    const QString capTxt = d.bytes > 0 ? Nas::humanSize(d.bytes)
                                       : QStringLiteral("--");
    {
        const QRectF box(r.left() + r.width() * kSidePad,
                         r.bottom() - r.height() * 0.155,
                         r.width() * (1.0 - 2 * kSidePad), r.height() * 0.135);
        qreal pt = qMax(6.0, r.height() * 0.065);
        QFont probe = mono;
        probe.setPointSizeF(pt);
        // 缩到放得下为止，但低于 6pt 就不缩了（再小也读不清），
        // 宁可让 QFontMetrics 省略号截断，也不要显示错的数字。
        while (pt > 6.0) {
            QFontMetricsF fm(probe);
            if (fm.horizontalAdvance(capTxt) <= box.width()) break;
            pt -= 0.5;
            probe.setPointSizeF(pt);
        }
        p.setFont(probe);
        QFontMetricsF fm(probe);
        p.drawText(box, Qt::AlignRight | Qt::AlignVCenter,
                   fm.elidedText(capTxt, Qt::ElideRight, box.width()));
    }

    // 系统盘：压暗之外再给个锁标，否则"为什么不能选"要悬停才知道
    if (!d.selectable && d.systemDisk && !empty) {
        QFont lk = font();
        lk.setPointSizeF(qMax(9.0, r.height() * 0.11));
        p.setFont(lk);
        p.setPen(Theme::textDim());
        p.drawText(QRectF(r.right() - r.width() * 0.30,
                          r.top() + r.height() * 0.33,
                          r.width() * 0.24, r.height() * 0.16),
                   Qt::AlignCenter, QString::fromUtf8("\xf0\x9f\x94\x92"));
    }

    // 设备名：容量下面那行放不下，放在通风网下沿的空隙里
    if (r.height() > 84) {
        QFont tiny = mono;
        tiny.setPointSizeF(6.5);
        p.setFont(tiny);
        p.setPen(Theme::textMute());
        p.drawText(QRectF(r.left() + r.width() * kSidePad,
                          r.bottom() - r.height() * 0.28,
                          r.width() * (1.0 - 2 * kSidePad), r.height() * 0.11),
                   Qt::AlignLeft | Qt::AlignVCenter, d.label);
    }

    p.restore();

    // 条带序号画在最上层：它压在 LED 位置上，必须在 restore 之后画，
    // 否则会被压暗逻辑影响，也容易被后面的描边盖住。
    if (member) drawStripeBadge(p, r, order);
}

void NasBoardWidget::drawHandle(QPainter &p, const QRectF &bay)
{
    QRectF h(bay.left() + bay.width() * 0.05,
             bay.top()  + bay.height() * kHandleTop,
             bay.width() * 0.90,
             bay.height() * kHandleH);
    QLinearGradient hg(h.topLeft(), h.bottomLeft());
    hg.setColorAt(0, QColor(0x2b, 0x3a, 0x4a));
    hg.setColorAt(1, QColor(0x1c, 0x27, 0x32));
    p.setBrush(hg);
    p.setPen(QPen(seam(), 1));
    p.drawRoundedRect(h, 2, 2);

    // 握持凹槽
    QRectF grip(h.left() + h.width() * 0.16,
                h.top()  + h.height() * 0.34,
                h.width() * 0.68,
                h.height() * 0.32);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x0d, 0x13, 0x19));
    p.drawRoundedRect(grip, 2, 2);

    // 转轴
    for (int s = 0; s < 2; ++s) {
        QPointF pc(s ? bay.right()  - bay.width() * 0.03 - 2.5
                     : bay.left()   + bay.width() * 0.03 + 2.5,
                   bay.top() + bay.height() * (kHandleTop + kHandleH + 0.01));
        p.setBrush(QColor(0x3a, 0x4a, 0x5a));
        p.setPen(QPen(seam(), 1));
        p.drawEllipse(pc, 2.5, 2.5);
    }
}

void NasBoardWidget::drawVent(QPainter &p, const QRectF &bay)
{
    QRectF v(bay.left()  + bay.width() * 0.09,
             bay.top()   + bay.height() * kVentTop,
             bay.width() * 0.82,
             bay.height() * kVentH);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x0b, 0x10, 0x16));
    p.drawRoundedRect(v, 2, 2);

    // 冲孔网：代替 repeating-linear-gradient。孔距跟盘位高度走，
    // 固定 4px 在小尺寸下会糊成一片黑。
    const qreal step = qMax(4.5, bay.height() * 0.085);
    p.setBrush(QColor(0, 0, 0, 168));
    for (qreal y = v.top() + step * 0.5; y < v.bottom() - 1.5; y += step)
        p.drawRect(QRectF(v.left() + 2, y, v.width() - 4, qMax(1.5, step * 0.42)));
}

void NasBoardWidget::drawBayLeds(QPainter &p, const QRectF &bay,
                                 const Disk &d, int order)
{
    // 扁托架下高度只剩 50px 左右，按高度算 LED 会小于 3px，
    // 所以给一个 5.5px 的下限 —— 指示灯看不清就等于没有。
    const qreal s = qMax(5.5, bay.height() * 0.075);
    qreal lx = bay.right() - bay.width() * 0.06 - s;
    qreal ly = bay.top()  + bay.height() * 0.34;

    auto led = [&](const QColor &col) {
        if (col.isValid()) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(col.red(), col.green(), col.blue(), 58));
            p.drawRect(QRectF(lx - 2, ly - 2, s + 4, s + 4));
        }
        p.setBrush(col.isValid() ? col : QColor(0x15, 0x1d, 0x26));
        p.setPen(QPen(QColor(0x04, 0x07, 0x0a), 1));
        p.drawRect(QRectF(lx, ly, s, s));
        ly += s + 3.5;
    };

    // 状态灯（下）+ 活动灯（上）。真实托架就是这两颗。
    const bool empty = d.bytes <= 0;
    if (empty)                          { led(QColor()); led(QColor()); }
    else if (!d.selectable)             { led(Theme::bad()); led(QColor()); }
    else if (d.inOtherArray)            { led(Theme::warn()); led(m_blinkOn ? Theme::warn() : QColor()); }
    else if (order >= 0)                { led(Theme::ok());   led(m_blinkOn ? Theme::warn() : QColor()); }
    else                                { led(QColor());      led(QColor()); }
}

void NasBoardWidget::drawStripeBadge(QPainter &p, const QRectF &bay, int order)
{
    const qreal s = qMax(14.0, bay.height() * 0.15);
    QRectF badge(bay.right() - bay.width() * 0.06 - s,
                 bay.top()  + bay.height() * 0.335,
                 s, s);

    p.setPen(QPen(QColor(Theme::accent().red(), Theme::accent().green(),
                         Theme::accent().blue(), 90), 2.5));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(badge.adjusted(-1, -1, 1, 1), 4, 4);

    p.setPen(Qt::NoPen);
    p.setBrush(Theme::accent());
    p.drawRoundedRect(badge, 3, 3);

    QFont mono = font();
    mono.setFamily(QStringLiteral("DejaVu Sans Mono"));
    mono.setBold(true);
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSizeF(s * 0.58);
    p.setFont(mono);
    p.setPen(Theme::onAccent());
    p.drawText(badge, Qt::AlignCenter, QString::number(order));
}

void NasBoardWidget::drawDiagonalHatch(QPainter &p, const QRectF &r, const QColor &c)
{
    p.save();
    p.setClipRect(r);
    p.setPen(QPen(c, 5));
    for (qreal d = -r.height(); d < r.width(); d += 16)
        p.drawLine(QPointF(r.left() + d, r.bottom()),
                   QPointF(r.left() + d + r.height(), r.top()));
    p.restore();
}

// ── 交互 ────────────────────────────────────────────────────
QString NasBoardWidget::tipForSlot(int i) const
{
    if (i < 0 || i >= m_disks.size()) return QString();
    const Disk &d = m_disks[i];
    QString t = d.label;
    if (!d.fstype.isEmpty()) t += QStringLiteral("  ·  %1").arg(d.fstype);
    if (!d.mountpoint.isEmpty()) t += QStringLiteral("  ·  %1").arg(d.mountpoint);
    t += QStringLiteral("\n%1").arg(Nas::humanSize(d.bytes));
    if (d.freeBytes > 0)
        t += QStringLiteral("  （可用 %1）").arg(Nas::humanSize(d.freeBytes));
    if (!d.selectable && !d.blockReason.isEmpty())
        t += QStringLiteral("\n\n不可用：%1").arg(d.blockReason);
    else if (d.inOtherArray)
        t += QStringLiteral("\n\n已在其它阵列中");
    else if (memberOrderOf(d.device) >= 0)
        t += QStringLiteral("\n\n条带序号 %1，点击移出阵列").arg(memberOrderOf(d.device));
    else
        t += QStringLiteral("\n\n点击加入阵列");
    return t;
}

void NasBoardWidget::mouseMoveEvent(QMouseEvent *e)
{
    const int i = slotIndexAt(e->position());
    if (i == m_hover) return;
    m_hover = i;
    setCursor(i >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    if (i >= 0) QToolTip::showText(e->globalPosition().toPoint(), tipForSlot(i), this);
    else        QToolTip::hideText();
    update();
}

void NasBoardWidget::leaveEvent(QEvent *)
{
    if (m_hover == -1) return;
    m_hover = -1;
    QToolTip::hideText();
    update();
}

void NasBoardWidget::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) return;
    const int i = slotIndexAt(e->position());
    if (i < 0 || i >= m_disks.size()) return;

    const Disk &d = m_disks[i];
    if (!d.selectable) {
        // 原因由 Nas::judge 算好，这里不重复判定，避免两处规则漂移
        emit slotRejected(d.device, i,
                          d.blockReason.isEmpty()
                              ? QStringLiteral("该设备不可用") : d.blockReason);
        return;
    }
    emit slotClicked(d.device, i);
}
