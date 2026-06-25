#ifndef STORAGEDEVICEPAGE_H
#define STORAGEDEVICEPAGE_H

#include <QWidget>
#include <QLabel>
#include <QTreeWidget>
#include <QPushButton>
#include <QProcess>

class StorageDevicePage : public QWidget
{
    Q_OBJECT

public:
    explicit StorageDevicePage(QWidget *parent = nullptr);

private slots:
    void refresh();
    void onSelectionChanged();
    void showSmart();
    void showMountDialog();
    void showFormatDialog();

private:
    void runCmd(const QString &cmd, const QStringList &args,
                std::function<void(const QString &)> cb);
    void buildTree(const QString &json);
    QTreeWidgetItem *addDevice(QTreeWidgetItem *parent,
                               const QString &name, const QString &size,
                               const QString &type, const QString &fstype,
                               const QString &mount);

    QTreeWidget *m_tree;
    QLabel *m_status;
    QPushButton *m_refreshBtn;
    QPushButton *m_smartBtn;
    QPushButton *m_mountBtn;
    QPushButton *m_formatBtn;
};

#endif // STORAGEDEVICEPAGE_H
