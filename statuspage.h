#ifndef STATUSPAGE_H
#define STATUSPAGE_H

#include <QWidget>
#include <QLabel>
#include <QTableWidget>
#include <QTimer>
#include <QProcess>

class StatusPage : public QWidget
{
    Q_OBJECT

public:
    explicit StatusPage(QWidget *parent = nullptr);

private slots:
    void refresh();

private:
    void runCmd(const QString &cmd, const QStringList &args,
                std::function<void(const QString &)> cb);
    void setLine(int row, const QString &val, const QString &suffix = QString());

    // 系统信息标签（12 个）
    QLabel *m_labels[12];
    // 磁盘监控表格
    QTableWidget *m_diskTable;
    QLabel *m_diskStatus;
    QTimer *m_timer;
    bool m_disksInited = false;
};

#endif // STATUSPAGE_H
