#include "mountpage.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QRegularExpression>
#include <QMessageBox>

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
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->verticalHeader()->hide();
    connect(m_table, &QTableWidget::itemSelectionChanged,
            this, &MountPage::onSelectionChanged);
    layout->addWidget(m_table);

    auto *row = new QHBoxLayout();
    m_status = new QLabel("就绪");
    m_status->setStyleSheet("color: #888;");
    row->addWidget(m_status, 1);

    m_unmountBtn = new QPushButton("⏏️ 卸载");
    m_unmountBtn->setEnabled(false);
    m_unmountBtn->setToolTip("卸载选中的设备");
    connect(m_unmountBtn, &QPushButton::clicked, this, &MountPage::unmount);
    row->addWidget(m_unmountBtn);

    m_refreshBtn = new QPushButton("刷新");
    connect(m_refreshBtn, &QPushButton::clicked, this, &MountPage::refresh);
    row->addWidget(m_refreshBtn);
    layout->addLayout(row);

    refresh();
}

void MountPage::onSelectionChanged()
{
    m_unmountBtn->setEnabled(m_table->currentRow() >= 0);
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
    p->setProcessChannelMode(QProcess::MergedChannels);
    p->start(cmd, args);
}

void MountPage::refresh()
{
    m_status->setText("正在检测…");

    runCmd("df", {"-h", "--output=source,size,used,avail,target"}, [this](const QString &out) {
        auto lines = out.split('\n');
        if (lines.isEmpty()) return;
        lines.removeFirst();

        m_table->setRowCount(0);
        for (const auto &line : lines) {
            if (line.trimmed().isEmpty()) continue;
            auto parts = line.split(QRegularExpression("\\s+"));
            if (parts.size() < 5) continue;

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
        onSelectionChanged();
    });
}

void MountPage::unmount()
{
    int row = m_table->currentRow();
    if (row < 0) return;

    QString dev  = m_table->item(row, 0)->text();
    QString mnt  = m_table->item(row, 4)->text();

    // 不能卸载根目录
    if (mnt == "/") {
        QMessageBox::warning(this, "拒绝操作", "不能卸载根目录 /");
        return;
    }

    auto ret = QMessageBox::question(this, "确认卸载",
        QString("确定卸载 %1  (%2 → %3)?")
            .arg(m_table->item(row, 0)->text(),
                 m_table->item(row, 4)->text(),
                 m_table->item(row, 1)->text()),
        QMessageBox::Yes | QMessageBox::No);
    if (ret != QMessageBox::Yes) return;

    m_status->setText(QString("正在卸载 %1…").arg(mnt));
    m_unmountBtn->setEnabled(false);

    auto *p = new QProcess(this);
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, p, mnt](int exitCode, QProcess::ExitStatus) {
        QString err = QString::fromUtf8(p->readAllStandardError()).trimmed();
        p->deleteLater();

        if (exitCode == 0) {
            m_status->setText(QString("✅ %1 已卸载").arg(mnt));
            refresh();
        } else {
            m_status->setText(QString("❌ 卸载失败"));
            QMessageBox::warning(this, "卸载失败",
                QString("无法卸载 %1:\n%2").arg(mnt, err));
            m_unmountBtn->setEnabled(true);
        }
    });
    p->start("pkexec", {"umount", mnt});
}
