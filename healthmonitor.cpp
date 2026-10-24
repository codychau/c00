#include "healthmonitor.h"
#include "logger.h"

#include <QTimer>
#include <QTcpSocket>
#include <QByteArray>
#include <QAbstractSocket>
#include <QProcess>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QTextStream>

#include <algorithm>
#include <memory>
#include <sstream>
#include <fstream>
#include <cstdio>

#include <unistd.h>
#include <fcntl.h>

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

    if (m_cfg.restEnabled) {
        if (m_cfg.restDays.isEmpty())
            log("机器休息窗口已启用，但未选择休息日，不会触发");
        else
            log(QString("机器休息窗口：每周 %1，%2–%3（进入窗口即优雅重启）")
                    .arg(restDaysText(m_cfg.restDays), m_cfg.restStart, m_cfg.restEnd));
    }

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
        m_cfg.forceKernelRebootMode = tbl["force_kernel_reboot"].value_or(false);

        m_cfg.restEnabled = tbl["rest_enabled"].value_or(false);
        m_cfg.restDays.clear();
        if (auto *arr = tbl["rest_days"].as_array()) {
            for (auto &v : *arr) {
                const int d = static_cast<int>(v.value_or(int64_t(0)));
                if (d >= 1 && d <= 7)
                    m_cfg.restDays.append(d);
            }
        }
        m_cfg.restStart = QString::fromStdString(
            tbl["rest_start"].value_or(std::string("03:00")));
        m_cfg.restEnd = QString::fromStdString(
            tbl["rest_end"].value_or(std::string("04:00")));
        m_cfg.restLastRun = QString::fromStdString(
            tbl["rest_last_run"].value_or(std::string("")));

        if (!parseClock(m_cfg.restStart).isValid()) m_cfg.restStart = "03:00";
        if (!parseClock(m_cfg.restEnd).isValid())   m_cfg.restEnd = "04:00";

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
    root.emplace("force_kernel_reboot", false);

    toml::array restDays;
    restDays.push_back(int64_t(7));   // 默认周日
    root.emplace("rest_enabled", false);
    root.emplace("rest_days", std::move(restDays));
    root.emplace("rest_start", std::string("03:00"));
    root.emplace("rest_end", std::string("04:00"));

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

bool HealthMonitor::configForceKernelReboot(const QString &path)
{
    if (!QFile::exists(path))
        return false;

    try {
        auto tbl = toml::parse_file(path.toStdString());
        return tbl["force_kernel_reboot"].value_or(false);
    } catch (const toml::parse_error &) {
        return false;
    }
}

bool HealthMonitor::setConfigPorts(const QString &path, int nginxPort, int sshPort,
                                   bool forceKernelRebootMode)
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
    root.insert_or_assign("force_kernel_reboot", forceKernelRebootMode);

    std::ofstream ofs(path.toStdString());
    if (!ofs)
        return false;
    ofs << root << "\n";
    return true;
}

// ── 机器中途休息（每周定时优雅重启） ──

QTime HealthMonitor::parseClock(const QString &s)
{
    return QTime::fromString(s, "HH:mm");
}

QString HealthMonitor::formatClock(const QTime &t)
{
    return t.isValid() ? t.toString("HH:mm") : QString();
}

QString HealthMonitor::restDaysText(const QVector<int> &days)
{
    static const char *kNames[7] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};
    QVector<int> sorted = days;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());

    QStringList list;
    for (int d : sorted) {
        if (d >= 1 && d <= 7)
            list << QString::fromUtf8(kNames[d - 1]);
    }
    return list.isEmpty() ? QStringLiteral("（未选择）") : list.join("、");
}

void HealthMonitor::configRestWindow(const QString &path, bool *enabled,
                                     QVector<int> *days, QString *start, QString *end)
{
    bool en = false;
    QVector<int> dd;
    QString s = "03:00", e = "04:00";

    if (QFile::exists(path)) {
        try {
            auto tbl = toml::parse_file(path.toStdString());
            en = tbl["rest_enabled"].value_or(false);
            if (auto *arr = tbl["rest_days"].as_array()) {
                for (auto &v : *arr) {
                    const int d = static_cast<int>(v.value_or(int64_t(0)));
                    if (d >= 1 && d <= 7)
                        dd.append(d);
                }
            }
            s = QString::fromStdString(tbl["rest_start"].value_or(std::string("03:00")));
            e = QString::fromStdString(tbl["rest_end"].value_or(std::string("04:00")));
        } catch (const toml::parse_error &) {
        }
    }

    if (!parseClock(s).isValid()) s = "03:00";
    if (!parseClock(e).isValid()) e = "04:00";

    if (enabled) *enabled = en;
    if (days)    *days = dd;
    if (start)   *start = s;
    if (end)     *end = e;
}

bool HealthMonitor::setConfigRestWindow(const QString &path, bool enabled,
                                        const QVector<int> &days,
                                        const QString &start, const QString &end)
{
    ensureConfigFile(path);

    toml::table root;
    try {
        root = toml::parse_file(path.toStdString());
    } catch (const toml::parse_error &) {
        return false;
    }

    QVector<int> sorted = days;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());

    toml::array arr;
    for (int d : sorted)
        arr.push_back(int64_t(d));

    root.insert_or_assign("rest_enabled", enabled);
    root.insert_or_assign("rest_days", std::move(arr));
    root.insert_or_assign("rest_start", start.toStdString());
    root.insert_or_assign("rest_end", end.toStdString());

    std::ofstream ofs(path.toStdString());
    if (!ofs)
        return false;
    ofs << root << "\n";
    return true;
}

bool HealthMonitor::setConfigRestLastRun(const QString &path, const QString &date)
{
    ensureConfigFile(path);

    toml::table root;
    try {
        root = toml::parse_file(path.toStdString());
    } catch (const toml::parse_error &) {
        return false;
    }

    root.insert_or_assign("rest_last_run", date.toStdString());

    std::ofstream ofs(path.toStdString());
    if (!ofs)
        return false;
    ofs << root << "\n";
    return true;
}

QString HealthMonitor::kernelConfigPath()
{
    return QStringLiteral("/etc/sysctl.d/95-servertoolbox-hang.conf");
}

QString HealthMonitor::buildKernelConfigText(bool hungTaskPanic)
{
    QString text;
    QTextStream out(&text);
    out << "# 由 ServerToolbox 看门狗安装器生成，请勿手工修改。\n"
        << "# 删除本文件并执行 sysctl --system 即可还原。\n"
        << "kernel.hung_task_timeout_secs = 120\n"
        << "kernel.hung_task_panic = " << (hungTaskPanic ? 1 : 0) << "\n"
        << "kernel.softlockup_panic = 1\n"
        << "kernel.hardlockup_panic = 1\n"
        << "kernel.panic_on_oops = 1\n"
        << "kernel.panic = 10\n";
    return text;
}

QString HealthMonitor::buildKernelEnableScript(bool enableKdump, bool hungTaskPanic)
{
    QString script;
    QTextStream out(&script);
    out << "cat > " << kernelConfigPath() << " <<'STB_HANG_EOF'\n"
        << buildKernelConfigText(hungTaskPanic)
        << "STB_HANG_EOF\n"
        << "sysctl --system >/dev/null 2>&1 || true\n";
    if (enableKdump) {
        out << "if [ -f /etc/default/kdump-tools ]; then\n"
            << "  sed -i 's/^USE_KDUMP=.*/USE_KDUMP=1/' /etc/default/kdump-tools\n"
            << "  kdump-config load >/dev/null 2>&1 || systemctl restart kdump-tools >/dev/null 2>&1 || true\n"
            << "fi\n";
    }
    out << "printf 'KEXEC_CRASH_LOADED=%s\\n' \"$(cat /sys/kernel/kexec_crash_loaded 2>/dev/null || echo 0)\"\n"
        << "printf 'HUNG_TASK_PANIC=%s\\n' \"$(cat /proc/sys/kernel/hung_task_panic 2>/dev/null || echo ?)\"\n";
    return script;
}

QString HealthMonitor::buildKernelDisableScript()
{
    QString script;
    QTextStream out(&script);
    out << "rm -f " << kernelConfigPath() << "\n"
        << "sysctl --system >/dev/null 2>&1 || true\n"
        << "if [ -f /etc/default/kdump-tools ]; then\n"
        << "  sed -i 's/^USE_KDUMP=.*/USE_KDUMP=0/' /etc/default/kdump-tools\n"
        << "  kdump-config unload >/dev/null 2>&1 || systemctl stop kdump-tools >/dev/null 2>&1 || true\n"
        << "fi\n"
        << "printf 'KEXEC_CRASH_LOADED=%s\\n' \"$(cat /sys/kernel/kexec_crash_loaded 2>/dev/null || echo 0)\"\n";
    return script;
}

void HealthMonitor::log(const QString &message)
{
    Logger::log(QString::fromLatin1(kTag), message);
    fprintf(stderr, "[%s] %s\n", kTag, message.toUtf8().constData());
    fflush(stderr);
}

void HealthMonitor::check()
{
    // 休息窗口与探活相互独立：即使探活仍在进行，也要按时检查是否进入休息时段。
    checkRestWindow();

    if (m_busy)
        return;

    m_busy = true;
    probe(m_cfg.nginxHost, m_cfg.nginxPort, false, [this](Probe p) { onNginxResult(p); });
}

// 机器中途休息：进入每周设定的时间段即执行优雅重启。
// 每个"锚定休息日"最多触发一次（rest_last_run 落盘持久化），
// 因此重启后守护进程随 systemd 再次启动时，不会在同一个窗口里循环重启。
void HealthMonitor::checkRestWindow()
{
    if (!m_cfg.enabled || !m_cfg.restEnabled)
        return;
    if (m_cfg.restDays.isEmpty() || m_rebooting)
        return;

    const QTime start = parseClock(m_cfg.restStart);
    const QTime end = parseClock(m_cfg.restEnd);
    if (!start.isValid() || !end.isValid())
        return;

    const QDate today = QDate::currentDate();
    const QDateTime now(today, QTime::currentTime());
    const bool crossMidnight = end < start;

    // 窗口锚定在休息日当天的起始时刻；跨午夜窗口的结束时刻落在次日，
    // 所以同时检查"今天"与"昨天"两个锚定日。
    for (int back = 0; back <= 1; ++back) {
        const QDate anchor = today.addDays(-back);
        if (!m_cfg.restDays.contains(anchor.dayOfWeek()))
            continue;

        const QDateTime winStart(anchor, start);
        const QDateTime winEnd(anchor.addDays(crossMidnight ? 1 : 0), end);
        if (now < winStart || now > winEnd)
            continue;

        const QString anchorKey = anchor.toString("yyyy-MM-dd");
        if (m_cfg.restLastRun == anchorKey) {
            // 本窗口已执行过，跳过（防止重启后在窗口内反复触发）。
            return;
        }

        m_cfg.restLastRun = anchorKey;
        if (!setConfigRestLastRun(configPath(), anchorKey))
            log("休息窗口：无法写入 rest_last_run，重启后窗口内可能重复触发");

        log(QString("进入机器休息窗口（%1 %2–%3），执行优雅重启")
                .arg(anchorKey).arg(formatClock(start)).arg(formatClock(end)));

        m_rebooting = true;
        m_lastReboot = now;
        m_failStreak = 0;

        // 休息时段系统通常是健康的：走优雅重启；performReboot 内置 15 秒
        // 未生效则回退内核级硬重启的兜底。
        performReboot(false);
        QTimer::singleShot(30000, this, [this]() { m_rebooting = false; });
        return;
    }
}

void HealthMonitor::probe(const QString &host, int port, bool expectSshBanner,
                          std::function<void(Probe)> cb)
{
    auto *sock = new QTcpSocket(this);
    auto *timer = new QTimer(sock);
    timer->setSingleShot(true);

    auto settled = std::make_shared<bool>(false);
    auto buf = std::make_shared<QByteArray>();
    auto deliver = [sock, cb = std::move(cb), settled](Probe p) {
        if (*settled)
            return;
        *settled = true;
        sock->abort();
        sock->deleteLater();
        cb(p);
    };

    connect(sock, &QTcpSocket::connected, this, [deliver, expectSshBanner]() {
        // nginx 只需要确认端口能建立连接；SSH 必须等对端主动发来的协议标识串。
        if (!expectSshBanner)
            deliver(Probe::Ok);
    });
    connect(sock, &QTcpSocket::readyRead, this,
            [deliver, sock, buf, expectSshBanner]() {
                if (!expectSshBanner)
                    return;
                buf->append(sock->readAll());
                const int nl = buf->indexOf('\n');
                if (nl < 0) {
                    // 对端一直不发换行：交给总超时判定；同时防止无界累积。
                    if (buf->size() >= 255)
                        deliver(Probe::BadBanner);
                    return;
                }
                const QByteArray line = buf->left(nl).trimmed();
                deliver(line.startsWith("SSH-") ? Probe::Ok : Probe::BadBanner);
            });
    connect(sock, &QTcpSocket::disconnected, this,
            [deliver, expectSshBanner]() {
                // SSH 连接在收到标识串前被关闭，同样视为不可登录。
                if (expectSshBanner)
                    deliver(Probe::ClosedEarly);
            });
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
    probe(m_cfg.sshHost, m_cfg.sshPort, true, [this, nginx](Probe ssh) { onSshResult(nginx, ssh); });
}

void HealthMonitor::onSshResult(Probe nginx, Probe ssh)
{
    m_busy = false;

    // SSH 探测的“正常”必须是完成 SSH 协议握手（收到 SSH- 标识串）。
    // 只看 TCP 连接会误判：sshd 卡死或系统挂起时，内核仍可能替监听套接字
    // 完成三次握手，此时登录不了、也无法救援，必须视为异常。
    if (ssh == Probe::Ok) {
        log("nginx 异常但 SSH 正常（SSH 协议握手成功），系统存活，不重启");
        m_failStreak = 0;
        return;
    }

    m_failStreak++;
    log(QString("SSH 亦异常：%1（连续 %2/%3）")
                .arg(probeName(ssh)).arg(m_failStreak).arg(m_cfg.failThreshold));

    if (m_failStreak >= m_cfg.failThreshold)
        triggerReboot(QString("nginx(%1) 与 SSH(%2) 同时异常")
                      .arg(probeName(nginx), probeName(ssh)));
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

    performReboot(m_cfg.forceKernelRebootMode);

    QTimer::singleShot(30000, this, [this]() { m_rebooting = false; });
}

void HealthMonitor::performReboot(bool forceKernel)
{
    sync();

    // 勾选“直接内核级重启”：跳过优雅重启，直接 Magic SysRq。
    if (forceKernel) {
        log("配置为直接内核级重启，跳过优雅重启 (systemctl)");
        forceKernelReboot();
        return;
    }

    if (::geteuid() == 0)
        QProcess::startDetached("systemctl", {"reboot"});
    else
        QProcess::startDetached("pkexec", {"reboot"});

    // 15 秒后仍未重启（说明优雅重启也失效），回退到内核级硬重启。
    QTimer::singleShot(15000, this, [this]() {
        log("优雅重启 15 秒未生效，回退到内核级硬重启");
        forceKernelReboot();
    });
}

bool HealthMonitor::forceKernelReboot()
{
    log("系统无响应，触发内核级硬重启 (Magic SysRq)");

    // 启用所有 SysRq 功能（内核会补 0 终止符，"1" 即全部启用）。
    // /proc 值为运行时值，重启后自动恢复，此处覆盖无持久副作用。
    {
        int fd = open("/proc/sys/kernel/sysrq", O_WRONLY);
        if (fd < 0) {
            fprintf(stderr, "ERR:无法打开 /proc/sys/kernel/sysrq（未以 root 运行或内核未启用 Magic SysRq）\n");
            return false;
        }
        ssize_t w = write(fd, "1", 1);
        close(fd);
        if (w != 1) {
            fprintf(stderr, "ERR:启用 /proc/sys/kernel/sysrq 失败\n");
            return false;
        }
    }

    // 触发命令必须写入 /proc/sysrq-trigger（只写）。
    // 注意：内核对每次 write 只识别第一个字符，因此必须逐条写入。
    // s=sync 文件系统，u=只读卸载所有挂载，b=立即重启。
    // 内核直接响应，绕过所有 userspace 与 systemd。
    {
        int fd = open("/proc/sysrq-trigger", O_WRONLY);
        if (fd < 0) {
            fprintf(stderr, "ERR:无法打开 /proc/sysrq-trigger（未以 root 运行？）\n");
            return false;
        }

        const char pre[] = {'s', 'u'};   // 先落盘再卸载
        for (char c : pre) {
            if (write(fd, &c, 1) != 1) {
                fprintf(stderr, "ERR:写入 SysRq 命令 '%c' 失败，硬重启可能未生效\n", c);
                close(fd);
                return false;
            }
        }

        // 写入 'b' 后内核应立即重启，正常情况下此调用不会返回。
        const char boot = 'b';
        if (write(fd, &boot, 1) != 1) {
            fprintf(stderr, "ERR:写入 SysRq 命令 'b' 失败，硬重启可能未生效\n");
            close(fd);
            return false;
        }
        close(fd);

        // 能执行到这里，说明内核并未重启（被 lockdown/虚拟化限制或被忽略）。
        sleep(2);
        fprintf(stderr, "ERR:已发出重启命令但系统仍存活，硬重启未生效\n");
        return false;
    }
}

// nginx 探测只做 TCP 建连，超时/其它错误视为“忙但可能仍存活”，不计异常。
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
    case Probe::BadBanner:   return "未收到 SSH 协议标识串";
    default:                 return "其它错误";
    }
}
