#include <QApplication>

#include "mainwindow.h"
#include "statuspage.h"
#include "storagepage.h"
#include "entertainmentpage.h"
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
    m_tabs->addTab(new StatusPage(this),     "📊  状态仪表");
    m_tabs->addTab(new StoragePage(this),    "💾  存储管理");
    m_tabs->addTab(new VMPage(this),         "🖥️  虚机管理");
    m_tabs->addTab(new EntertainmentPage(this), "🎮  娱乐选项");
    m_tabs->addTab(new AIPage(this),         "🤖  AI 选项");
    m_tabs->addTab(new ServicePage(this),    "⚙️  服务控制");
    m_tabs->addTab(new LogPage(this),        "📋  日志管理");
    m_tabs->addTab(new AboutPage(this),      "ℹ️  关于");
    setCentralWidget(m_tabs);
}
