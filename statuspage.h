#ifndef STATUSPAGE_H
#define STATUSPAGE_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QProcess>
#include <QHash>
#include <QList>
#include <functional>

class QShowEvent;

class StatusPage : public QWidget
{
    Q_OBJECT

public:
    explicit StatusPage(QWidget *parent = nullptr);

private slots:
    void refresh();
    void fetchAllTemps();
    void onGpuContextMenu(const QPoint &pos);

protected:
    void showEvent(QShowEvent *event) override;

private:
    void runCmd(const QString &cmd, const QStringList &args,
                std::function<void(const QString &)> cb);
    void setLine(int row, const QString &val, const QString &suffix = QString());

    // smartctl 免密权限（借鉴 AI 方案：sudo -n smartctl）
    bool hasPasswordlessSmartctl() const;          // 检测 sudo -n smartctl 是否可用
    bool setupSmartctlSudo(const QString &password); // 一次性写入 /etc/sudoers.d/smartctl
    void fetchDiskTemp(const QString &devName, int tableRow);

    // GPU 温度自动检测（列出 GPU0..GPUx，含型号与温度）
    void fetchGpuTemps();
    struct GpuInfo {
        QString slot;      // PCI 槽位，如 "03:00.0"
        QString model;
        QString temp;
        QString sclk;
        QString fan;    // 风扇转速 RPM（无风扇则为空）
        QString vram;   // 显存占用，如 "19.58 / 31.86 GiB"
        bool canLimit = false;  // 是否支持修改核心频率
        bool hasOd = false;     // 是否有 pp_od_clk_voltage（可设任意上限）
        QString devPath;        // /sys/class/drm/cardN/device 路径
        QStringList limitBands; // 档位行，如 "2: 2350Mhz"（含索引，写入 pp_dpm_sclk 用）
        QString stockMax;       // 出厂最高档（如 2350Mhz）
    };
    void readGpuTemperature(GpuInfo &g);          // 按 PCI 槽位读 hwmon 温度
    void checkGpuTools(GpuInfo &g);               // 检测能否限频并收集档位
    void limitGpuFreq(const GpuInfo &g, const QString &maxMhz);  // 写入频率上限
    void resetGpuFreq(const GpuInfo &g);          // 恢复默认（不限制）

    // 系统信息标签（常量先声明，供数组使用）
    static constexpr int kLineCount = 12;
    QLabel *m_labels[kLineCount];
    // 磁盘监控表格
    QTableWidget *m_diskTable;
    QLabel *m_diskStatus;
    QPushButton *m_tempBtn;
    QPushButton *m_shutdownBtn;
    QPushButton *m_restartBtn;
    // GPU 温度表格（GPU0..GPUx）
    QTableWidget *m_gpuTable;
    QList<GpuInfo> m_gpus; // 当前检测到的 GPU（右键菜单使用）
    QTimer *m_timer;
    bool m_disksInited = false;
    bool m_hasShown = false;
    bool m_smartSetupAsked = false; // 每会话只询问一次
    bool m_smartSetupDone = false;  // 免密已配置成功
    QHash<QString, QString> m_tempCache; // 设备 -> 上次温度，供刷新时保留
};

#endif // STATUSPAGE_H
