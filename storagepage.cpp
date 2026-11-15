#include "storagepage.h"
#include "theme.h"
#include "storagedevicepage.h"
#include "mountpage.h"
#include "formatdialog.h"

#include <QGroupBox>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QHBoxLayout>

// 与 MainWindow 顶栏高度一致：主窗口的页面堆栈铺满整块主区，顶栏以浮层
// 形式盖在内容上做毛玻璃，因此正文要自行让出这段顶部空间。
static constexpr int kTopbarInset = 52;

StoragePage::StoragePage(QWidget *parent)
    : QWidget(parent)
{
    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, kTopbarInset, 0, 0);
    outerLayout->setSpacing(0);

    // ── 主体内容（可滚动） ──
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    // 显式开启横向滚动条：widgetResizable(true) 下默认横向策略是 Ignored，
    // 溢出内容会被静默裁掉而不出现滚动条
scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *container = new QWidget();
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    // 存储设备管理
    auto *deviceGroup = new QGroupBox("存储设备管理");
    auto *deviceLayout = new QVBoxLayout(deviceGroup);
    m_devicePage = new StorageDevicePage(deviceGroup);
    deviceLayout->addWidget(m_devicePage);
    layout->addWidget(deviceGroup);

    // 挂载管理
    auto *mountGroup = new QGroupBox("挂载管理");
    auto *mountLayout = new QVBoxLayout(mountGroup);
    m_mountPage = new MountPage(mountGroup);
    mountLayout->addWidget(m_mountPage);
    layout->addWidget(mountGroup);

    layout->addStretch();
    scroll->setWidget(container);
    outerLayout->addWidget(scroll, 1);

    // ── 底部全局状态栏（常驻） ──
    m_statusBar = new QWidget();
    m_statusBar->setObjectName("storageStatusBar");
    {
        const QString surf  = Theme::surface().name(QColor::HexRgb);
        const QString bdr   = Theme::border().name(QColor::HexRgb);
        const QString dim   = Theme::textDim().name(QColor::HexRgb);
        const QString btn   = Theme::surface2().name(QColor::HexRgb);
        const QString btnH  = Theme::surface3().name(QColor::HexRgb);
        const QString acc   = Theme::accent().name(QColor::HexRgb);
        m_statusBar->setStyleSheet(
            QString("#storageStatusBar {"
                    "  background: %1; border-top: 1px solid %2;"
                    "}"
                    "#storageStatusBar QLabel { color: %3; font-size: 12px; }"
                    "#storageStatusBar QPushButton {"
                    "  background: %4; color: %7; padding: 2px 12px;"
                    "  border-radius: 3px; font-size: 11px;"
                    "}"
                    "#storageStatusBar QPushButton:hover { background: %5; }"
                    "#storageStatusBar QProgressBar {"
                    "  border: 1px solid %2; border-radius: 3px;"
                    "  background: %1; height: 16px; text-align: center;"
                    "  color: %3; font-size: 10px;"
                    "}"
                    "#storageStatusBar QProgressBar::chunk {"
                    "  background: %6; border-radius: 2px;"
                    "}").arg(surf).arg(bdr).arg(dim).arg(btn).arg(btnH)
                       .arg(acc).arg(Theme::onAccent().name(QColor::HexRgb)));
    }

    auto *barLayout = new QHBoxLayout(m_statusBar);
    barLayout->setContentsMargins(0, 0, 0, 0);

    m_statusIcon = new QLabel("✅  就绪");
    barLayout->addWidget(m_statusIcon);

    m_progressBar = new QProgressBar();
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    m_progressBar->setFixedWidth(180);
    m_progressBar->setVisible(false);
    barLayout->addWidget(m_progressBar);

    barLayout->addStretch();

    m_restoreBtn = new QPushButton("📂 恢复窗口");
    m_restoreBtn->setVisible(false);
    connect(m_restoreBtn, &QPushButton::clicked, this, [this]() {
        // 找到 device page 上的 FormatDialog
        auto *dlg = findChild<FormatDialog *>();
        if (dlg) {
            dlg->show();
            dlg->raise();
            dlg->activateWindow();
        }
    });
    barLayout->addWidget(m_restoreBtn);

    outerLayout->addWidget(m_statusBar);

    // ── 连接子页面的信号 ──
    connect(m_devicePage, &StorageDevicePage::formatProgress,
            this, &StoragePage::onFormatProgress);
    connect(m_devicePage, &StorageDevicePage::formatFinished,
            this, &StoragePage::onFormatFinished);
}

void StoragePage::onFormatProgress(int percent, const QString &status)
{
    m_statusIcon->setText("⚠️  " + status);
    m_progressBar->setVisible(true);
    m_progressBar->setValue(percent);
    m_restoreBtn->setVisible(true);
}

void StoragePage::onFormatFinished(bool success)
{
    m_progressBar->setVisible(false);
    m_progressBar->setValue(0);
    m_restoreBtn->setVisible(false);
    m_statusIcon->setText(success ? "✅  格式化完成" : "❌  格式化失败");
}
