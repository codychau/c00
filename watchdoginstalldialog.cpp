#include "watchdoginstalldialog.h"

#include "adminhelper.h"

#include <QCoreApplication>
#include <QProcess>
#include <QTimer>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QSpinBox>
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QInputDialog>
#include <QTimeEdit>
#include <QTime>

WatchdogInstallDialog::WatchdogInstallDialog(int nginxPort, int sshPort, bool needPassword,
                                             bool forceKernelReboot,
                                             bool restEnabled, const QVector<int> &restDays,
                                             const QString &restStart, const QString &restEnd,
                                             QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("安装看门狗");
    setMinimumWidth(470);

    auto *layout = new QVBoxLayout(this);

    auto *desc = new QLabel(
        "将安装系统服务 servertoolbox-watchdog（root 权限，开机自启）。\n"
        "当 nginx 拒绝连接、且 SSH 也无法完成协议握手时，自动重启本机。\n"
        "可额外配置每周定时的「机器中途休息」，在设定时间段内自动优雅重启。");
    desc->setWordWrap(true);
    layout->addWidget(desc);

    auto *form = new QFormLayout();

    m_nginxSpin = new QSpinBox();
    m_nginxSpin->setRange(1, 65535);
    m_nginxSpin->setValue(nginxPort);
    form->addRow("nginx 端口:", m_nginxSpin);

    m_sshSpin = new QSpinBox();
    m_sshSpin->setRange(1, 65535);
    m_sshSpin->setValue(sshPort);
    form->addRow("SSH 端口:", m_sshSpin);

    m_passwordEdit = new QLineEdit();
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText("管理员密码（若已缓存 sudo 权限可留空）");
    form->addRow("管理员密码:", m_passwordEdit);

    layout->addLayout(form);

    m_forceKernelCheck = new QCheckBox("直接内核级重启（忽略优雅重启）");
    m_forceKernelCheck->setChecked(forceKernelReboot);
    m_forceKernelCheck->setToolTip(
        "勾选后，检测到异常时跳过 systemctl 优雅重启，直接通过内核 Magic SysRq 强制重启。");
    layout->addWidget(m_forceKernelCheck);

    m_kernelCheck = new QCheckBox("同时配置内核级死机检测（锁死/内核错误自动重启）");
    m_kernelCheck->setChecked(true);
    m_kernelCheck->setToolTip(
        "写入 /etc/sysctl.d/95-servertoolbox-hang.conf：\n"
        "CPU 软/硬锁死与内核 oops 触发 panic，10 秒后自动重启。");
    layout->addWidget(m_kernelCheck);

    m_kdumpCheck = new QCheckBox("开启 kdump 崩溃转储（保留 vmcore 与内核日志）");
    m_kdumpCheck->setChecked(true);
    m_kdumpCheck->setToolTip(
        "启用 USE_KDUMP 并装载 crash kernel；crashkernel 内存已在启动参数中预留。");
    layout->addWidget(m_kdumpCheck);

    m_hungTaskCheck = new QCheckBox("进程卡死(hung_task)超过 120s 也自动重启（可能因大 I/O 误重启）");
    m_hungTaskCheck->setChecked(false);
    m_hungTaskCheck->setToolTip(
        "默认只告警。若希望磁盘/IO 卡死也直接重启，可勾选此项（会带来误重启风险）。");
    layout->addWidget(m_hungTaskCheck);

    auto syncKernelOpts = [this]() {
        const bool on = m_kernelCheck->isChecked();
        m_kdumpCheck->setEnabled(on);
        m_hungTaskCheck->setEnabled(on);
    };
    connect(m_kernelCheck, &QCheckBox::toggled, this, syncKernelOpts);
    syncKernelOpts();

    // ── 机器中途休息（每周定时优雅重启） ──
    m_restCheck = new QCheckBox("机器中途休息（每周定时优雅重启）");
    m_restCheck->setChecked(restEnabled);
    m_restCheck->setToolTip(
        "勾选后，看门狗会在每周选定的休息日、设定的时间段内自动重启本机。\n"
        "进入时间段的第一个检测周期即触发，每个休息日最多一次（重启后不会在窗口内循环触发）。\n"
        "走优雅重启（systemctl reboot），15 秒未生效则回退内核级硬重启。");
    layout->addWidget(m_restCheck);

    m_restPanel = new QWidget();
    auto *restLayout = new QGridLayout(m_restPanel);
    restLayout->setContentsMargins(0, 0, 0, 0);
    restLayout->setSpacing(6);

    restLayout->addWidget(new QLabel("休息日："), 0, 0);
    static const char *kDayNames[7] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};
    for (int i = 0; i < 7; ++i) {
        m_dayChecks[i] = new QCheckBox(QString::fromUtf8(kDayNames[i]));
        m_dayChecks[i]->setChecked(restDays.contains(i + 1));
        restLayout->addWidget(m_dayChecks[i], 0, i + 1);
    }

    restLayout->addWidget(new QLabel("时间段："), 1, 0);

    QTime tStart = QTime::fromString(restStart, "HH:mm");
    QTime tEnd   = QTime::fromString(restEnd, "HH:mm");
    if (!tStart.isValid()) tStart = QTime(3, 0);
    if (!tEnd.isValid())   tEnd   = QTime(4, 0);

    m_restStart = new QTimeEdit(tStart);
    m_restStart->setDisplayFormat("HH:mm");
    m_restStart->setToolTip("窗口起始时刻，进入即触发优雅重启。");
    m_restEnd = new QTimeEdit(tEnd);
    m_restEnd->setDisplayFormat("HH:mm");
    m_restEnd->setToolTip("窗口结束时刻；起始晚于结束表示跨午夜窗口（归属起始日）。");

    auto *timeRow = new QHBoxLayout();
    timeRow->addWidget(m_restStart);
    timeRow->addWidget(new QLabel("→"));
    timeRow->addWidget(m_restEnd);
    timeRow->addStretch();
    restLayout->addLayout(timeRow, 1, 1, 1, 6);

    auto *restHint = new QLabel(
        "起始时间晚于结束时间表示跨午夜窗口（如 22:00 → 02:00，归属起始日）。\n"
        "每个休息日最多重启一次，重启后守护进程随开机自启，不会在窗口内反复重启。");
    restHint->setWordWrap(true);
    restLayout->addWidget(restHint, 2, 0, 1, 8);

    connect(m_restCheck, &QCheckBox::toggled, this, [this]() { syncRestPanel(); });
    layout->addWidget(m_restPanel);
    syncRestPanel();

    auto *testRow = new QHBoxLayout();
    auto *testLabel = new QLabel("系统测试：");
    testRow->addWidget(testLabel);
    m_testBtn = new QPushButton("🔄 测试硬重启");
    m_testBtn->setToolTip("通过内核 Magic SysRq 强制重启本机（sync→umount→reboot），会立即重启、丢失所有会话。");
    testRow->addWidget(m_testBtn);
    connect(m_testBtn, &QPushButton::clicked, this, &WatchdogInstallDialog::forceRebootTest);
    testRow->addStretch();
    layout->addLayout(testRow);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("安装");
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    connect(buttons, &QDialogButtonBox::accepted, this,
            &WatchdogInstallDialog::validateAndAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void WatchdogInstallDialog::forceRebootTest()
{
    const bool forceKernel = m_forceKernelCheck && m_forceKernelCheck->isChecked();

    if (QMessageBox::question(this, "测试重启",
                              (forceKernel
                                   ? QStringLiteral("将立即通过内核 Magic SysRq 强制重启本机")
                                   : QStringLiteral("将先尝试优雅重启（systemctl reboot），"
                                                    "15 秒未生效则回退到内核级硬重启"))
                                  + "，所有会话会丢失。\n确定要测试吗？")
        != QMessageBox::Yes) {
        return;
    }

    const QString bin = QCoreApplication::applicationFilePath();
    m_rebootDone = false;

    QString pw = m_passwordEdit->text();
    if (pw.isEmpty() && !AdminHelper::passwordlessSudoAvailable()) {
        bool ok = false;
        pw = QInputDialog::getText(this, "管理员权限", "请输入管理员密码：",
                                   QLineEdit::Password, QString(), &ok);
        if (!ok || pw.isEmpty())
            return;
    }

    if (!m_rebootProc) {
        m_rebootProc = new QProcess(this);
        m_rebootProc->setProcessChannelMode(QProcess::SeparateChannels);
        connect(m_rebootProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &WatchdogInstallDialog::onRebootFinished);
        connect(m_rebootProc, &QProcess::errorOccurred, this,
                [this](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart && !m_rebootDone) {
                m_rebootDone = true;
                m_testBtn->setEnabled(true);
                QMessageBox::warning(this, "硬重启未生效",
                    "无法启动 sudo，请确认系统已安装 sudo 命令。");
            }
        });
    }

    const QString mode = forceKernel ? QStringLiteral("--force-reboot")
                                     : QStringLiteral("--reboot");
    if (!pw.isEmpty()) {
        m_rebootProc->start("sudo", {"-S", "-p", "", bin, mode});
        m_rebootProc->write((pw + "\n").toUtf8());
    } else {
        m_rebootProc->start("sudo", {"-n", bin, mode});
    }
    m_rebootProc->closeWriteChannel();

    m_testBtn->setEnabled(false);

    // 优雅重启模式需等待 15 秒兜底，超时留长一些
    QTimer::singleShot(forceKernel ? 6000 : 25000,
                       this, &WatchdogInstallDialog::onRebootTimeout);
}

void WatchdogInstallDialog::onRebootFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (m_rebootDone)
        return;
    m_rebootDone = true;
    m_testBtn->setEnabled(true);

    const QString err = QString::fromUtf8(m_rebootProc->readAllStandardError());
    const QString output = err.isEmpty()
        ? QString::fromUtf8(m_rebootProc->readAllStandardOutput())
        : err;

    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
        QMessageBox::information(this, "重启",
            "已发出重启指令，系统将在数秒内重启。");
        return;
    }

    QString msg = QStringLiteral("系统未重启，重启操作可能未成功。\n\n");
    if (output.trimmed().isEmpty()) {
        msg += QStringLiteral(
            "常见原因：未以 root 运行、内核未启用 Magic SysRq，"
            "或运行在容器/沙箱环境中。\n\n"
            "启用 Magic SysRq：\n"
            "  echo 1 | sudo tee /proc/sys/kernel/sysrq");
    } else {
        msg += QStringLiteral("详细信息：\n") + output.trimmed();
    }
    QMessageBox::warning(this, "重启未生效", msg);
}

void WatchdogInstallDialog::onRebootTimeout()
{
    if (m_rebootDone)
        return;
    m_rebootDone = true;
    m_testBtn->setEnabled(true);

    QMessageBox::warning(this, "重启未生效",
        "系统仍未重启，重启操作可能未成功。\n\n"
        "常见原因：\n"
        "• 当前未以 root 运行，无法写入 /proc/sys/kernel/sysrq\n"
        "• 内核禁用了 Magic SysRq（/proc/sys/kernel/sysrq 不可写）\n"
        "• 运行在容器/沙箱环境中，内核重启被限制\n\n"
        "请以 root 权限运行并启用 Magic SysRq：\n"
        "  echo 1 | sudo tee /proc/sys/kernel/sysrq\n"
        "或改用系统自带的重启方式。");
}

int WatchdogInstallDialog::nginxPort() const
{
    return m_nginxSpin->value();
}

int WatchdogInstallDialog::sshPort() const
{
    return m_sshSpin->value();
}

QString WatchdogInstallDialog::password() const
{
    return m_passwordEdit ? m_passwordEdit->text() : QString();
}

bool WatchdogInstallDialog::forceKernelReboot() const
{
    return m_forceKernelCheck && m_forceKernelCheck->isChecked();
}

bool WatchdogInstallDialog::kernelDetection() const
{
    return m_kernelCheck && m_kernelCheck->isChecked();
}

bool WatchdogInstallDialog::kdumpEnabled() const
{
    return kernelDetection() && m_kdumpCheck && m_kdumpCheck->isChecked();
}

bool WatchdogInstallDialog::hungTaskPanic() const
{
    return kernelDetection() && m_hungTaskCheck && m_hungTaskCheck->isChecked();
}

void WatchdogInstallDialog::syncRestPanel()
{
    if (m_restPanel)
        m_restPanel->setEnabled(m_restCheck && m_restCheck->isChecked());
}

void WatchdogInstallDialog::validateAndAccept()
{
    if (m_restCheck && m_restCheck->isChecked()) {
        QVector<int> days;
        for (int i = 0; i < 7; ++i)
            if (m_dayChecks[i] && m_dayChecks[i]->isChecked())
                days.append(i + 1);

        if (days.isEmpty()) {
            QMessageBox::warning(this, "机器中途休息",
                "请至少选择一个休息日，否则休息窗口永远不会触发。");
            return;
        }
        if (m_restStart && m_restEnd && m_restStart->time() == m_restEnd->time()) {
            QMessageBox::warning(this, "机器中途休息",
                "时间段的起始与结束时间不能相同（窗口长度为 0，无法触发）。\n"
                "例如可设为 03:00 → 04:00。");
            return;
        }
    }
    accept();
}

bool WatchdogInstallDialog::restEnabled() const
{
    return m_restCheck && m_restCheck->isChecked();
}

QVector<int> WatchdogInstallDialog::restDays() const
{
    QVector<int> days;
    if (!m_restCheck || !m_restCheck->isChecked())
        return days;
    for (int i = 0; i < 7; ++i) {
        if (m_dayChecks[i] && m_dayChecks[i]->isChecked())
            days.append(i + 1);
    }
    return days;
}

QString WatchdogInstallDialog::restStart() const
{
    return m_restStart ? m_restStart->time().toString("HH:mm") : QStringLiteral("03:00");
}

QString WatchdogInstallDialog::restEnd() const
{
    return m_restEnd ? m_restEnd->time().toString("HH:mm") : QStringLiteral("04:00");
}
