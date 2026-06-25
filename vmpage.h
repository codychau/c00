#ifndef VMPAGE_H
#define VMPAGE_H

#include <QWidget>
#include <QLabel>
#include <QTableWidget>
#include <QPushButton>
#include <QTimer>

#include "vmconfig.h"

class VMPage : public QWidget
{
    Q_OBJECT

public:
    explicit VMPage(QWidget *parent = nullptr);
    ~VMPage() override = default;

private slots:
    void addVM();
    void editVM(int row);
    void deleteVM();
    void startVM();
    void connectVNC();
    void refreshTable();
    void refreshStatus();
    void startAutoStartVMs();

private:
    void saveConfig();
    QString findVNCViewer() const;

    VMConfigManager m_mgr;
    QTableWidget   *m_table;
    QLabel         *m_status;
    QPushButton    *m_addBtn;
    QPushButton    *m_editBtn;
    QPushButton    *m_deleteBtn;
    QPushButton    *m_startBtn;
    QPushButton    *m_vncBtn;
    QTimer         *m_statusTimer;
    QString         m_vncViewer;   // 检测到的 VNC 客户端路径
};

#endif // VMPAGE_H
