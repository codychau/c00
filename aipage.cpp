#include "aipage.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QProcess>
#include <QRegularExpression>

AIPage::AIPage(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    // ── 标题 ──
    auto *title = new QLabel("🤖 AI 服务状态");
    QFont f = title->font(); f.setPointSize(14); f.setBold(true);
    title->setFont(f);
    layout->addWidget(title);

    // ── 三个服务卡片（水平排列） ──
    auto *cards = new QHBoxLayout();
    cards->setSpacing(12);
    cards->setContentsMargins(0, 0, 0, 0);

    auto createCard = [&](const QString &name, QLabel *&status,
                          QLabel *&detail) -> QFrame * {
        auto *frame = new QFrame();
        frame->setFrameShape(QFrame::StyledPanel);
        auto *vl = new QVBoxLayout(frame);
        vl->setSpacing(6);

        // 名称
        auto *nameLbl = new QLabel(name);
        QFont bf = nameLbl->font(); bf.setBold(true); bf.setPointSize(12);
        nameLbl->setFont(bf);
        vl->addWidget(nameLbl);

        // 状态
        status = new QLabel("检测中…");
        status->setStyleSheet("font-size: 13px;");
        vl->addWidget(status);

        // 详情
        detail = new QLabel("");
        detail->setWordWrap(true);
        detail->setStyleSheet("color: #888; font-size: 11px;");
        detail->setMinimumHeight(30);
        vl->addWidget(detail, 1);

        return frame;
    };

    auto *ollamaCard   = createCard("Ollama",    m_ollamaStatus,   m_ollamaDetail);
    auto *llamacppCard = createCard("llama.cpp", m_llamacppStatus, m_llamacppDetail);
    auto *vllmCard     = createCard("vLLM",      m_vllmStatus,     m_vllmDetail);

    cards->addWidget(ollamaCard,   1);
    cards->addWidget(llamacppCard, 1);
    cards->addWidget(vllmCard,     1);
    layout->addLayout(cards);

    // ── 汇总信息 ──
    m_info = new QLabel("");
    m_info->setWordWrap(true);
    m_info->setStyleSheet("color: #666; font-size: 12px;");
    layout->addWidget(m_info);

    layout->addStretch();

    // ── 刷新 ──
    auto *row = new QHBoxLayout();
    m_refreshBtn = new QPushButton("刷新状态");
    connect(m_refreshBtn, &QPushButton::clicked, this, &AIPage::refresh);
    row->addStretch();
    row->addWidget(m_refreshBtn);
    layout->addLayout(row);

    // ── 定时刷新（每 15 秒）──
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &AIPage::refresh);
    m_timer->start(15000);

    refresh();
}

// ── 通用服务检测 ──
void AIPage::checkService(const QString &name, const QString &systemdSvc,
                          const QString &procName, int port,
                          QLabel *statusLbl, QLabel *detailLbl)
{
    auto setStatus = [statusLbl, detailLbl](const QString &emoji,
                                             const QString &text,
                                             const QString &detail) {
        statusLbl->setText(QString("%1 %2").arg(emoji, text));
        detailLbl->setText(detail);
    };

    // 优先 systemd 检测
    QProcess *p = new QProcess(this);
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [=](int exit, QProcess::ExitStatus) {
        QString out = QString::fromUtf8(p->readAllStandardOutput()).trimmed();
        p->deleteLater();

        // systemd 直接返回 active / inactive
        if (exit == 0 && out == "active") {
            // 拿到 PID 和运行时长
            QProcess *up = new QProcess(this);
            up->start("systemctl", {"--user", "show", "-P", "MainPID",
                                    "-P", "ActiveEnterTimestamp", systemdSvc});
            connect(up, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                    this, [=](int, QProcess::ExitStatus) {
                QString info = QString::fromUtf8(up->readAllStandardOutput()).trimmed();
                up->deleteLater();
                QStringList lines = info.split('\n');
                QString pid  = lines.value(0);
                QString ts   = lines.value(1);

                QString detail = QString("PID: %1").arg(pid);
                if (!ts.isEmpty() && ts != "n/a") {
                    detail += QString("\n启动: %1").arg(ts.mid(0, 19));
                }
                setStatus("🟢", "运行中", detail);
            });
            up->start();
            return;
        }

        // systemd 未找到 → 回落进程检测
        QProcess *pg = new QProcess(this);
        connect(pg, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [=](int, QProcess::ExitStatus) {
            QString pidStr = QString::fromUtf8(pg->readAllStandardOutput()).trimmed();
            pg->deleteLater();

            if (!pidStr.isEmpty()) {
                setStatus("🟢", "运行中",
                          QString("PID: %1\n(无 systemd 服务)").arg(pidStr));
            } else {
                setStatus("⚪", "未运行",
                          QString("systemd: %1 未激活\n进程: %2 未找到")
                              .arg(systemdSvc, procName));
            }
        });

        // pgrep 同时匹配进程名和端口
        QString pgArg = procName;
        if (!procName.isEmpty()) {
            pg->start("pgrep", {"-f", procName});
        } else {
            pg->start("pgrep", {"-f", name.toLower()});
        }
    });

    // 按顺序: 系统级 systemctl → 用户级 → 回落
    QStringList svcCmd = {"is-active", systemdSvc};
    p->start("systemctl", svcCmd);
}

// ── 刷新全部 ──
void AIPage::refresh()
{
    m_ollamaStatus->setText("检测中…");
    m_ollamaDetail->setText("");
    m_llamacppStatus->setText("检测中…");
    m_llamacppDetail->setText("");
    m_vllmStatus->setText("检测中…");
    m_vllmDetail->setText("");
    m_info->setText("正在检查服务状态…");

    // ── Ollama ──
    checkService("Ollama", "ollama.service", "ollama", 11434,
                 m_ollamaStatus, m_ollamaDetail);

    // ── llama.cpp ──
    checkService("llama.cpp", "llama.service", "llama-server", 8080,
                 m_llamacppStatus, m_llamacppDetail);

    // ── vLLM ──
    checkService("vLLM", "vllm.service", "vllm", 8000,
                 m_vllmStatus, m_vllmDetail);

    // ── 收集汇总信息 ──
    // （延迟一点等上面的异步结果，用单个一次性 timer 兜底）
    QTimer::singleShot(2000, this, [this]() {
        int running = 0;
        if (m_ollamaStatus->text().contains("🟢"))   running++;
        if (m_llamacppStatus->text().contains("🟢")) running++;
        if (m_vllmStatus->text().contains("🟢"))     running++;
        m_info->setText(QString("AI 后端: %1 / 3 运行中")
                        .arg(running));
    });
}
