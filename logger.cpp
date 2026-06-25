#include "logger.h"

#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QDateTime>
#include <QStandardPaths>
#include <QFileInfo>

QString Logger::logFilePath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dir + "/app.log";
}

void Logger::log(const QString &message)
{
    log("TOOLBOX", message);
}

void Logger::log(const QString &tag, const QString &message)
{
    QString path = logFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QFile file(path);
    if (!file.open(QIODevice::Append | QIODevice::Text))
        return;

    QTextStream out(&file);
    QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    out << ts << "  [" << tag << "]  " << message << "\n";
    file.close();

    // 文件超过 1MB 截断保留后半
    if (file.size() > 1024 * 1024) {
        QStringList lines = readAll(8000);
        file.open(QIODevice::WriteOnly | QIODevice::Text);
        QTextStream trunc(&file);
        for (const auto &line : lines)
            trunc << line << "\n";
        file.close();
    }
}

QStringList Logger::readAll(int maxLines)
{
    QStringList result;
    QFile file(logFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return result;

    QTextStream in(&file);
    while (!in.atEnd())
        result.append(in.readLine());
    file.close();

    // 最旧 -> 最新，反转使最新在最前
    std::reverse(result.begin(), result.end());

    if (result.size() > maxLines)
        result = result.mid(0, maxLines);

    return result;
}

void Logger::clear()
{
    QFile file(logFilePath());
    if (file.exists())
        file.remove();
}
