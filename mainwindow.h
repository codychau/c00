#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QPushButton>
#include <QList>
#include <QString>

class QEvent;

class QStackedWidget;
class QVBoxLayout;
class QLabel;

// 主窗口：左侧竖向导航 + 顶栏标题 + 右侧页面堆栈，
// 对应 doc/demos/01-dark-console.html 的 .sidebar / .topbar / .main 结构。
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    class NavItem;   // 侧栏一行（checkable QPushButton）

    // 顶栏与堆栈几何（两者重叠，堆栈铺满 + 顶栏贴顶浮层）
    void relayoutMainCol();

    // 向侧栏追加一项；group 非空时先插入分组小标题。
    // sub 为顶栏副标题（各页刷新节奏不同，不能一概写"每 8 秒自动刷新"）。
    void addNav(const QString &group, const QString &icon, const QString &text,
                QWidget *page, const QString &sub = QString());

    QStackedWidget       *m_pages = nullptr;
    QWidget              *m_mainCol = nullptr;
    class GlassPanel     *m_topbar = nullptr;
    QVBoxLayout          *m_navLay = nullptr;
    QList<NavItem *>      m_items;
    QLabel               *m_title = nullptr;
    QLabel               *m_subtitle = nullptr;
};

#endif // MAINWINDOW_H
