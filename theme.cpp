#include "theme.h"

#include <QColor>
#include <QString>
#include <QStringList>

// ─────────────────────────────────────────────────────────────
//  QSS 生成：一整段模板 + @令牌@ 占位符替换。
//
//  之所以不用字符串拼接：分段拼接极易在边界丢掉 ';' 或 '}'
//  （历史上就漏过两处收尾花括号）。模板整体可读、可 diff，
//  替换阶段再逐条校验，保证不会出现半截规则。
// ─────────────────────────────────────────────────────────────

namespace {

// 把 QColor 转成 #rrggbb
static QString hex(const QColor &c)
{
    return QStringLiteral("#") + QString::number(c.red(),   16).rightJustified(2, '0')
                               + QString::number(c.green(), 16).rightJustified(2, '0')
                               + QString::number(c.blue(),  16).rightJustified(2, '0');
}

// 模板本体：颜色一律写成 @name@，由下方令牌表替换。
// 注意：每个选择器块都必须在本模板内自成一体，不要跨段拼接。
static const char *kTemplate = R"QSS(
QWidget {
    color: @text@;
    background: @bg@;
    font-family: "Noto Sans CJK SC", "PingFang SC", "Microsoft YaHei", sans-serif;
    font-size: 13px;
}
QMainWindow, QDialog, QStackedWidget, QScrollArea, QTabWidget > QWidget {
    background: @bg@;
}
QToolTip {
    color: @text@; background: @surface@;
    border: 1px solid @border_hi@; border-radius: 8px; padding: 6px 9px;
}
QStatusBar {
    color: @dim@; background: @surface@;
    border-top: 1px solid @border@;
}

/* ── 页签：胶囊式 ── */
QTabWidget::pane {
    border: 1px solid @border@; border-radius: 12px;
    background: @surface@; top: -1px;
}
QTabBar { background: transparent; qproperty-drawBase: 0; }
QTabBar::tab {
    color: @dim@; background: transparent;
    padding: 7px 14px; margin: 4px 1px 4px 0;
    border: 1px solid transparent; border-radius: 999px;
}
QTabBar::tab:hover { color: @text@; background: @surface2@; }
QTabBar::tab:selected {
    color: @text@; background: @surface2@; font-weight: 600;
    border-color: @border_hi@;
}

/* ── 侧栏导航（demo 01 的 .sidebar / .nav-item / .nav-sep / .brand）──
   QSS 不支持 box-shadow 与 backdrop-filter，所以：
   - demo 的 inset 2px 主色指示条 → 用 border-left 实心复刻
   - 半透明侧栏底 → 用不透明的深色底近似
   - hover 的 translateX(2px) → 无法实现，改为变色 */
QWidget#sidebar {
    background: @bg@;
    border-right: 1px solid @border@;
}
QLabel#brandMark {
    color: @on_accent@; font-size: 17px;
    background: @accent@; border-radius: 8px;
}
QLabel#brandName { color: @text@; font-size: 14px; font-weight: 700; }
QLabel#brandSub  { color: @mute@; font-size: 11px; }
QWidget#topbar {
    background: @surface@;
    border-bottom: 1px solid @border@;
}
QLabel#pageTitle { color: @text@; font-size: 16px; font-weight: 700; }
QLabel#pageSub   { color: @mute@; font-size: 12px; }
QLabel#navSep {
    color: @mute@; font-size: 11px; font-weight: 600;
    padding: 4px 8px 2px 8px; letter-spacing: 1px;
}
QPushButton#navItem {
    min-height: 30px; max-height: 30px;
    padding: 0 10px;
    color: @dim@; background: transparent;
    border: 1px solid transparent; border-left: 2px solid transparent;
    border-radius: 7px;
    text-align: left;
}
QPushButton#navItem:hover {
    color: @text@; background: @surface2@;
    border-left-color: @border_hi@;
}
QPushButton#navItem:checked {
    color: @text@; background: @surface2@; font-weight: 600;
    border: 1px solid @border@; border-left: 2px solid @accent@;
}

/* ── 卡片化分组框 ── */
QGroupBox {
    color: @text@; background: @surface@;
    border: 1px solid @border@; border-radius: 12px;
    margin-top: 12px; padding: 14px 12px 12px 12px; font-weight: 600;
}
QGroupBox::title {
    subcontrol-origin: margin; left: 12px; padding: 0 6px;
    color: @dim@; font-weight: 600;
}
QGroupBox:disabled { color: @mute@; border-color: @border@; }

/* 提示条：QFrame 打 role="callout" */
QFrame[role="callout"] {
    background: @surface2@; border: 1px solid @border@;
    border-left: 3px solid @accent@; border-radius: 8px;
}

/* ── 按钮：保留原有 25px 高度约束，不撑破既有布局 ── */
QPushButton {
    min-height: 25px; max-height: 25px; padding: 2px 12px;
    color: @text@; background: @surface2@;
    border: 1px solid @border@; border-radius: 8px;
}
QPushButton:hover   { background: @surface3@; border-color: @border_hi@; }
QPushButton:pressed { background: @surface@; }
QPushButton:focus   { border-color: @accent@; }
QPushButton:checked {
    background: @accent@; color: @on_accent@;
    border-color: @accent@; font-weight: 600;
}
QPushButton:disabled {
    color: @mute@; background: @surface@; border-color: @border@;
}
/* 主操作：btn->setProperty("role", "primary") */
QPushButton[role="primary"] {
    color: @on_accent@; background: @accent@;
    border: 1px solid @accent@; font-weight: 700;
}
QPushButton[role="primary"]:hover   { background: @accent2@; border-color: @accent2@; }
QPushButton[role="primary"]:disabled {
    color: @mute@; background: @surface2@; border-color: @border@;
}
/* 危险操作：btn->setProperty("role", "danger") */
QPushButton[role="danger"] {
    color: @bad@; background: @surface@; border-color: @bad@;
}
QPushButton[role="danger"]:hover { background: @surface3@; }
/* 折叠分组标题：占满整行、左对齐 */
QPushButton[role="toggle"] {
    min-height: 34px; max-height: 34px; padding: 6px 12px;
    text-align: left; font-weight: 600;
    background: @surface2@; border-color: @border@; border-radius: 10px;
}
QPushButton[role="toggle"]:hover {
    background: @surface3@; border-color: @border_hi@;
}
/* 表格里的小按钮 */
QPushButton[role="tool"] {
    min-height: 22px; max-height: 22px; padding: 1px 8px; font-size: 12px;
}

/* ── 表格 / 树 / 列表 ── */
QTableWidget, QTreeView, QListView, QListWidget, QTreeWidget {
    color: @text@; background: @surface@;
    alternate-background-color: @surface2@;
    border: 1px solid @border@; border-radius: 10px;
    gridline-color: @border@; outline: 0;
    selection-background-color: @surface3@; selection-color: @accent@;
}
/* 表格获得焦点时描一圈主色，替代系统默认的虚线焦点框 */
QTableView:focus, QTreeView:focus, QListView:focus,
QTableWidget:focus, QTreeWidget:focus, QListWidget:focus {
    border: 1px solid @border_hi@;
}
QHeaderView { background: transparent; }
QHeaderView::section {
    color: @dim@; background: @surface2@;
    padding: 7px 8px; border: none;
    border-bottom: 1px solid @border@; font-weight: 600;
}
/* 纵向表头（行号）同样走暗色：部分表格未调用 verticalHeader()->hide() */
QTableView QTableCornerButton::section {
    color: @mute@; background: @surface2@;
    border: none; border-bottom: 1px solid @border@;
}
QHeaderView::section:vertical {
    color: @mute@; background: @surface2@;
    border: none; border-right: 1px solid @border@; padding: 4px;
}
QTableWidget::item { padding: 4px 6px; border: none; }
QTableWidget::item:hover    { background: @surface2@; }
QTableWidget::item:selected { background: @accent@; color: @on_accent@; }
QTableView::item:selected   { background: @accent@; color: @on_accent@; }
QListWidget::item, QTreeWidget::item { padding: 4px 6px; border-radius: 6px; }
QListWidget::item:hover, QTreeWidget::item:hover { background: @surface2@; }
QListWidget::item:selected, QTreeWidget::item:selected {
    background: @surface3@; color: @accent@;
}

/* ── 输入控件 ── */
QLineEdit, QPlainTextEdit, QTextEdit, QAbstractSpinBox, QComboBox {
    color: @text@; background: @surface2@;
    border: 1px solid @border@; border-radius: 8px; padding: 5px 9px;
    selection-background-color: @surface3@; selection-color: @accent@;
}
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus,
QAbstractSpinBox:focus, QComboBox:focus { border-color: @accent@; }
QLineEdit:disabled, QPlainTextEdit:disabled, QTextEdit:disabled,
QAbstractSpinBox:disabled, QComboBox:disabled {
    color: @mute@; background: @surface@;
}
QLineEdit[role="mono"], QPlainTextEdit[role="mono"],
QTextEdit[role="mono"], QLabel[role="mono"] {
    font-family: "JetBrains Mono", "DejaVu Sans Mono", monospace; font-size: 12px;
}
QComboBox::drop-down { border: none; width: 20px; }
QComboBox QAbstractItemView {
    color: @text@; background: @surface@;
    border: 1px solid @border_hi@; border-radius: 10px;
    selection-background-color: @surface3@; selection-color: @accent@;
}
QCheckBox, QRadioButton { spacing: 8px; background: transparent; }
QCheckBox::indicator, QRadioButton::indicator {
    width: 16px; height: 16px;
    border: 1px solid @border_hi@; background: @surface2@;
}
QCheckBox::indicator { border-radius: 5px; }
QRadioButton::indicator { border-radius: 9px; }
QCheckBox::indicator:checked, QRadioButton::indicator:checked {
    background: @accent@; border-color: @accent@;
}
QSlider::groove:horizontal { height: 6px; background: @surface@; border-radius: 3px; }
QSlider::sub-page:horizontal { background: @accent@; border-radius: 3px; }
QSlider::handle:horizontal {
    width: 14px; margin: -5px 0; border-radius: 7px; background: @text@;
}
QSlider::handle:horizontal:hover { background: @accent@; }

/* 进度条：圆角细身；按阈值取色请覆盖 chunk（见 Theme::levelColor） */
QProgressBar {
    background: @surface@; border: none; border-radius: 4px;
    min-height: 8px; max-height: 8px; text-align: center; color: transparent;
}
QProgressBar::chunk { border-radius: 4px; background: @accent@; }

QScrollBar:vertical { background: transparent; width: 11px; margin: 2px; }
QScrollBar::handle:vertical {
    background: @border_hi@; border-radius: 4px; min-height: 32px;
}
QScrollBar::handle:vertical:hover { background: @accent@; }
QScrollBar:horizontal { background: transparent; height: 11px; margin: 2px; }
QScrollBar::handle:horizontal {
    background: @border_hi@; border-radius: 4px; min-width: 32px;
}
QScrollBar::handle:horizontal:hover { background: @accent@; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

/* ── 文字层级：给控件打 role，颜色集中在这里 ── */
QLabel { background: transparent; }
QLabel[role="title"] { color: @text@; font-size: 17px; font-weight: 700; }
QLabel[role="h2"]    { color: @text@; font-size: 14px; font-weight: 700; }
QLabel[role="dim"]   { color: @dim@; }
QLabel[role="mute"]  { color: @mute@; font-size: 12px; }
QLabel[role="stat"]  { color: @text@; font-size: 20px; font-weight: 700; }
QLabel[role="key"]   { color: @dim@; font-weight: 600; }
QLabel[role="val"]   { color: @text@; }
QLabel[role="ok"]    { color: @ok@; font-size: 12px; font-weight: 600; }
QLabel[role="warn"]  { color: @warn@; font-size: 12px; font-weight: 600; }
QLabel[role="bad"]   { color: @bad@; font-size: 12px; font-weight: 600; }
QLabel[role="badge"] {
    color: @dim@; background: @surface2@;
    border: 1px solid @border@; border-radius: 999px;
    padding: 2px 10px; font-size: 12px;
}

    /* 说明框：给「这件事还没验证 / 还没接」的提示用，刻意区别于错误。
       用 @warn@ 会像故障，用 @bad@ 更像坏了；@accent2@ 表示补充信息，
       不占用状态语义。 */
    QLabel[role="note"] {
        padding: 8px 10px; border-radius: 5px;
        background: rgba(167, 139, 250, 20);
        border: 1px solid rgba(167, 139, 250, 72);
        color: @accent2@;
    }

    /* 代码块：比 @surface@ 更深一档的底色，主题里原本没有这一级 */
    QLabel[role="code"] {
        padding: 8px 10px; border-radius: 5px;
        background: @bg@;
        border: 1px solid @border@;
        color: @dim@;
    }

    QLabel[role="pill"] {
        padding: 2px 8px; border-radius: 4px;
        background: @surface3@;
        color: @mute@;
    }
QLabel[role="hero"] {
    color: @text@; background: @surface2@;
    border: 1px solid @border@; border-left: 3px solid @accent2@;
    border-radius: 10px; padding: 8px 12px; font-weight: 600;
}

/* ── 菜单 / 对话框 / 分割线 ── */
QMenu {
    color: @text@; background: @surface@;
    border: 1px solid @border_hi@; border-radius: 10px; padding: 6px;
}
QMenu::item { padding: 6px 24px 6px 16px; border-radius: 6px; }
QMenu::item:selected { background: @accent@; color: @on_accent@; }
QMenu::item:disabled { color: @mute@; }
QMenu::separator { height: 1px; background: @border@; margin: 6px 4px; }
QMenu::icon { padding-left: 6px; }
QMessageBox, QInputDialog, QFileDialog { background: @surface@; }
QSplitter::handle { background: @border@; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }
)QSS";

} // namespace

QString Theme::globalStyleSheet()
{
    // 令牌表：模板里的 @name@ 换成这里的十六进制
    const QList<QPair<QString, QColor>> tokens = {
        { "bg",         bg()        },
        { "surface",    surface()   },
        { "surface2",   surface2()  },
        { "surface3",   surface3()  },
        { "border",     border()    },
        { "border_hi",  borderHi()  },
        { "text",       text()      },
        { "dim",        textDim()   },
        { "mute",       textMute()  },
        { "accent",     accent()    },
        { "accent2",    accent2()   },
        { "on_accent",  onAccent()  },
        { "ok",         ok()        },
        { "warn",       warn()      },
        { "bad",        bad()       },
        { "info",       info()      },
    };

    QString qss = QString::fromUtf8(kTemplate);

    // 顶部注明主题与"请勿在此手改颜色"，便于日后排查
    qss.prepend(QStringLiteral("/* 家用服务器工具箱 · 主题 %1 · 颜色请改 theme.h，勿改此文件 */\n")
                    .arg(QString::fromUtf8(name())));

    for (const auto &t : tokens)
        qss.replace(QStringLiteral("@%1@").arg(t.first), hex(t.second));

    // 防御：模板里若漏写了令牌，或花括号不配平，立刻在 debug 构建暴露
    Q_ASSERT_X(!qss.contains('@'), "Theme::globalStyleSheet",
               "QSS 模板存在未替换的 @令牌@");
    Q_ASSERT_X(qss.count('{') == qss.count('}'), "Theme::globalStyleSheet",
               "QSS 花括号不配平，检查 theme.cpp 模板");

    return qss;
}
