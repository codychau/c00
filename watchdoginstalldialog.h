#ifndef WATCHDOGINSTALLDIALOG_H
#define WATCHDOGINSTALLDIALOG_H

#include <QDialog>
#include <QString>

class QSpinBox;
class QLineEdit;

class WatchdogInstallDialog : public QDialog
{
    Q_OBJECT

public:
    WatchdogInstallDialog(int nginxPort, int sshPort, bool needPassword,
                          QWidget *parent = nullptr);

    int nginxPort() const;
    int sshPort() const;
    QString password() const;

private:
    QSpinBox *m_nginxSpin;
    QSpinBox *m_sshSpin;
    QLineEdit *m_passwordEdit = nullptr;
};

#endif // WATCHDOGINSTALLDIALOG_H
