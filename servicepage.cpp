#include "servicepage.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>

ServicePage::ServicePage(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);

    auto *title = new QLabel("⚙️ 服务控制");
    QFont f = title->font(); f.setPointSize(14); f.setBold(true);
    title->setFont(f);
    layout->addWidget(title);

    m_table = new QTableWidget(0, 3);
    m_table->setHorizontalHeaderLabels({"服务名称", "状态", "描述"});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->hide();
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this]() {
        int r = m_table->currentRow();
        if (r >= 0) {
            m_selectedName = m_table->item(r, 0)->text();
            m_selectedStatus = m_table->item(r, 1)->text();
            m_selectedLabel->setText(
                QString("已选择：%1  [%2]").arg(m_selectedName, m_selectedStatus));
            m_startStopBtn->setEnabled(true);
            if (m_selectedStatus.contains("active", Qt::CaseInsensitive))
                m_startStopBtn->setText("⏹  停止");
            else
                m_startStopBtn->setText("▶  启动");
        }
    });
    layout->addWidget(m_table);

    m_selectedLabel = new QLabel("未选择服务");
    m_selectedLabel->setStyleSheet("color: #888;");
    layout->addWidget(m_selectedLabel);

    auto *row = new QHBoxLayout();
    m_startStopBtn = new QPushButton("▶  启动");
    m_startStopBtn->setEnabled(false);
    connect(m_startStopBtn, &QPushButton::clicked, this, &ServicePage::onStartStop);
    row->addWidget(m_startStopBtn);
    row->addStretch();

    m_status = new QLabel("");
    m_status->setStyleSheet("color: #666;");
    row->addWidget(m_status, 1);

    m_refreshBtn = new QPushButton("刷新");
    connect(m_refreshBtn, &QPushButton::clicked, this, &ServicePage::refresh);
    row->addWidget(m_refreshBtn);
    layout->addLayout(row);

    refresh();
}

void ServicePage::runCmd(const QString &cmd, const QStringList &args,
                         std::function<void(const QString &)> cb)
{
    auto *p = new QProcess(this);
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [p, cb](int, QProcess::ExitStatus) {
                cb(QString::fromUtf8(p->readAllStandardOutput()).trimmed());
                p->deleteLater();
            });
    p->start(cmd, args);
}

void ServicePage::refresh()
{
    m_status->setText("正在检测…");
    m_table->setRowCount(0);

    // 常用服务列表
    QStringList services = {
        "fnos.service", "ollama.service", "pipewire.service",
        "sshd.service", "nfs-server.service", "smb.service",
        "cups.service"
    };

    // 先查 user 服务
    runCmd("systemctl", {"--user", "list-units", "--type=service",
                         "--no-legend", "--no-pager"},
           [this, services](const QString &out) {
               QString result;
               for (const auto &svc : services) {
                   // 检查 user 服务
                   bool found = false;
                   for (const auto &l : out.split('\n')) {
                       auto parts = l.split(QRegularExpression("\\s+"),
                                            Qt::SkipEmptyParts);
                       if (parts.size() >= 2 && parts[0] == svc) {
                           int row = m_table->rowCount();
                           m_table->insertRow(row);
                           QString status = parts[1];
                           m_table->setItem(row, 0,
                               new QTableWidgetItem(svc));
                           m_table->setItem(row, 1,
                               new QTableWidgetItem(status));
                           m_table->setItem(row, 2,
                               new QTableWidgetItem("systemd (user)"));
                           found = true;
                           break;
                       }
                   }
                   if (!found) {
                       int row = m_table->rowCount();
                       m_table->insertRow(row);
                       m_table->setItem(row, 0,
                           new QTableWidgetItem(svc));
                       m_table->setItem(row, 1,
                           new QTableWidgetItem("—"));
                       m_table->setItem(row, 2,
                           new QTableWidgetItem("systemd (user) — 未加载"));
                   }
               }

               m_status->setText(
                   QString("共 %1 个服务").arg(m_table->rowCount()));
           });
}

void ServicePage::onStartStop()
{
    if (m_selectedName.isEmpty()) return;

    bool isActive = m_selectedStatus.contains("active", Qt::CaseInsensitive);
    QString action = isActive ? "stop" : "start";
    m_status->setText(
        QString("%1 %2…").arg(isActive ? "停止" : "启动").arg(m_selectedName));

    runCmd("systemctl", {"--user", action, m_selectedName},
           [this, action](const QString &) {
               m_status->setText(
                   QString("✅ %1 操作完成").arg(action == "stop" ? "停止" : "启动"));
               refresh();
           });
}
