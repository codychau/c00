#ifndef STORAGEPAGE_H
#define STORAGEPAGE_H

#include <QWidget>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>

class StorageDevicePage;
class MountPage;
class FormatDialog;

class StoragePage : public QWidget
{
    Q_OBJECT

public:
    explicit StoragePage(QWidget *parent = nullptr);

private slots:
    void onFormatProgress(int percent, const QString &status);
    void onFormatFinished(bool success);

private:
    StorageDevicePage *m_devicePage;
    MountPage *m_mountPage;

    // 底部全局状态栏
    QWidget     *m_statusBar;
    QLabel      *m_statusIcon;
    QProgressBar *m_progressBar;
    QPushButton *m_restoreBtn;
};

#endif // STORAGEPAGE_H
