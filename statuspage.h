#ifndef STATUSPAGE_H
#define STATUSPAGE_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QProcess>
#include <QHash>
#include <functional>

class QShowEvent;

class StatusPage : public QWidget
{
    Q_OBJECT

public:
    explicit StatusPage(QWidget *parent = nullptr);

private slots:
    void refresh();
    void fetchAllTemps();

protected:
    void showEvent(QShowEvent *event) override;

private:
    void runCmd(const QString &cmd, const QStringList &args,
                std::function<void(const QString &)> cb);
    void setLine(int row, const QString &val, const QString &suffix = QString());

    // smartctl 免密权限（借鉴 AI 方案：sudo -n smartctl）
    bool hasPasswordlessSmartctl() const;          // 检测 sudo -n smartctl 是否可用
    bool setupSmartctlSudo(const QString &password); // 一次性写入 /etc/sudoers.d/smartctl
    void fetchDiskTemp(const QString &devName, int tableRow);

    // 系统信息标签（12 个）
    QLabel *m_labels[12];
    // 磁盘监控表格
    QTableWidget *m_diskTable;
    QLabel *m_diskStatus;
    QPushButton *m_tempBtn;
    QPushButton *m_shutdownBtn;
    QPushButton *m_restartBtn;
    QTimer *m_timer;
    bool m_disksInited = false;
    bool m_hasShown = false;
    bool m_smartSetupAsked = false; // 每会话只询问一次
    bool m_smartSetupDone = false;  // 免密已配置成功
    QHash<QString, QString> m_tempCache; // 设备 -> 上次温度，供刷新时保留
};

#endif // STATUSPAGE_H
