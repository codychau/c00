#ifndef VMPAGE_H
#define VMPAGE_H

#include <QWidget>
#include <QLabel>
#include <QTableWidget>
#include <QPushButton>
#include <QTimer>
#include <QMap>
#include <QProcess>

#include "vmconfig.h"

class VMPage : public QWidget
{
    Q_OBJECT

public:
    explicit VMPage(QWidget *parent = nullptr);
    ~VMPage() override = default;

    // 供场景定时任务等外部调用（不弹交互对话框）
    bool startVMByName(const QString &name, bool quiet = true);
    bool stopVMByName(const QString &name, bool force = true, bool quiet = true);
    bool isVMRunning(const QString &name) const;

private slots:
    void addVM();
    void editVM(int row);
    void deleteVM();
    void startVM();
    void stopVM();
    void connectVNC();
    void refreshTable();
    void refreshStatus();
    void startAutoStartVMs();
    void onSelectionChanged();

private:
    bool startVMAt(int row, bool quiet);
    bool stopVMAt(int row, bool force, bool quiet);
    void saveConfig();
    void updateStartButton();
    void sendQemuPowerdown(const QString &vmName);
    QString findVNCViewer() const;
    QString qmpSocketPath(const QString &vmName) const;
    bool checkPermissionError(const QString &stderr);
    void configureQemuSudoers(const QString &vmName);

    VMConfigManager m_mgr;
    QTableWidget   *m_table;
    QLabel         *m_status;
    QPushButton    *m_addBtn;
    QPushButton    *m_editBtn;
    QPushButton    *m_deleteBtn;
    QPushButton    *m_startBtn;
    QPushButton    *m_vncBtn;
    QTimer         *m_statusTimer;
    QString         m_vncViewer;

    // 跟踪运行的虚机进程，用于停止操作
    QMap<QString, QProcess*> m_runningProcs;
};

#endif // VMPAGE_H
