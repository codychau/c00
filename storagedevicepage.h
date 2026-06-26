#ifndef STORAGEDEVICEPAGE_H
#define STORAGEDEVICEPAGE_H

#include <QWidget>
#include <QLabel>
#include <QTreeWidget>
#include <QPushButton>
#include <QProcess>

class FormatDialog;

class StorageDevicePage : public QWidget
{
    Q_OBJECT

public:
    explicit StorageDevicePage(QWidget *parent = nullptr);
    ~StorageDevicePage() override;

private slots:
    void refresh();
    void onSelectionChanged();
    void showSmart();
    void showMountDialog();
    void showFormatDialog();
    void onFormatFinished(bool success);

private:
    void runCmd(const QString &cmd, const QStringList &args,
                std::function<void(const QString &)> cb);
    void buildTree(const QString &json);
    QTreeWidgetItem *addDevice(QTreeWidgetItem *parent,
                               const QString &name, const QString &size,
                               const QString &type, const QString &fstype,
                               const QString &mount);
    void setFormatRunning(bool running);

    QTreeWidget *m_tree;
    QLabel *m_status;
    QLabel *m_formatBanner;       // 后台格式化时的提示条
    QPushButton *m_refreshBtn;
    QPushButton *m_smartBtn;
    QPushButton *m_mountBtn;
    QPushButton *m_formatBtn;

    FormatDialog *m_formatDlg = nullptr;
    bool m_formatRunning = false;
};

#endif // STORAGEDEVICEPAGE_H
