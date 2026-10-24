#ifndef HEALTHMONITOR_H
#define HEALTHMONITOR_H

#include <QObject>
#include <QString>
#include <QDateTime>
#include <QDate>
#include <QTime>
#include <QVector>
#include <functional>

class QTimer;

class HealthMonitor : public QObject
{
    Q_OBJECT

public:
    struct Config {
        bool enabled = true;
        int intervalSec = 30;
        int timeoutMs = 2000;
        int failThreshold = 3;
        int cooldownSec = 300;
        bool forceKernelRebootMode = false;

        // 机器中途休息：每周指定星期几的某个时间段内自动优雅重启。
        // rest_days 为 ISO 星期（1=周一 … 7=周日）；rest_last_run 记录
        // 最近一次执行的"锚定日期"(yyyy-MM-dd)，避免重启后在窗口内循环触发。
        bool restEnabled = false;
        QVector<int> restDays;
        QString restStart = "03:00";   // HH:mm
        QString restEnd = "04:00";     // HH:mm；早于 restStart 表示跨午夜
        QString restLastRun;           // yyyy-MM-dd

        QString nginxHost = "127.0.0.1";
        int nginxPort = 80;
        QString sshHost = "127.0.0.1";
        int sshPort = 22;
    };

    explicit HealthMonitor(const QString &configPath = QString(), QObject *parent = nullptr);

    void start();

    static QString defaultConfigPath();
    static QString serviceUnitPath();
    static QString buildUnitContent(const QString &binPath, const QString &configPath);
    static QString defaultConfigText();
    static void ensureConfigFile(const QString &path);
    static void configPorts(const QString &path, int *nginxPort, int *sshPort);
    static bool configForceKernelReboot(const QString &path);
    static bool setConfigPorts(const QString &path, int nginxPort, int sshPort,
                               bool forceKernelRebootMode = false);

    // 机器中途休息窗口（每周定时优雅重启）
    static void configRestWindow(const QString &path, bool *enabled,
                                 QVector<int> *days, QString *start, QString *end);
    static bool setConfigRestWindow(const QString &path, bool enabled,
                                    const QVector<int> &days,
                                    const QString &start, const QString &end);
    static bool setConfigRestLastRun(const QString &path, const QString &date);
    static QString restDaysText(const QVector<int> &days);

    // 内核级死机检测：写入 /etc/sysctl.d drop-in（可一键移除），并可选装载 kdump。
    static QString kernelConfigPath();
    static QString buildKernelConfigText(bool hungTaskPanic);
    static QString buildKernelEnableScript(bool enableKdump, bool hungTaskPanic);
    static QString buildKernelDisableScript();

    bool forceKernelReboot();
    void performReboot(bool forceKernel);

private:
    // BadBanner 仅 SSH 探测会产生：TCP 已连接但未收到合法的 SSH 协议标识串。
    enum class Probe { Ok, Refused, ClosedEarly, Timeout, BadBanner, Other };

    QString configPath() const;
    void log(const QString &message);
    void loadConfig();
    void check();
    void checkRestWindow();
    static QTime parseClock(const QString &s);
    static QString formatClock(const QTime &t);
    // expectSshBanner=true 时，仅当收到以 "SSH-" 开头的协议标识串才算 Ok；
    // 仅 TCP 连接成功不足以证明 sshd 可用（守护进程卡死时内核仍可能完成握手）。
    void probe(const QString &host, int port, bool expectSshBanner,
               std::function<void(Probe)> cb);
    void onNginxResult(Probe nginx);
    void onSshResult(Probe nginx, Probe ssh);
    void triggerReboot(const QString &reason);

    static bool isAbnormal(Probe p);
    static QString probeName(Probe p);

    Config m_cfg;
    QString m_configPath;
    QTimer *m_timer = nullptr;
    int m_failStreak = 0;
    QDateTime m_lastHealthyLog;
    QDateTime m_lastReboot;
    bool m_busy = false;
    bool m_rebooting = false;
};

#endif // HEALTHMONITOR_H
