#include "servicepage.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QShowEvent>
#include <QRegularExpression>
#include <QMenu>
#include <QSharedPointer>

ServicePage::ServicePage(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);

    auto *title = new QLabel("⚙️ 服务控制");
    QFont f = title->font(); f.setPointSize(14); f.setBold(true);
    title->setFont(f);
    layout->addWidget(title);

    m_searchBox = new QLineEdit();
    m_searchBox->setPlaceholderText("搜索服务…");
    connect(m_searchBox, &QLineEdit::textChanged, this, &ServicePage::filterServices);
    layout->addWidget(m_searchBox);

    m_table = new QTableWidget(0, 4);
    m_table->setHorizontalHeaderLabels({"服务名称", "状态", "作用域", "描述"});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->hide();
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->setColumnWidth(0, 220);
    m_table->setColumnWidth(1, 80);
    m_table->setColumnWidth(2, 65);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this]() {
        int r = m_table->currentRow();
        if (r >= 0 && !m_table->isRowHidden(r)) {
            m_selectedName = m_table->item(r, 0)->text();
            m_selectedStatus = m_table->item(r, 1)->text();
            m_selectedLabel->setText(
                QString("已选择：%1  [%2]").arg(m_selectedName, m_selectedStatus));
            m_startStopBtn->setEnabled(true);
            if (m_selectedStatus== "active")
                m_startStopBtn->setText("⏹  停止");
            else
                m_startStopBtn->setText("▶  启动");
        }
    });
    connect(m_table, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QTableWidgetItem *item = m_table->itemAt(pos);
        if (!item) return;

        int row = item->row();
        if (row < 0 || m_table->isRowHidden(row)) return;

        QString name = m_table->item(row, 0)->text();
        QString status = m_table->item(row, 1)->text();

        ServiceEntry entry;
        bool found = false;
        for (const auto &e : m_allServices) {
            if (e.name == name) { entry = e; found = true; break; }
        }
        if (!found) return;

        QMenu menu(this);
        bool isActive = status== "active";
        QAction *actStartStop = menu.addAction(isActive ? "⏹  停止" : "▶  启动");
        bool isEnabled = entry.enabled == "enabled";
        QString enableLabel = isEnabled ? "🔒  禁用" : "🔓  启用";
        if (entry.enabled == "static" || entry.enabled == "indirect")
            enableLabel = QString("□  %1").arg(entry.enabled);
        QAction *actEnable = menu.addAction(enableLabel);
        if (entry.enabled == "static" || entry.enabled == "indirect")
            actEnable->setEnabled(false);

        QAction *chosen = menu.exec(m_table->mapToGlobal(pos));
        if (!chosen) return;

        if (chosen == actStartStop) {
            m_selectedName = name;
            m_selectedStatus = status;
            onStartStop();
        } else if (chosen == actEnable) {
            QString action = isEnabled ? "disable" : "enable";
            m_status->setText(QString("%1 %2…")
                .arg(isEnabled ? "禁用" : "启用").arg(name));
            auto onDone = [this, action](const QString &) {
                m_status->setText(QString("✅ %1 操作完成")
                    .arg(action == "disable" ? "禁用" : "启用"));
                refresh();
            };
            if (entry.scope == "user")
                runCmd("systemctl", {"--user", action, name}, onDone);
            else
                runCmd("pkexec", {"systemctl", action, name}, onDone);
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

void ServicePage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    refresh();
}

void ServicePage::refresh()
{
    m_status->setText("正在检测…");
    m_table->setRowCount(0);
    m_allServices.clear();

    struct ScopeBuf {
        QMap<QString, QString> enabled;
        QVector<ServiceEntry> entries;
    };

    auto userBuf = QSharedPointer<ScopeBuf>::create();
    auto sysBuf = QSharedPointer<ScopeBuf>::create();
    auto pending = QSharedPointer<int>::create(0);
    auto onOneDone = [this, pending, userBuf, sysBuf]() {
        if (++(*pending) < 4) return;

        QSet<QString> userNames;
        for (auto &e : userBuf->entries) {
            e.enabled = userBuf->enabled.value(e.name, "unknown");
            m_allServices.append(e);
            userNames.insert(e.name);
        }
        for (auto &e : sysBuf->entries) {
            if (!userNames.contains(e.name)) {
                e.enabled = sysBuf->enabled.value(e.name, "unknown");
                m_allServices.append(e);
            }
        }

        populateTable();
        m_status->setText(QString("共 %1 个服务").arg(m_table->rowCount()));
    };

    // 1) user list-unit-files → enabled map
    runCmd("systemctl", {"--user", "list-unit-files", "--type=service",
                         "--no-legend", "--no-pager"},
           [userBuf, onOneDone](const QString &out) {
               for (const auto &line : out.split('\n', Qt::SkipEmptyParts)) {
                   auto p = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
                   if (p.size() >= 2 && p[0].endsWith(".service"))
                       userBuf->enabled[p[0]] = p[1];
               }
               onOneDone();
           });

    // 2) system list-unit-files → enabled map
    runCmd("systemctl", {"list-unit-files", "--type=service",
                         "--no-legend", "--no-pager"},
           [sysBuf, onOneDone](const QString &out) {
               for (const auto &line : out.split('\n', Qt::SkipEmptyParts)) {
                   auto p = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
                   if (p.size() >= 2 && p[0].endsWith(".service"))
                       sysBuf->enabled[p[0]] = p[1];
               }
               onOneDone();
           });

    // 3) user list-units → entries
    runCmd("systemctl", {"--user", "list-units", "--type=service", "--all",
                         "--no-legend", "--no-pager"},
           [userBuf, onOneDone](const QString &out) {
               for (const auto &line : out.split('\n', Qt::SkipEmptyParts)) {
                   auto parts = line.split(QRegularExpression("\\s{2,}"),
                                           Qt::SkipEmptyParts);
                   if (parts.size() >= 4 && parts[0].endsWith(".service")) {
                       ServiceEntry e;
                       e.name = parts[0];
                       e.active = parts[2];
                       e.sub = parts[3];
                       e.scope = "user";
                       e.description = parts.size() > 4
                           ? parts.mid(4).join(' ') : "";
                       userBuf->entries.append(e);
                   }
               }
               onOneDone();
           });

    // 4) system list-units → entries
    runCmd("systemctl", {"list-units", "--type=service", "--all",
                         "--no-legend", "--no-pager"},
           [sysBuf, onOneDone](const QString &out) {
               for (const auto &line : out.split('\n', Qt::SkipEmptyParts)) {
                   auto parts = line.split(QRegularExpression("\\s{2,}"),
                                           Qt::SkipEmptyParts);
                   if (parts.size() >= 4 && parts[0].endsWith(".service")) {
                       ServiceEntry e;
                       e.name = parts[0];
                       e.active = parts[2];
                       e.sub = parts[3];
                       e.scope = "system";
                       e.description = parts.size() > 4
                           ? parts.mid(4).join(' ') : "";
                       sysBuf->entries.append(e);
                   }
               }
               onOneDone();
           });
}

void ServicePage::populateTable()
{
    m_table->setRowCount(0);
    for (const auto &e : m_allServices) {
        int row = m_table->rowCount();
        m_table->insertRow(row);
        m_table->setItem(row, 0, new QTableWidgetItem(e.name));
        m_table->setItem(row, 1, new QTableWidgetItem(e.active));
        m_table->setItem(row, 2, new QTableWidgetItem(e.scope));
        m_table->setItem(row, 3, new QTableWidgetItem(e.description));

        if (e.active == "active")
            m_table->item(row, 1)->setForeground(QColor("#27ae60"));
        else if (e.active == "failed")
            m_table->item(row, 1)->setForeground(QColor("#e74c3c"));
        else
            m_table->item(row, 1)->setForeground(QColor("#95a5a6"));
    }
}

void ServicePage::filterServices(const QString &text)
{
    for (int i = 0; i < m_table->rowCount(); ++i) {
        bool match = text.isEmpty()
            || m_table->item(i, 0)->text().contains(text, Qt::CaseInsensitive)
            || m_table->item(i, 3)->text().contains(text, Qt::CaseInsensitive);
        m_table->setRowHidden(i, !match);
    }
}

void ServicePage::onStartStop()
{
    if (m_selectedName.isEmpty()) return;

    ServiceEntry entry;
    bool found = false;
    for (const auto &e : m_allServices) {
        if (e.name == m_selectedName) {
            entry = e;
            found = true;
            break;
        }
    }
    if (!found) return;

    bool isActive = m_selectedStatus== "active";
    QString action = isActive ? "stop" : "start";
    m_status->setText(
        QString("%1 %2…").arg(isActive ? "停止" : "启动").arg(m_selectedName));

    if (entry.scope == "user") {
        runCmd("systemctl", {"--user", action, m_selectedName},
               [this, action](const QString &) {
                   m_status->setText(
                       QString("✅ %1 操作完成")
                           .arg(action == "stop" ? "停止" : "启动"));
                   refresh();
               });
    } else {
        runCmd("pkexec", {"systemctl", action, m_selectedName},
               [this, action](const QString &) {
                   m_status->setText(
                       QString("✅ %1 操作完成")
                           .arg(action == "stop" ? "停止" : "启动"));
                   refresh();
               });
    }
}
