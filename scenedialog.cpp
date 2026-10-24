#include "scenedialog.h"
#include "vmconfig.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QMap>
#include <QVector>
#include <QColor>

#include <algorithm>

SceneDialog::SceneDialog(const Scene &scene, QWidget *parent)
    : QDialog(parent)
    , m_scene(scene)
{
    setWindowTitle(scene.name.isEmpty() ? "新建场景"
                                        : QString("编辑场景 — %1").arg(scene.name));
    resize(780, 660);

    auto *layout = new QVBoxLayout(this);

    auto *nameRow = new QHBoxLayout();
    nameRow->addWidget(new QLabel("场景名称："));
    m_nameEdit = new QLineEdit(scene.name);
    m_nameEdit->setPlaceholderText("例如：工作、夜间、周末");
    nameRow->addWidget(m_nameEdit, 1);
    m_enabledCheck = new QCheckBox("启用");
    m_enabledCheck->setChecked(scene.enabled);
    nameRow->addWidget(m_enabledCheck);
    layout->addLayout(nameRow);

    auto *hint = new QLabel(
        "为场景勾选需要控制的系统服务和虚机。定时场景在开始时间执行「启动」项，结束时间执行「停止」项；"
        "手动场景请在场景页面下方勾选「手动触发场景」，只通过「立即执行启动 / 结束」按钮触发。");
    hint->setWordWrap(true);
    hint->setStyleSheet("color: #888;");
    layout->addWidget(hint);

    auto *tabs = new QTabWidget();

    // ── 服务 ──
    auto *serviceTab = new QWidget();
    auto *serviceLayout = new QVBoxLayout(serviceTab);

    m_serviceSearch = new QLineEdit();
    m_serviceSearch->setPlaceholderText("搜索服务…");
    connect(m_serviceSearch, &QLineEdit::textChanged,
            this, &SceneDialog::filterServices);
    serviceLayout->addWidget(m_serviceSearch);

    m_serviceTable = new QTableWidget(0, 3);
    m_serviceTable->setHorizontalHeaderLabels({"控制", "服务", "作用域"});
    m_serviceTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_serviceTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_serviceTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_serviceTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_serviceTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_serviceTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_serviceTable->verticalHeader()->hide();
    serviceLayout->addWidget(m_serviceTable, 1);

    auto *serviceBtns = new QHBoxLayout();
    auto *svcStartBtn = new QPushButton("✓  设为启动");
    auto *svcStopBtn = new QPushButton("⏹  设为停止");
    auto *svcClearBtn = new QPushButton("清除控制");
    connect(svcStartBtn, &QPushButton::clicked, this, [this]() {
        setSelectedAction(m_serviceTable, "start");
    });
    connect(svcStopBtn, &QPushButton::clicked, this, [this]() {
        setSelectedAction(m_serviceTable, "stop");
    });
    connect(svcClearBtn, &QPushButton::clicked, this, [this]() {
        setSelectedAction(m_serviceTable, QString());
    });
    serviceBtns->addWidget(svcStartBtn);
    serviceBtns->addWidget(svcStopBtn);
    serviceBtns->addWidget(svcClearBtn);
    serviceBtns->addStretch();
    serviceLayout->addLayout(serviceBtns);
    tabs->addTab(serviceTab, "⚙️ 服务");

    // ── 虚机 ──
    auto *vmTab = new QWidget();
    auto *vmLayout = new QVBoxLayout(vmTab);

    auto *vmHint = new QLabel("选择虚机管理中的虚机：启动项会在开始时间启动，停止项会在结束时间关机。");
    vmHint->setStyleSheet("color: #888;");
    vmHint->setWordWrap(true);
    vmLayout->addWidget(vmHint);

    m_vmTable = new QTableWidget(0, 2);
    m_vmTable->setHorizontalHeaderLabels({"控制", "虚机"});
    m_vmTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_vmTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_vmTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_vmTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_vmTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_vmTable->verticalHeader()->hide();
    vmLayout->addWidget(m_vmTable, 1);

    auto *vmBtns = new QHBoxLayout();
    auto *vmStartBtn = new QPushButton("✓  设为启动");
    auto *vmStopBtn = new QPushButton("⏹  设为停止");
    auto *vmClearBtn = new QPushButton("清除控制");
    connect(vmStartBtn, &QPushButton::clicked, this, [this]() {
        setSelectedAction(m_vmTable, "start");
    });
    connect(vmStopBtn, &QPushButton::clicked, this, [this]() {
        setSelectedAction(m_vmTable, "stop");
    });
    connect(vmClearBtn, &QPushButton::clicked, this, [this]() {
        setSelectedAction(m_vmTable, QString());
    });
    vmBtns->addWidget(vmStartBtn);
    vmBtns->addWidget(vmStopBtn);
    vmBtns->addWidget(vmClearBtn);
    vmBtns->addStretch();
    vmLayout->addLayout(vmBtns);
    tabs->addTab(vmTab, "🖥️ 虚机");

    layout->addWidget(tabs, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("保存");
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        if (m_nameEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, "提示", "请填写场景名称");
            return;
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    QApplication::setOverrideCursor(Qt::WaitCursor);
    loadServices();
    loadVms();
    QApplication::restoreOverrideCursor();
}

QString SceneDialog::actionKey(const QString &scope, const QString &name)
{
    return scope + '\n' + name;
}

QString SceneDialog::actionText(const QString &action)
{
    if (action == "start") return "启动";
    if (action == "stop")  return "停止";
    return QString();
}

void SceneDialog::addServiceRow(const QString &name, const QString &scope,
                                const QString &action)
{
    const int row = m_serviceTable->rowCount();
    m_serviceTable->insertRow(row);

    auto *actionItem = new QTableWidgetItem(actionText(action));
    actionItem->setData(Qt::UserRole, action);
    if (action == "start")
        actionItem->setForeground(QColor("#27ae60"));
    else if (action == "stop")
        actionItem->setForeground(QColor("#e74c3c"));
    m_serviceTable->setItem(row, 0, actionItem);

    m_serviceTable->setItem(row, 1, new QTableWidgetItem(name));
    m_serviceTable->setItem(row, 2, new QTableWidgetItem(scope));
}

void SceneDialog::addVmRow(const QString &name, const QString &action)
{
    const int row = m_vmTable->rowCount();
    m_vmTable->insertRow(row);

    auto *actionItem = new QTableWidgetItem(actionText(action));
    actionItem->setData(Qt::UserRole, action);
    if (action == "start")
        actionItem->setForeground(QColor("#27ae60"));
    else if (action == "stop")
        actionItem->setForeground(QColor("#e74c3c"));
    m_vmTable->setItem(row, 0, actionItem);

    m_vmTable->setItem(row, 1, new QTableWidgetItem(name));
}

void SceneDialog::loadServices()
{
    QMap<QString, QString> actions;
    for (const auto &svc : m_scene.startServices)
        actions.insert(actionKey(svc.scope, svc.name), "start");
    for (const auto &svc : m_scene.stopServices)
        actions.insert(actionKey(svc.scope, svc.name), "stop");

    struct ServiceRow {
        QString name;
        QString scope;
    };
    QVector<ServiceRow> services;
    QSet<QString> seen;

    auto collect = [&](const QString &scope, const QStringList &scopeArgs) {
        auto run = [&](const QStringList &args) {
            QProcess proc;
            proc.start("systemctl", args);
            if (!proc.waitForFinished(5000))
                return;

            const QString out = QString::fromUtf8(proc.readAllStandardOutput());
            for (const auto &line : out.split('\n', Qt::SkipEmptyParts)) {
                const auto parts = line.split(QRegularExpression("\\s+"),
                                              Qt::SkipEmptyParts);
                if (parts.isEmpty() || !parts[0].endsWith(".service"))
                    continue;
                const QString key = actionKey(scope, parts[0]);
                if (seen.contains(key))
                    continue;
                seen.insert(key);
                services.append({parts[0], scope});
            }
        };

        run(scopeArgs + QStringList{"list-unit-files", "--type=service",
                                    "--no-legend", "--no-pager"});
        run(scopeArgs + QStringList{"list-units", "--type=service", "--all",
                                    "--no-legend", "--no-pager"});
    };

    collect("system", {});
    collect("user", {"--user"});

    std::sort(services.begin(), services.end(),
              [](const ServiceRow &a, const ServiceRow &b) {
                  if (a.name == b.name) return a.scope < b.scope;
                  return a.name < b.name;
              });

    for (const auto &svc : services)
        addServiceRow(svc.name, svc.scope,
                      actions.value(actionKey(svc.scope, svc.name)));

    // systemctl 列表不可用时，仍保留场景中已配置的服务
    for (auto it = actions.constBegin(); it != actions.constEnd(); ++it) {
        const int sep = it.key().indexOf('\n');
        if (sep < 0) continue;
        const QString scope = it.key().left(sep);
        const QString name = it.key().mid(sep + 1);
        if (!seen.contains(it.key())) {
            seen.insert(it.key());
            addServiceRow(name, scope, it.value());
        }
    }
}

void SceneDialog::loadVms()
{
    QMap<QString, QString> actions;
    for (const auto &name : m_scene.startVms)
        actions.insert(name, "start");
    for (const auto &name : m_scene.stopVms)
        actions.insert(name, "stop");

    QStringList names;
    VMConfigManager mgr(VMConfigManager::defaultConfigPath());
    mgr.load();
    for (const auto &vm : mgr.vms()) {
        if (!names.contains(vm.name))
            names.append(vm.name);
    }
    for (auto it = actions.constBegin(); it != actions.constEnd(); ++it) {
        if (!names.contains(it.key()))
            names.append(it.key());
    }
    names.sort(Qt::CaseInsensitive);

    for (const auto &name : names)
        addVmRow(name, actions.value(name));
}

void SceneDialog::filterServices(const QString &text)
{
    for (int row = 0; row < m_serviceTable->rowCount(); ++row) {
        const QString name = m_serviceTable->item(row, 1)->text();
        const QString scope = m_serviceTable->item(row, 2)->text();
        const bool match = text.isEmpty()
            || name.contains(text, Qt::CaseInsensitive)
            || scope.contains(text, Qt::CaseInsensitive);
        m_serviceTable->setRowHidden(row, !match);
    }
}

void SceneDialog::setSelectedAction(QTableWidget *table, const QString &action)
{
    QSet<int> rows;
    const auto ranges = table->selectedRanges();
    for (const auto &range : ranges) {
        for (int row = range.topRow(); row <= range.bottomRow(); ++row) {
            if (row >= 0 && row < table->rowCount())
                rows.insert(row);
        }
    }
    if (rows.isEmpty() && table->currentRow() >= 0)
        rows.insert(table->currentRow());

    for (int row : rows) {
        if (table->isRowHidden(row))
            continue;
        auto *item = table->item(row, 0);
        if (!item) continue;
        item->setData(Qt::UserRole, action);
        item->setText(actionText(action));
        if (action == "start")
            item->setForeground(QColor("#27ae60"));
        else if (action == "stop")
            item->setForeground(QColor("#e74c3c"));
        else
            item->setForeground(QColor());
    }
}

Scene SceneDialog::scene() const
{
    Scene result = m_scene;
    result.name = m_nameEdit->text().trimmed();
    result.enabled = m_enabledCheck->isChecked();

    result.startServices.clear();
    result.stopServices.clear();
    for (int row = 0; row < m_serviceTable->rowCount(); ++row) {
        const QString action = m_serviceTable->item(row, 0)->data(Qt::UserRole).toString();
        if (action != "start" && action != "stop")
            continue;
        SceneService svc;
        svc.name = m_serviceTable->item(row, 1)->text();
        svc.scope = m_serviceTable->item(row, 2)->text();
        if (action == "start")
            result.startServices.append(svc);
        else
            result.stopServices.append(svc);
    }

    result.startVms.clear();
    result.stopVms.clear();
    for (int row = 0; row < m_vmTable->rowCount(); ++row) {
        const QString action = m_vmTable->item(row, 0)->data(Qt::UserRole).toString();
        if (action != "start" && action != "stop")
            continue;
        const QString name = m_vmTable->item(row, 1)->text();
        if (action == "start")
            result.startVms.append(name);
        else
            result.stopVms.append(name);
    }

    return result;
}
