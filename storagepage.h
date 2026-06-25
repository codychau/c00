#ifndef STORAGEPAGE_H
#define STORAGEPAGE_H

#include <QWidget>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QScrollArea>

class StorageDevicePage;
class MountPage;

class StoragePage : public QWidget
{
    Q_OBJECT

public:
    explicit StoragePage(QWidget *parent = nullptr);

private:
    StorageDevicePage *m_devicePage;
    MountPage *m_mountPage;
};

#endif // STORAGEPAGE_H
