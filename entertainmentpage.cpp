#include "entertainmentpage.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QHash>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QFrame>

#include <memory>

EntertainmentPage::EntertainmentPage(QWidget *parent)
    : QWidget(parent)
{
    // 整个页面用 QScrollArea 包裹
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
    auto *title = new QLabel("🎮 娱乐选项");
    QFont f = title->font(); f.setPointSize(14); f.setBold(true);
    title->setFont(f);
    layout->addWidget(title);

    // ── 状态栏 ──
    m_status = new QLabel("正在检测…");
    m_status->setStyleSheet("color: #666;");
    layout->addWidget(m_status);

    // ==================================================
    // 分辨率调节
    // ==================================================
    auto *resG = new QGroupBox("显示分辨率");
    auto *resL = new QVBoxLayout(resG);

    m_currentResLabel = new QLabel("当前分辨率：检测中…");
    resL->addWidget(m_currentResLabel);

    auto *resRow = new QHBoxLayout();
    m_resolutionCombo = new QComboBox();
    m_resolutionCombo->setMinimumHeight(30);
    m_resolutionCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    resRow->addWidget(m_resolutionCombo, 1);

    m_resApplyBtn = new QPushButton("应用");
    m_resApplyBtn->setFixedWidth(80);
    connect(m_resApplyBtn, &QPushButton::clicked, this, &EntertainmentPage::onResolutionChanged);
    resRow->addWidget(m_resApplyBtn);

    auto *resRefreshBtn = new QPushButton("刷新");
    resRefreshBtn->setFixedWidth(60);
    connect(resRefreshBtn, &QPushButton::clicked, this, &EntertainmentPage::refreshResolution);
    resRow->addWidget(resRefreshBtn);

    resL->addLayout(resRow);

    m_resStatus = new QLabel("");
    m_resStatus->setStyleSheet("color: #888; font-size: 12px;");
    resL->addWidget(m_resStatus);

    layout->addWidget(resG);

    // ==================================================
    // 音频输出
    // ==================================================
    auto *sinkG = new QGroupBox("音频输出设备");
    auto *sinkL = new QVBoxLayout(sinkG);

    m_sinkLabel = new QLabel("当前输出：");
    sinkL->addWidget(m_sinkLabel);

    auto *comboR = new QHBoxLayout();
    m_sinkCombo = new QComboBox();
    m_sinkCombo->setMinimumHeight(30);
    comboR->addWidget(m_sinkCombo, 1);

    m_refreshBtn = new QPushButton("刷新");
    m_refreshBtn->setFixedWidth(60);
    connect(m_refreshBtn, &QPushButton::clicked, this, &EntertainmentPage::refreshStatus);
    comboR->addWidget(m_refreshBtn);
    sinkL->addLayout(comboR);

    layout->addWidget(sinkG);

    connect(m_sinkCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &EntertainmentPage::onSwitchSink);

    // ==================================================
    // 音量
    // ==================================================
    auto *volG = new QGroupBox("音量控制");
    auto *volL = new QVBoxLayout(volG);

    m_volumeLabel = new QLabel("音量：50%");
    volL->addWidget(m_volumeLabel);

    m_volumeSlider = new QSlider(Qt::Horizontal);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setMinimumHeight(26);
    connect(m_volumeSlider, &QSlider::sliderReleased,
            this, &EntertainmentPage::onVolumeSliderReleased);
    volL->addWidget(m_volumeSlider);

    auto *btnR = new QHBoxLayout();
    btnR->addStretch();
    m_volDown = new QPushButton("−  5%");
    m_volDown->setFixedWidth(80);
    connect(m_volDown, &QPushButton::clicked, this, &EntertainmentPage::onVolumeDown);
    btnR->addWidget(m_volDown);
    m_volUp = new QPushButton("+  5%");
    m_volUp->setFixedWidth(80);
    connect(m_volUp, &QPushButton::clicked, this, &EntertainmentPage::onVolumeUp);
    btnR->addWidget(m_volUp);
    btnR->addStretch();
    volL->addLayout(btnR);

    layout->addWidget(volG);

    // ==================================================
    // 手柄检测 [实验性]
    // ==================================================
    auto *ctrlG = new QGroupBox();
    auto *ctrlTitleRow = new QHBoxLayout();

    auto *ctrlTitle = new QLabel("🎮 手柄检测与设置");
    QFont cf = ctrlTitle->font(); cf.setBold(true);
    ctrlTitle->setFont(cf);
    ctrlTitleRow->addWidget(ctrlTitle);

    auto *ctrlBadge = new QLabel("  [实验性]  ");
    ctrlBadge->setStyleSheet(
        "background-color: #f59e0b; color: white; border-radius: 4px;"
        "padding: 2px 8px; font-weight: bold; font-size: 11px;");
    ctrlTitleRow->addWidget(ctrlBadge);
    ctrlTitleRow->addStretch();

    // QGroupBox 没有直接的 setTitleLayout, 用 QVBoxLayout 包装
    auto *ctrlOuter = new QVBoxLayout(ctrlG);
    ctrlOuter->addLayout(ctrlTitleRow);

    auto *ctrlDetectRow = new QHBoxLayout();
    m_ctrlDetectBtn = new QPushButton("🔍 检测手柄");
    connect(m_ctrlDetectBtn, &QPushButton::clicked, this, &EntertainmentPage::detectControllers);
    ctrlDetectRow->addWidget(m_ctrlDetectBtn);
    ctrlDetectRow->addStretch();
    ctrlOuter->addLayout(ctrlDetectRow);

    m_ctrlStatusLabel = new QLabel("未检测");
    m_ctrlStatusLabel->setStyleSheet("color: #888;");
    ctrlOuter->addWidget(m_ctrlStatusLabel);

    m_ctrlListLabel = new QLabel("");
    m_ctrlListLabel->setStyleSheet("color: #555; font-family: monospace;");
    ctrlOuter->addWidget(m_ctrlListLabel);

    layout->addWidget(ctrlG);

    // ==================================================
    // 游戏中心启动器 [自动检测 PATH / Flatpak / Snap]
    // ==================================================
    auto *gameG = new QGroupBox("游戏中心启动器");
    auto *gameL = new QHBoxLayout(gameG);
    gameL->setContentsMargins(16, 10, 16, 10);
    gameL->addStretch();

    // 候选程序：原生包名 + Flatpak/Snap 应用 ID
    QStringList candidates = {
        "steam",
        "goggalaxy", "gog-galaxy", "goggalaxy2",
        "com.valvedigital.Steam",
        "com.bistom.GogGalaxy", "com.bistom.GogGalaxy2"
    };
    QStringList sh;
    sh << "#!/bin/sh";
    for (const auto &cand : candidates) {
        sh << ("for p in $(command -v " + cand + " 2>/dev/null); do echo \"path|$p\"; done");
        if (cand.startsWith("com.")) {
            sh << ("flatpak list --columns application 2>/dev/null | grep -qx \"" + cand + "\" && echo \"flatpak|" + cand + "\"");
            sh << ("snap list --columns name 2>/dev/null | grep -qx \"" + cand + "\" && echo \"snap|" + cand + "\"");
        }
    }
    QString script = sh.join("\n");

    {
        auto *p = new QProcess(this);
        auto *noneLbl = new QLabel("正在检测游戏启动器…");
        noneLbl->setStyleSheet("color: #888;");
        gameL->addWidget(noneLbl, 1);

        connect(p, &QProcess::finished, this, [p, gameL, noneLbl](int, QProcess::ExitStatus) {
            noneLbl->deleteLater();
            QString out = QString::fromUtf8(p->readAllStandardOutput()).trimmed();
            p->deleteLater();

            QHash<QString, QStringList> seen;
            auto addBtn = [&seen, gameL](const QString &label, const QStringList &launch) {
                if (label.isEmpty() || launch.isEmpty() || seen.contains(label)) return;
                seen.insert(label, launch);
                auto *b = new QPushButton(label);
                b->setMinimumWidth(150);
                b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
                b->setStyleSheet(
                    "QPushButton { background-color: #2d2d2d; color: #eee; border: 1px solid #444;"
                    "border-radius: 8px; padding: 2px 10px; font-size: 13px; font-weight: bold; }"
                    "QPushButton:hover { background-color: #3d3d3d; border-color: #5a9; }"
                    "QPushButton:pressed { background-color: #2b5797; }");
                connect(b, &QPushButton::clicked, [launch]() {
                    if (launch.isEmpty()) return;
                    QProcess::startDetached(launch.at(0), launch.mid(1));
                });
                gameL->addWidget(b);
            };

            for (const auto &raw : out.split('\n')) {
                auto t = raw.trimmed();
                if (t.isEmpty() || !t.contains('|')) continue;
                int pipe = t.indexOf('|');
                QString src = t.left(pipe);
                QString val = t.mid(pipe + 1);
                if (val.isEmpty()) continue;
                if (src == "path") {
                    if (val.contains("gog", Qt::CaseInsensitive))
                        addBtn("🎲 GOG Galaxy", { val });
                    else if (val.contains("steam", Qt::CaseInsensitive))
                        addBtn("🎮 Steam", { val });
                } else if (src == "flatpak" || src == "snap") {
                    QString launcher = (src == "flatpak" ? "flatpak" : "snap");
                    if (val.contains("steam", Qt::CaseInsensitive))
                        addBtn("🎮 Steam", { launcher, "run", val });
                    else if (val.contains("gog", Qt::CaseInsensitive))
                        addBtn("🎲 GOG Galaxy", { launcher, "run", val });
                }
            }

            if (seen.isEmpty()) {
                auto *lbl = new QLabel("未检测到 Steam / GOG Galaxy");
                lbl->setStyleSheet("color: #999;");
                gameL->addWidget(lbl, 1);
            }
            gameL->addStretch();
        });

        p->setProcessChannelMode(QProcess::MergedChannels);
        p->start("sh", { "-c", script });
    }

    layout->addWidget(gameG);

    layout->addStretch();

    // 设置 scroll area
    scrollArea->setWidget(inner);
    outerLayout->addWidget(scrollArea);

    // 初始刷新
    refreshStatus();
    refreshResolution();
}

void EntertainmentPage::runCmd(const QString &cmd, const QStringList &args,
                               std::function<void(const QString &)> cb)
{
    auto *p = new QProcess(this);
    auto done = std::make_shared<bool>(false);

    auto finish = [p, cb, done](const QString &out) {
        if (*done) return;
        *done = true;
        cb(out);
        p->deleteLater();
    };

    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [p, finish](int, QProcess::ExitStatus) {
                finish(QString::fromUtf8(p->readAllStandardOutput()).trimmed());
            });

    // 命令不存在时（例如 Ubuntu + niri 下没有 hyprctl）也要回调，避免界面卡在“检测中”。
    connect(p, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError) {
        finish(QString());
    });

    p->setProcessChannelMode(QProcess::MergedChannels);
    p->start(cmd, args);
}

// ── 分辨率 ──────────────────────────────────────────────

QString EntertainmentPage::detectResolutionBackend() const
{
    if (qEnvironmentVariableIsSet("NIRI_SOCKET"))
        return "niri";

    if (qEnvironmentVariableIsSet("HYPRLAND_INSTANCE_SIGNATURE"))
        return "hyprctl";

    const QString wayland = qEnvironmentVariable("WAYLAND_DISPLAY");
    const QString x11 = qEnvironmentVariable("DISPLAY");

    if (!wayland.isEmpty()) {
        if (!QStandardPaths::findExecutable("niri").isEmpty())
            return "niri";
        if (!QStandardPaths::findExecutable("hyprctl").isEmpty())
            return "hyprctl";
        return "wayland";
    }

    if (!x11.isEmpty() && !QStandardPaths::findExecutable("xrandr").isEmpty())
        return "xrandr";

    return "none";
}

void EntertainmentPage::fillResolutionCombo(const QString &currentRes)
{
    m_resolutionCombo->blockSignals(true);
    m_resolutionCombo->clear();

    for (const auto &mode : m_availableModes)
        m_resolutionCombo->addItem(mode);

    if (!currentRes.isEmpty()) {
        int idx = m_resolutionCombo->findText(currentRes);
        if (idx >= 0)
            m_resolutionCombo->setCurrentIndex(idx);
        m_currentResLabel->setText(
            QString("当前分辨率：%1  [显示器: %2]").arg(currentRes, m_monitorName));
    } else {
        m_currentResLabel->setText("当前分辨率：未知");
    }

    m_resolutionCombo->blockSignals(false);
    m_resStatus->setText(
        QString("共 %1 种分辨率模式（%2）")
            .arg(m_availableModes.size())
            .arg(m_resolutionBackend));
}

void EntertainmentPage::parseNiriResolution(const QString &out)
{
    m_availableModes.clear();
    m_monitorName.clear();

    QString currentRes;
    bool inAvailable = false;
    bool firstOutput = true;

    for (const auto &line : out.split('\n')) {
        const auto t = line.trimmed();

        if (t.startsWith("Output ")) {
            const int open = t.indexOf('(');
            const int close = t.indexOf(')', open);
            if (open >= 0 && close > open) {
                const QString name = t.mid(open + 1, close - open - 1);
                if (firstOutput) {
                    m_monitorName = name;
                    firstOutput = false;
                }
            }
            inAvailable = false;
            continue;
        }

        if (t.startsWith("Current mode:")) {
            QRegularExpression re(R"((\d+x\d+)\s*@\s*([\d.]+))");
            const auto m = re.match(t);
            if (m.hasMatch())
                currentRes = m.captured(1) + "@" + m.captured(2);
            continue;
        }

        if (t.startsWith("Available modes:")) {
            inAvailable = true;
            continue;
        }

        if (inAvailable) {
            QRegularExpression modeRe(R"((\d+x\d+@[\d.]+))");
            const auto m = modeRe.match(t);
            if (m.hasMatch()) {
                const QString mode = m.captured(1);
                if (!m_availableModes.contains(mode))
                    m_availableModes.append(mode);
            }
        }
    }

    fillResolutionCombo(currentRes);
}

void EntertainmentPage::parseHyprResolution(const QString &out)
{
    m_availableModes.clear();
    m_monitorName.clear();

    QString currentRes;
    for (const auto &line : out.split('\n')) {
        auto t = line.trimmed();
        if (t.startsWith("Monitor ")) {
            m_monitorName = t.section(' ', 1, 1).chopped(1);
        }
        if (t.startsWith("availableModes:")) {
            QString modesStr = t.mid(QString("availableModes:").length()).trimmed();
            QStringList seen;
            for (const auto &m : modesStr.split(' ')) {
                if (m.isEmpty()) continue;
                if (!seen.contains(m)) {
                    seen.append(m);
                    m_availableModes.append(m);
                }
            }
        }

        QRegularExpression resRe(R"((\d+x\d+@[\d.]+)\s+at\s+0x0)");
        auto match = resRe.match(t);
        if (match.hasMatch())
            currentRes = match.captured(1);
    }

    fillResolutionCombo(currentRes);
}

void EntertainmentPage::parseXrandrResolution(const QString &out)
{
    m_availableModes.clear();
    m_monitorName.clear();

    QString currentRes;
    bool outputSelected = false;

    for (const auto &line : out.split('\n')) {
        const auto t = line.trimmed();

        if (!outputSelected && t.contains(" connected ")) {
            const auto parts = t.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            if (!parts.isEmpty())
                m_monitorName = parts[0];

            QRegularExpression currentRe(R"((\d+x\d+)\+\d+\+\d+)");
            const auto m = currentRe.match(t);
            if (m.hasMatch())
                currentRes = m.captured(1);

            outputSelected = true;
            continue;
        }

        if (!outputSelected)
            continue;

        const auto parts = t.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (parts.size() < 2)
            continue;

        const QString res = parts[0];
        const QString rate = parts[1];
        if (!res.contains('x') || !rate.contains('.'))
            continue;

        const QString refresh = rate.split('*', Qt::SkipEmptyParts).first();
        const QString mode = res + "@" + refresh;

        if (!m_availableModes.contains(mode))
            m_availableModes.append(mode);

        if (rate.contains('*'))
            currentRes = mode;
    }

    fillResolutionCombo(currentRes);
}

void EntertainmentPage::refreshResolution()
{
    m_resStatus->setText("检测中…");
    m_resolutionBackend = detectResolutionBackend();

    if (m_resolutionBackend == "niri") {
        runCmd("niri", {"msg", "outputs"}, [this](const QString &out) {
            if (out.isEmpty()) {
                m_resStatus->setText("⚠️ niri 未返回输出信息（请确认在 niri 会话内运行）");
                return;
            }
            parseNiriResolution(out);
        });
    } else if (m_resolutionBackend == "hyprctl") {
        runCmd("hyprctl", {"monitors"}, [this](const QString &out) {
            if (out.isEmpty()) {
                m_resStatus->setText("⚠️ hyprctl 未返回显示器信息");
                return;
            }
            parseHyprResolution(out);
        });
    } else if (m_resolutionBackend == "xrandr") {
        runCmd("xrandr", {"--query"}, [this](const QString &out) {
            if (out.isEmpty()) {
                m_resStatus->setText("⚠️ xrandr 未返回显示器信息");
                return;
            }
            parseXrandrResolution(out);
        });
    } else {
        m_availableModes.clear();
        m_monitorName.clear();
        m_resolutionCombo->blockSignals(true);
        m_resolutionCombo->clear();
        m_resolutionCombo->blockSignals(false);
        m_currentResLabel->setText("当前分辨率：不可用");
        m_resStatus->setText("未找到可用的分辨率后端（niri / hyprctl / xrandr）");
    }
}

void EntertainmentPage::onResolutionChanged()
{
    QString mode = m_resolutionCombo->currentText();
    if (mode.isEmpty() || m_monitorName.isEmpty()) return;

    m_resStatus->setText(QString("⏳ 切换到 %1…").arg(mode));

    if (m_resolutionBackend == "niri") {
        runCmd("niri", {"msg", "output", m_monitorName, "mode", mode},
               [this, mode](const QString &out) {
                   if (out.isEmpty()) {
                       m_resStatus->setText(QString("✅ 已切换到 %1").arg(mode));
                   } else {
                       m_resStatus->setText(QString("⚠️ %1").arg(out));
                   }
                   QTimer::singleShot(1500, this, &EntertainmentPage::refreshResolution);
               });
        return;
    }

    if (m_resolutionBackend == "hyprctl") {
        // hyprctl 格式: monitor=NAME,RES@HZ,auto,1
        runCmd("hyprctl", {"keyword", "monitor",
                           QString("%1,%2,auto,1").arg(m_monitorName, mode)},
               [this, mode](const QString &out) {
                   if (out.contains("error", Qt::CaseInsensitive) || out.isEmpty()) {
                       m_resStatus->setText(QString("✅ 已切换到 %1").arg(mode));
                   } else {
                       m_resStatus->setText(QString("⚠️ %1").arg(out));
                   }
                   QTimer::singleShot(1500, this, &EntertainmentPage::refreshResolution);
               });
        return;
    }

    if (m_resolutionBackend == "xrandr") {
        const int sep = mode.indexOf('@');
        const QString res = sep >= 0 ? mode.left(sep) : mode;
        const QString rate = sep >= 0 ? mode.mid(sep + 1) : QString();

        QStringList args;
        args << "--output" << m_monitorName << "--mode" << res;
        if (!rate.isEmpty())
            args << "--rate" << rate;

        runCmd("xrandr", args, [this, mode](const QString &out) {
            if (out.contains("error", Qt::CaseInsensitive)) {
                m_resStatus->setText(QString("⚠️ %1").arg(out));
            } else {
                m_resStatus->setText(QString("✅ 已切换到 %1").arg(mode));
            }
            QTimer::singleShot(1500, this, &EntertainmentPage::refreshResolution);
        });
        return;
    }

    m_resStatus->setText("未找到可用的分辨率后端");
}

// ── 音频 ──────────────────────────────────────────────

QString EntertainmentPage::detectAudioBackend() const
{
    if (!QStandardPaths::findExecutable("pactl").isEmpty())
        return "pactl";
    if (!QStandardPaths::findExecutable("wpctl").isEmpty())
        return "wpctl";
    return "none";
}

QString EntertainmentPage::friendlyAudioDescription(const QString &desc) const
{
    if (desc.contains("HDMI", Qt::CaseInsensitive))
        return "🔊 HDMI 音频输出（显卡）";
    if (desc.contains("IEC958", Qt::CaseInsensitive))
        return "🔊 SPDIF 光纤输出（主板）";
    if (desc.contains("analog", Qt::CaseInsensitive))
        return "🔊 模拟输出（主板）";
    return desc;
}

void EntertainmentPage::fillSinkCombo(int defaultIdx)
{
    m_sinkCombo->blockSignals(true);
    m_sinkCombo->clear();

    for (const auto &si : m_sinks)
        m_sinkCombo->addItem(si.friendlyDesc);

    if (!m_sinks.isEmpty()) {
        if (defaultIdx < 0)
            defaultIdx = 0;

        const auto &si = m_sinks[defaultIdx];
        m_currentSinkId = si.id;
        m_currentSinkName = si.name;
        m_currentVolume = si.volumePercent;

        m_sinkCombo->setCurrentIndex(defaultIdx);
        m_sinkLabel->setText(QString("当前输出：%1").arg(si.friendlyDesc));

        m_volumeSlider->blockSignals(true);
        m_volumeSlider->setValue(m_currentVolume);
        m_volumeSlider->blockSignals(false);
        updateVolumeLabel();

        m_status->setText(QString("就绪（%1）").arg(m_audioBackend));
    } else {
        m_currentSinkId.clear();
        m_currentSinkName.clear();
        m_sinkLabel->setText("当前输出：（未检测到）");
        m_status->setText("⚠️ 未检测到音频设备");
    }

    m_sinkCombo->blockSignals(false);
}

void EntertainmentPage::parsePactlSinks(const QString &out, const QString &defaultSinkName)
{
    m_sinks.clear();
    QString curName, curFriendly;
    int curVol = 50;
    int defaultIdx = -1;

    auto lines = out.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        QString t = lines[i].trimmed();

        if (t.startsWith("Sink #")) {
            if (!curName.isEmpty()) {
                SinkInfo si;
                si.id = curName;
                si.name = curName;
                si.friendlyDesc = curFriendly;
                si.volumePercent = curVol;
                m_sinks.append(si);
                if (curName == defaultSinkName)
                    defaultIdx = m_sinks.size() - 1;
            }
            curName.clear();
            curFriendly.clear();
            curVol = 50;
            continue;
        }

        if (t.startsWith("Name: "))
            curName = t.mid(QString("Name: ").length()).trimmed();
        else if (t.startsWith("Description: "))
            curFriendly = friendlyAudioDescription(t.mid(QString("Description: ").length()).trimmed());
        else if (t.startsWith("Volume:")) {
            QRegularExpression vr(R"((\d+)%)");
            auto m = vr.match(t);
            if (m.hasMatch())
                curVol = m.captured(1).toInt();
        }
    }

    if (!curName.isEmpty()) {
        SinkInfo si;
        si.id = curName;
        si.name = curName;
        si.friendlyDesc = curFriendly;
        si.volumePercent = curVol;
        m_sinks.append(si);
        if (curName == defaultSinkName)
            defaultIdx = m_sinks.size() - 1;
    }

    fillSinkCombo(defaultIdx);
}

void EntertainmentPage::parseWpctlSinks(const QString &out)
{
    m_sinks.clear();

    bool inSinks = false;
    bool inDefaultSection = false;
    QString defaultName;
    int defaultIdx = -1;

    for (const auto &line : out.split('\n')) {
        const auto t = line.trimmed();

        if (t.startsWith("├─ Sinks:")) {
            inSinks = true;
            continue;
        }

        if (inSinks) {
            if (t.startsWith("├─ Sources:") ||
                t.startsWith("├─ Filters:") ||
                t.startsWith("└─ Streams:")) {
                inSinks = false;
                continue;
            }

            if (!t.startsWith("│"))
                continue;

            QString body = t.mid(1).trimmed();
            const bool isDefault = body.startsWith("*");
            if (isDefault)
                body = body.mid(1).trimmed();

            const int dot = body.indexOf(". ");
            if (dot <= 0)
                continue;

            const QString id = body.left(dot);
            const QString rest = body.mid(dot + 2);

            const int bracket = rest.indexOf(" [vol:");
            const QString desc = bracket >= 0 ? rest.left(bracket).trimmed() : rest.trimmed();

            int vol = 50;
            QRegularExpression volRe(R"(\[vol:\s*([0-9.]+)\])");
            const auto m = volRe.match(rest);
            if (m.hasMatch())
                vol = qRound(m.captured(1).toDouble() * 100.0);

            SinkInfo si;
            si.id = id;
            si.name = desc;
            si.friendlyDesc = friendlyAudioDescription(desc);
            si.volumePercent = vol;
            m_sinks.append(si);

            if (isDefault)
                defaultIdx = m_sinks.size() - 1;
        }

        if (t.startsWith("Default Configured Devices:")) {
            inDefaultSection = true;
            continue;
        }

        if (inDefaultSection && t.contains("Audio/Sink")) {
            const int dot = t.indexOf(". ");
            const QString rest = dot >= 0 ? t.mid(dot + 2) : t;
            const int sp = rest.indexOf(' ');
            if (sp >= 0)
                defaultName = rest.mid(sp + 1).trimmed();
        }
    }

    if (defaultIdx < 0 && !defaultName.isEmpty()) {
        for (int i = 0; i < m_sinks.size(); ++i) {
            if (m_sinks[i].name == defaultName) {
                defaultIdx = i;
                break;
            }
        }
    }

    fillSinkCombo(defaultIdx);
}

void EntertainmentPage::parseSinks()
{
    m_audioBackend = detectAudioBackend();

    if (m_audioBackend == "pactl") {
        runCmd("pactl", {"info"}, [this](const QString &info) {
            QString defaultSinkName;
            for (const auto &l : info.split('\n')) {
                if (l.startsWith("Default Sink:"))
                    defaultSinkName = l.mid(QString("Default Sink:").length()).trimmed();
            }

            runCmd("pactl", {"list", "sinks"}, [this, defaultSinkName](const QString &out) {
                parsePactlSinks(out, defaultSinkName);
            });
        });
        return;
    }

    if (m_audioBackend == "wpctl") {
        runCmd("wpctl", {"status"}, [this](const QString &out) {
            if (out.isEmpty()) {
                m_status->setText("⚠️ wpctl 未返回音频信息");
                return;
            }
            parseWpctlSinks(out);
        });
        return;
    }

    m_sinks.clear();
    m_currentSinkId.clear();
    m_currentSinkName.clear();
    m_sinkCombo->blockSignals(true);
    m_sinkCombo->clear();
    m_sinkCombo->blockSignals(false);
    m_sinkLabel->setText("当前输出：（不可用）");
    m_status->setText("未找到音频后端（pactl / wpctl）");
}

void EntertainmentPage::refreshStatus()
{
    m_status->setText("正在刷新…");
    parseSinks();
}

void EntertainmentPage::onSwitchSink(int index)
{
    if (index < 0 || index >= m_sinks.size()) return;

    const auto &si = m_sinks[index];

    if (m_audioBackend == "pactl") {
        if (si.name == m_currentSinkName) return;

        m_status->setText("⏳ 切换中…");
        runCmd("pactl", {"set-default-sink", si.name},
               [this, index](const QString &) {
                   m_currentSinkName = m_sinks[index].name;
                   m_sinkLabel->setText(
                       QString("当前输出：%1").arg(m_sinks[index].friendlyDesc));
                   m_status->setText(
                       QString("✅ 已切换到 %1").arg(m_sinks[index].friendlyDesc));
                   parseSinks();
               });
        return;
    }

    if (m_audioBackend == "wpctl") {
        if (si.id == m_currentSinkId) return;

        m_status->setText("⏳ 切换中…");
        runCmd("wpctl", {"set-default", si.id},
               [this, index](const QString &) {
                   m_currentSinkId = m_sinks[index].id;
                   m_currentSinkName = m_sinks[index].name;
                   m_sinkLabel->setText(
                       QString("当前输出：%1").arg(m_sinks[index].friendlyDesc));
                   m_status->setText(
                       QString("✅ 已切换到 %1").arg(m_sinks[index].friendlyDesc));
                   parseSinks();
               });
        return;
    }

    m_status->setText("未找到音频后端");
}

void EntertainmentPage::onVolumeSliderReleased()
{
    setVolume(m_volumeSlider->value());
}

void EntertainmentPage::setVolume(int percent)
{
    int v = qBound(0, percent, 100);

    if (m_audioBackend == "pactl") {
        if (m_currentSinkName.isEmpty()) return;

        runCmd("pactl", {"set-sink-volume", m_currentSinkName,
                         QString("%1%").arg(v)},
               [this, v](const QString &) {
                   m_currentVolume = v;
                   updateVolumeLabel();
               });
        return;
    }

    if (m_audioBackend == "wpctl") {
        if (m_currentSinkId.isEmpty()) return;

        runCmd("wpctl", {"set-volume", m_currentSinkId,
                         QString("%1%").arg(v)},
               [this, v](const QString &) {
                   m_currentVolume = v;
                   updateVolumeLabel();
               });
        return;
    }
}

void EntertainmentPage::onVolumeUp()
{
    if (m_audioBackend == "pactl") {
        if (m_currentSinkName.isEmpty()) return;

        runCmd("pactl", {"set-sink-volume", m_currentSinkName, "+5%"},
               [this](const QString &) { refreshVolumeOnly(); });
        return;
    }

    if (m_audioBackend == "wpctl") {
        if (m_currentSinkId.isEmpty()) return;

        runCmd("wpctl", {"set-volume", m_currentSinkId, "5%+"},
               [this](const QString &) { refreshVolumeOnly(); });
        return;
    }
}

void EntertainmentPage::onVolumeDown()
{
    if (m_audioBackend == "pactl") {
        if (m_currentSinkName.isEmpty()) return;

        runCmd("pactl", {"set-sink-volume", m_currentSinkName, "-5%"},
               [this](const QString &) { refreshVolumeOnly(); });
        return;
    }

    if (m_audioBackend == "wpctl") {
        if (m_currentSinkId.isEmpty()) return;

        runCmd("wpctl", {"set-volume", m_currentSinkId, "5%-"},
               [this](const QString &) { refreshVolumeOnly(); });
        return;
    }
}

void EntertainmentPage::refreshVolumeOnly()
{
    if (m_audioBackend == "pactl") {
        if (m_currentSinkName.isEmpty()) return;

        runCmd("pactl", {"list", "sinks"}, [this](const QString &out) {
            bool found = false;
            for (const auto &l : out.split('\n')) {
                QString t = l.trimmed();
                if (t.startsWith("Name: ") &&
                    t.mid(QString("Name: ").length()).trimmed() == m_currentSinkName) {
                    found = true;
                    continue;
                }
                if (found && t.startsWith("Volume:")) {
                    QRegularExpression vr(R"((\d+)%)");
                    auto m = vr.match(t);
                    if (m.hasMatch()) {
                        m_currentVolume = m.captured(1).toInt();
                        m_volumeSlider->blockSignals(true);
                        m_volumeSlider->setValue(m_currentVolume);
                        m_volumeSlider->blockSignals(false);
                        updateVolumeLabel();
                    }
                    break;
                }
            }
        });
        return;
    }

    if (m_audioBackend == "wpctl") {
        if (m_currentSinkId.isEmpty()) return;

        runCmd("wpctl", {"status"}, [this](const QString &out) {
            bool inSinks = false;

            for (const auto &line : out.split('\n')) {
                const auto t = line.trimmed();

                if (t.startsWith("├─ Sinks:")) {
                    inSinks = true;
                    continue;
                }

                if (inSinks &&
                    (t.startsWith("├─ Sources:") ||
                     t.startsWith("├─ Filters:") ||
                     t.startsWith("└─ Streams:"))) {
                    inSinks = false;
                    continue;
                }

                if (!inSinks || !t.startsWith("│"))
                    continue;

                QString body = t.mid(1).trimmed();
                if (body.startsWith("*"))
                    body = body.mid(1).trimmed();

                const int dot = body.indexOf(". ");
                if (dot <= 0)
                    continue;

                const QString id = body.left(dot);
                if (id != m_currentSinkId)
                    continue;

                QRegularExpression volRe(R"(\[vol:\s*([0-9.]+)\])");
                const auto m = volRe.match(body);
                if (m.hasMatch()) {
                    m_currentVolume = qRound(m.captured(1).toDouble() * 100.0);
                    m_volumeSlider->blockSignals(true);
                    m_volumeSlider->setValue(m_currentVolume);
                    m_volumeSlider->blockSignals(false);
                    updateVolumeLabel();
                }
                break;
            }
        });
        return;
    }
}

void EntertainmentPage::updateVolumeLabel()
{
    m_volumeLabel->setText(QString("音量：%1%").arg(m_currentVolume));
}

// ── 手柄检测 ──────────────────────────────────────────

void EntertainmentPage::detectControllers()
{
    m_ctrlStatusLabel->setText("正在检测手柄…");
    m_ctrlListLabel->clear();
    m_ctrlDetectBtn->setEnabled(false);

    // 用 lsusb 和 /dev/input 检测
    runCmd("sh", {"-c",
        "ls /dev/input/js* 2>/dev/null | head -5; "
        "echo '---USB---'; "
        "lsusb | grep -iE 'gamepad|joystick|controller|xbox|playstation|nintendo|8bitdo|game' 2>/dev/null; "
        "echo '---EVDEV---'; "
        "grep -l '' /sys/class/input/event*/device/name 2>/dev/null | "
        "xargs -I{} sh -c 'echo -n \"{}: \"; cat {}' 2>/dev/null | "
        "grep -iE 'joystick|gamepad|controller|xbox|playstation|nintendo'"
    }, [this](const QString &out) {
        m_ctrlDetectBtn->setEnabled(true);

        QStringList lines;
        for (const auto &l : out.split('\n')) {
            if (!l.trimmed().isEmpty())
                lines << l.trimmed();
        }

        if (lines.isEmpty()) {
            m_ctrlStatusLabel->setText("❌ 未检测到手柄");
            m_ctrlListLabel->setText("未连接任何手柄设备。");
        } else {
            int devCount = 0;
            QStringList devices;
            for (const auto &l : lines) {
                if (l.startsWith("/dev/input/js")) {
                    devCount++;
                    devices << QString("📦 %1").arg(l);
                } else if (!l.contains("---")) {
                    devices << QString("🔌 %1").arg(l);
                }
            }
            m_ctrlStatusLabel->setText(
                QString("✅ 检测到 %1 个手柄设备").arg(devCount));
            m_ctrlListLabel->setText(devices.join("\n"));
        }
    });
}
