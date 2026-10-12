#include "healthmonitor.h"
#include "logger.h"

#include <QTimer>
#include <QTcpSocket>
#include <QAbstractSocket>
#include <QProcess>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QTextStream>

#include <memory>
#include <sstream>
#include <fstream>
#include <cstdio>

#include <unistd.h>

#include <toml++/toml.h>

static const char *kTag = "WATCHDOG";

HealthMonitor::HealthMonitor(const QString &configPath, QObject *parent)
    : QObject(parent)
    , m_configPath(configPath)
{
}

QString HealthMonitor::defaultConfigPath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return dir + "/health.toml";
}

QString HealthMonitor::serviceUnitPath()
{
    return "/etc/systemd/system/servertoolbox-watchdog.service";
}

QString HealthMonitor::buildUnitContent(const QString &binPath, const QString &configPath)
{
    QString unit;
    QTextStream out(&unit);
    out << "[Unit]\n"
        << "Description=ServerToolbox Health Watchdog\n"
        << "After=network-online.target\n"
        << "Wants=network-online.target\n"
        << "\n"
        << "[Service]\n"
        << "Type=simple\n"
        << "ExecStart=\"" << binPath << "\" --health-daemon --health-config \"" << configPath << "\"\n"
        << "Restart=always\n"
        << "RestartSec=5\n"
        << "\n"
        << "[Install]\n"
        << "WantedBy=multi-user.target\n";
    return unit;
}

QString HealthMonitor::configPath() const
{
    return m_configPath.isEmpty() ? defaultConfigPath() : m_configPath;
}

void HealthMonitor::start()
{
    loadConfig();

    if (!m_cfg.enabled) {
        log("健康监控已禁用（health.toml enabled=false）");
        return;
    }

    m_timer = new QTimer(this);
    m_timer->setInterval(m_cfg.intervalSec * 1000);
    connect(m_timer, &QTimer::timeout, this, &HealthMonitor::check);
    m_timer->start();

    log(QString("健康监控启动：nginx %1:%2，SSH %3:%4，间隔 %5s，超时 %6ms，阈值 %7")
                .arg(m_cfg.nginxHost).arg(m_cfg.nginxPort)
                .arg(m_cfg.sshHost).arg(m_cfg.sshPort)
                .arg(m_cfg.intervalSec).arg(m_cfg.timeoutMs).arg(m_cfg.failThreshold));

    check();
}

void HealthMonitor::loadConfig()
{
    const QString path = configPath();

    if (!QFile::exists(path)) {
        ensureConfigFile(path);
        log(QString("未找到配置文件，已生成默认配置：%1").arg(path));
        return;
    }

    try {
        auto tbl = toml::parse_file(path.toStdString());

        m_cfg.enabled = tbl["enabled"].value_or(true);
        m_cfg.intervalSec = static_cast<int>(tbl["interval_sec"].value_or(int64_t(30)));
        m_cfg.timeoutMs = static_cast<int>(tbl["timeout_ms"].value_or(int64_t(2000)));
        m_cfg.failThreshold = static_cast<int>(tbl["fail_threshold"].value_or(int64_t(3)));
        m_cfg.cooldownSec = static_cast<int>(tbl["cooldown_sec"].value_or(int64_t(300)));

        if (auto *n = tbl["nginx"].as_table()) {
            m_cfg.nginxHost = QString::fromStdString((*n)["host"].value_or(std::string("127.0.0.1")));
            m_cfg.nginxPort = static_cast<int>((*n)["port"].value_or(int64_t(80)));
        }
        if (auto *s = tbl["ssh"].as_table()) {
            m_cfg.sshHost = QString::fromStdString((*s)["host"].value_or(std::string("127.0.0.1")));
            m_cfg.sshPort = static_cast<int>((*s)["port"].value_or(int64_t(22)));
        }
    } catch (const toml::parse_error &) {
        log("health.toml 解析失败，使用默认配置");
    }

    if (m_cfg.intervalSec < 1) m_cfg.intervalSec = 1;
    if (m_cfg.timeoutMs < 100) m_cfg.timeoutMs = 100;
    if (m_cfg.failThreshold < 1) m_cfg.failThreshold = 1;
}

QString HealthMonitor::defaultConfigText()
{
    toml::table nginx;
    nginx.emplace("host", std::string("127.0.0.1"));
    nginx.emplace("port", int64_t(80));

    toml::table ssh;
    ssh.emplace("host", std::string("127.0.0.1"));
    ssh.emplace("port", int64_t(22));

    toml::table root;
    root.emplace("enabled", true);
    root.emplace("interval_sec", int64_t(30));
    root.emplace("timeout_ms", int64_t(2000));
    root.emplace("fail_threshold", int64_t(3));
    root.emplace("cooldown_sec", int64_t(300));
    root.emplace("nginx", std::move(nginx));
    root.emplace("ssh", std::move(ssh));

    std::ostringstream oss;
    oss << root << "\n";
    return QString::fromStdString(oss.str());
}

void HealthMonitor::ensureConfigFile(const QString &path)
{
    if (QFile::exists(path))
        return;

    QDir().mkpath(QFileInfo(path).absolutePath());

    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text))
        f.write(defaultConfigText().toUtf8());
}

void HealthMonitor::configPorts(const QString &path, int *nginxPort, int *sshPort)
{
    int np = 80;
    int sp = 22;

    if (QFile::exists(path)) {
        try {
            auto tbl = toml::parse_file(path.toStdString());
            if (auto *n = tbl["nginx"].as_table())
                np = static_cast<int>((*n)["port"].value_or(int64_t(80)));
            if (auto *s = tbl["ssh"].as_table())
                sp = static_cast<int>((*s)["port"].value_or(int64_t(22)));
        } catch (const toml::parse_error &) {
        }
    }

    if (nginxPort) *nginxPort = (np >= 1 && np <= 65535) ? np : 80;
    if (sshPort) *sshPort = (sp >= 1 && sp <= 65535) ? sp : 22;
}

bool HealthMonitor::setConfigPorts(const QString &path, int nginxPort, int sshPort)
{
    ensureConfigFile(path);

    toml::table root;
    try {
        root = toml::parse_file(path.toStdString());
    } catch (const toml::parse_error &) {
        return false;
    }

    auto setPort = [&root](const char *key, int port) {
        if (auto *t = root[key].as_table()) {
            t->insert_or_assign("port", int64_t(port));
        } else {
            toml::table tbl;
            tbl.emplace("host", std::string("127.0.0.1"));
            tbl.emplace("port", int64_t(port));
            root.insert_or_assign(key, std::move(tbl));
        }
    };

    setPort("nginx", nginxPort);
    setPort("ssh", sshPort);

    std::ofstream ofs(path.toStdString());
    if (!ofs)
        return false;
    ofs << root << "\n";
    return true;
}

void HealthMonitor::log(const QString &message)
{
    Logger::log(QString::fromLatin1(kTag), message);
    fprintf(stderr, "[%s] %s\n", kTag, message.toUtf8().constData());
    fflush(stderr);
}

void HealthMonitor::check()
{
    if (m_busy)
        return;

    m_busy = true;
    probe(m_cfg.nginxHost, m_cfg.nginxPort, [this](Probe p) { onNginxResult(p); });
}

void HealthMonitor::probe(const QString &host, int port, std::function<void(Probe)> cb)
{
    auto *sock = new QTcpSocket(this);
    auto *timer = new QTimer(sock);
    timer->setSingleShot(true);

    auto settled = std::make_shared<bool>(false);
    auto deliver = [sock, cb = std::move(cb), settled](Probe p) {
        if (*settled)
            return;
        *settled = true;
        sock->abort();
        sock->deleteLater();
        cb(p);
    };

    connect(sock, &QTcpSocket::connected, this, [deliver]() { deliver(Probe::Ok); });
    connect(sock, &QTcpSocket::errorOccurred, this,
            [deliver](QAbstractSocket::SocketError e) {
                switch (e) {
                case QAbstractSocket::ConnectionRefusedError:
                    deliver(Probe::Refused);
                    break;
                case QAbstractSocket::RemoteHostClosedError:
                    deliver(Probe::ClosedEarly);
                    break;
                case QAbstractSocket::SocketTimeoutError:
                    deliver(Probe::Timeout);
                    break;
                default:
                    deliver(Probe::Other);
                    break;
                }
            });
    connect(timer, &QTimer::timeout, this, [deliver]() { deliver(Probe::Timeout); });

    timer->start(m_cfg.timeoutMs);
    sock->connectToHost(host, static_cast<quint16>(port));
}

void HealthMonitor::onNginxResult(Probe nginx)
{
    if (!isAbnormal(nginx)) {
        const QDateTime now = QDateTime::currentDateTime();
        const bool due = !m_lastHealthyLog.isValid()
                         || m_lastHealthyLog.secsTo(now) >= 60;
        if (due) {
            if (nginx == Probe::Ok)
                log(QString("nginx %1:%2 正常").arg(m_cfg.nginxHost).arg(m_cfg.nginxPort));
            else
                log(QString("nginx 探测：%1（正常报错，忽略）").arg(probeName(nginx)));
            m_lastHealthyLog = now;
        }
        if (m_failStreak != 0) {
            log("nginx 恢复正常，失败计数清零");
            m_failStreak = 0;
        }
        m_busy = false;
        return;
    }

    log(QString("nginx 异常：%1，立即检测 SSH ...").arg(probeName(nginx)));
    probe(m_cfg.sshHost, m_cfg.sshPort, [this, nginx](Probe ssh) { onSshResult(nginx, ssh); });
}

void HealthMonitor::onSshResult(Probe nginx, Probe ssh)
{
    m_busy = false;

    if (isAbnormal(ssh)) {
        m_failStreak++;
        log(QString("SSH 亦异常：%1（连续 %2/%3）")
                    .arg(probeName(ssh)).arg(m_failStreak).arg(m_cfg.failThreshold));

        if (m_failStreak >= m_cfg.failThreshold)
            triggerReboot(QString("nginx(%1) 与 SSH(%2) 同时异常")
                          .arg(probeName(nginx), probeName(ssh)));
    } else {
        log(QString("nginx 异常但 SSH 正常（%1），系统存活，不重启")
                    .arg(probeName(ssh)));
        m_failStreak = 0;
    }
}

void HealthMonitor::triggerReboot(const QString &reason)
{
    if (m_rebooting)
        return;

    if (m_lastReboot.isValid()
        && m_lastReboot.secsTo(QDateTime::currentDateTime()) < m_cfg.cooldownSec) {
        log("处于重启冷却期，暂不触发");
        return;
    }

    m_rebooting = true;
    m_lastReboot = QDateTime::currentDateTime();
    m_failStreak = 0;

    log(QString("触发自动重启：%1").arg(reason));
    if (::geteuid() == 0)
        QProcess::startDetached("systemctl", {"reboot"});
    else
        QProcess::startDetached("pkexec", {"reboot"});

    QTimer::singleShot(10000, this, [this]() { m_rebooting = false; });
}

bool HealthMonitor::isAbnormal(Probe p)
{
    return p == Probe::Refused || p == Probe::ClosedEarly;
}

QString HealthMonitor::probeName(Probe p)
{
    switch (p) {
    case Probe::Ok:          return "连接成功";
    case Probe::Refused:     return "拒绝连接(ECONNREFUSED)";
    case Probe::ClosedEarly: return "提前断开(E_CLOSED_EARLY)";
    case Probe::Timeout:     return "超时";
    default:                 return "其它错误";
    }
}
