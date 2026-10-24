#include <QApplication>
#include <QCoreApplication>
#include <QLockFile>
#include <QTimer>
#include <QDir>
#include <QString>

#include "mainwindow.h"
#include "healthmonitor.h"

int main(int argc, char *argv[])
{
    bool daemon = false;
    bool forceReboot = false;
    bool gracefulReboot = false;
    QString configPath;

    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "--health-daemon") {
            daemon = true;
        } else if (arg == "--health-config" && i + 1 < argc) {
            configPath = QString::fromLocal8Bit(argv[++i]);
        } else if (arg == "--force-reboot") {
            forceReboot = true;
        } else if (arg == "--reboot") {
            gracefulReboot = true;
        }
    }

    if (forceReboot) {
        HealthMonitor monitor;
        return monitor.forceKernelReboot() ? 0 : 1;
    }

    if (gracefulReboot) {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName("ServerToolbox");

        HealthMonitor monitor;
        monitor.performReboot(false);

        // 优雅重启未生效时，performReboot 会在 15 秒后触发内核级兜底。
        QTimer::singleShot(22000, &app, &QCoreApplication::quit);
        return app.exec();
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

    app.setStyleSheet(
        "QPushButton {"
        "  min-height: 25px;"
        "  max-height: 25px;"
        "  padding: 2px 12px;"
        "}"
        "QPushButton:disabled {"
        "  color: #888;"
        "}");

    MainWindow w;
    w.show();

    return app.exec();
}
