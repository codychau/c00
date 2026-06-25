#include "entertainmentpage.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QRegularExpression>
#include <QTimer>
#include <QFrame>

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
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [p, cb](int, QProcess::ExitStatus) {
                cb(QString::fromUtf8(p->readAllStandardOutput()).trimmed());
                p->deleteLater();
            });
    p->setProcessChannelMode(QProcess::MergedChannels);
    p->start(cmd, args);
}

// ── 分辨率 ──────────────────────────────────────────────

void EntertainmentPage::refreshResolution()
{
    m_resStatus->setText("检测中…");

    runCmd("hyprctl", {"monitors"}, [this](const QString &out) {
        m_availableModes.clear();
        m_monitorName.clear();
        m_resolutionCombo->blockSignals(true);
        m_resolutionCombo->clear();

        QString currentRes;
        for (const auto &line : out.split('\n')) {
            auto t = line.trimmed();
            if (t.startsWith("Monitor ")) {
                m_monitorName = t.section(' ', 1, 1).chopped(1);
            }
            if (t.startsWith("availableModes:")) {
                QString modesStr = t.mid(QString("availableModes:").length()).trimmed();
                // 去重（hyprctl 有重复）
                QStringList seen;
                for (const auto &m : modesStr.split(' ')) {
                    if (m.isEmpty()) continue;
                    if (!seen.contains(m)) {
                        seen.append(m);
                        m_availableModes.append(m);
                    }
                }
            }
            // 当前分辨率行
            QRegularExpression resRe(R"((\d+x\d+@[\d.]+)\s+at\s+0x0)");
            auto match = resRe.match(t);
            if (match.hasMatch()) {
                currentRes = match.captured(1);
            }
        }

        // 填充下拉
        for (const auto &mode : m_availableModes) {
            m_resolutionCombo->addItem(mode);
        }

        // 选中当前分辨率
        if (!currentRes.isEmpty()) {
            int idx = m_resolutionCombo->findText(currentRes);
            if (idx >= 0) m_resolutionCombo->setCurrentIndex(idx);
            m_currentResLabel->setText(
                QString("当前分辨率：%1  [显示器: %2]").arg(currentRes, m_monitorName));
        } else {
            m_currentResLabel->setText(
                QString("当前分辨率：%1").arg(currentRes.isEmpty() ? "未知" : currentRes));
        }

        m_resolutionCombo->blockSignals(false);
        m_resStatus->setText(
            QString("共 %1 种分辨率模式").arg(m_availableModes.size()));
    });
}

void EntertainmentPage::onResolutionChanged()
{
    QString mode = m_resolutionCombo->currentText();
    if (mode.isEmpty() || m_monitorName.isEmpty()) return;

    m_resStatus->setText(QString("⏳ 切换到 %1…").arg(mode));

    // hyprctl 格式: monitor=NAME,RES@HZ,auto,1
    runCmd("hyprctl", {"keyword", "monitor",
                       QString("%1,%2,auto,1").arg(m_monitorName, mode)},
           [this, mode](const QString &out) {
               if (out.contains("error", Qt::CaseInsensitive) || out.isEmpty()) {
                   m_resStatus->setText(QString("✅ 已切换到 %1").arg(mode));
               } else {
                   m_resStatus->setText(QString("⚠️ %1").arg(out));
               }
               // 延迟刷新确认
               QTimer::singleShot(1500, this, &EntertainmentPage::refreshResolution);
           });
}

// ── 音频 ──────────────────────────────────────────────

void EntertainmentPage::parseSinks()
{
    runCmd("pactl", {"info"}, [this](const QString &info) {
        QString defaultSinkName;
        for (const auto &l : info.split('\n'))
            if (l.startsWith("Default Sink:"))
                defaultSinkName = l.mid(QString("Default Sink:").length()).trimmed();

        runCmd("pactl", {"list", "sinks"}, [this, defaultSinkName](const QString &out) {
            m_sinks.clear();
            QString curName, curDesc, curFriendly;
            int curVol = 50, foundIdx = 0;

            auto lines = out.split('\n');
            for (int i = 0; i < lines.size(); ++i) {
                QString t = lines[i].trimmed();

                if (t.startsWith("Sink #")) {
                    if (!curName.isEmpty()) {
                        SinkInfo si;
                        si.name = curName;
                        si.friendlyDesc = curFriendly;
                        si.volumePercent = curVol;
                        m_sinks.append(si);
                    }
                    curName.clear(); curDesc.clear();
                    curVol = 50;
                    continue;
                }
                if (t.startsWith("Name: "))
                    curName = t.mid(QString("Name: ").length()).trimmed();
                else if (t.startsWith("Description: ")) {
                    curDesc = t.mid(QString("Description: ").length()).trimmed();
                    if (curDesc.contains("HDMI", Qt::CaseInsensitive))
                        curFriendly = "🔊 HDMI 音频输出（显卡）";
                    else if (curDesc.contains("IEC958", Qt::CaseInsensitive))
                        curFriendly = "🔊 SPDIF 光纤输出（主板）";
                    else if (curDesc.contains("analog", Qt::CaseInsensitive))
                        curFriendly = "🔊 模拟输出（主板）";
                    else
                        curFriendly = curDesc;
                } else if (t.startsWith("Volume:")) {
                    QRegularExpression vr(R"((\d+)%)");
                    auto m = vr.match(t);
                    if (m.hasMatch())
                        curVol = m.captured(1).toInt();
                }
            }
            if (!curName.isEmpty()) {
                SinkInfo si;
                si.name = curName;
                si.friendlyDesc = curFriendly;
                si.volumePercent = curVol;
                m_sinks.append(si);
            }

            m_sinkCombo->blockSignals(true);
            m_sinkCombo->clear();
            int selectIdx = 0;
            for (int i = 0; i < m_sinks.size(); ++i) {
                m_sinkCombo->addItem(m_sinks[i].friendlyDesc);
                if (m_sinks[i].name == defaultSinkName) {
                    selectIdx = i;
                    m_currentSinkName = m_sinks[i].name;
                    m_currentVolume = m_sinks[i].volumePercent;
                }
            }
            if (!m_sinks.isEmpty()) {
                m_sinkCombo->setCurrentIndex(selectIdx);
                m_sinkLabel->setText(
                    QString("当前输出：%1").arg(m_sinks[selectIdx].friendlyDesc));
                m_volumeSlider->blockSignals(true);
                m_volumeSlider->setValue(m_currentVolume);
                m_volumeSlider->blockSignals(false);
                updateVolumeLabel();
            } else {
                m_sinkLabel->setText("当前输出：（未检测到）");
            }
            m_sinkCombo->blockSignals(false);

            m_status->setText(m_sinks.isEmpty() ? "⚠️ 未检测到音频设备" : "就绪");
        });
    });
}

void EntertainmentPage::refreshStatus()
{
    m_status->setText("正在刷新…");
    parseSinks();
}

void EntertainmentPage::onSwitchSink(int index)
{
    if (index < 0 || index >= m_sinks.size()) return;
    if (m_sinks[index].name == m_currentSinkName) return;

    m_status->setText("⏳ 切换中…");
    runCmd("pactl", {"set-default-sink", m_sinks[index].name},
           [this, index](const QString &) {
               m_currentSinkName = m_sinks[index].name;
               m_sinkLabel->setText(
                   QString("当前输出：%1").arg(m_sinks[index].friendlyDesc));
               m_status->setText(
                   QString("✅ 已切换到 %1").arg(m_sinks[index].friendlyDesc));
               parseSinks();
           });
}

void EntertainmentPage::onVolumeSliderReleased()
{
    if (m_currentSinkName.isEmpty()) return;
    setVolume(m_volumeSlider->value());
}

void EntertainmentPage::setVolume(int percent)
{
    if (m_currentSinkName.isEmpty()) return;
    int v = qBound(0, percent, 100);
    runCmd("pactl", {"set-sink-volume", m_currentSinkName,
                     QString("%1%").arg(v)},
           [this, v](const QString &) { m_currentVolume = v; updateVolumeLabel(); });
}

void EntertainmentPage::onVolumeUp()
{
    if (m_currentSinkName.isEmpty()) return;
    runCmd("pactl", {"set-sink-volume", m_currentSinkName, "+5%"},
           [this](const QString &) { refreshVolumeOnly(); });
}

void EntertainmentPage::onVolumeDown()
{
    if (m_currentSinkName.isEmpty()) return;
    runCmd("pactl", {"set-sink-volume", m_currentSinkName, "-5%"},
           [this](const QString &) { refreshVolumeOnly(); });
}

void EntertainmentPage::refreshVolumeOnly()
{
    if (m_currentSinkName.isEmpty()) return;
    runCmd("pactl", {"list", "sinks"}, [this](const QString &out) {
        bool found = false;
        for (const auto &l : out.split('\n')) {
            QString t = l.trimmed();
            if (t.startsWith("Name: ") &&
                t.mid(QString("Name: ").length()).trimmed() == m_currentSinkName) {
                found = true; continue;
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
}

void EntertainmentPage::updateVolumeLabel()
{
    m_volumeLabel->setText(QString("音量：%1%").arg(m_currentVolume));
}
