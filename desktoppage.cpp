#include "desktoppage.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QFrame>
#include <QTableWidget>
#include <QHeaderView>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QFileDialog>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcessEnvironment>

static void setComboByData(QComboBox *combo, const QString &data);
static QString waybarConfigPath();
static QStringList parseModules(const QString &text);

static void setComboByData(QComboBox *combo, const QString &data)
{
    int idx = combo->findData(data);
    if (idx >= 0)
        combo->setCurrentIndex(idx);
}

DesktopPage::DesktopPage(QWidget *parent)
    : QWidget(parent)
{
    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    auto *scrollArea = new QScrollArea();
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto *inner = new QWidget();
    auto *layout = new QVBoxLayout(inner);
    layout->setContentsMargins(16, 8, 16, 8);
    layout->setSpacing(8);

    // ── 标题 ──
    auto *title = new QLabel("🎨 个性化桌面");
    QFont tf = title->font(); tf.setPointSize(14); tf.setBold(true);
    title->setFont(tf);
    layout->addWidget(title);

    // ── 兼容性提示 ──
    auto *notice = new QLabel(
        "⚠️ 本功能仅兼容 Wayland 系列桌面：sway、hyprland 与 niri，"
        "暂不支持 X11 及其他桌面环境（未来考虑支持 KDE）。");
    notice->setWordWrap(true);
    notice->setStyleSheet(
        "background-color: #fff7ed; border: 1px solid #f59e0b; "
        "border-radius: 6px; padding: 8px; color: #92400e; font-size: 13px;");
    layout->addWidget(notice);

    m_status = new QLabel("正在检测…");
    m_status->setStyleSheet("color: #666;");
    layout->addWidget(m_status);

    // ==================================================
    // 前 · 窗口样式
    // ==================================================
    addSectionHeader(layout, "🔷 前 · 窗口样式");

    auto *winG = new QGroupBox("动画与模糊");
    auto *winL = new QVBoxLayout(winG);

    auto *animRow = new QHBoxLayout();
    auto *animLabel = new QLabel("窗口动画：");
    animRow->addWidget(animLabel);
    m_animCombo = new QComboBox();
    m_animCombo->addItem("淡入淡出（fade）", "fade");
    m_animCombo->addItem("弹出（pop）", "pop");
    m_animCombo->addItem("滑动（slide）", "slide");
    m_animCombo->setMinimumHeight(30);
    animRow->addWidget(m_animCombo, 1);
    m_animApplyBtn = new QPushButton("应用");
    m_animApplyBtn->setFixedWidth(70);
    connect(m_animApplyBtn, &QPushButton::clicked, this, &DesktopPage::applyWindowAnimation);
    animRow->addWidget(m_animApplyBtn);
    winL->addLayout(animRow);

    auto *blurRow = new QHBoxLayout();
    auto *blurLabel = new QLabel("窗口模糊：");
    blurRow->addWidget(blurLabel);
    blurRow->addStretch();
    m_blurToggleBtn = new QPushButton("关闭");
    m_blurToggleBtn->setFixedWidth(70);
    connect(m_blurToggleBtn, &QPushButton::clicked, this, &DesktopPage::toggleBlur);
    blurRow->addWidget(m_blurToggleBtn);
    winL->addLayout(blurRow);

    m_windowStyleNote = new QLabel(
        "窗口样式由 compositor 管理，此处仅集成 Hyprland 的可调项。");
    m_windowStyleNote->setStyleSheet("color: #888; font-size: 12px;");
    m_windowStyleNote->setWordWrap(true);
    winL->addWidget(m_windowStyleNote);

    layout->addWidget(winG);

    // ==================================================
    // 中 · Waybar 任务栏 / 状态栏
    // ==================================================
    addSectionHeader(layout, "🔶 中 · 任务栏 / 状态栏（Waybar）");

    auto *wbNote = new QLabel(
        "sway、hyprland、niri 均支持以 Waybar 作为状态栏 / 任务栏。"
        "下方设置将写入 ~/.config/waybar/config 并自动重载。");
    wbNote->setWordWrap(true);
    wbNote->setStyleSheet("color: #666; font-size: 12px;");
    layout->addWidget(wbNote);

    auto *wbG = new QGroupBox("Waybar 布局");
    auto *wbL = new QVBoxLayout(wbG);

    auto *topRow = new QHBoxLayout();
    auto *posLabel = new QLabel("位置：");
    topRow->addWidget(posLabel);
    m_waybarPosition = new QComboBox();
    m_waybarPosition->addItem("顶部", "top");
    m_waybarPosition->addItem("底部", "bottom");
    m_waybarPosition->addItem("左侧", "left");
    m_waybarPosition->addItem("右侧", "right");
    m_waybarPosition->setMinimumHeight(30);
    topRow->addWidget(m_waybarPosition, 1);

    auto *layerLabel = new QLabel("图层：");
    topRow->addWidget(layerLabel);
    m_waybarLayer = new QComboBox();
    m_waybarLayer->addItem("窗口前方（top）", "top");
    m_waybarLayer->addItem("窗口后方（bottom）", "bottom");
    m_waybarLayer->setMinimumHeight(30);
    topRow->addWidget(m_waybarLayer);
    wbL->addLayout(topRow);

    auto *sizeRow = new QHBoxLayout();
    auto *heightLabel = new QLabel("高度：");
    sizeRow->addWidget(heightLabel);
    m_waybarHeight = new QSpinBox();
    m_waybarHeight->setRange(12, 200);
    m_waybarHeight->setValue(30);
    m_waybarHeight->setSuffix(" px");
    m_waybarHeight->setMinimumHeight(28);
    sizeRow->addWidget(m_waybarHeight);

    auto *spacingLabel = new QLabel("间距：");
    sizeRow->addWidget(spacingLabel);
    m_waybarSpacing = new QSpinBox();
    m_waybarSpacing->setRange(0, 20);
    m_waybarSpacing->setValue(4);
    m_waybarSpacing->setSuffix(" px");
    m_waybarSpacing->setMinimumHeight(28);
    sizeRow->addWidget(m_waybarSpacing);
    sizeRow->addStretch();
    wbL->addLayout(sizeRow);

    auto *leftRow = new QHBoxLayout();
    auto *leftLabel = new QLabel("左侧模块：");
    leftRow->addWidget(leftLabel);
    m_waybarLeft = new QLineEdit();
    m_waybarLeft->setPlaceholderText("例：workspaces, custom/menu");
    m_waybarLeft->setMinimumHeight(28);
    m_waybarLeft->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    leftRow->addWidget(m_waybarLeft, 1);
    wbL->addLayout(leftRow);

    auto *centerRow = new QHBoxLayout();
    auto *centerLabel = new QLabel("中间模块：");
    centerRow->addWidget(centerLabel);
    m_waybarCenter = new QLineEdit();
    m_waybarCenter->setPlaceholderText("例：clock, window");
    m_waybarCenter->setMinimumHeight(28);
    m_waybarCenter->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    centerRow->addWidget(m_waybarCenter, 1);
    wbL->addLayout(centerRow);

    auto *rightRow = new QHBoxLayout();
    auto *rightLabel = new QLabel("右侧模块：");
    rightRow->addWidget(rightLabel);
    m_waybarRight = new QLineEdit();
    m_waybarRight->setPlaceholderText("例：tray, battery, clock");
    m_waybarRight->setMinimumHeight(28);
    m_waybarRight->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    rightRow->addWidget(m_waybarRight, 1);
    wbL->addLayout(rightRow);

    auto *wbBtnRow = new QHBoxLayout();
    m_waybarApplyBtn = new QPushButton("应用并重载");
    connect(m_waybarApplyBtn, &QPushButton::clicked, this, &DesktopPage::applyWaybar);
    wbBtnRow->addWidget(m_waybarApplyBtn);
    m_waybarRefreshBtn = new QPushButton("刷新当前");
    connect(m_waybarRefreshBtn, &QPushButton::clicked, this, &DesktopPage::refreshWaybar);
    wbBtnRow->addWidget(m_waybarRefreshBtn);
    wbBtnRow->addStretch();
    wbL->addLayout(wbBtnRow);

    m_waybarStatus = new QLabel("");
    m_waybarStatus->setStyleSheet("color: #888; font-size: 12px;");
    m_waybarStatus->setWordWrap(true);
    wbL->addWidget(m_waybarStatus);

    layout->addWidget(wbG);

    // ==================================================
    // 后 · 背景（静态 / 动态）
    // ==================================================
    addSectionHeader(layout, "🔹 后 · 背景");

    auto *curG = new QGroupBox("当前壁纸");
    auto *curL = new QVBoxLayout(curG);
    m_currentLabel = new QLabel("当前壁纸：");
    curL->addWidget(m_currentLabel);
    m_currentWallpaper = new QLabel("未知");
    m_currentWallpaper->setStyleSheet("color: #333; word-wrap:;");
    m_currentWallpaper->setWordWrap(true);
    curL->addWidget(m_currentWallpaper);
    auto *curRow = new QHBoxLayout();
    m_refreshBtn = new QPushButton("刷新");
    m_refreshBtn->setFixedWidth(60);
    connect(m_refreshBtn, &QPushButton::clicked, this, &DesktopPage::refreshCurrent);
    curRow->addWidget(m_refreshBtn);
    curRow->addStretch();
    curL->addLayout(curRow);
    layout->addWidget(curG);

    auto *wpG = new QGroupBox("设置壁纸（静态背景）");
    auto *wpL = new QVBoxLayout(wpG);
    auto *pathRow = new QHBoxLayout();
    m_pathEdit = new QLineEdit();
    m_pathEdit->setPlaceholderText("输入壁纸图片路径…");
    m_pathEdit->setMinimumHeight(30);
    m_pathEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    pathRow->addWidget(m_pathEdit, 1);
    m_pickBtn = new QPushButton("浏览…");
    m_pickBtn->setFixedWidth(70);
    connect(m_pickBtn, &QPushButton::clicked, this, &DesktopPage::pickWallpaper);
    pathRow->addWidget(m_pickBtn);
    m_applyBtn = new QPushButton("应用");
    m_applyBtn->setFixedWidth(70);
    connect(m_applyBtn, &QPushButton::clicked, this, &DesktopPage::applyWallpaper);
    pathRow->addWidget(m_applyBtn);
    wpL->addLayout(pathRow);
    layout->addWidget(wpG);

    auto *dynG = new QGroupBox("动态背景（过渡动画）");
    auto *dynL = new QVBoxLayout(dynG);
    auto *transRow = new QHBoxLayout();
    auto *transLabel = new QLabel("切换动画：");
    transRow->addWidget(transLabel);
    m_transitionCombo = new QComboBox();
    m_transitionCombo->addItem("无（瞬时切换）", "none");
    m_transitionCombo->addItem("淡入淡出", "fade");
    m_transitionCombo->addItem("滑动", "slide");
    m_transitionCombo->addItem("圆环", "outer");
    m_transitionCombo->setMinimumHeight(30);
    m_transitionCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    transRow->addWidget(m_transitionCombo, 1);
    dynL->addLayout(transRow);
    layout->addWidget(dynG);

    // ==================================================
    // ⌨️ 快捷键（sway / niri）
    // ==================================================
    addSectionHeader(layout, "⌨️ 快捷键");

    m_bindStatus = new QLabel("检测中…");
    m_bindStatus->setStyleSheet("color: #666;");
    layout->addWidget(m_bindStatus);

    auto *hb = new QHBoxLayout();
    m_table = new QTableWidget(0, 3);
    m_table->setHorizontalHeaderLabels({"快捷键", "动作", "命令"});
    m_table->setSelectionBehavior(QTableWidget::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setShowGrid(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this]() {
        int row = m_table->currentRow();
        if (row < 0 || row >= m_bindings.size())
            return;
        m_addMode = false;
        m_btnApply->setText("应用并重载");
        loadEditor(m_bindings[row]);
    });
    hb->addWidget(m_table, 3);

    auto *evb = new QVBoxLayout();
    auto *eg = new QGroupBox("编辑");
    auto *el = new QVBoxLayout(eg);

    auto *mrow = new QHBoxLayout();
    mrow->addWidget(new QLabel("组合键："));
    auto mkchk = [&](const QString &label, QCheckBox *&out) {
        out = new QCheckBox(label);
        mrow->addWidget(out);
    };
    mkchk("Super", m_chkSuper);
    mkchk("Alt", m_chkAlt);
    mkchk("Shift", m_chkShift);
    mkchk("Control", m_chkControl);
    mrow->addStretch();
    el->addLayout(mrow);

    auto *krow = new QHBoxLayout();
    krow->addWidget(new QLabel("按键："));
    m_keyCombo = new QComboBox();
    m_keyCombo->setEditable(true);
    QStringList commonKeys = {"Return", "Tab", "Escape", "Space", "Up", "Down", "Left", "Right"};
    for (char c = 'a'; c <= 'z'; ++c)
        commonKeys << QString(QChar(c));
    for (int i = 1; i <= 12; ++i)
        commonKeys << ("F" + QString::number(i));
    m_keyCombo->addItems(commonKeys);
    m_keyCombo->setCurrentIndex(0);
    m_keyCombo->setMinimumHeight(30);
    krow->addWidget(m_keyCombo, 1);
    krow->addWidget(new QLabel("动作："));
    m_typeCombo = new QComboBox();
    m_typeCombo->addItem("启动", (int)ActionType::Launch);
    m_typeCombo->addItem("执行命令", (int)ActionType::LaunchShell);
    m_typeCombo->addItem("切换工作区", (int)ActionType::FocusWorkspace);
    m_typeCombo->addItem("退出", (int)ActionType::Quit);
    m_typeCombo->addItem("截屏", (int)ActionType::Screenshot);
    m_typeCombo->addItem("截屏(全屏)", (int)ActionType::ScreenshotScreen);
    m_typeCombo->addItem("截屏(窗口)", (int)ActionType::ScreenshotWindow);
    m_typeCombo->addItem("自定义", (int)ActionType::Other);
    m_typeCombo->setMinimumHeight(30);
    krow->addWidget(m_typeCombo);
    el->addLayout(krow);

    auto *crow = new QHBoxLayout();
    crow->addWidget(new QLabel("命令："));
    m_cmdEdit = new QLineEdit();
    m_cmdEdit->setMinimumHeight(30);
    crow->addWidget(m_cmdEdit, 1);
    el->addLayout(crow);

    auto *wrow = new QHBoxLayout();
    wrow->addWidget(new QLabel("工作区："));
    m_wsSpin = new QSpinBox();
    m_wsSpin->setRange(1, 99);
    m_wsSpin->setValue(1);
    m_wsSpin->setMinimumHeight(30);
    wrow->addWidget(m_wsSpin);
    wrow->addStretch();
    el->addLayout(wrow);

    el->addStretch();
    evb->addWidget(eg);

    auto *brow = new QHBoxLayout();
    m_btnAdd = new QPushButton("新增");
    connect(m_btnAdd, &QPushButton::clicked, this, &DesktopPage::onAdd);
    brow->addWidget(m_btnAdd);
    m_btnDelete = new QPushButton("删除");
    connect(m_btnDelete, &QPushButton::clicked, this, &DesktopPage::onDelete);
    brow->addWidget(m_btnDelete);
    m_btnRefresh = new QPushButton("刷新");
    connect(m_btnRefresh, &QPushButton::clicked, this, &DesktopPage::refreshBindings);
    brow->addWidget(m_btnRefresh);
    m_btnApply = new QPushButton("应用并重载");
    connect(m_btnApply, &QPushButton::clicked, this, &DesktopPage::onApply);
    brow->addWidget(m_btnApply);
    brow->addStretch();
    evb->addLayout(brow);

    hb->addLayout(evb, 2);
    layout->addLayout(hb);

    layout->addStretch();

    scrollArea->setWidget(inner);
    outerLayout->addWidget(scrollArea);

    detectCompositor();
    refreshBindings();
}

void DesktopPage::addSectionHeader(QVBoxLayout *target, const QString &text)
{
    auto *h = new QLabel(text);
    QFont hf = h->font(); hf.setPointSize(12); hf.setBold(true);
    h->setFont(hf);
    h->setStyleSheet("color: #333; margin-top: 4px;");
    target->addWidget(h);
    auto *line = new QFrame();
    line->setFrameShape(QFrame::HLine);
    line->setStyleSheet("background-color: #e5e7eb;");
    target->addWidget(line);
}

void DesktopPage::detectCompositor()
{
    QString desktop = QProcessEnvironment::systemEnvironment().value("XDG_CURRENT_DESKTOP");
    QString name = detectCompositorName();

    if (name.isEmpty() || name == "other") {
        m_status->setText("⚠️ 未识别到受支持的 Wayland 桌面（sway/hyprland/niri），部分功能不可用。");
        m_windowStyleNote->setVisible(false);
    } else if (name == "hyprland") {
        m_status->setText("就绪（Hyprland）");
        m_windowStyleNote->setVisible(false);
    } else {
        m_status->setText("就绪（" + name + "）");
        m_windowStyleNote->setVisible(true);
    }

    applyWaybarDefaults(name);
    refreshWaybar();
    checkTool();
}

QString DesktopPage::detectCompositorName()
{
    switch (KeybindingConfig::detect()) {
    case Compositor::Hyprland:
        return "hyprland";
    case Compositor::Sway:
        return "sway";
    case Compositor::Niri:
        return "niri";
    default:
        return "other";
    }
}

void DesktopPage::applyWaybarDefaults(const QString &name)
{
    if (name == "sway") {
        m_waybarLeft->setText("sway/workspaces, sway/mode");
        m_waybarCenter->setText("sway/window");
        m_waybarRight->setText("battery, clock, tray");
    } else if (name == "niri") {
        m_waybarLeft->setText("niri/workspaces");
        m_waybarCenter->setText("niri/window");
        m_waybarRight->setText("clock, tray");
    } else {
        m_waybarLeft->setText("");
        m_waybarCenter->setText("");
        m_waybarRight->setText("clock, tray");
    }
}

// ── 后景：壁纸 ────────────────────────────────────────────

void DesktopPage::checkTool()
{
    runCmd("which", {"swww"}, [this](const QString &out) {
        if (out.trimmed().isEmpty()) {
            m_applyBtn->setEnabled(false);
            if (!m_status->text().contains("swww"))
                m_status->setText(m_status->text() + " | ⚠️ 未找到 swww，壁纸功能不可用。");
        } else {
            m_applyBtn->setEnabled(true);
        }
    });
}

void DesktopPage::pickWallpaper()
{
    QString dir = QStandardPaths::locate(QStandardPaths::PicturesLocation, ".",
                                         QStandardPaths::LocateDirectory);
    QStringList filters;
    filters << "图片" << "*.png" << "*.jpg" << "*.jpeg" << "*.gif" << "*.bmp"
            << "*.webp" << "*.svg";

    QString file = QFileDialog::getOpenFileName(this, "选择壁纸", dir, filters.join(";;"));
    if (!file.isEmpty())
        m_pathEdit->setText(file);
}

void DesktopPage::applyWallpaper()
{
    QString path = m_pathEdit->text().trimmed();
    if (path.isEmpty()) {
        m_status->setText("⚠️ 请先输入或选择壁纸路径");
        return;
    }

    QFileInfo fi(path);
    if (!fi.exists()) {
        m_status->setText("❌ 文件不存在：" + path);
        return;
    }

    QString transition = m_transitionCombo->currentData().toString();
    QStringList args = {"img", path};
    if (transition != "none")
        args << "--transition-type" << transition;

    m_status->setText("⏳ 正在应用壁纸…");
    runCmd("swww", args, [this, path](const QString &out) {
        if (out.contains("error", Qt::CaseInsensitive) || out.isEmpty())
            m_status->setText("❌ 应用失败，请确认 swww daemon 正在运行：" + path);
        else
            m_status->setText("✅ 已切换到新壁纸");
        refreshCurrent();
    });
}

void DesktopPage::refreshCurrent()
{
    runCmd("swww", {"query"}, [this](const QString &out) {
        QString path;
        for (const auto &l : out.split('\n')) {
            QString t = l.trimmed();
            if (!t.isEmpty() && !t.startsWith("Monitor")) {
                auto parts = t.split(' ');
                if (parts.size() >= 4)
                    path = parts.at(3);
            }
        }
        if (path.isEmpty()) {
            m_currentWallpaper->setText("未设置");
        } else {
            m_currentWallpaper->setText(path);
        }
    });
}

// ── 中：Waybar ────────────────────────────────────────────

static QString waybarConfigPath()
{
    QString cfgDir = QDir::homePath() + "/.config/waybar";
    return cfgDir + "/config";
}

void DesktopPage::applyWaybar()
{
    QString layer = m_waybarLayer->currentData().toString();
    QString position = m_waybarPosition->currentData().toString();
    QJsonObject root;
    root["layer"] = layer;
    root["position"] = position;
    root["height"] = m_waybarHeight->value();
    root["spacing"] = m_waybarSpacing->value();
    root["modules-left"] = QJsonArray::fromStringList(parseModules(m_waybarLeft->text()));
    root["modules-center"] = QJsonArray::fromStringList(parseModules(m_waybarCenter->text()));
    root["modules-right"] = QJsonArray::fromStringList(parseModules(m_waybarRight->text()));

    QJsonDocument doc(root);

    QFile file(waybarConfigPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_waybarStatus->setText("❌ 无法写入配置文件：" + waybarConfigPath());
        return;
    }
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();

    m_waybarStatus->setText("✅ 已写入配置，正在重载 Waybar…");
    runCmd("sh", {"-c", "pkill -USR2 waybar 2>/dev/null || true"},
           [this](const QString &) {
               m_waybarStatus->setText("✅ 已保存并重载 Waybar。");
               refreshWaybar();
           });
}

static QStringList parseModules(const QString &text)
{
    QStringList cleaned;
    for (const auto &i : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        QString t = i.trimmed();
        if (!t.isEmpty())
            cleaned << t;
    }
    return cleaned;
}

void DesktopPage::refreshWaybar()
{
    QFile file(waybarConfigPath());
    if (!file.open(QIODevice::ReadOnly)) {
        m_waybarStatus->setText("⚠️ 未找到 waybar 配置文件（~/.config/waybar/config）。"
                                "点击“应用并重载”创建。");
        return;
    }
    QByteArray data = file.readAll();
    file.close();

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        m_waybarStatus->setText("⚠️ 配置文件解析失败或格式不正确。");
        return;
    }

    QJsonObject root = doc.object();
    setComboByData(m_waybarPosition, root.value("position").toString());
    setComboByData(m_waybarLayer, root.value("layer").toString());
    if (root.contains("height")) m_waybarHeight->setValue(root.value("height").toInt());
    if (root.contains("spacing")) m_waybarSpacing->setValue(root.value("spacing").toInt());

    auto arrToStr = [&](const QString &key, QLineEdit *edit) {
        if (root.contains(key)) {
            QJsonArray a = root.value(key).toArray();
            QStringList names;
            for (const auto &v : a) names << v.toString();
            edit->setText(names.join(", "));
        }
    };
    arrToStr("modules-left", m_waybarLeft);
    arrToStr("modules-center", m_waybarCenter);
    arrToStr("modules-right", m_waybarRight);

    m_waybarStatus->setText("已读取当前 Waybar 配置。");
}

// ── 前：窗口样式（Hyprland） ────────────────────────────────

void DesktopPage::applyWindowAnimation()
{
    QString name = m_animCombo->currentData().toString();
    m_status->setText("⏳ 正在应用窗口动画 " + name + "…");
    runCmd("hyprctl", {"keyword", "animation:" + name},
           [this, name](const QString &out) {
               if (out.contains("error", Qt::CaseInsensitive) || out.isEmpty())
                   m_status->setText("❌ 应用失败，请确认当前在 Hyprland 下运行。");
               else
                   m_status->setText("✅ 已切换到窗口动画：" + name);
           });
}

void DesktopPage::toggleBlur()
{
    bool currentlyOn = m_blurToggleBtn->text() == "开启";
    int value = currentlyOn ? 0 : 1;
    QString verb = currentlyOn ? QString::fromUtf8("关闭") : QString::fromUtf8("开启");
    m_blurToggleBtn->setEnabled(false);
    m_status->setText(QString("⏳ 正在") + verb + QString("窗口模糊…"));
    runCmd("hyprctl", {"keyword", "decoration:blur:enabled", QString::number(value)},
           [this, currentlyOn, verb](const QString &out) {
               m_blurToggleBtn->setEnabled(true);
               if (out.contains("error", Qt::CaseInsensitive) || out.isEmpty()) {
                   m_status->setText("❌ 切换失败，请确认当前在 Hyprland 下运行。");
               } else {
                   m_blurToggleBtn->setText(verb);
                   m_status->setText(QString("✅ 已") + verb + QString("窗口模糊。"));
               }
           });
}

void DesktopPage::runCmd(const QString &cmd, const QStringList &args,
                         std::function<void(const QString &)> cb)
{
    auto *p = new QProcess(this);
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [p, cb](int, QProcess::ExitStatus) {
                cb(QString::fromUtf8(p->readAllStandardOutput()).trimmed());
                p->deleteLater();
            });
    p->setProcessChannelMode(QProcess::MergedChannels);
    p->start(cmd, args);
}

// ------------------------------------------------------------------ 快捷键
static QString cmdText(const Keybinding &b)
{
    switch (b.type) {
    case ActionType::Launch:
        return b.spawnArgs.join(" ");
    case ActionType::LaunchShell:
        return "exec " + b.shellCommand;
    case ActionType::FocusWorkspace:
        return (b.workspace > 0) ? "workspace " + QString::number(b.workspace)
                                 : b.customBody;
    case ActionType::Quit:
    case ActionType::Other:
    case ActionType::Screenshot:
    case ActionType::ScreenshotScreen:
    case ActionType::ScreenshotWindow:
        return b.customBody;
    }
    return b.customBody;
}

static QString typeLabel(ActionType t)
{
    switch (t) {
    case ActionType::Launch:
        return "启动";
    case ActionType::LaunchShell:
        return "执行命令";
    case ActionType::FocusWorkspace:
        return "切换工作区";
    case ActionType::Quit:
        return "退出";
    case ActionType::Screenshot:
        return "截屏";
    case ActionType::ScreenshotScreen:
        return "截屏(全屏)";
    case ActionType::ScreenshotWindow:
        return "截屏(窗口)";
    case ActionType::Other:
        return "自定义";
    }
    return "";
}

void DesktopPage::refreshBindings()
{
    m_comp = KeybindingConfig::detect();
    if (m_comp != Compositor::Sway && m_comp != Compositor::Niri) {
        m_bindStatus->setText("⚠️ 当前桌面不受支持（仅 sway / niri），快捷键功能不可用。");
        m_table->setRowCount(0);
        setEditorEnabled(false);
        return;
    }

    QString path = KeybindingConfig::configPath(m_comp);
    m_niriFmt = (path.endsWith(".yml", Qt::CaseInsensitive)) ? NiriFormat::Yaml
                                                             : NiriFormat::Kdl;
    QFileInfo fi(path);
    if (!fi.exists()) {
        m_bindings.clear();
        m_bindStatus->setText("未找到配置文件：" + path + "（新增后点“应用并重载”创建）。");
        m_table->setRowCount(0);
        setEditorEnabled(true);
        return;
    }

    QFile f(path);
    QString text;
    if (f.open(QIODevice::ReadOnly)) {
        text = QString::fromUtf8(f.readAll());
        f.close();
    }
    if (m_comp == Compositor::Niri) {
        NiriFormat fmt = KeybindingConfig::niriFormat(text);
        if (fmt != NiriFormat::Unknown)
            m_niriFmt = fmt;
    }
    m_bindings = KeybindingConfig::parse(m_comp, text);
    renderTable();
    m_bindStatus->setText("已加载 " + QString::number(m_bindings.size()) + " 条快捷键 · " + path);
    setEditorEnabled(true);
}

void DesktopPage::renderTable()
{
    m_table->setRowCount(m_bindings.size());
    for (int i = 0; i < m_bindings.size(); ++i) {
        const Keybinding &b = m_bindings[i];
        m_table->setItem(i, 0, new QTableWidgetItem(keybindingDescribe(b, m_comp)));
        m_table->setItem(i, 1, new QTableWidgetItem(typeLabel(b.type)));
        m_table->setItem(i, 2, new QTableWidgetItem(cmdText(b)));
    }
}

void DesktopPage::setEditorEnabled(bool en)
{
    m_keyCombo->setEnabled(en);
    m_typeCombo->setEnabled(en);
    m_cmdEdit->setEnabled(en);
    m_wsSpin->setEnabled(en);
    m_chkSuper->setEnabled(en);
    m_chkAlt->setEnabled(en);
    m_chkShift->setEnabled(en);
    m_chkControl->setEnabled(en);
    m_btnApply->setEnabled(en);
}

void DesktopPage::clearEditor()
{
    m_chkSuper->setChecked(false);
    m_chkAlt->setChecked(false);
    m_chkShift->setChecked(false);
    m_chkControl->setChecked(false);
    m_keyCombo->setCurrentIndex(0);
    m_typeCombo->setCurrentIndex(0);
    m_cmdEdit->clear();
    m_wsSpin->setValue(1);
}

void DesktopPage::loadEditor(const Keybinding &b)
{
    m_chkSuper->setChecked(b.modifiers.contains("Super", Qt::CaseInsensitive));
    m_chkAlt->setChecked(b.modifiers.contains("Alt", Qt::CaseInsensitive));
    m_chkShift->setChecked(b.modifiers.contains("Shift", Qt::CaseInsensitive));
    m_chkControl->setChecked(b.modifiers.contains("Control", Qt::CaseInsensitive));
    if (!b.key.isEmpty())
        m_keyCombo->setCurrentText(b.key);
    int ti = m_typeCombo->findData((int)b.type);
    if (ti >= 0)
        m_typeCombo->setCurrentIndex(ti);
    m_wsSpin->setValue(b.workspace > 0 ? b.workspace : 1);
    m_cmdEdit->setText(cmdText(b));
}

void DesktopPage::saveEditor(Keybinding &b)
{
    b.modifiers.clear();
    QStringList order = {"Super", "Alt", "Shift", "Control"};
    for (const QString &m : order) {
        QCheckBox *chk = (m == "Super") ? m_chkSuper
                        : (m == "Alt") ? m_chkAlt
                        : (m == "Shift") ? m_chkShift : m_chkControl;
        if (chk->isChecked())
            b.modifiers.append(m);
    }
    b.key = m_keyCombo->currentText().trimmed().toLower();
    b.type = (ActionType)m_typeCombo->currentData().toInt();
    b.workspace = m_wsSpin->value();
    b._hasKeycode = false;
    b._extra.clear();

    QString cmd = m_cmdEdit->text().trimmed();
    b.spawnArgs.clear();
    b.shellCommand.clear();
    switch (b.type) {
    case ActionType::Launch:
        b.spawnArgs = cmd.split(' ', Qt::SkipEmptyParts);
        b.customBody = cmd;
        break;
    case ActionType::LaunchShell:
        b.shellCommand = cmd.startsWith("exec", Qt::CaseInsensitive) ? cmd.mid(4).trimmed() : cmd;
        b.customBody = cmd;
        break;
    case ActionType::FocusWorkspace:
        b.customBody = "workspace " + QString::number(b.workspace);
        break;
    case ActionType::Quit:
    case ActionType::Other:
    case ActionType::Screenshot:
    case ActionType::ScreenshotScreen:
    case ActionType::ScreenshotWindow:
        b.customBody = cmd;
        break;
    }
}

void DesktopPage::onAdd()
{
    m_addMode = true;
    m_btnApply->setText("保存");
    clearEditor();
    m_cmdEdit->setFocus();
}

void DesktopPage::onDelete()
{
    int row = m_table->currentRow();
    if (row < 0) {
        m_bindStatus->setText("⚠️ 请先在列表中选择一条。");
        return;
    }
    m_bindings.removeAt(row);
    renderTable();
    m_addMode = false;
    m_btnApply->setText("应用并重载");
    m_bindStatus->setText("已删除 1 条（未保存，点“应用并重载”生效）。");
}

void DesktopPage::onApply()
{
    if (m_comp != Compositor::Sway && m_comp != Compositor::Niri) {
        m_bindStatus->setText("⚠️ 当前桌面不受支持，无法保存。");
        return;
    }

    Keybinding cur;
    saveEditor(cur);
    if (m_addMode)
        m_bindings.append(cur);
    else {
        int row = m_table->currentRow();
        if (row >= 0)
            m_bindings[row] = cur;
        else
            m_bindings.append(cur);
    }
    m_addMode = false;
    m_btnApply->setText("应用并重载");

    QString path = KeybindingConfig::configPath(m_comp);
    QFileInfo fi(path);
    if (fi.exists()) {
        QFile f(path);
        QString bp = KeybindingConfig::backupPath(path);
        if (f.copy(bp))
            f.close();
    }

    QString orig;
    if (fi.exists()) {
        QFile rf(path);
        if (rf.open(QIODevice::ReadOnly)) {
            orig = QString::fromUtf8(rf.readAll());
            rf.close();
        }
    }

    NiriFormat fmt = m_niriFmt;
    if (m_comp == Compositor::Niri) {
        NiriFormat detected = KeybindingConfig::niriFormat(orig);
        if (detected != NiriFormat::Unknown)
            fmt = detected;
    }
    QString rendered = KeybindingConfig::render(m_comp, m_bindings, fmt);
    QStringList origLines = orig.split('\n');
    QStringList block = rendered.split('\n');
    QPair<int, int> reg = KeybindingConfig::editableRegion(m_comp, orig);

    QString result;
    if (reg.first < 0) {
        // 找不到可编辑区域：空文件/新文件才生成，否则追加，绝不覆盖已有内容
        QString wrapper;
        if (m_comp == Compositor::Niri) {
            wrapper = (fmt == NiriFormat::Yaml)
                          ? "bindings:\n" + rendered
                          : "binds {\n" + rendered + "\n}";
        } else {
            wrapper = rendered;
        }
        if (orig.trimmed().isEmpty())
            result = wrapper;
        else
            result = orig + "\n" + wrapper + "\n";
    } else {
        QStringList head = origLines.mid(0, reg.first);
        QStringList tail = origLines.mid(reg.second);
        QString before = head.isEmpty() ? QString() : head.join('\n');
        QString after = tail.isEmpty() ? QString() : ("\n" + tail.join('\n'));
        result = before.isEmpty() ? (block.join('\n') + after)
                                  : (before + "\n" + block.join('\n') + after);
    }

    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_bindStatus->setText("❌ 无法写入配置文件：" + path);
        return;
    }
    out.write(result.toUtf8());
    out.close();

    reloadAfterSave(path);
}

void DesktopPage::reloadAfterSave(const QString &path)
{
    QStringList rc = KeybindingConfig::reloadCommand(m_comp);
    if (rc.isEmpty()) {
        refreshBindings();
        m_bindStatus->setText("✅ 已保存（请手动重载使配置生效）。");
        return;
    }
    m_bindStatus->setText("⏳ 正在重载…");
    runCmd(rc.first(), rc.mid(1), [this](const QString &) {
        refreshBindings();
        m_bindStatus->setText("✅ 已保存并重载。");
    });
}
