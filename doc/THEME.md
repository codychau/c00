# 界面主题（深色机房控制台）

配色来自 `doc/demos/01-dark-console.html`，落地为 Qt 全局样式表。

## 唯一颜色来源

**`theme.h`** 定义所有颜色，`theme.cpp` 生成 QSS。改动配色只改 `theme.h`。

```cpp
Theme::bg()        // #0b0f14  窗口底色
Theme::surface()   // #131a23  卡片 / 表格
Theme::text()      // #e6edf3  正文
Theme::accent()    // #38bdf8  主色（选中 / 焦点 / 进度）
Theme::ok() / warn() / bad()   // 语义色
Theme::levelColor(ratio)       // 按使用率取色（<60% 绿 / <80% 黄 / 其余红）
```

`main.cpp` 里一次性装载：

```cpp
app.setStyleSheet(Theme::globalStyleSheet());
```

## 写页面时怎么做

**不要**再写 `setStyleSheet("color: #888")`。给控件打 `role` 属性，
颜色由 QSS 的属性选择器统一驱动，换肤时自动跟随：

```cpp
label->setProperty("role", "mute");     // 弱化提示（单位、副标题）
label->setProperty("role", "dim");      // 次要文字
label->setProperty("role", "key");      // 键值对的键
label->setProperty("role", "stat");     // 大号数值
label->setProperty("role", "badge");    // 胶囊标签
label->setProperty("role", "hero");     // 强调块（左侧亮条）
label->setProperty("role", "ok|warn|bad");   // 状态文字

btn->setProperty("role", "primary");    // 主操作（保存、安装）
btn->setProperty("role", "danger");     // 危险操作（删除、关机）
btn->setProperty("role", "toggle");     // 折叠分组标题（整行、左对齐）
btn->setProperty("role", "tool");       // 表格内小按钮

edit->setProperty("role", "mono");      // 等宽（命令、路径）
frame->setProperty("role", "callout");  // 提示条（左侧主色竖线）
```

自绘控件（`paintEvent`）直接取 `Theme::` 的 `QColor`；
需要半透明用 `Theme::alpha(color, 0-255)`。

## QSS 的能力边界

QSS 不支持 CSS 的 `conic-gradient` / `backdrop-filter` / `box-shadow` / 自定义属性。
demo 里这几类效果在 Qt 中的对应做法：

| demo 效果 | Qt 做法 |
|---|---|
| 阴影 | `QGraphicsDropShadowEffect`，或自绘偏移半透明层 |
| 环形进度 | 自绘 `QPainter`（见 `MotherboardWidget`） |
| 毛玻璃 | 做不到，改用分层实色 + 描边 |
| 渐变 | `QLinearGradient`（仅自绘内） |

## 自检

改完配色后编译并检查生成的 QSS：

```sh
cmake -S . -B build -DENABLE_THEME_SHOT=ON && cmake --build build -t board_shot
```

`theme.cpp` 在 debug 构建下有 `Q_ASSERT`：模板残留未替换的 `@令牌@`
或花括号不配平会当场断言。

## 截图脚手架

`preview/board_shot.cpp` 只画主板（不跑外部命令，秒级出图）：

```sh
QT_QPA_PLATFORM=offscreen ./build/board_shot
```

> 截图工具：`preview/app_shot.cpp` 渲染整个主窗口，`argv[1]` 传标签序号可截指定页
> （`./build/app_shot 2` → `build/tab_2.png`）；`preview/board_shot.cpp` 只截主板画板。
>
> **踩过的坑（重要）**：页面在启动路径上同步弹模态框会让截图（和用户）永远等不到——
> `StatusPage::fetchAllTemps()` 原本在 `showEvent → refresh()` 里直接
> `QInputDialog::getText()` 索要 root 密码，等于"软件刚打开就先要密码"。
> 现已改为 `QTimer::singleShot(0)` 延迟到界面渲染之后再弹。
> 写页面时请遵守：**构造函数与 showEvent 里不得弹模态框、不得同步等待外部命令**。
> `lspci` / `smartctl` 等外部命令。想快速截图请只实例化目标页面或单个控件，
> 不要整窗渲染。


## 侧栏导航（对应 demo 01 的 .sidebar）

主窗口不再是顶部 `QTabWidget`，而是「左侧导航 + 顶栏标题 + 右侧堆栈」：

```
QWidget#sidebar (208px)  │  QWidget#topbar（页面标题 + 副标题）
  brandMark / brandName   │  QStackedWidget（当前页）
  navSep 监控|管理|系统    │
  QPushButton#navItem ×11 │
```

**为什么没用 `QTabWidget` 的侧栏**：`QTabBar` 竖排只有 `RoundedWest/East` 两种形状，
而 Qt 在这两种形状下会把标签文字旋转 90°，中文竖排不可读。因此改为每页一个
checkable `QPushButton`（文字天然水平），点击时手动清掉其它项的选中态实现互斥。
页面索引存在按钮的 `stackIndex()` 以及 `plainText` / `subText` 属性里，切换不依赖文本匹配。

侧栏样式在 [theme.cpp](../theme.cpp) 的 `QPushButton#navItem` 段，
选中态用 `border-left: 2px solid @accent@` 复刻 demo 的 `inset 2px 0 0 var(--accent)`
（QSS 无 `box-shadow`）；hover 的 `translateX(2px)` 无法实现，降级为变色。

## 踩过的坑二：横向内容被静默裁掉

`QScrollArea::setWidgetResizable(true)` 的横向滚动条策略默认是 `Qt::ScrollBarIgnored`，
含义是"永不显示横向滚动条"。配合 `widgetResizable`，Qt 会把内容压到可视宽度，
超出部分**既不滚动也不提示，直接被裁**。概览页右列（内核名、磁盘容量）就是这么没的。

修法（[statuspage.cpp](../statuspage.cpp) 等 5 个页面都已加）：

```cpp
scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
scroll->setMinimumWidth(1000);   // 不锁最小宽的话上面的策略形同虚设
```

写带 `QScrollArea` 的页面时这两行要一起写，只写第一行没有效果。


## 顶栏毛玻璃（GlassPanel）

demo 01 的 `.topbar` 用的是 `backdrop-filter: blur(10px)`。**Qt 的 QSS 完全没有
backdrop-filter / filter**，样式表里写这个属性会被直接忽略，所以只能自己画。

实现在 [glasspanel.cpp](../glasspanel.cpp)（自包含控件，不含业务）：

1. 从背景源控件抓取面板矩形内的像素（`render()` 带 `QRegion` 裁剪）
2. 降采样到 0.5 倍 → **盒模糊 3 次** → 放大回来
   盒模糊用滑动窗口实现，代价与半径无关；三次叠加按中心极限定理逼近高斯核
3. 叠深色底（`rgba(15,21,28,.47)`）→ 提亮层 → 环境辉光渐变 → 顶部高光 → 下边框

### 为什么必须让内容铺到玻璃底下

`backdrop` 指的是控件**背后**的像素。原来顶栏和页面是 `QVBoxLayout` 上下排的，
玻璃背后是空的，模糊出来等于纯色条。现在 `MainWindow` 故意不用布局管主区的两个孩子：

```cpp
m_pages->setGeometry(a);                       // 铺满，从 y=0 开始
m_topbar->setGeometry(0, 0, a.width(), kTopbarHeight);  // 浮层盖在顶上
m_topbar->raise();
```

代价是每个页面要自己让出顶部空间，否则正文第一行被标题压住。所以 11 个页面的
最外层 `setContentsMargins` 顶部都改成 `kTopbarInset = 52`。页面滚动时正文会滑到
玻璃底下，模糊跟着变——这才是真的毛玻璃，不是贴了张半透明图。

### 缓存失效

模糊很贵，必须缓存，但背景一变就得重算。`GlassPanel` 对背景源装了事件过滤器，
收到 `UpdateRequest / Scroll / Resize / Show` 就标脏。切页时 `MainWindow` 也显式
调 `revalidate()`。

### 静态首屏为什么不够明显

首屏时玻璃背后只有那 52px 页面边距，几乎采不到有色彩的内容，模糊出来自然接近
纯色条。补了一层"环境辉光"（主色 → 辅色的低透明渐变）来解决"背后没内容时也要
能看出这是玻璃"。真机上滚一下页面，彩色内容穿过玻璃的效果会明显得多。
