#ifndef MOTHERBOARDWIDGET_H
#define MOTHERBOARDWIDGET_H

#include <QWidget>
#include <QVector>
#include <QRectF>
#include <QColor>
#include <QString>
#include <QStringList>

class QPaintEvent;
class QResizeEvent;
class QMouseEvent;
class QEvent;
class QPainter;

// ── 主板上的一个可交互部件 ──
struct BoardPart {
    QString id;          // "cpu" / "mem" / "swap" / "m2_0" / "sata1" / "gpu0" / "pch" / "lan" / "power"
    QString title;       // 部件名（画在部件内第一行）
    QStringList lines;   // 部件内第二、三行（简短数据）
    QStringList info;    // 悬停浮层的完整信息（key value 行）
    QColor color;        // 类别配色：CPU / GPU / 内存 / 磁盘接口
    QRectF rect;         // 画布上的位置
    double fill = -1.0;  // 0..1 使用率进度条，-1 = 不画
    bool chip = false;   // 画成芯片（四周画引脚）
};

// ── 卡通主板画板：鼠标指到部件上就显示该部件的信息、温度、容量 ──
class MotherboardWidget : public QWidget
{
    Q_OBJECT
public:
    struct DiskPart {
        QString name, model, size, used, mount, temp;
        bool nvme = false;
        double fill = -1.0;
    };
    struct GpuPart {
        QString name, model, slot, temp, sclk, vram, fan;
        double fill = -1.0;
    };

    explicit MotherboardWidget(QWidget *parent = nullptr);

    void setCpu(const QString &usage, const QString &temp);
    void setMemory(const QString &text, double ratio);
    void setSwap(const QString &text, double ratio);
    void setBoardInfo(const QString &os, const QString &kernel, const QString &uptime);
    void setNetwork(const QString &text);
    void setNvmeTemp(const QString &temp);
    void setDisks(const QVector<DiskPart> &disks);
    void setGpus(const QVector<GpuPart> &gpus);

    QSize sizeHint() const override { return QSize(420, 572); }

    // 调试/自检辅助（供测试程序读取部件布局）
    int partCount() const { return m_parts.size(); }
    int hitTest(const QPointF &pos) const { return partAt(pos); }
    QString partId(int i) const { return (i >= 0 && i < m_parts.size()) ? m_parts[i].id : QString(); }
    QRectF partRect(int i) const { return (i >= 0 && i < m_parts.size()) ? m_parts[i].rect : QRectF(); }
    QStringList partLines(int i) const { return (i >= 0 && i < m_parts.size()) ? m_parts[i].lines : QStringList(); }
    QString partTitle(int i) const { return (i >= 0 && i < m_parts.size()) ? m_parts[i].title : QString(); }
    QString tipAt(int i) const { return (i >= 0 && i < m_parts.size()) ? tipHtml(m_parts[i]) : QString(); }

signals:
    // 点击部件：StatusPage 用来展开对应的折叠分组
    void partClicked(const QString &id);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QRectF boardRect() const;
    void relayout();
    int partAt(const QPointF &pos) const;
    QString tipHtml(const BoardPart &p) const;
    void drawPart(QPainter &p, const BoardPart &part, bool hot);
    void drawInfoStrip(QPainter &p);
    void drawLegend(QPainter &p, const QRectF &area);

    QVector<BoardPart> m_parts;
    int m_hover = -1;

    QString m_cpuUsage = "—", m_cpuTemp = "—";
    QString m_memText = "—", m_swapText = "—";
    QString m_os = "—", m_kernel = "—", m_uptime = "—", m_net = "—", m_nvmeTemp = "—";
    double m_memRatio = -1.0, m_swapRatio = -1.0;
    QVector<DiskPart> m_disks;
    QVector<GpuPart> m_gpus;
};

#endif // MOTHERBOARDWIDGET_H
