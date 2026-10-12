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
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QInputDialog>

WatchdogInstallDialog::WatchdogInstallDialog(int nginxPort, int sshPort, bool needPassword,
                                             bool forceKernelReboot, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("安装看门狗");
    setMinimumWidth(360);

    auto *layout = new QVBoxLayout(this);

    auto *desc = new QLabel(
        "将安装系统服务 servertoolbox-watchdog（root 权限，开机自启）。\n"
        "当 nginx 与 SSH 端口同时拒绝连接时，自动重启本机。");
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
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
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
