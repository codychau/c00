#ifndef GLASSPANEL_H
#define GLASSPANEL_H

#include <QWidget>

// 毛玻璃面板。
//
// QSS 没有 backdrop-filter，所以这里手动实现"高斯模糊玻璃"：
//   1. 从祖先控件抓取本面板背后的像素（背景内容）
//   2. 对这块图做盒模糊（box blur）三次叠加 —— 按中心极限定理，
//      三次盒模糊在视觉上已经非常接近真正的高斯核
//   3. 叠一层半透明提亮色 + 顶部高光描边，模拟玻璃的反光
//
// 用法：面板必须与"要模糊的内容"重叠（通常是把内容铺满、面板浮在上层），
// 这样它背后才有真实像素可抓。见 MainWindow 里顶栏的用法。
class GlassPanel : public QWidget
{
    Q_OBJECT

public:
    explicit GlassPanel(QWidget *parent = nullptr);

    // 内容顶部留白：页面从 y=0 铺起（这样玻璃背后才有真实内容可采样），
    // 正文则通过 inset 推到顶栏下方。滚动时内容会从玻璃底下穿过。
    // 由 MainWindow 给每个页面设置，玻璃只负责记录高度。
    void setBackdropSource(QWidget *source)
    {
        if (m_source == source) return;
        if (m_source) m_source->removeEventFilter(this);
        m_source = source;
        // 监听源控件的内容变化，及时丢弃模糊缓存
        if (m_source) m_source->installEventFilter(this);
        revalidate();
    }

    void setBlurRadius(int px) { m_radius = px; revalidate(); }
    void setTint(const QColor &tint) { m_tint = tint; update(); }
    QColor tint() const { return m_tint; }

    // 环境辉光：从左到右的一层低饱和渐变，叠在模糊结果之上。
    // 用途是"背后没有内容时也能看出这是玻璃"——比如本应用顶栏在
    // 首屏时背后只有页面空白边距，纯模糊会看起来像普通深色条。
    void setAmbientGlow(const QColor &left, const QColor &right)
    {
        m_glowL = left; m_glowR = right; m_hasGlow = true; update();
    }

    // 文字安静区：标题/副标题都在玻璃左侧，辉光与顶部高光是整条 fillRect，
    // 会连文字背后一起染色 —— 背后内容一亮，文字就像"加了底色"。把这两个
    // 装饰层裁剪到本线右侧，文字背后就只剩"模糊 + 深色底"的干净玻璃。
    // 传 0 或不设 = 不裁剪（整条都上色，恢复旧观感）。
    void setTextQuietRight(int x) { m_quietRight = x; update(); }

    // 背景内容发生变化时丢弃缓存（比如页面切换、滚动、窗口尺寸变化）
    void revalidate() { m_dirty = true; update(); }

protected:
    void paintEvent(QPaintEvent *event) override;
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void rebuild();

    QWidget *m_source = nullptr;
    int      m_radius = 16;      // 单次盒模糊半径
    int      m_passes = 3;       // 盒模糊叠加次数（3 次 ≈ 高斯）
    QColor   m_tint;             // 玻璃叠加色（含 alpha）
    QColor   m_glowL, m_glowR;   // 环境辉光两端色
    bool     m_hasGlow = false;
    int      m_quietRight = 0;   // 文字安静区右边界（玻璃本地 x）
    QPixmap  m_blurred;          // 缓存：已模糊的背景
    bool     m_dirty = true;
};

#endif // GLASSPANEL_H
