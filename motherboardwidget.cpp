#include "motherboardwidget.h"

#include <QPainter>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QToolTip>
#include <QFontMetricsF>
#include <QEvent>
#include <QPair>

// ── 类别配色：CPU / 内存 / 磁盘接口 / GPU ──
static const QColor kCpuColor("#f59e0b");
static const QColor kMemColor("#3b82f6");
static const QColor kNvmeColor("#10b981");
static const QColor kSataColor("#a855f7");
static const QColor kGpuColor("#ef4444");
static const QColor kPchColor("#64748b");
static const QColor kNetColor("#06b6d4");
static const QColor kEmptyColor("#6b7280");  // 空槽位

MotherboardWidget::MotherboardWidget(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumSize(430, 340);
    setToolTip("把鼠标移到主板部件上查看信息");
}

QRectF MotherboardWidget::boardRect() const
{
    // 底部留出信息条
    return QRectF(rect()).adjusted(10, 10, -10, -62);
}

void MotherboardWidget::setCpu(const QString &usage, const QString &temp)
{
    m_cpuUsage = usage; m_cpuTemp = temp;
    relayout(); update();
}

void MotherboardWidget::setMemory(const QString &text, double ratio)
{
    m_memText = text; m_memRatio = ratio;
    relayout(); update();
}

void MotherboardWidget::setSwap(const QString &text, double ratio)
{
    m_swapText = text; m_swapRatio = ratio;
    relayout(); update();
}

void MotherboardWidget::setBoardInfo(const QString &os, const QString &kernel, const QString &uptime)
{
    m_os = os; m_kernel = kernel; m_uptime = uptime;
    relayout(); update();
}

void MotherboardWidget::setNetwork(const QString &text)
{
    m_net = text;
    relayout(); update();
}

void MotherboardWidget::setNvmeTemp(const QString &temp)
{
    m_nvmeTemp = temp;
    relayout(); update();
}

void MotherboardWidget::setDisks(const QVector<DiskPart> &disks)
{
    m_disks = disks;
    relayout(); update();
}

void MotherboardWidget::setGpus(const QVector<GpuPart> &gpus)
{
    m_gpus = gpus;
    relayout(); update();
}

// 把 "1.2T / 2.0T" 压成一行短文本
static QString oneLine(const QString &s)
{
    QString t = s.simplified();
    t.replace(" / ", "/");
    return t;
}

// ── 排版：把部件按分区摆到画布上 ──
void MotherboardWidget::relayout()
{
    QRectF b = boardRect();
    const double W = b.width(), H = b.height();
    m_parts.clear();
    if (W < 200 || H < 300)
        return;

    // ATX 比例（高 ≈ 1.25 × 宽）：左列 CPU+芯片组 → 网口 → M.2 → GPU；右列 内存 → 磁盘接口
    const double leftX  = b.left() + 0.060 * W;
    const double leftW  = 0.510 * W;
    const double rightX = b.left() + 0.610 * W;
    const double rightW = 0.340 * W;
    const double topY   = b.top()  + 0.070 * H;
    const double gap    = 0.030 * H;

    // ── 第一行：CPU 芯片 + 芯片组（放在一起） ──
    const double cpuSide = qMin(0.33 * W, 0.16 * H);
    BoardPart cpu;
    cpu.id = "cpu";
    cpu.title = "CPU";
    cpu.color = kCpuColor;
    cpu.chip = true;
    cpu.rect = QRectF(leftX, topY, cpuSide, cpuSide);
    const QString cpuUse = (m_cpuUsage.isEmpty() || m_cpuUsage == "—") ? "采样中" : m_cpuUsage;
    cpu.lines = { QString("占用 %1").arg(cpuUse),
                  QString("温度 %1").arg(m_cpuTemp) };
    cpu.info  = { QString("占用率 %1").arg(m_cpuUsage),
                  QString("温度 %1").arg(m_cpuTemp) };
    m_parts.append(cpu);

    const double pchSide = qMin(0.16 * W, 0.13 * H);
    BoardPart pch;
    pch.id = "pch";
    pch.title = "芯片组";
    pch.color = kPchColor;
    pch.chip = true;
    pch.rect = QRectF(leftX + cpuSide + 0.04 * W, topY + (cpuSide - pchSide) / 2.0, pchSide, pchSide);
    pch.lines = { m_kernel };
    pch.info = { "系统 " + m_os,
                 "内核 " + m_kernel,
                 "运行时间 " + m_uptime };
    m_parts.append(pch);

    // ── 网口（CPU 下方） ──
    const double lanY = topY + cpuSide + gap;
    const double lanH = 0.085 * H;
    BoardPart lan;
    lan.id = "lan";
    lan.title = "网口";
    lan.color = kNetColor;
    lan.rect = QRectF(leftX, lanY, leftW, lanH);
    lan.lines = { m_net };
    lan.info  = { "网络 " + m_net };
    m_parts.append(lan);

    // ── 槽位高度按剩余空间自适应，保证网口下方的 M.2 + GPU 全部放得下 ──
    QVector<DiskPart> nvme, sata;
    for (const auto &d : m_disks) {
        if (d.nvme) nvme.append(d);
        else sata.append(d);
    }
    const int m2Slots   = qMax(1, qMin((int)nvme.size(), 2));
    const int gpuSlots  = qMax(1, qMin((int)m_gpus.size(), 3));
    const int sataSlots = qMax(1, qMin((int)sata.size(), 2));

    const double dimmH = 0.42 * H;
    const double slotsTop = lanY + lanH + gap;
    const double slotsFree = b.bottom() - slotsTop;
    const int leftSlotCount = m2Slots + gpuSlots;
    const double slotH = qMax(46.0, qMin(0.12 * H,
                         (slotsFree - (leftSlotCount - 1) * gap) / leftSlotCount));

    // ── M.2 / NVMe（网口下方） ──
    for (int i = 0; i < m2Slots; ++i) {
        BoardPart s;
        s.id = QString("m2_%1").arg(i);
        s.rect = QRectF(leftX, slotsTop + i * (slotH + gap), leftW, slotH);

        if (i < nvme.size()) {
            const DiskPart &d = nvme[i];
            const QString temp = (d.temp.isEmpty() || d.temp == "—") ? m_nvmeTemp : d.temp;
            s.color = kNvmeColor;
            s.title = QString("M.2 · %1").arg(d.name);
            s.lines = { QString("温度 %1 · %2").arg(temp, oneLine(d.used.isEmpty() ? d.size : d.used)) };
            s.fill = d.fill;
            s.info = { QString("设备 /dev/%1").arg(d.name),
                       QString("型号 %1").arg(d.model.isEmpty() ? "—" : d.model),
                       QString("容量 %1").arg(d.size),
                       QString("已用 %1").arg(d.used.isEmpty() ? "—" : d.used),
                       QString("挂载 %1").arg(d.mount.isEmpty() ? "未挂载" : d.mount),
                       QString("温度 %1").arg(temp) };
            if (d.fill >= 0.0)
                s.info << QString("容量使用率 %1%").arg(d.fill * 100.0, 0, 'f', 1);
            if (i + 1 < nvme.size()) {
                QStringList rest;
                for (int k = i + 1; k < nvme.size(); ++k) rest << nvme[k].name;
                s.info << "其余 NVMe：" + rest.join(", ") + "（点击展开下方磁盘分组）";
            }
        } else {
            s.color = kEmptyColor;
            s.title = QString("M.2 槽 #%1").arg(i + 1);
            s.lines = { "空槽" };
            s.info = { "M.2 槽位（未插盘）" };
        }
        m_parts.append(s);
    }

    // ── GPU：PCIe 长插槽（M.2 下方） ──
    const double gpuY = slotsTop + m2Slots * (slotH + gap);
    for (int i = 0; i < gpuSlots; ++i) {
        BoardPart g;
        g.id = QString("gpu%1").arg(i);
        g.color = kGpuColor;
        g.rect = QRectF(leftX, gpuY + i * (slotH + gap), leftW, slotH);

        if (i < m_gpus.size()) {
            const GpuPart &gp = m_gpus[i];
            g.title = QString("%1 · %2").arg(gp.name, gp.model);
            g.lines = { QString("温度 %1 · %2").arg(gp.temp, oneLine(gp.vram)) };
            g.fill = gp.fill;
            g.info = { QString("型号 %1").arg(gp.model),
                       QString("PCI 槽位 %1").arg(gp.slot),
                       QString("温度 %1").arg(gp.temp),
                       QString("核心频率 %1").arg(gp.sclk),
                       QString("显存 %1").arg(gp.vram),
                       QString("风扇 %1").arg(gp.fan) };
            if (gp.fill >= 0.0)
                g.info << QString("显存使用率 %1%").arg(gp.fill * 100.0, 0, 'f', 1);
            if (i + 1 < m_gpus.size()) {
                QStringList rest;
                for (int k = i + 1; k < m_gpus.size(); ++k) rest << m_gpus[k].name;
                g.info << "其余显卡：" + rest.join(", ") + "（点击展开下方显卡分组）";
            }
        } else {
            g.title = QString("PCIe x16 #%1").arg(i + 1);
            g.lines = { "空槽" };
            g.color = kEmptyColor;
            g.info = { "PCIe x16 槽位（未插卡）" };
        }
        m_parts.append(g);
    }

    // ── 内存 / 交换：两根 DIMM（右列，占满上部） ──
    const double dimmW = qMin(0.140 * W, 0.45 * dimmH);
    const double dimmGap = 0.035 * W;
    for (int i = 0; i < 2; ++i) {
        BoardPart dimm;
        dimm.id = (i == 0) ? "mem" : "swap";
        dimm.title = (i == 0) ? "内存" : "交换";
        dimm.color = kMemColor;
        dimm.rect = QRectF(rightX + i * (dimmW + dimmGap), topY, dimmW, dimmH);

        const QString txt  = (i == 0) ? m_memText : m_swapText;
        const double ratio = (i == 0) ? m_memRatio : m_swapRatio;
        dimm.fill = ratio;

        const QStringList halves = txt.split(" / ");
        if (halves.size() == 2)
            dimm.lines = { halves[0], "/ " + halves[1] };
        else
            dimm.lines = { txt };
        if (ratio >= 0.0)
            dimm.lines << QString("%1%").arg(ratio * 100.0, 0, 'f', 0);

        dimm.info = { QString("已用 / 总量 %1").arg(txt) };
        if (ratio >= 0.0)
            dimm.info << QString("使用率 %1%").arg(ratio * 100.0, 0, 'f', 1);
        if (i == 1)
            dimm.info << "交换分区（Swap）";
        m_parts.append(dimm);
    }

    // ── 磁盘接口：SATA（右列，内存下方） ──
    const double sataY = topY + dimmH + gap;
    for (int i = 0; i < sataSlots; ++i) {
        BoardPart s;
        s.id = QString("sata%1").arg(i);
        s.rect = QRectF(rightX, sataY + i * (slotH + gap), rightW, slotH);

        if (i < sata.size()) {
            const DiskPart &d = sata[i];
            s.color = kSataColor;
            s.title = QString("SATA · %1").arg(d.name);
            s.lines = { QString("温度 %1 · %2").arg(d.temp.isEmpty() ? "—" : d.temp,
                                                    oneLine(d.used.isEmpty() ? d.size : d.used)) };
            s.fill = d.fill;
            s.info = { QString("设备 /dev/%1").arg(d.name),
                       QString("型号 %1").arg(d.model.isEmpty() ? "—" : d.model),
                       QString("容量 %1").arg(d.size),
                       QString("已用 %1").arg(d.used.isEmpty() ? "—" : d.used),
                       QString("挂载 %1").arg(d.mount.isEmpty() ? "未挂载" : d.mount),
                       QString("温度 %1").arg(d.temp.isEmpty() ? "—" : d.temp) };
            if (d.fill >= 0.0)
                s.info << QString("容量使用率 %1%").arg(d.fill * 100.0, 0, 'f', 1);
            if (i + 1 < sata.size()) {
                QStringList rest;
                for (int k = i + 1; k < sata.size(); ++k) rest << sata[k].name;
                s.info << "其余磁盘：" + rest.join(", ") + "（点击展开下方磁盘分组）";
            }
        } else {
            s.color = kEmptyColor;
            s.title = QString("SATA #%1").arg(i + 1);
            s.lines = { "空" };
            s.info = { "SATA 接口（无设备）" };
        }
        m_parts.append(s);
    }
}

// ── 命中测试 ──
int MotherboardWidget::partAt(const QPointF &pos) const
{
    for (int i = m_parts.size() - 1; i >= 0; --i) {
        if (m_parts[i].rect.contains(pos))
            return i;
    }
    return -1;
}

QString MotherboardWidget::tipHtml(const BoardPart &p) const
{
    QString html = QString("<b>%1</b>").arg(p.title.toHtmlEscaped());
    for (const auto &l : p.info) {
        if (!l.isEmpty())
            html += "<br>" + l.toHtmlEscaped();
    }
    return html;
}

// ── 画一个部件 ──
void MotherboardWidget::drawPart(QPainter &p, const BoardPart &part, bool hot)
{
    QRectF r = part.rect;
    QColor base = part.color;

    // 芯片引脚
    if (part.chip) {
        p.setPen(QPen(base.darker(150), 1.6));
        const int n = 9;
        for (int i = 1; i <= n; ++i) {
            double t = i / double(n + 1);
            double x = r.left() + t * r.width();
            double y = r.top() + t * r.height();
            p.drawLine(QPointF(x, r.top() - 4), QPointF(x, r.top()));
            p.drawLine(QPointF(x, r.bottom()), QPointF(x, r.bottom() + 4));
            p.drawLine(QPointF(r.left() - 4, y), QPointF(r.left(), y));
            p.drawLine(QPointF(r.right(), y), QPointF(r.right(), y + 4));
        }
    }

    QLinearGradient g(r.topLeft(), r.bottomRight());
    g.setColorAt(0.0, base.lighter(128));
    g.setColorAt(1.0, base.lighter(104));
    p.setBrush(g);
    p.setPen(QPen(hot ? QColor("#ffffff") : base.darker(190), hot ? 2.4 : 1.4));
    p.drawRoundedRect(r, 6, 6);

    if (hot) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 70));
        p.drawRoundedRect(r.adjusted(2, 2, -2, -2), 5, 5);
    }

    // 容量 / 使用率进度条（槽位太矮时让位给文字）
    const bool showBar = (part.fill >= 0.0) && (r.height() >= 46.0);
    const double barZone = showBar ? 18.0 : 0.0;
    if (showBar) {
        QRectF bar(r.left() + 5, r.bottom() - 15, r.width() - 10, 11);
        p.setPen(QPen(QColor(0, 0, 0, 60), 0.8));
        p.setBrush(QColor(255, 255, 255, 120));
        p.drawRoundedRect(bar, 3, 3);            // 容量条底槽
        double w = bar.width() * qBound(0.0, part.fill, 1.0);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#f8fafc"));
        p.drawRoundedRect(QRectF(bar.left(), bar.top(), qMax(w, 3.0), bar.height()), 3, 3);
        QFont bf = font();
        bf.setPointSizeF(qMax(7.0, font().pointSizeF() - 1.5));
        p.setFont(bf);
        p.setPen(QColor("#0b1e14"));
        p.drawText(bar.adjusted(4, -1, -4, -1), Qt::AlignRight | Qt::AlignVCenter,
                   QString("%1%").arg(part.fill * 100.0, 0, 'f', 0));
    }

    // 部件内文字：标题 + 尽量多的数据行
    QRectF textArea(r.left() + 5, r.top() + 3, r.width() - 10, r.height() - 6 - barZone);

    QFont tf = font();
    tf.setBold(true);
    tf.setPointSizeF(qBound(8.0, tf.pointSizeF() + 0.5, 10.0));
    QFontMetricsF tfm(tf);
    const double titleH = tfm.height();

    QFont sf = font();
    sf.setBold(false);
    sf.setPointSizeF(qBound(7.0, font().pointSizeF() - 1.0, 9.0));
    QFontMetricsF sfm(sf);
    const double lineH = sfm.height() + 1;

    p.setFont(tf);
    p.setPen(QColor("#0b1e14"));
    p.drawText(QRectF(textArea.left(), textArea.top(), textArea.width(), titleH),
               Qt::AlignCenter,
               tfm.elidedText(part.title, Qt::ElideRight, textArea.width()));

    double y = textArea.top() + titleH + 1;
    p.setFont(sf);
    p.setPen(QColor("#1f2937"));
    for (int i = 0; i < part.lines.size() && y + lineH - 1 <= textArea.bottom(); ++i) {
        p.drawText(QRectF(textArea.left(), y, textArea.width(), lineH),
                   Qt::AlignCenter,
                   sfm.elidedText(part.lines[i], Qt::ElideRight, textArea.width()));
        y += lineH;
    }
}

// ── 图例：颜色 = 部件类别 ──
void MotherboardWidget::drawLegend(QPainter &p, const QRectF &area)
{
    const QList<QPair<QColor, QString>> items = {
        { kCpuColor,  "CPU" },
        { kMemColor,  "内存" },
        { kNvmeColor, "NVMe" },
        { kSataColor, "SATA" },
        { kGpuColor,  "GPU" },
        { kPchColor,  "芯片组" },
    };

    QFont lf = font();
    lf.setPointSizeF(qMax(font().pointSizeF() - 1.0, 7.5));
    QFontMetricsF lfm(lf);
    p.setFont(lf);

    double x = area.left();
    for (const auto &it : items) {
        if (x > area.right() - 30) break;
        QRectF sw(x, area.top() + (area.height() - 10) / 2.0, 10, 10);
        p.setPen(QPen(QColor("#0f172a"), 1.0));
        p.setBrush(it.first);
        p.drawRoundedRect(sw, 2, 2);
        x += 13;
        p.setPen(QColor("#334155"));
        p.drawText(QRectF(x, area.top(), lfm.horizontalAdvance(it.second) + 6, area.height()),
                   Qt::AlignLeft | Qt::AlignVCenter, it.second);
        x += lfm.horizontalAdvance(it.second) + 14;
    }
}

// ── 底部信息条：显示当前悬停部件的信息 ──
void MotherboardWidget::drawInfoStrip(QPainter &p)
{
    QRectF strip(10, height() - 52, width() - 20, 42);
    p.setPen(QPen(QColor("#cbd5e1"), 1.2));
    p.setBrush(QColor("#f8fafc"));
    p.drawRoundedRect(strip, 8, 8);

    QFont tf = font();
    tf.setBold(true);
    tf.setPointSizeF(qMin(tf.pointSizeF() + 1.0, 11.0));
    QFontMetricsF tfm(tf);

    QFont sf = font();
    sf.setBold(false);
    sf.setPointSizeF(qMax(font().pointSizeF(), 8.5));
    QFontMetricsF sfm(sf);

    const double left = strip.left() + 10;
    const double avail = strip.width() - 20;

    if (m_hover >= 0 && m_hover < m_parts.size()) {
        const BoardPart &pt = m_parts[m_hover];
        p.setFont(tf);
        p.setPen(QColor("#0f172a"));
        p.drawText(QRectF(left, strip.top() + 3, avail, tfm.height()),
                   Qt::AlignLeft, tfm.elidedText(pt.title, Qt::ElideRight, avail));

        p.setFont(sf);
        p.setPen(QColor("#475569"));
        double y = strip.top() + 3 + tfm.height();
        for (int i = 0; i < pt.info.size() && y + sfm.height() <= strip.bottom() - 2; ++i) {
            p.drawText(QRectF(left, y, avail, sfm.height()), Qt::AlignLeft,
                       sfm.elidedText(pt.info[i], Qt::ElideRight, avail));
            y += sfm.height();
        }
    } else {
        p.setFont(sf);
        p.setPen(QColor("#64748b"));
        const QString hint = "把鼠标移到主板部件上：这里显示它的信息、温度、容量（点击部件可展开下方分组）";
        p.drawText(QRectF(left, strip.top() + 4, avail, sfm.height()), Qt::AlignLeft,
                   sfm.elidedText(hint, Qt::ElideRight, avail));
        drawLegend(p, QRectF(left, strip.top() + 22, avail, 18));
    }
}

void MotherboardWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    QRectF b = boardRect();

    // PCB
    QLinearGradient g(b.topLeft(), b.bottomRight());
    g.setColorAt(0.0, QColor("#1f6b52"));
    g.setColorAt(1.0, QColor("#12352a"));
    p.setPen(QPen(QColor("#0a231b"), 3.0));
    p.setBrush(g);
    p.drawRoundedRect(b, 14, 14);

    // 装饰走线
    p.setPen(QPen(QColor("#2f8f6d"), 1.2));
    p.drawLine(QPointF(b.left() + 8, b.top() + 0.44 * b.height()),
               QPointF(b.left() + 0.34 * b.width(), b.top() + 0.44 * b.height()));
    p.drawLine(QPointF(b.left() + 0.39 * b.width(), b.top() + 8),
               QPointF(b.left() + 0.39 * b.width(), b.top() + 0.46 * b.height()));
    p.drawLine(QPointF(b.left() + 0.78 * b.width(), b.top() + 0.30 * b.height()),
               QPointF(b.right() - 8, b.top() + 0.30 * b.height()));
    p.drawLine(QPointF(b.left() + 0.42 * b.width(), b.bottom() - 10),
               QPointF(b.right() - 10, b.bottom() - 10));

    // 安装孔
    const QList<QPointF> holes = {
        QPointF(b.left() + 14, b.top() + 14),
        QPointF(b.right() - 14, b.top() + 14),
        QPointF(b.left() + 14, b.bottom() - 14),
        QPointF(b.right() - 14, b.bottom() - 14),
    };
    for (const auto &h : holes) {
        p.setPen(QPen(QColor("#3b7f66"), 1.5));
        p.setBrush(QColor("#0a231b"));
        p.drawEllipse(h, 6, 6);
    }

    // 丝印（右列空档）
    QFont sf = font();
    sf.setBold(true);
    sf.setPointSizeF(qMax(font().pointSizeF() - 1.0, 7.5));
    p.setFont(sf);
    p.setPen(QColor("#7fb99f"));
    QFontMetricsF sfm(sf);
    QRectF silk(b.left() + 0.610 * b.width(), b.bottom() - 0.10 * b.height(),
               0.340 * b.width(), 0.08 * b.height());
    p.drawText(silk, Qt::AlignCenter, sfm.elidedText("SERVER TOOLBOX · MB", Qt::ElideRight, silk.width()));

    // 部件
    for (int i = 0; i < m_parts.size(); ++i)
        drawPart(p, m_parts[i], i == m_hover);

    // 信息条
    drawInfoStrip(p);
}

void MotherboardWidget::mouseMoveEvent(QMouseEvent *event)
{
    int idx = partAt(event->pos());
    if (idx != m_hover) {
        m_hover = idx;
        update();
    }
    if (idx >= 0) {
        QToolTip::showText(event->globalPosition().toPoint(), tipHtml(m_parts[idx]), this);
        setCursor(Qt::PointingHandCursor);
    } else {
        QToolTip::hideText();
        setCursor(Qt::ArrowCursor);
    }
    QWidget::mouseMoveEvent(event);
}

void MotherboardWidget::mousePressEvent(QMouseEvent *event)
{
    int idx = partAt(event->pos());
    if (idx >= 0)
        emit partClicked(m_parts[idx].id);
    QWidget::mousePressEvent(event);
}

void MotherboardWidget::leaveEvent(QEvent *event)
{
    if (m_hover != -1) {
        m_hover = -1;
        update();
    }
    QToolTip::hideText();
    QWidget::leaveEvent(event);
}
