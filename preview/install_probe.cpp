#include "naspage.h"
#include "nascontroller.h"
#include "theme.h"
#include <QApplication>
#include <QPushButton>
#include <QDebug>
static int fails=0, passes=0;
static void check(bool ok, const QString &w){ if(ok){++passes;qInfo()<<"  OK  "<<w;} else {++fails;qInfo()<<"  !!"<<w;} }
int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setStyleSheet(Theme::globalStyleSheet());

    // 1) 控制器层：canInstallFuse 的真值表
    NasController::Env e;
    e.fuseMount=false; e.aptFamily=true;  e.aptAvailable=true;
    check(e.canInstallFuse(), "缺fuse + apt系 + apt在 -> 可装");
    e.fuseMount=true;
    check(!e.canInstallFuse(), "fuse 已在 -> 不该可装（按钮不该出现）");
    e.fuseMount=false; e.aptFamily=false; e.aptAvailable=true;
    check(!e.canInstallFuse(), "非 apt 系 -> 不可装");
    e.aptFamily=true; e.aptAvailable=false;
    check(!e.canInstallFuse(), "无 apt -> 不可装");

    // 2) 页面层：本机 fuse 已装，按钮应默认不可见
    NasPage page;
    page.resize(1208, 900);
    page.show();
    for (int i=0;i<8;++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 60);
    QPushButton *btn=nullptr;
    for (QPushButton *b : page.findChildren<QPushButton *>())
        if (b->text().contains(QString::fromUtf8("一键安装"))) btn=b;
    check(btn!=nullptr, "找到一键安装按钮控件");
    // 切到挂载出口让它进入可见路径（但 canInstallFuse 为假时仍应隐藏）
    page.setMemberDevicesForTest({ QString::fromUtf8("/dev/sda1"), QString::fromUtf8("/dev/sdb1") });
    for (int i=0;i<6;++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    if (btn) check(!btn->isVisible(), "本机 fuse 已就绪 -> 按钮隐藏（不误露）");

    qInfo() << "";
    qInfo() << "   通过" << passes << "条，失败" << fails << "条";
    if (!fails) qInfo() << "OK 全部通过 (0 失败)";
    return fails?1:0;
}
