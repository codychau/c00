#include <QApplication>

#include "mainwindow.h"
#include "statuspage.h"
#include "scenepage.h"
#include "storagepage.h"
#include "entertainmentpage.h"
#include "desktoppage.h"
#include "aipage.h"
#include "servicepage.h"
#include "aboutpage.h"
#include "vmpage.h"
#include "logpage.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("家用服务器工具箱");
    resize(560, 420);

    m_tabs = new QTabWidget(this);
    auto *vmPage = new VMPage(this);
    m_tabs->addTab(new StatusPage(this),     "📊  状态仪表");
    m_tabs->addTab(new ScenePage(vmPage, this), "🎬  场景设定");
    m_tabs->addTab(new StoragePage(this),    "💾  存储管理");
    m_tabs->addTab(vmPage,                   "🖥️  虚机管理");
    m_tabs->addTab(new EntertainmentPage(this), "🎮  娱乐选项");
    m_tabs->addTab(new DesktopPage(this),      "🎨  个性化桌面");
    m_tabs->addTab(new AIPage(this),         "🤖  AI 选项");
    m_tabs->addTab(new ServicePage(this),    "⚙️  服务控制");
    m_tabs->addTab(new LogPage(this),        "📋  日志管理");
    m_tabs->addTab(new AboutPage(this),      "ℹ️  关于");
    setCentralWidget(m_tabs);
}
