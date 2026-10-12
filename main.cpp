#include <QApplication>
#include <QCoreApplication>
#include <QLockFile>
#include <QDir>
#include <QString>

#include "mainwindow.h"
#include "healthmonitor.h"

int main(int argc, char *argv[])
{
    bool daemon = false;
    QString configPath;

    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "--health-daemon") {
            daemon = true;
        } else if (arg == "--health-config" && i + 1 < argc) {
            configPath = QString::fromLocal8Bit(argv[++i]);
        }
    }

    if (daemon) {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName("ServerToolbox");

        QLockFile lock(QDir::tempPath() + "/servertoolbox-health.lock");
        if (!lock.tryLock(0))
            return 0;

        HealthMonitor monitor(configPath);
        monitor.start();
        return app.exec();
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName("ServerToolbox");

    MainWindow w;
    w.show();

    return app.exec();
}
