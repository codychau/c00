#ifndef HEALTHMONITOR_H
#define HEALTHMONITOR_H

#include <QObject>
#include <QString>
#include <QDateTime>
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
    static bool setConfigPorts(const QString &path, int nginxPort, int sshPort);

private:
    enum class Probe { Ok, Refused, ClosedEarly, Timeout, Other };

    QString configPath() const;
    void log(const QString &message);
    void loadConfig();
    void check();
    void probe(const QString &host, int port, std::function<void(Probe)> cb);
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
