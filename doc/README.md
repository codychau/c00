# 界面风格 Demo

四个纯静态 HTML 风格样机，用来探索「家用服务器工具箱」换肤后的观感。
数据全部取自真实代码（README、`statuspage.cpp`、`vmpage.cpp`、`vmdialog.cpp`、`scenepage.cpp`），
不是 Lorem Ipsum：Ubuntu 26.04.1 LTS / 内核 7.0.0-42-generic、Radeon AI PRO R9700 + Raphael、
nvme0n1 / sda / sdb 三块盘、4 台虚机、「夜间影音」场景。

## 怎么看

不需要构建，直接用浏览器打开索引页：

```bash
xdg-open doc/demos/index.html      # Linux
# 或者起个静态服务器（推荐，这样下载 QSS 等功能也正常）
python3 -m http.server -d doc/demos 8000
# 然后访问 http://127.0.0.1:8000
```

## 文件

| 文件 | 主题 | 内容 |
| --- | --- | --- |
| `index.html` | — | 索引 + 落地路线建议 |
| `01-dark-console.html` | 深色机房控制台 | 状态仪表：指标卡、主板画板、磁盘/显卡折叠分组 |
| `02-light-cards.html` | 浅色柔和卡片 | 虚机卡片化 + 场景设定时间轴 |
| `03-glass-bento.html` | 玻璃拟态 Bento | 12 栅格一屏总览，含实时数值动效 |
| `04-tokens.html` | 三主题实时切换 | **设计令牌 + 组件库，可一键导出 Qt QSS** |
| `demo.css` | 共享样式 | 43 个变量、3 套主题（靠 `data-theme` 切换） |

四个 demo 结构完全一致，只是 design token 不同 —— 这正是 Qt 落地时想要的形状：
**样式集中在一处，结构不随之变动。**

## 建议的落地顺序

1. **先只改 `main.cpp`。** 现在的 `app.setStyleSheet(...)` 只设了按钮高度（min/max-height 25px）。
   在 `04-tokens.html` 选一个主题，点「复制 QSS」，整段替换进去，10 个页面立刻统一。
   风险最低、收益最大，且导出的 QSS 保留了 25px 高度约束，不会撑破既有布局。
2. **收拢散落的颜色。** `aboutpage.cpp`、`logpage.cpp`、`aipage.cpp`、`logpage.cpp` 等
   有几十处 `setStyleSheet("color: #888")`。改成
   `label->setProperty("role", "mute")`，交给全局 QSS 的 `QLabel[role="mute"]` 驱动，
   颜色从此只有一个来源。
3. **给按钮分层。** 主操作 `setObjectName("primary")`，危险操作 `("danger")`，
   用视觉层级表达「哪个按钮该按」。
4. **先吃免费的。** 进度条圆角、表格 hover/selected、滚动条细身、菜单圆角，
   QSS 都能一比一做到，无需改 C++。
5. **再自绘难的。** QSS 不支持 `conic-gradient`（环形仪表）、`backdrop-filter`（玻璃模糊）、
   `box-shadow`（改用 `QGraphicsDropShadowEffect`）、CSS 变量（导出时已硬编码成十六进制）。
   主板画板本来就是 QPainter 手绘的，把填充换成 `QLinearGradient` + `drawRoundedRect`，
   再给 hover 的槽位加一圈描边即可，逻辑不用动。
6. **页签换成侧边导航。** 顶部 `QTabWidget` 的 10 个页签已经开始挤了，demo 里都改成侧边栏；
   Qt 侧用 `QListWidget` + `QStackedWidget` 即可复刻，不必换框架。

## 说明

- 这些 demo 只是设计参考，**不参与 CMake 构建**，可以随时整目录删除。
- 颜色用了 `color-mix()`、`conic-gradient`、`backdrop-filter` 等现代 CSS 特性，
  需要较新的浏览器（Chrome 111+ / Firefox 113+ / Safari 16.4+）。
