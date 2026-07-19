#ifndef SERVICEPAGE_H
#define SERVICEPAGE_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QProcess>
#include <QStringList>
#include <QLineEdit>
#include <QVector>

class QShowEvent;

class ServicePage : public QWidget
{
    Q_OBJECT

public:
    explicit ServicePage(QWidget *parent = nullptr);

private slots:
    void refresh();
    void onStartStop();
    void filterServices(const QString &text);

protected:
    void showEvent(QShowEvent *event) override;

private:
    void runCmd(const QString &cmd, const QStringList &args,
                std::function<void(const QString &)> cb);
    void populateTable();

    struct ServiceEntry {
        QString name;
        QString active;
        QString sub;
        QString scope;
        QString enabled;
        QString description;
    };

    QVector<ServiceEntry> m_allServices;

    QLineEdit *m_searchBox;
    QTableWidget *m_table;
    QLabel *m_status;
    QPushButton *m_refreshBtn;
    QPushButton *m_startStopBtn;
    QLabel *m_selectedLabel;
    QString m_selectedName;
    QString m_selectedStatus;
};

#endif // SERVICEPAGE_H
