#pragma once

#include <QColor>
#include <QString>

// ─────────────────────────────────────────────────────────────
//  全局主题：深色机房控制台
//
//  颜色只有这一个来源。页面代码不要再写 setStyleSheet("color: #xxx")，
//  改为给控件打 role 属性，由 globalStyleSheet() 里的属性选择器统一驱动：
//
//      label->setProperty("role", "mute");
//      btn->setProperty("role", "primary");
//
//  自绘控件（主板画板等）直接取本命名空间的 QColor。
// ─────────────────────────────────────────────────────────────
namespace Theme {

// 换肤只需改这一处：主题名，写入 QSS 顶部注释便于排查
inline const char *name() { return "深色机房控制台"; }

// ── 背景与容器 ──
inline QColor bg()        { return QColor("#0b0f14"); }   // 窗口底色
inline QColor bgRaised()  { return QColor("#0e141b"); }   // 滚动区 / 内凹区
inline QColor surface()   { return QColor("#131a23"); }   // 卡片 / 表格 / 弹层
inline QColor surface2()  { return QColor("#1a2430"); }   // 次级：表头、输入框、按钮
inline QColor surface3()  { return QColor("#202d3b"); }   // hover / 三级
inline QColor border()    { return QColor("#26333f"); }   // 常规描边
inline QColor borderHi()  { return QColor("#33475b"); }   // hover 描边

// ── 文字层级 ──
inline QColor text()      { return QColor("#e6edf3"); }   // 正文与数值
inline QColor textDim()   { return QColor("#8b9bab"); }   // 次要
inline QColor textMute()  { return QColor("#5b6b7b"); }   // 弱化：单位、提示、图例

// ── 强调色（主板画板的芯片配色沿用同一组） ──
inline QColor accent()    { return QColor("#38bdf8"); }   // 主色：选中、焦点、进度
inline QColor accent2()   { return QColor("#a78bfa"); }   // 辅色：渐变收尾
inline QColor onAccent()  { return QColor("#04121c"); }   // 落在 accent 上的文字

// ── 语义色：状态含义跨页面保持一致 ──
inline QColor ok()        { return QColor("#4ade80"); }
inline QColor warn()      { return QColor("#fbbf24"); }
inline QColor bad()       { return QColor("#f87171"); }
inline QColor info()      { return QColor("#60a5fa"); }

// ── 进度条：按使用率取色（与状态页告警阈值一致） ──
inline QColor levelColor(double ratio)
{
    if (ratio < 0.0)  return accent();
    if (ratio < 0.60) return ok();
    if (ratio < 0.80) return warn();
    return bad();
}

// 半透明叠加（画板上的一次性遮罩、发光等）
inline QColor alpha(const QColor &c, int a) { QColor r = c; r.setAlpha(a); return r; }

// ── 全局 QSS：在 main() 里 app.setStyleSheet(Theme::globalStyleSheet()) ──
QString globalStyleSheet();

} // namespace Theme
