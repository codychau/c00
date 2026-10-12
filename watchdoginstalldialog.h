#ifndef WATCHDOGINSTALLDIALOG_H
#define WATCHDOGINSTALLDIALOG_H

#include <QDialog>
#include <QProcess>
#include <QString>

class QSpinBox;
class QLineEdit;
class QPushButton;
class QCheckBox;

class WatchdogInstallDialog : public QDialog
{
    Q_OBJECT

public:
    WatchdogInstallDialog(int nginxPort, int sshPort, bool needPassword,
                           bool forceKernelReboot, QWidget *parent = nullptr);

    int nginxPort() const;
    int sshPort() const;
    QString password() const;
    bool forceKernelReboot() const;

private slots:
    void forceRebootTest();
    void onRebootFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onRebootTimeout();

private:
    QSpinBox *m_nginxSpin;
    QSpinBox *m_sshSpin;
    QLineEdit *m_passwordEdit = nullptr;
    QPushButton *m_testBtn = nullptr;
    QCheckBox *m_forceKernelCheck = nullptr;
    QProcess *m_rebootProc = nullptr;
    bool m_rebootDone = false;
};

#endif // WATCHDOGINSTALLDIALOG_H
