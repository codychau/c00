#ifndef STORAGEDEVICEPAGE_H
#define STORAGEDEVICEPAGE_H

#include <QWidget>
#include <QLabel>
#include <QProgressBar>
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

signals:
    // 向父页面（StoragePage）透传格式化状态
    void formatProgress(int percent, const QString &status);
    void formatFinished(bool success);

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
    void onFormatProgress(int percent, const QString &status);

    QTreeWidget *m_tree;
    QLabel *m_status;

    // 后台格式化状态栏（控件在 StoragePage，这里只保留引用以更新按钮状态）
    QPushButton *m_refreshBtn;
    QPushButton *m_smartBtn;
    QPushButton *m_mountBtn;
    QPushButton *m_formatBtn;

    FormatDialog *m_formatDlg = nullptr;
    bool m_formatRunning = false;
};

#endif // STORAGEDEVICEPAGE_H
