#include "adminhelper.h"

#include <QProcess>
#include <QInputDialog>
#include <QLineEdit>

namespace {

bool passwordlessSudoWorks()
{
    QProcess p;
    p.start("sudo", {"-n", "true"});
    if (!p.waitForFinished(2000)) {
        p.kill();
        p.waitForFinished(500);
        return false;
    }
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

}

bool AdminHelper::passwordlessSudoAvailable()
{
    return passwordlessSudoWorks();
}

bool AdminHelper::isAuthFailure(const QString &err)
{
    return err.contains("incorrect password", Qt::CaseInsensitive)
        || err.contains("Sorry, try again", Qt::CaseInsensitive)
        || err.contains("a password is required", Qt::CaseInsensitive)
        || err.contains("no password was provided", Qt::CaseInsensitive);
}

bool AdminHelper::runAsRootScriptWithPassword(const QString &script, const QString &password,
                                              QString *output, QString *error)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start("sudo", {"-S", "-p", "", "bash", "-c", script});

    if (!p.waitForStarted(5000)) {
        if (error) *error = QString("无法启动 sudo：%1").arg(p.errorString());
        return false;
    }

    if (!password.isEmpty())
        p.write((password + "\n").toUtf8());
    p.closeWriteChannel();

    if (!p.waitForFinished(120000)) {
        p.kill();
        p.waitForFinished(500);
        if (error) *error = "执行超时";
        return false;
    }

    const QString out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    const QString err = QString::fromUtf8(p.readAllStandardError()).trimmed();

    if (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0) {
        if (output) *output = out;
        return true;
    }

    if (error) *error = err.isEmpty() ? out : err;
    return false;
}

bool AdminHelper::runAsRootScript(const QString &script, QWidget *parent,
                                  QString *output, QString *error)
{
    if (passwordlessSudoWorks())
        return runAsRootScriptWithPassword(script, QString(), output, error);

    QString lastError;
    for (int attempt = 0; attempt < 3; ++attempt) {
        bool ok = false;
        const QString prompt = attempt == 0 ? QString("请输入管理员密码：")
                                            : QString("密码错误，请重新输入：");
        const QString password = QInputDialog::getText(parent, "管理员权限", prompt,
                                                       QLineEdit::Password, QString(), &ok);
        if (!ok) {
            if (error) *error = "已取消";
            return false;
        }

        if (runAsRootScriptWithPassword(script, password, output, &lastError))
            return true;

        if (!isAuthFailure(lastError)) {
            if (error) *error = lastError;
            return false;
        }
    }

    if (error) *error = lastError.isEmpty() ? QString("认证失败") : lastError;
    return false;
}
