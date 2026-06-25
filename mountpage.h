#ifndef MOUNTPAGE_H
#define MOUNTPAGE_H

#include <QWidget>
#include <QLabel>
#include <QTableWidget>
#include <QPushButton>
#include <QProcess>
#include <QMessageBox>

class MountPage : public QWidget
{
    Q_OBJECT

public:
    explicit MountPage(QWidget *parent = nullptr);

private slots:
    void refresh();
    void unmount();
    void onSelectionChanged();

private:
    void runCmd(const QString &cmd, const QStringList &args,
                std::function<void(const QString &)> cb);

    QTableWidget *m_table;
    QLabel *m_status;
    QPushButton *m_refreshBtn;
    QPushButton *m_unmountBtn;
};

#endif // MOUNTPAGE_H
