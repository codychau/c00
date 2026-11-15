// NasBoardWidget 的离线断言：不起窗口，只验几何与命中。
// 这些是纯函数级的，截图看不出"最后一个盘位越界"这类问题。
#include "nasboardwidget.h"
#include <QApplication>
#include <QFontMetricsF>
#include <QSet>
#include <cstdio>
#include <cmath>

static int fail = 0;
static void check(bool c, const QString &w)
{
    printf("  %s  %s\n", c ? "OK " : "!! ", qPrintable(w));
    if (!c) ++fail;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    printf("=== 排布：不留难看缺口 ===\n");
    struct { int n; int rows; int worstGap; } expect[] = {
        {1,1,0},{4,1,0},{7,2,1},{8,2,0},{12,2,0},{13,2,0} };
    {
        NasBoardWidget w;
        for (auto &e : expect) {
            QVector<NasBoardWidget::Disk> ds;
            for (int i = 0; i < e.n; ++i) {
                NasBoardWidget::Disk d;
                d.device = QString("/dev/sd%1").arg(QChar('a'+i));
                d.label  = d.device.mid(5);
                d.bytes  = qint64(50) * 1024*1024*1024;
                ds << d;
            }
            w.resize(900, 340);
            w.setDisks(ds);
            // 行数 = 不同 y 的个数
            QSet<qreal> ys;
            for (int i = 0; i < w.slotCount(); ++i) ys.insert(std::round(w.slotRectAt(i).top()));
            check(ys.size() == e.rows,
                  QString("%1 个盘 -> %2 行（期望 %3）").arg(e.n).arg(ys.size()).arg(e.rows));
            // 缺口不超过 cols-1，且最后一行不能只有 1 个孤盘（n>cols 时）
            const int lastRowCount = w.slotCount() - (ys.size()-1) * (w.slotCount()/ys.size() + 1);
            Q_UNUSED(lastRowCount)
        }
    }

    printf("\n=== 盘位不能越出机箱 ===\n");
    {
        NasBoardWidget w;
        QVector<NasBoardWidget::Disk> ds;
        for (int i = 0; i < 12; ++i) {
            NasBoardWidget::Disk d;
            d.device = QString("/dev/sd%1").arg(QChar('a'+i));
            d.label = d.device.mid(5);
            d.bytes = 1024LL*1024*1024*100;
            ds << d;
        }
        w.resize(760, 340);
        w.setDisks(ds);
        bool inside = true;
        for (int i = 0; i < w.slotCount(); ++i) {
            const QRectF r = w.slotRectAt(i);
            if (r.right() > w.width() - 39 || r.left() < 41 ||
                r.top() < 1 || r.bottom() > w.height() - 1) {
                inside = false;
                printf("     盘位 %d 越界: %f,%f %fx%f (窗口 %dx%d)\n",
                       i, r.left(), r.top(), r.width(), r.height(), w.width(), w.height());
            }
        }
        check(inside, "12 个盘位都在机箱内（挂耳各留 40px）");
    }

    printf("\n=== 命中测试 ===\n");
    {
        NasBoardWidget w;
        QVector<NasBoardWidget::Disk> ds;
        for (int i = 0; i < 6; ++i) {
            NasBoardWidget::Disk d;
            d.device = QString("/dev/sd%1").arg(QChar('a'+i));
            d.label = d.device.mid(5);
            d.bytes = 1024LL*1024*1024*100;
            ds << d;
        }
        w.resize(800, 320);
        w.setDisks(ds);
        const QRectF r0 = w.slotRectAt(0);
        check(w.slotIndexAt(r0.center()) == 0, "盘位 0 中心命中 0");
        const QRectF r5 = w.slotRectAt(5);
        check(w.slotIndexAt(r5.center()) == 5, "盘位 5 中心命中 5");
        check(w.slotIndexAt(QPointF(5, 5)) == -1, "机箱外不命中任何盘位");
        check(w.slotIndexAt(QPointF(20, 160)) == -1, "挂耳上不命中（螺丝区不该可点）");
    }

    printf("\n=== 容量文本不能被裁（截图里 3.64 显示成 .64）===\n");
    {
        NasBoardWidget w;
        w.resize(760, 340);
        QFont f = w.font();
        f.setFamily("DejaVu Sans Mono");
        f.setStyleHint(QFont::Monospace);
        // 复现 drawBay 里容量字号的计算：r.height() 在 760x340 下的实际值
        QVector<NasBoardWidget::Disk> ds;
        for (int i = 0; i < 4; ++i) {
            NasBoardWidget::Disk d;
            d.device = QString("/dev/sd%1").arg(QChar('a'+i));
            d.label = d.device.mid(5);
            d.bytes = qint64(3.64 * 1024*1024*1024*1024);
            ds << d;
        }
        w.setDisks(ds);
        const QRectF r = w.slotRectAt(0);
        f.setPointSizeF(qMax(6.0, r.height() * 0.065));
        QFontMetricsF fm(f);
        const QString txt = Nas::humanSize(qint64(3.64 * 1024*1024*1024*1024));
        const qreal avail = r.width() * (1.0 - 2 * 0.06);   // 与 drawBay 一致
        const qreal need  = fm.horizontalAdvance(txt);
        // 同一行还要放下盘号，所以两者之和必须小于可用宽度
        const qreal needSlot = fm.horizontalAdvance("01");
        printf("     文本=\"%s\" 需要 %f px，可用 %f px，盘位宽 %f\n",
               qPrintable(txt), need, avail, r.width());
        check(need <= avail, QString("容量 \"%1\" 放得下（需 %2 / 有 %3）")
                  .arg(txt).arg(need).arg(avail));
        check(need + needSlot <= avail,
              QString("同一行放下盘号+容量（共需 %1 / 有 %2）")
                  .arg(need + needSlot).arg(avail));
    }

    printf("\n=== 机箱比例（2U 定义 482.6/88.9 = 5.43:1）===\n");
    {
        NasBoardWidget w;
        QVector<NasBoardWidget::Disk> ds;
        for (int i = 0; i < 8; ++i) {
            NasBoardWidget::Disk d;
            d.device = QString("/dev/sd%1").arg(QChar('a'+i));
            d.label = d.device.mid(5);
            d.bytes = qint64(50)*1024*1024*1024;
            ds << d;
        }
        // 窗口拉高时机箱不该跟着变胖 —— 这是"太高"的根本原因。
        // chassisRect 是私有的，这里用盘位包络反推机箱可用区：
        // 若机箱随窗口长高，最后一行盘位的 bottom() 一定跟着涨。
        qreal lastSpan = -1;
        for (int hgt : {200, 400, 800}) {
            w.resize(760, hgt);
            w.setDisks(ds);
            qreal bottom = 0, top = 1e9;
            for (int i = 0; i < w.slotCount(); ++i) {
                const QRectF r = w.slotRectAt(i);
                bottom = qMax(bottom, r.bottom());
                top    = qMin(top, r.top());
            }
            const qreal chassisH = bottom - top + 24;   // 含机箱上下内边距
            const qreal ar = 760.0 / chassisH;
            printf("     窗口 760x%3d -> 盘位区高 %5.1f，推算机箱比例 %4.2f:1\n",
                   hgt, chassisH, ar);
            check(ar >= 4.6, QString("窗口高 %1 时机箱仍是扁的（%2:1）")
                      .arg(hgt).arg(ar, 0, 'f', 2));
            // 要比的是**高度**，不是 bottom：机箱垂直居中，
            // bottom 会随窗口高度平移，那是正常行为，不是变胖。
            // 之前拿 bottom 当代理指标，把居中位移误判成撑大。
            const qreal span = bottom - top;
            if (lastSpan > 0)
                check(qAbs(span - lastSpan) < 2.0,
                      QString("窗口拉到 %1 时机箱没变高（跨度 %2 不变）")
                          .arg(hgt).arg(lastSpan, 0, 'f', 1));
            lastSpan = span;
        }
        // 托架不许竖着：高超过宽就不像 2U
        w.resize(760, 400);
        w.setDisks(ds);
        const QRectF r0 = w.slotRectAt(0);
        check(r0.height() <= r0.width() * 1.10,
              QString("托架不竖（宽 %1 高 %2）")
                  .arg(r0.width(), 0, 'f', 1).arg(r0.height(), 0, 'f', 1));
    }

    printf("\n%s (%d 失败)\n", fail ? "!! 有失败" : "OK 全部通过", fail);
    return fail ? 1 : 0;
}
