#ifndef WATCHDOGINSTALLDIALOG_H
#define WATCHDOGINSTALLDIALOG_H

#include <QDialog>
#include <QProcess>
#include <QString>
#include <QVector>

class QSpinBox;
class QLineEdit;
class QPushButton;
class QCheckBox;
class QTimeEdit;
class QWidget;

class WatchdogInstallDialog : public QDialog
{
    Q_OBJECT

public:
    WatchdogInstallDialog(int nginxPort, int sshPort, bool needPassword,
                          bool forceKernelReboot,
                          bool restEnabled, const QVector<int> &restDays,
                          const QString &restStart, const QString &restEnd,
                          QWidget *parent = nullptr);

    int nginxPort() const;
    int sshPort() const;
    QString password() const;
    bool forceKernelReboot() const;

    // 机器中途休息（每周定时优雅重启）
    bool restEnabled() const;
    QVector<int> restDays() const;
    QString restStart() const;   // "HH:mm"
    QString restEnd() const;

    // 内核级死机检测选项（同一枚 sudo 密码内一并配置）
    bool kernelDetection() const;
    bool kdumpEnabled() const;
    bool hungTaskPanic() const;

private slots:
    void forceRebootTest();
    void onRebootFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onRebootTimeout();
    void validateAndAccept();

private:
    void syncRestPanel();

    QSpinBox *m_nginxSpin;
    QSpinBox *m_sshSpin;
    QLineEdit *m_passwordEdit = nullptr;
    QPushButton *m_testBtn = nullptr;
    QCheckBox *m_forceKernelCheck = nullptr;
    QCheckBox *m_kernelCheck = nullptr;
    QCheckBox *m_kdumpCheck = nullptr;
    QCheckBox *m_hungTaskCheck = nullptr;
    QCheckBox *m_restCheck = nullptr;
    QWidget *m_restPanel = nullptr;
    QTimeEdit *m_restStart = nullptr;
    QTimeEdit *m_restEnd = nullptr;
    QCheckBox *m_dayChecks[7] = {};
    QProcess *m_rebootProc = nullptr;
    bool m_rebootDone = false;
};

#endif // WATCHDOGINSTALLDIALOG_H
