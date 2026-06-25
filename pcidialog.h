#ifndef PCIDIALOG_H
#define PCIDIALOG_H

#include <QDialog>
#include <QTableWidget>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QVector>

// 单个 PCI 设备信息
struct PCIDeviceInfo {
    QString bdf;         // 0000:01:00.0
    QString desc;        // 描述
    QString driver;      // 内核驱动 (空 = 无驱动)
    bool selected = false;
};

class PCIDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PCIDialog(QWidget *parent = nullptr);

    QStringList selectedBDFs() const;
    // 预勾选已有 BDF
    void selectBDF(const QString &bdf);

private slots:
    void applyFilter(const QString &text);
    void toggleRow(int row, int col);
    void updateStatus();

private:
    void loadPCIDevices();
    void populateTable();

    QVector<PCIDeviceInfo> m_allDevices;   // 全量
    QVector<PCIDeviceInfo> m_filtered;     // 当前过滤

    QTableWidget *m_table;
    QLineEdit    *m_searchEdit;
    QLabel       *m_statusLabel;
};

#endif // PCIDIALOG_H
