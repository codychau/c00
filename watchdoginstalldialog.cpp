#include "watchdoginstalldialog.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QSpinBox>
#include <QLineEdit>
#include <QPushButton>
#include <QHBoxLayout>
#include <QDialogButtonBox>

WatchdogInstallDialog::WatchdogInstallDialog(int nginxPort, int sshPort, bool needPassword,
                                             QWidget *parent)
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

    if (needPassword) {
        m_passwordEdit = new QLineEdit();
        m_passwordEdit->setEchoMode(QLineEdit::Password);
        m_passwordEdit->setPlaceholderText("管理员密码");
        form->addRow("管理员密码:", m_passwordEdit);
    }

    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("安装");
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
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
