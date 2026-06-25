#include "logpage.h"
#include "logger.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFont>
#include <QDateTime>
#include <QRegularExpression>
#include <QMessageBox>

LogPage::LogPage(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    // ── 标题 ──
    auto *title = new QLabel("📋  日志管理");
    QFont f = title->font(); f.setPointSize(14); f.setBold(true);
    title->setFont(f);
    layout->addWidget(title);

    // ── 标签页 ──
    m_tabs = new QTabWidget();

    // ════ 应用日志 ════
    auto *appTab = new QWidget();
    auto *appLayout = new QVBoxLayout(appTab);

    auto *appHint = new QLabel("工具箱自身产生的日志（虚机启动、错误等）");
    appHint->setStyleSheet("color: #888; font-size: 11px;");
    appLayout->addWidget(appHint);

    m_appLogEdit = new QPlainTextEdit();
    m_appLogEdit->setReadOnly(true);
    m_appLogEdit->setFont(QFont("Monospace", 9));
    m_appLogEdit->setMaximumBlockCount(10000);
    m_appLogEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
    appLayout->addWidget(m_appLogEdit);

    auto *appBtnRow = new QHBoxLayout();
    m_clearBtn = new QPushButton("🗑️ 清空日志");
    connect(m_clearBtn, &QPushButton::clicked, this, &LogPage::clearAppLog);
    appBtnRow->addWidget(m_clearBtn);
    appBtnRow->addStretch();
    appLayout->addLayout(appBtnRow);

    m_tabs->addTab(appTab, "应用日志");

    // ════ 系统日志 ════
    auto *sysTab = new QWidget();
    auto *sysLayout = new QVBoxLayout(sysTab);

    auto *sysHint = new QLabel("系统日志（journalctl），可切换过滤条件");
    sysHint->setStyleSheet("color: #888; font-size: 11px;");
    sysLayout->addWidget(sysHint);

    m_sysLogEdit = new QPlainTextEdit();
    m_sysLogEdit->setReadOnly(true);
    m_sysLogEdit->setFont(QFont("Monospace", 9));
    m_sysLogEdit->setMaximumBlockCount(10000);
    m_sysLogEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
    sysLayout->addWidget(m_sysLogEdit);

    auto *sysFilterRow = new QHBoxLayout();
    auto *filterLabel = new QLabel("筛选:");
    sysFilterRow->addWidget(filterLabel);

    m_sysFilterCombo = new QComboBox();
    m_sysFilterCombo->addItem("最近 50 条",      "50");
    m_sysFilterCombo->addItem("最近 200 条",     "200");
    m_sysFilterCombo->addItem("最近 1 小时",     "1h");
    m_sysFilterCombo->addItem("QEMU 相关",       "qemu");
    m_sysFilterCombo->addItem("内核消息",         "kernel");
    m_sysFilterCombo->addItem("存储/挂载",        "mount");
    m_sysFilterCombo->setCurrentIndex(0);
    connect(m_sysFilterCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this]() { refreshSystemLog(); });
    sysFilterRow->addWidget(m_sysFilterCombo);

    sysFilterRow->addStretch();
    sysLayout->addLayout(sysFilterRow);

    m_tabs->addTab(sysTab, "系统日志");

    layout->addWidget(m_tabs);

    // ── 底部状态 ──
    auto *bottomRow = new QHBoxLayout();
    m_status = new QLabel("");
    m_status->setStyleSheet("color: #888;");
    bottomRow->addWidget(m_status, 1);

    m_refreshBtn = new QPushButton("🔄 刷新");
    connect(m_refreshBtn, &QPushButton::clicked, this, [this]() {
        if (m_tabs->currentIndex() == 0)
            refreshAppLog();
        else
            refreshSystemLog();
    });
    bottomRow->addWidget(m_refreshBtn);
    layout->addLayout(bottomRow);

    // ── 初始加载 + 定时刷新（每 10 秒刷新应用日志）──
    refreshAppLog();

    m_autoRefresh = new QTimer(this);
    connect(m_autoRefresh, &QTimer::timeout, this, [this]() {
        if (m_tabs->currentIndex() == 0)
            refreshAppLog();
    });
    m_autoRefresh->start(10000);
}

// ═══════════════════════════════════════════════════════════════════
//  应用日志
// ═══════════════════════════════════════════════════════════════════
void LogPage::refreshAppLog()
{
    QStringList lines = Logger::readAll();
    m_appLogEdit->clear();
    for (const auto &line : lines)
        m_appLogEdit->appendPlainText(line);

    m_status->setText(QString("共 %1 条应用日志").arg(lines.size()));
}

void LogPage::clearAppLog()
{
    auto ret = QMessageBox::question(this, "确认清空",
        "确定清空所有应用日志？",
        QMessageBox::Yes | QMessageBox::No);
    if (ret != QMessageBox::Yes) return;

    Logger::clear();
    refreshAppLog();
    Logger::log("日志已清空");
}

// ═══════════════════════════════════════════════════════════════════
//  系统日志
// ═══════════════════════════════════════════════════════════════════
void LogPage::refreshSystemLog()
{
    m_sysLogEdit->clear();
    m_sysLogEdit->appendPlainText("正在获取系统日志…");
    m_status->setText("获取中…");

    QString filter = m_sysFilterCombo->currentData().toString();
    QStringList args;

    if (filter == "50") {
        args << "-n" << "50" << "--no-pager";
    } else if (filter == "200") {
        args << "-n" << "200" << "--no-pager";
    } else if (filter == "1h") {
        args << "--since" << "1 hour ago" << "--no-pager";
    } else if (filter == "qemu") {
        args << "-n" << "100" << "--no-pager"
             << "-u" << "qemu*"
             << "--grep" << "qemu";
    } else if (filter == "kernel") {
        args << "-n" << "100" << "--no-pager" << "-k";
    } else if (filter == "mount") {
        args << "-n" << "100" << "--no-pager"
             << "--grep" << "mount\\|sd \\|nvme\\|xfs\\|ext4\\|btrfs";
    } else {
        args << "-n" << "50" << "--no-pager";
    }

    auto *proc = new QProcess(this);
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, proc](int, QProcess::ExitStatus) {
        QString out = QString::fromUtf8(proc->readAllStandardOutput()).trimmed();
        QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed();
        proc->deleteLater();

        m_sysLogEdit->clear();
        if (!out.isEmpty()) {
            m_sysLogEdit->appendPlainText(out);
            m_status->setText(QString("系统日志 — %1 行")
                .arg(out.count('\n') + 1));
        } else if (!err.isEmpty()) {
            m_sysLogEdit->appendPlainText("[错误] " + err);
            m_status->setText("获取失败");
        } else {
            m_sysLogEdit->appendPlainText("(无匹配日志)");
            m_status->setText("无结果");
        }
    });

    proc->start("journalctl", args);
}
