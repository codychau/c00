#include "mountpage.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QRegularExpression>

MountPage::MountPage(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_table = new QTableWidget(0, 5);
    m_table->setHorizontalHeaderLabels({"设备", "容量", "已用", "可用", "挂载点"});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->verticalHeader()->hide();
    layout->addWidget(m_table);

    auto *row = new QHBoxLayout();
    m_status = new QLabel("双击设备行查看详情");
    m_status->setStyleSheet("color: #888;");
    row->addWidget(m_status, 1);
    m_refreshBtn = new QPushButton("刷新");
    connect(m_refreshBtn, &QPushButton::clicked, this, &MountPage::refresh);
    row->addWidget(m_refreshBtn);
    layout->addLayout(row);

    refresh();
}

void MountPage::runCmd(const QString &cmd, const QStringList &args,
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

void MountPage::refresh()
{
    m_status->setText("正在检测…");

    runCmd("df", {"-h", "--output=source,size,used,avail,target"}, [this](const QString &out) {
        auto lines = out.split('\n');
        if (lines.isEmpty()) return;
        lines.removeFirst(); // 去掉表头

        m_table->setRowCount(0);
        for (const auto &line : lines) {
            if (line.trimmed().isEmpty()) continue;
            auto parts = line.split(QRegularExpression("\\s+"));
            if (parts.size() < 5) continue;

            // 只显示真实块设备, 跳过 tmpfs/devtmpfs/overlay
            if (!parts[0].startsWith("/dev/")) continue;

            int row = m_table->rowCount();
            m_table->insertRow(row);
            m_table->setItem(row, 0, new QTableWidgetItem(parts[0]));
            m_table->setItem(row, 1, new QTableWidgetItem(parts[1]));
            m_table->setItem(row, 2, new QTableWidgetItem(parts[2]));
            m_table->setItem(row, 3, new QTableWidgetItem(parts[3]));
            m_table->setItem(row, 4, new QTableWidgetItem(parts[4]));
        }

        m_status->setText(
            QString("共 %1 个挂载设备").arg(m_table->rowCount()));
    });
}
