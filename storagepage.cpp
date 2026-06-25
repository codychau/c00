#include "storagepage.h"
#include "storagedevicepage.h"
#include "mountpage.h"

#include <QGroupBox>
#include <QVBoxLayout>
#include <QScrollArea>

StoragePage::StoragePage(QWidget *parent)
    : QWidget(parent)
{
    // 外层 scroll area：两个 section 可能超出高度
    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *container = new QWidget();
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    // ── 存储设备管理 ──
    auto *deviceGroup = new QGroupBox("存储设备管理");
    auto *deviceLayout = new QVBoxLayout(deviceGroup);
    m_devicePage = new StorageDevicePage(deviceGroup);
    deviceLayout->addWidget(m_devicePage);
    layout->addWidget(deviceGroup);

    // ── 挂载管理 ──
    auto *mountGroup = new QGroupBox("挂载管理");
    auto *mountLayout = new QVBoxLayout(mountGroup);
    m_mountPage = new MountPage(mountGroup);
    mountLayout->addWidget(m_mountPage);
    layout->addWidget(mountGroup);

    layout->addStretch();
    scroll->setWidget(container);
    outerLayout->addWidget(scroll);
}
