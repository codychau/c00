#ifndef SCENEPAGE_H
#define SCENEPAGE_H

#include <QWidget>
#include <QDateTime>
#include <QSet>

#include "sceneconfig.h"

class VMPage;
class QTableWidget;
class QLabel;
class QPushButton;
class QTimeEdit;
class QCheckBox;
class QGroupBox;
class QTimer;

// 场景设定：上半部分维护场景，下半部分设置开始 / 结束时间
class ScenePage : public QWidget
{
    Q_OBJECT

public:
    explicit ScenePage(VMPage *vmPage, QWidget *parent = nullptr);

private slots:
    void addScene();
    void editScene();
    void deleteScene();
    void onSelectionChanged();
    void saveSchedule();
    void runNowStart();
    void runNowStop();
    void onTick();

private:
    void refreshTable(const QString &selectName = QString());
    void loadScheduleControls();
    void setScheduleControlsEnabled(bool enabled);
    void setTimeControlsEnabled(bool enabled);
    void updateStatusColumn();
    void applyStart(const Scene &scene);
    void applyStop(const Scene &scene);
    void runServiceActions(const QString &sceneName,
                           const QVector<SceneService> &services,
                           const QString &action);
    void setStatus(const QString &text);

    int selectedRow() const;
    Scene *selectedScene();
    QList<int> checkedDays() const;
    void applyScheduleControls(Scene &scene) const;

    static bool isManual(const Scene &scene);
    static bool inWindow(const Scene &scene, const QDateTime &now);
    static QString daysText(const QList<int> &days);
    static QString controlSummary(const Scene &scene);
    static QString controlDetails(const Scene &scene);

    SceneConfigManager m_mgr;
    VMPage *m_vmPage;

    QTableWidget *m_table;
    QPushButton  *m_addBtn;
    QPushButton  *m_editBtn;
    QPushButton  *m_deleteBtn;
    QLabel       *m_statusLabel;

    QGroupBox    *m_timeBox;
    QLabel       *m_selectedLabel;
    QTimeEdit    *m_startEdit;
    QTimeEdit    *m_endEdit;
    QCheckBox    *m_manualCheck;
    QCheckBox    *m_enabledCheck;
    QCheckBox    *m_everyDayCheck;
    QCheckBox    *m_dayChecks[7];
    QPushButton  *m_saveBtn;
    QPushButton  *m_startNowBtn;
    QPushButton  *m_stopNowBtn;

    QTimer *m_timer;
    QSet<QString> m_activeScenes;
};

#endif // SCENEPAGE_H
