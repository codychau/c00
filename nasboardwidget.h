#ifndef NASBOARDWIDGET_H
#define NASBOARDWIDGET_H

#include <QWidget>
#include <QVector>
#include <QHash>
#include <QRectF>
#include <QColor>
#include <QString>
#include "nasmodel.h"

class QPaintEvent;
class QResizeEvent;
class QMouseEvent;
class QEvent;
class QTimer;
class QPainter;

// ─────────────────────────────────────────────────────────────
//  NAS 面板：画成 2U 服务器前面板，点盘位把它"插进"阵列。
//  样式基准见 doc/demos/30-nas-2u-panel.html。
//
//  ┌─ 机箱 ─────────────────────────────────────────────┐
//  │⊕│⏻ PWR│ [01][02][03][04][05][06] │⊕              │
//  │ │⚪ HDD│ [07][08][09][10][11][12] │               │
//  │ │⚪ UID│                          │               │
//  │⊕└──────────────────────────────────┘⊕
//  └────────────────────────────────────────────────────┘
//
//  QSS 不能用的 CSS 特性都在这里用 QPainter 实现：
//    linear/radial-gradient → QLinearGradient / QRadialGradient
//    box-shadow（外发光）   → 先画一圈半透明描边再画本体
//    repeating-linear-gradient 冲孔网 → 循环 fillRect
//    ::after 斜纹           → setClipRect + 平行线
//
//  状态语义（和 demo 一致，别在这里额外发明）：
//    青色描边+外发光+条带序号 = 本次选为成员
//    紫色描边                = 已被其它阵列占用（不是故障，不画红纹）
//    红色斜纹 + 红 LED        = 被安全规则拦下，tooltip 说明原因
//    压暗去饱和              = 空位
//  条带序号画在成员盘上：RAID0 按 0,1,2… 轮流下发，顺序决定数据落在哪个盘，
//  重组阵列时顺序错就等于数据废掉，所以这是运维信息而非装饰。
//
//  这一层只画、只报"用户想插哪块盘"，不执行任何磁盘操作。
// ─────────────────────────────────────────────────────────────
class NasBoardWidget : public QWidget
{
    Q_OBJECT
public:
    struct Disk
    {
        QString device;         // /dev/sdb1
        QString label;          // sdb1
        QString fstype;
        QString mountpoint;
        qint64  bytes = 0;
        qint64  freeBytes = 0;
        bool    selectable = true;
        QString blockReason;    // 不可选原因，进 tooltip
        bool    inOtherArray = false;   // 已在别的阵列里 → 紫色
        bool    systemDisk   = false;   // 承载系统/用户分区 → 画锁标
        bool    faulty       = false;   // 真故障（SMART 等）→ 红斜纹；
                                        // 仅"不可选"不足以判成故障，别一律画红纹
    };

    explicit NasBoardWidget(QWidget *parent = nullptr);

    void setDisks(const QVector<Disk> &disks);
    void setEngine(Nas::Engine engine);
    void setEngineLabel(const QString &text);

    // 成员顺序 = 条带顺序，所以这里存的是**有序**列表而不是集合
    void setMembers(const QVector<QString> &orderedDevices);

    // 已在某个阵列/合并卷里的分区 → 盘位底下画一条该阵列的横线。
    // 颜色由页面分配（同色 = 同一个阵列），这里只负责画 —— 颜色语义
    // 属于"这台机器上有哪几套卷"，不是画板该知道的事。
    struct SlotMark
    {
        QColor color;      // 横线颜色
        QString note;      // 悬停时说明是哪一套（"自研镜像「nas」"）
    };
    void setSlotMarks(const QHash<QString, SlotMark> &marks);
    QVector<QString> members() const { return m_members; }

    // 活动 LED 的闪动（真实硬盘灯就是闪的；静止的黄灯会被读成"故障"）
    void setBlink(bool on);

    // 高度由宽度按 2U 比例（482.6mm x 88.9mm = 5.43:1）算出，
    // 三个尺寸函数口径一致，布局才拉不高它。
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    // 与 chassisRect 同一算法：给定宽度下机箱应有的高度
    qreal idealHeight(int forWidth) const;

    // 自检辅助（无 GUI 断言用）
    int diskCount() const { return m_disks.size(); }
    int slotCount() const { return m_slotRects.size(); }
    int memberCount() const { return m_members.size(); }
    QRectF slotRectAt(int i) const;
    int slotIndexAt(const QPointF &pos) const;          // -1 = 不在盘位上
    int memberOrderOf(const QString &device) const;      // -1 = 非成员
    QString tipForSlot(int i) const;

signals:
    void slotClicked(const QString &device, int slotIndex);
    // 用户点了不可用的盘位；原因已由 judge 算好
    void slotRejected(const QString &device, int slotIndex, const QString &why);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QRectF chassisRect() const;
    void relayout();

    void drawChassis(QPainter &p, const QRectF &c);
    void drawControlColumn(QPainter &p, const QRectF &area);
    void drawBay(QPainter &p, const Disk &d, int slot, const QRectF &r);
    void drawHandle(QPainter &p, const QRectF &bay);
    void drawVent(QPainter &p, const QRectF &bay);
    void drawBayLeds(QPainter &p, const QRectF &bay, const Disk &d, int order);
    void drawStripeBadge(QPainter &p, const QRectF &bay, int order);
    void drawDiagonalHatch(QPainter &p, const QRectF &r, const QColor &c);

    int diskIndexByDevice(const QString &device) const;
    // 按当前宽度压住 maximumHeight，防布局拉伸
    void applyHeightLimit();
    static int columnsFor(int count);

    QVector<Disk> m_disks;
    QVector<QRectF> m_slotRects;
    QVector<QString> m_members;         // 有序 = 条带顺序
    QHash<QString, SlotMark> m_slotMarks;   // 键 = 设备路径
    Nas::Engine m_engine = Nas::Engine::Mdadm;
    QString m_engineLabel = QStringLiteral("mdadm");

    int m_hover = -1;
    bool m_blinkOn = true;
    QTimer *m_blinkTimer = nullptr;
};

#endif // NASBOARDWIDGET_H
