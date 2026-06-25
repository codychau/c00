#include "aboutpage.h"

#include <QVBoxLayout>
#include <QProcess>
#include <QApplication>

AboutPage::AboutPage(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(12);

    auto *title = new QLabel("ℹ️  关于家用服务器工具箱");
    QFont f = title->font(); f.setPointSize(16); f.setBold(true);
    title->setFont(f);
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);

    auto *desc = new QLabel(
        "家用服务器工具箱\n\n"
        "一个轻量级的家用服务器管理工具，\n"
        "集成系统监控、存储管理、音频控制、\n"
        "AI 服务管理等功能。\n\n"
        "基于 Qt6 + C++ 编写，\n"
        "通过调用系统命令获取信息与控制服务。");
    desc->setWordWrap(true);
    desc->setAlignment(Qt::AlignCenter);
    desc->setStyleSheet("color: #555; font-size: 13px;");
    layout->addWidget(desc);

    // 系统信息
    auto *sysInfo = new QLabel();
    sysInfo->setStyleSheet("color: #888; font-size: 11px;");
    sysInfo->setAlignment(Qt::AlignCenter);

    QProcess p;
    p.start("uname", {"-a"});
    p.waitForFinished(2000);
    QString uname = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    sysInfo->setText(QString("系统: %1\nQt: %2\n应用版本: 1.0.0")
                         .arg(uname, QT_VERSION_STR));
    layout->addWidget(sysInfo);

    layout->addStretch();

    auto *footer = new QLabel("用 ❤️ 和 C++ 编写");
    footer->setAlignment(Qt::AlignCenter);
    footer->setStyleSheet("color: #aaa; font-size: 11px;");
    layout->addWidget(footer);
}
