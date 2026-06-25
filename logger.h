#ifndef LOGGER_H
#define LOGGER_H

#include <QString>
#include <QStringList>

// 简单的应用日志工具 —— 写入文件，可供 LogPage 读取
class Logger
{
public:
    // 写一条日志
    static void log(const QString &message);
    static void log(const QString &tag, const QString &message);

    // 读取全部日志（最新在前）
    static QStringList readAll(int maxLines = 500);

    // 清空日志
    static void clear();

    // 日志文件路径
    static QString logFilePath();
};

#endif // LOGGER_H
