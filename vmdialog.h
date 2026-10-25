#ifndef VMDIALOG_H
#define VMDIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QListWidget>
#include <QCheckBox>
#include <QTableWidget>
#include <QGroupBox>

#include "vmconfig.h"

class VMDialog : public QDialog
{
    Q_OBJECT

public:
    explicit VMDialog(QWidget *parent = nullptr);

    void setVMConfig(const VMConfig &vm);
    VMConfig vmConfig() const;

private:
    // 左列基本参数
    QLineEdit      *m_nameEdit;
    QSpinBox       *m_cpuSpin;
    QSpinBox       *m_memSpin;
    QLineEdit      *m_isoEdit;
    QPushButton    *m_isoBtn;

    // 右列高级参数
    QComboBox      *m_qemuCombo;
    QComboBox      *m_machineCombo;
    QComboBox      *m_cpuTypeCombo;
    QCheckBox      *m_kvmCb;
    QComboBox      *m_vgaCombo;
    QComboBox      *m_netCombo;
    QComboBox      *m_nicCombo;
    QSpinBox       *m_vncSpin;
    QCheckBox      *m_ramfbCb;

    // 端口转发表
    QTableWidget   *m_portTable;
    QGroupBox      *m_portGroup;

    // 磁盘列表 (系统盘 + 数据盘)
    QListWidget    *m_diskList;

    // 自动启动
    QCheckBox      *m_autoStartCb;

    // 等待挂载
    QCheckBox      *m_waitMountCb;

    // 硬件直通
    QListWidget    *m_pciList;
    QCheckBox      *m_hugepagesCb;

    // 网络设置 (QEMU / VirtualBox 各一页)
    QGroupBox      *m_netGroup;
    QWidget        *m_netPageQemu;
    QWidget        *m_netPageVbox;
    QComboBox      *m_vboxNicTypeCombo;
    QComboBox      *m_vboxNicModelCombo;
    QLineEdit      *m_vboxBridgeEdit;

    // 虚拟机类型 / VirtualBox 专属控件
    QComboBox      *m_vmTypeCombo;
    QGroupBox      *m_vboxGroup;
    QComboBox      *m_ostypeCombo;
    QComboBox      *m_storageBackendCombo;
    QSpinBox       *m_vboxDiskSizeSpin;
    QCheckBox      *m_vboxAcpiCb;
    QCheckBox      *m_vboxIoapicCb;
    QCheckBox      *m_vboxUefiCb;
    QCheckBox      *m_vboxRdpCb;
    QSpinBox       *m_vboxRdpPortSpin;

    void updateVmTypeUI();  // 按虚拟机类型显示/隐藏对应控件

    // 额外参数
    QPlainTextEdit *m_extraEdit;
    QGroupBox      *m_extraGroup;
    QGroupBox      *m_advGroup;

    // 按钮
    QPushButton    *m_portAddBtn;
    QPushButton    *m_portEditBtn;
    QPushButton    *m_portRemoveBtn;
    QPushButton    *m_diskAddBtn;
    QPushButton    *m_diskEditBtn;
    QPushButton    *m_diskRemoveBtn;
    QPushButton    *m_pciAddBtn;
    QPushButton    *m_pciRemoveBtn;
};

#endif // VMDIALOG_H
