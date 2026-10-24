#include "scenepage.h"
#include "scenedialog.h"
#include "vmpage.h"
#include "logger.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QHeaderView>
#include <QGroupBox>
#include <QTimeEdit>
#include <QCheckBox>
#include <QMessageBox>
#include <QProcess>
#include <QTimer>
#include <QSignalBlocker>
#include <QMap>
#include <QColor>

#include <algorithm>

namespace {
const char *kSceneTag = "SCENE";

QString actionLabel(const QString &action)
{
    return action == "start" ? "启动" : "停止";
}
}

ScenePage::ScenePage(VMPage *vmPage, QWidget *parent)
    : QWidget(parent)
    , m_vmPage(vmPage)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);

    auto *title = new QLabel("🎬 场景设定");
    QFont f = title->font(); f.setPointSize(14); f.setBold(true);
    title->setFont(f);
    layout->addWidget(title);

    auto *hint = new QLabel(
        "定时场景会在「开始时间」启动指定服务 / 虚机，在「结束时间」停止它们；"
        "手动场景只通过「立即执行启动 / 结束」按钮触发。定时执行需要保持工具箱运行。");
    hint->setStyleSheet("color: #888;");
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_table = new QTableWidget(0, 6);
    m_table->setHorizontalHeaderLabels(
        {"场景名称", "状态", "启动时间", "结束时间", "重复", "控制内容"});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->hide();
    m_table->setAlternatingRowColors(true);
    connect(m_table, &QTableWidget::itemSelectionChanged,
            this, &ScenePage::onSelectionChanged);
    connect(m_table, &QTableWidget::cellDoubleClicked,
            this, [this](int, int) { editScene(); });
    layout->addWidget(m_table, 1);

    auto *buttonRow = new QHBoxLayout();
    m_addBtn = new QPushButton("➕  新建场景");
    m_editBtn = new QPushButton("✏️  编辑");
    m_deleteBtn = new QPushButton("🗑️  删除");
    m_editBtn->setEnabled(false);
    m_deleteBtn->setEnabled(false);
    connect(m_addBtn, &QPushButton::clicked, this, &ScenePage::addScene);
    connect(m_editBtn, &QPushButton::clicked, this, &ScenePage::editScene);
    connect(m_deleteBtn, &QPushButton::clicked, this, &ScenePage::deleteScene);
    buttonRow->addWidget(m_addBtn);
    buttonRow->addWidget(m_editBtn);
    buttonRow->addWidget(m_deleteBtn);
    buttonRow->addStretch();
    layout->addLayout(buttonRow);

    // ── 启动 / 结束时间 ──
    m_timeBox = new QGroupBox("⏱  启动与结束时间");
    auto *grid = new QGridLayout(m_timeBox);

    m_selectedLabel = new QLabel("未选择场景");
    QFont selFont = m_selectedLabel->font();
    selFont.setBold(true);
    m_selectedLabel->setFont(selFont);
    grid->addWidget(m_selectedLabel, 0, 0, 1, 6);

    grid->addWidget(new QLabel("开始时间："), 1, 0);
    m_startEdit = new QTimeEdit();
    m_startEdit->setDisplayFormat("HH:mm");
    m_startEdit->setCalendarPopup(false);
    grid->addWidget(m_startEdit, 1, 1);

    grid->addWidget(new QLabel("结束时间："), 1, 2);
    m_endEdit = new QTimeEdit();
    m_endEdit->setDisplayFormat("HH:mm");
    m_endEdit->setCalendarPopup(false);
    grid->addWidget(m_endEdit, 1, 3);

    grid->addWidget(new QLabel("重复："), 2, 0);
    auto *daysRow = new QHBoxLayout();
    m_everyDayCheck = new QCheckBox("每天");
    daysRow->addWidget(m_everyDayCheck);
    static const char *dayNames[7] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};
    for (int i = 0; i < 7; ++i) {
        m_dayChecks[i] = new QCheckBox(dayNames[i]);
        daysRow->addWidget(m_dayChecks[i]);
    }
    daysRow->addStretch();
    grid->addLayout(daysRow, 2, 1, 1, 5);

    connect(m_everyDayCheck, &QCheckBox::toggled, this, [this](bool checked) {
        for (int i = 0; i < 7; ++i)
            m_dayChecks[i]->setEnabled(!checked && !m_manualCheck->isChecked());
    });

    m_manualCheck = new QCheckBox("手动触发场景（不使用时间段）");
    connect(m_manualCheck, &QCheckBox::toggled, this, [this](bool checked) {
        setTimeControlsEnabled(!checked);
    });
    grid->addWidget(m_manualCheck, 3, 0, 1, 6);

    m_enabledCheck = new QCheckBox("启用该场景");
    grid->addWidget(m_enabledCheck, 4, 0, 1, 2);

    auto *actionRow = new QHBoxLayout();
    m_startNowBtn = new QPushButton("▶  立即执行启动");
    m_stopNowBtn = new QPushButton("⏹  立即执行结束");
    m_saveBtn = new QPushButton("💾  保存设置");
    connect(m_startNowBtn, &QPushButton::clicked, this, &ScenePage::runNowStart);
    connect(m_stopNowBtn, &QPushButton::clicked, this, &ScenePage::runNowStop);
    connect(m_saveBtn, &QPushButton::clicked, this, &ScenePage::saveSchedule);
    actionRow->addStretch();
    actionRow->addWidget(m_startNowBtn);
    actionRow->addWidget(m_stopNowBtn);
    actionRow->addWidget(m_saveBtn);
    grid->addLayout(actionRow, 4, 2, 1, 4);

    auto *windowHint = new QLabel(
        "开始与结束时间相同表示全天生效；开始晚于结束时为跨午夜窗口。"
        "勾选「手动触发场景」后，时间设置会被禁用。");
    windowHint->setStyleSheet("color: #888;");
    windowHint->setWordWrap(true);
    grid->addWidget(windowHint, 5, 0, 1, 6);

    layout->addWidget(m_timeBox);

    m_statusLabel = new QLabel("");
    m_statusLabel->setStyleSheet("color: #666;");
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);

    setScheduleControlsEnabled(false);

    if (!m_mgr.load())
        setStatus("⚠️ 场景配置文件读取失败");

    refreshTable();

    m_timer = new QTimer(this);
    m_timer->setInterval(10000);
    connect(m_timer, &QTimer::timeout, this, &ScenePage::onTick);
    m_timer->start();
    QTimer::singleShot(1500, this, &ScenePage::onTick);
}

// ── 表格 ──

void ScenePage::refreshTable(const QString &selectName)
{
    QString wanted = selectName;
    if (wanted.isEmpty()) {
        const int row = m_table->currentRow();
        if (row >= 0 && row < m_mgr.scenes().size())
            wanted = m_mgr.scenes()[row].name;
    }

    const QSignalBlocker blocker(m_table);
    m_table->setRowCount(0);

    for (const auto &scene : m_mgr.scenes()) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);

        auto *nameItem = new QTableWidgetItem(scene.name);
        m_table->setItem(row, 0, nameItem);

        auto *statusItem = new QTableWidgetItem();
        m_table->setItem(row, 1, statusItem);

        const bool manual = isManual(scene);
        m_table->setItem(row, 2, new QTableWidgetItem(
            manual ? "手动" : scene.startTime.toString("HH:mm")));
        m_table->setItem(row, 3, new QTableWidgetItem(
            manual ? "—" : scene.endTime.toString("HH:mm")));
        m_table->setItem(row, 4, new QTableWidgetItem(
            manual ? "手动触发" : daysText(scene.days)));

        auto *summaryItem = new QTableWidgetItem(controlSummary(scene));
        summaryItem->setToolTip(controlDetails(scene));
        m_table->setItem(row, 5, summaryItem);
    }

    int selectRow = -1;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (m_table->item(row, 0)->text() == wanted) {
            selectRow = row;
            break;
        }
    }
    if (selectRow < 0 && m_table->rowCount() > 0)
        selectRow = 0;

    if (selectRow >= 0)
        m_table->setCurrentCell(selectRow, 0);

    m_editBtn->setEnabled(selectRow >= 0);
    m_deleteBtn->setEnabled(selectRow >= 0);
    setScheduleControlsEnabled(selectRow >= 0);

    updateStatusColumn();
    loadScheduleControls();
}

void ScenePage::updateStatusColumn()
{
    if (m_table->rowCount() != m_mgr.scenes().size()) {
        refreshTable();
        return;
    }

    const QDateTime now = QDateTime::currentDateTime();
    for (int row = 0; row < m_mgr.scenes().size(); ++row) {
        const Scene &scene = m_mgr.scenes()[row];
        auto *item = m_table->item(row, 1);
        if (!item) continue;

        QString text;
        QColor color;
        const bool manual = isManual(scene);

        if (!scene.enabled) {
            text = "已禁用";
            color = QColor("#9ca3af");
        } else if (manual) {
            text = "待手动触发";
            color = QColor("#2980b9");
        } else if (m_activeScenes.contains(scene.name)) {
            text = "● 运行中";
            color = QColor("#27ae60");
        } else if (inWindow(scene, now)) {
            text = "进行中";
            color = QColor("#2980b9");
        } else {
            text = "等待";
            color = QColor("#9ca3af");
        }
        item->setText(text);
        item->setForeground(color);
    }
}

int ScenePage::selectedRow() const
{
    const int row = m_table->currentRow();
    return (row >= 0 && row < m_mgr.scenes().size()) ? row : -1;
}

Scene *ScenePage::selectedScene()
{
    const int row = selectedRow();
    return row >= 0 ? &m_mgr.scenes()[row] : nullptr;
}

void ScenePage::onSelectionChanged()
{
    const bool hasSelection = selectedRow() >= 0;
    m_editBtn->setEnabled(hasSelection);
    m_deleteBtn->setEnabled(hasSelection);
    setScheduleControlsEnabled(hasSelection);
    loadScheduleControls();
}

void ScenePage::setScheduleControlsEnabled(bool enabled)
{
    m_timeBox->setEnabled(enabled);
}

void ScenePage::setTimeControlsEnabled(bool enabled)
{
    m_startEdit->setEnabled(enabled);
    m_endEdit->setEnabled(enabled);
    m_everyDayCheck->setEnabled(enabled);

    const bool everyDay = m_everyDayCheck->isChecked();
    for (int i = 0; i < 7; ++i)
        m_dayChecks[i]->setEnabled(enabled && !everyDay);
}

// ── 场景增删改 ──

void ScenePage::addScene()
{
    Scene initial;
    initial.mode = "time";
    initial.startTime = QTime(8, 0);
    initial.endTime = QTime(23, 0);

    SceneDialog dlg(initial, this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    Scene scene = dlg.scene();
    for (const auto &existing : m_mgr.scenes()) {
        if (existing.name == scene.name) {
            QMessageBox::warning(this, "提示",
                QString("场景「%1」已存在，请使用其他名称").arg(scene.name));
            return;
        }
    }

    m_mgr.addScene(scene);
    if (!m_mgr.save()) {
        QMessageBox::warning(this, "保存失败",
            QString("无法写入配置文件：\n%1").arg(m_mgr.configPath()));
        return;
    }
    refreshTable(scene.name);
    setStatus(QString("✅ 已创建场景「%1」").arg(scene.name));
}

void ScenePage::editScene()
{
    const int row = selectedRow();
    if (row < 0) return;

    Scene current = m_mgr.scenes()[row];
    // 带上尚未保存的时间设置，避免编辑服务 / 虚机时丢失
    current.mode = m_manualCheck->isChecked() ? "manual" : "time";
    current.startTime = m_startEdit->time();
    current.endTime = m_endEdit->time();
    current.enabled = m_enabledCheck->isChecked();
    const QList<int> days = checkedDays();
    if (m_everyDayCheck->isChecked() || !days.isEmpty())
        current.days = days;

    SceneDialog dlg(current, this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    Scene scene = dlg.scene();
    for (int i = 0; i < m_mgr.scenes().size(); ++i) {
        if (i != row && m_mgr.scenes()[i].name == scene.name) {
            QMessageBox::warning(this, "提示",
                QString("场景「%1」已存在，请使用其他名称").arg(scene.name));
            return;
        }
    }

    m_mgr.updateScene(row, scene);
    if (!m_mgr.save()) {
        QMessageBox::warning(this, "保存失败",
            QString("无法写入配置文件：\n%1").arg(m_mgr.configPath()));
        return;
    }
    refreshTable(scene.name);
    setStatus(QString("✅ 已保存场景「%1」").arg(scene.name));
}

void ScenePage::deleteScene()
{
    const int row = selectedRow();
    if (row < 0) return;

    const QString name = m_mgr.scenes()[row].name;
    if (QMessageBox::question(this, "删除场景",
            QString("确定删除场景「%1」？").arg(name))
        != QMessageBox::Yes)
        return;

    m_activeScenes.remove(name);
    m_mgr.removeScene(row);
    if (!m_mgr.save()) {
        QMessageBox::warning(this, "保存失败",
            QString("无法写入配置文件：\n%1").arg(m_mgr.configPath()));
        return;
    }
    refreshTable();
    setStatus(QString("🗑️ 已删除场景「%1」").arg(name));
}

// ── 时间设置 ──

QList<int> ScenePage::checkedDays() const
{
    QList<int> days;
    for (int i = 0; i < 7; ++i) {
        if (m_dayChecks[i]->isChecked())
            days.append(i + 1);
    }
    return days;
}

void ScenePage::applyScheduleControls(Scene &scene) const
{
    scene.enabled = m_enabledCheck->isChecked();
    scene.mode = m_manualCheck->isChecked() ? "manual" : "time";

    if (scene.mode == "manual")
        return;

    scene.startTime = m_startEdit->time();
    scene.endTime = m_endEdit->time();
    scene.days = m_everyDayCheck->isChecked() ? QList<int>() : checkedDays();
}

void ScenePage::loadScheduleControls()
{
    const int row = selectedRow();
    if (row < 0) {
        m_selectedLabel->setText("未选择场景");
        return;
    }

    const Scene &scene = m_mgr.scenes()[row];
    const bool manual = isManual(scene);

    m_selectedLabel->setText(manual
        ? QString("当前场景：%1（手动触发）").arg(scene.name)
        : QString("当前场景：%1").arg(scene.name));

    m_manualCheck->setChecked(manual);
    m_startEdit->setTime(scene.startTime);
    m_endEdit->setTime(scene.endTime);
    m_enabledCheck->setChecked(scene.enabled);

    const bool everyDay = scene.days.isEmpty() || scene.days.size() == 7;
    m_everyDayCheck->setChecked(everyDay);
    for (int i = 0; i < 7; ++i) {
        m_dayChecks[i]->setChecked(!everyDay && scene.days.contains(i + 1));
    }

    setTimeControlsEnabled(!manual);
}

void ScenePage::saveSchedule()
{
    const int row = selectedRow();
    if (row < 0) return;

    Scene scene = m_mgr.scenes()[row];

    if (!m_manualCheck->isChecked()) {
        const QList<int> days = checkedDays();
        if (!m_everyDayCheck->isChecked() && days.isEmpty()) {
            QMessageBox::warning(this, "提示",
                "请至少选择一个重复的星期，或勾选「每天」。");
            return;
        }
    }

    applyScheduleControls(scene);
    m_mgr.updateScene(row, scene);
    if (!m_mgr.save()) {
        QMessageBox::warning(this, "保存失败",
            QString("无法写入配置文件：\n%1").arg(m_mgr.configPath()));
        return;
    }

    refreshTable(scene.name);

    if (isManual(scene)) {
        setStatus(QString("✅ 已保存「%1」：手动触发场景").arg(scene.name));
    } else {
        setStatus(QString("✅ 已保存「%1」的时间设置：%2 → %3")
                      .arg(scene.name,
                           scene.startTime.toString("HH:mm"),
                           scene.endTime.toString("HH:mm")));
    }
}

void ScenePage::runNowStart()
{
    Scene *scene = selectedScene();
    if (!scene) return;

    if (m_manualCheck->isChecked()) {
        Logger::log(kSceneTag, QString("手动启动场景「%1」").arg(scene->name));
    }

    applyStart(*scene);
    updateStatusColumn();
}

void ScenePage::runNowStop()
{
    Scene *scene = selectedScene();
    if (!scene) return;

    if (m_manualCheck->isChecked()) {
        Logger::log(kSceneTag, QString("手动结束场景「%1」").arg(scene->name));
    }

    applyStop(*scene);
    updateStatusColumn();
}

// ── 定时调度 ──

bool ScenePage::isManual(const Scene &scene)
{
    return scene.mode == "manual";
}

bool ScenePage::inWindow(const Scene &scene, const QDateTime &now)
{
    if (isManual(scene))
        return false;

    const QTime start = scene.startTime;
    const QTime end = scene.endTime;
    if (!start.isValid() || !end.isValid())
        return false;

    const QTime t = now.time();
    const int today = now.date().dayOfWeek();
    const int yesterday = today == 1 ? 7 : today - 1;
    const bool everyDay = scene.days.isEmpty() || scene.days.size() == 7;

    // 开始与结束时间相同表示全天生效
    if (start == end)
        return everyDay || scene.days.contains(today);

    if (start < end) {
        if (t < start || t >= end)
            return false;
        return everyDay || scene.days.contains(today);
    }

    // 跨午夜窗口：结束时间属于开始日的次日
    if (t >= start)
        return everyDay || scene.days.contains(today);
    if (t < end)
        return everyDay || scene.days.contains(yesterday);
    return false;
}

void ScenePage::onTick()
{
    const QDateTime now = QDateTime::currentDateTime();
    bool changed = false;

    // 拷贝一份，避免启动虚机时 processEvents 导致配置在遍历中被修改
    const QVector<Scene> scenes = m_mgr.scenes();
    for (const auto &scene : scenes) {
        if (isManual(scene))
            continue;

        const bool wantsActive = scene.enabled && inWindow(scene, now);
        const bool wasActive = m_activeScenes.contains(scene.name);

        if (wantsActive && !wasActive) {
            m_activeScenes.insert(scene.name);
            Logger::log(kSceneTag, QString("场景「%1」进入时间窗口，执行启动动作")
                                      .arg(scene.name));
            applyStart(scene);
            changed = true;
        } else if (!wantsActive && wasActive) {
            m_activeScenes.remove(scene.name);
            Logger::log(kSceneTag, QString("场景「%1」离开时间窗口，执行停止动作")
                                      .arg(scene.name));
            applyStop(scene);
            changed = true;
        }
    }

    // 清理已删除或已改名的场景
    QSet<QString> names;
    for (const auto &scene : m_mgr.scenes())
        names.insert(scene.name);
    for (auto it = m_activeScenes.begin(); it != m_activeScenes.end();) {
        if (!names.contains(*it))
            it = m_activeScenes.erase(it);
        else
            ++it;
    }

    updateStatusColumn();
    if (changed)
        setStatus("⏱ 场景定时任务已执行");
}

// ── 执行动作 ──

void ScenePage::applyStart(const Scene &scene)
{
    Logger::log(kSceneTag,
        QString("▶ 场景「%1」启动：服务 %2 个，虚机 %3 台")
            .arg(scene.name)
            .arg(scene.startServices.size())
            .arg(scene.startVms.size()));

    runServiceActions(scene.name, scene.startServices, "start");

    if (m_vmPage) {
        for (const auto &vm : scene.startVms) {
            if (!m_vmPage->startVMByName(vm, true))
                Logger::log(kSceneTag,
                    QString("⚠️ 场景「%1」启动虚机失败：%2").arg(scene.name, vm));
        }
    }

    setStatus(QString("▶ 场景「%1」已执行启动动作").arg(scene.name));
}

void ScenePage::applyStop(const Scene &scene)
{
    Logger::log(kSceneTag,
        QString("⏹ 场景「%1」结束：服务 %2 个，虚机 %3 台")
            .arg(scene.name)
            .arg(scene.stopServices.size())
            .arg(scene.stopVms.size()));

    runServiceActions(scene.name, scene.stopServices, "stop");

    if (m_vmPage) {
        for (const auto &vm : scene.stopVms) {
            if (!m_vmPage->stopVMByName(vm, false, true))
                Logger::log(kSceneTag,
                    QString("⚠️ 场景「%1」停止虚机失败或未运行：%2").arg(scene.name, vm));
        }
    }

    setStatus(QString("⏹ 场景「%1」已执行结束动作").arg(scene.name));
}

void ScenePage::runServiceActions(const QString &sceneName,
                                  const QVector<SceneService> &services,
                                  const QString &action)
{
    if (services.isEmpty())
        return;

    QMap<QString, QStringList> groups;
    for (const auto &svc : services) {
        const QString scope = svc.scope == "user" ? "user" : "system";
        if (!groups[scope].contains(svc.name))
            groups[scope].append(svc.name);
    }

    for (auto it = groups.constBegin(); it != groups.constEnd(); ++it) {
        const QString scope = it.key();
        const QStringList names = it.value();
        if (names.isEmpty())
            continue;

        QString program;
        QStringList args;
        if (scope == "user") {
            program = "systemctl";
            args << "--user" << action << names;
        } else {
            program = "pkexec";
            args << "systemctl" << action << names;
        }

        Logger::log(kSceneTag,
            QString("场景「%1」%2 %3 服务：%4")
                .arg(sceneName, actionLabel(action), scope, names.join(", ")));

        auto *proc = new QProcess(this);
        connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [proc, sceneName, action, scope, names](int code,
                                                              QProcess::ExitStatus status) {
            const QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed();
            if (status == QProcess::NormalExit && code == 0) {
                Logger::log(kSceneTag,
                    QString("✅ 场景「%1」%2 %3 服务完成：%4")
                        .arg(sceneName, actionLabel(action), scope, names.join(", ")));
            } else {
                Logger::log(kSceneTag,
                    QString("❌ 场景「%1」%2 %3 服务失败（exit=%4）：%5")
                        .arg(sceneName, actionLabel(action), scope)
                        .arg(code)
                        .arg(err.left(300)));
            }
            proc->deleteLater();
        });
        proc->start(program, args);
    }
}

// ── 辅助 ──

QString ScenePage::daysText(const QList<int> &days)
{
    if (days.isEmpty() || days.size() == 7)
        return "每天";

    QList<int> sorted = days;
    std::sort(sorted.begin(), sorted.end());

    if (sorted.size() == 5 && sorted == QList<int>({1, 2, 3, 4, 5}))
        return "工作日";
    if (sorted.size() == 2 && sorted == QList<int>({6, 7}))
        return "周末";

    static const char *names[8] = {"", "周一", "周二", "周三", "周四",
                                   "周五", "周六", "周日"};
    QStringList parts;
    for (int day : sorted) {
        if (day >= 1 && day <= 7)
            parts << names[day];
    }
    return parts.join("、");
}

QString ScenePage::controlSummary(const Scene &scene)
{
    QStringList parts;
    if (!scene.startServices.isEmpty() || !scene.stopServices.isEmpty()) {
        parts << QString("服务：启 %1 / 停 %2")
                     .arg(scene.startServices.size())
                     .arg(scene.stopServices.size());
    }
    if (!scene.startVms.isEmpty() || !scene.stopVms.isEmpty()) {
        parts << QString("虚机：启 %1 / 停 %2")
                     .arg(scene.startVms.size())
                     .arg(scene.stopVms.size());
    }
    return parts.isEmpty() ? "未配置控制项" : parts.join("；");
}

QString ScenePage::controlDetails(const Scene &scene)
{
    QStringList details;
    for (const auto &svc : scene.startServices)
        details << QString("启动服务：%1（%2）").arg(svc.name, svc.scope);
    for (const auto &svc : scene.stopServices)
        details << QString("停止服务：%1（%2）").arg(svc.name, svc.scope);
    for (const auto &vm : scene.startVms)
        details << QString("启动虚机：%1").arg(vm);
    for (const auto &vm : scene.stopVms)
        details << QString("停止虚机：%1").arg(vm);
    return details.isEmpty() ? "未配置控制项" : details.join('\n');
}

void ScenePage::setStatus(const QString &text)
{
    m_statusLabel->setText(text);
}
