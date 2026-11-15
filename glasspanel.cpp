#include "glasspanel.h"

#include <QPainter>
#include <QLinearGradient>
#include <QImage>
#include <QPixmap>
#include <QEvent>
#include <QtMath>

// ── 盒模糊（积分图实现，O(n) 与半径无关）──
//
// 用 QImage::format_RGB32 直接操作 QRgb。对每个像素取以它为中心、
// 半径 r 的正方形窗口内均值。三次叠加后逼近高斯核（中心极限定理）。
static void boxBlurImage(QImage &img, int r)
{
    if (r < 1 || img.isNull()) return;
    const int w = img.width();
    const int h = img.height();

    QImage tmp = img;   // 水平/垂直两趟需要一块同尺寸暂存

    // 水平方向
    for (int y = 0; y < h; ++y) {
        const QRgb *src = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        QRgb      *dst = reinterpret_cast<QRgb *>(tmp.scanLine(y));
        qint64 sumR = 0, sumG = 0, sumB = 0;
        // 初始窗口 [-r, r]，边界处用边缘像素延展（避免边缘发暗）
        for (int k = -r; k <= r; ++k) {
            const QRgb p = src[qBound(0, k, w - 1)];
            sumR += qRed(p); sumG += qGreen(p); sumB += qBlue(p);
        }
        const qint64 n = 2 * r + 1;
        for (int x = 0; x < w; ++x) {
            dst[x] = qRgb(int(sumR / n), int(sumG / n), int(sumB / n));
            // 滑动窗口：加右邻、去左邻（越界用边缘像素补，保持窗口大小恒定）
            const QRgb add = src[qBound(0, x + r + 1, w - 1)];
            const QRgb sub = src[qBound(0, x - r, w - 1)];
            sumR += qRed(add) - qRed(sub);
            sumG += qGreen(add) - qGreen(sub);
            sumB += qBlue(add) - qBlue(sub);
        }
    }

    // 垂直方向（tmp -> img）。
    // 用每列一个滑动累加器，按行推进：既避免 QImage::pixel() 的逐点开销
    //（Qt6 已无 constPixel），也让访问模式保持按行连续。
    QVector<qint64> accR(w, 0), accG(w, 0), accB(w, 0);
    for (int k = -r; k <= r; ++k) {
        const QRgb *row = reinterpret_cast<const QRgb *>(tmp.constScanLine(qBound(0, k, h - 1)));
        for (int x = 0; x < w; ++x) {
            accR[x] += qRed(row[x]); accG[x] += qGreen(row[x]); accB[x] += qBlue(row[x]);
        }
    }
    const qint64 n = 2 * r + 1;
    for (int y = 0; y < h; ++y) {
        QRgb *dst = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < w; ++x)
            dst[x] = qRgb(int(accR[x] / n), int(accG[x] / n), int(accB[x] / n));
        if (y + r + 1 >= h && y - r < 0) continue;
        const QRgb *add = reinterpret_cast<const QRgb *>(tmp.constScanLine(qBound(0, y + r + 1, h - 1)));
        const QRgb *sub = reinterpret_cast<const QRgb *>(tmp.constScanLine(qBound(0, y - r, h - 1)));
        for (int x = 0; x < w; ++x) {
            accR[x] += qRed(add[x]) - qRed(sub[x]);
            accG[x] += qGreen(add[x]) - qGreen(sub[x]);
            accB[x] += qBlue(add[x]) - qBlue(sub[x]);
        }
    }
}

GlassPanel::GlassPanel(QWidget *parent)
    : QWidget(parent)
{
    m_tint = QColor(255, 255, 255, 22);   // 深色主题下玻璃的提亮量
    // 面板浮在内容之上，背景由 paintEvent 自己画，不需要 Qt 再清一遍
    setAttribute(Qt::WA_NoSystemBackground, true);
}

bool GlassPanel::event(QEvent *ev)
{
    // 尺寸变化后缓存的区域就不对了
    if (ev->type() == QEvent::Resize)
        m_dirty = true;
    return QWidget::event(ev);
}

bool GlassPanel::eventFilter(QObject *watched, QEvent *ev)
{
    // 背景内容重绘 / 滚动 / 尺寸变化 → 玻璃缓存过期。
    // 子控件的 UpdateRequest 会向上传播到源容器，所以只监听源本身即可。
    switch (ev->type()) {
    case QEvent::UpdateRequest:
    case QEvent::Scroll:
    case QEvent::Resize:
    case QEvent::Show:
    case QEvent::WindowStateChange:
        if (!m_dirty) { m_dirty = true; update(); }
        break;
    default:
        break;
    }
    return QWidget::eventFilter(watched, ev);
}

void GlassPanel::rebuild()
{
    const QSize sz = size();
    if (sz.isEmpty()) { m_blurred = QPixmap(); m_dirty = false; return; }

    QWidget *src = m_source;
    if (!src) { m_blurred = QPixmap(); m_dirty = false; return; }

    // 本面板与背景源是兄弟（同为 mainCol 的子控件）。
    //
    // 这里不能用 src->mapFrom(this, ...)：Qt6 的 mapFrom 要求参数控件与自己在
    // 父子链上（互为祖先），兄弟关系不满足，它会打印
    //     QWidget::mapFrom(): parent must be in parent hierarchy
    // 并返回错位的坐标 —— 实测两个都摆在 (0,0) 的同父兄弟之间，
    // mapFrom 给出的是 (-210,-2) 而不是 (0,0)。
    //
    // 同父控件共享同一个坐标系统，所以直接对 pos() 相减即可。
    const QPoint topLeftInSrc = this->pos() - src->pos();

    // 用 grab() 而不是 render()：grab 走控件自身的绘制路径，不需要父子链；
    // 本面板是源的兄弟而非子孙，不会被画进去，没有递归问题。
    const QPixmap shot = src->grab();
    QImage img = shot.copy(QRect(topLeftInSrc, sz)).toImage()
                    .convertToFormat(QImage::Format_RGB32);
    if (img.isNull() || img.size() != sz) {
        // 源还没显示或区域越界：退回不画，别拿脏数据糊上去
        m_blurred = QPixmap();
        m_dirty = false;
        return;
    }

    // 降采样 + 放大：等效放大模糊半径而开销更小（玻璃不需要保留高频细节）
    const double scale = 0.5;
    QImage small = img.scaled(int(img.width() * scale), int(img.height() * scale),
                              Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const int r = qMax(1, int(m_radius * scale / 2.0));
    for (int i = 0; i < m_passes; ++i)
        boxBlurImage(small, r);

    m_blurred = QPixmap::fromImage(
        small.scaled(sz, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    m_dirty = false;
}

void GlassPanel::paintEvent(QPaintEvent *)
{
    if (m_dirty)
        rebuild();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF r = rect();

    // 1) 模糊后的背景
    if (!m_blurred.isNull())
        p.drawPixmap(r.toRect(), m_blurred);
    else
        p.fillRect(r, QColor("#0b0f14"));

    // 2) 深色底：CSS 的 background: rgba(15,20,27,.6) 等效。
    //    透明度不宜再高，否则背后内容几乎不可见，就退化成普通深色条了。
    p.fillRect(r, QColor(15, 21, 28, 120));
    // 3) 再叠提亮色（等效 background: rgba(255,255,255,.05)）
    p.fillRect(r, m_tint);

    // 4) 环境辉光：让玻璃即使背后是空白也带一点色彩层次
    if (m_hasGlow) {
        QLinearGradient h2(r.left(), r.top(), r.right(), r.bottom());
        h2.setColorAt(0.0, m_glowL);
        h2.setColorAt(1.0, m_glowR);
        p.fillRect(r, h2);
    }

    // 5) 顶部高光渐变，模拟玻璃上缘反光
    QLinearGradient g(r.topLeft(), r.bottomLeft());
    g.setColorAt(0.0, QColor(255, 255, 255, 40));
    g.setColorAt(0.45, QColor(255, 255, 255, 0));
    p.fillRect(r, g);

    // 6) 下边框（demo 01 顶栏的 border-bottom）
    p.setPen(QColor(255, 255, 255, 28));
    p.setPen(QColor(255, 255, 255, 34));
    p.drawLine(QPointF(r.left(), r.bottom() - 0.5), QPointF(r.right(), r.bottom() - 0.5));
}
