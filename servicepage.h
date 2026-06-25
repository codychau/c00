#ifndef SERVICEPAGE_H
#define SERVICEPAGE_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QProcess>
#include <QStringList>

class ServicePage : public QWidget
{
    Q_OBJECT

public:
    explicit ServicePage(QWidget *parent = nullptr);

private slots:
    void refresh();
    void onStartStop();

private:
    void runCmd(const QString &cmd, const QStringList &args,
                std::function<void(const QString &)> cb);

    struct ServiceInfo {
        QString name;
        QString status; // active / inactive / failed
    };

    void parseAndUpdate(const QString &output);

    QTableWidget *m_table;
    QLabel *m_status;
    QPushButton *m_refreshBtn;
    QPushButton *m_startStopBtn;
    QLabel *m_selectedLabel;
    QString m_selectedName;
    QString m_selectedStatus;
};

#endif // SERVICEPAGE_H
