#include "theme.h"
#include "statuspage.h"
#include "vmpage.h"
#include "storagepage.h"
#include "scenepage.h"
#include "entertainmentpage.h"
#include "desktoppage.h"
#include "aipage.h"
#include "servicepage.h"
#include "logpage.h"
#include "aboutpage.h"
#include "naspage.h"

#include <QApplication>
#include <cstring>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>

template <typename T>
static void probe(const char *name)
{
    printf("-- %s ...\n", name); fflush(stdout);
    QElapsedTimer t; t.start();
    T *w = new T();
    printf("   构造 %lldms\n", (long long)t.elapsed()); fflush(stdout);
    w->resize(900, 700);
    w->show();
    for (int r = 0; r < 12; ++r) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    printf("   show+泵 %lldms  [完成]\n", (long long)t.elapsed()); fflush(stdout);
    // 不 delete：QProcess 子对象析构在本沙箱会挂住（与主题无关）
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    QApplication app(argc, argv);
    app.setStyleSheet(Theme::globalStyleSheet());
    const char *only = argc > 1 ? argv[1] : "";
    auto want = [&](const char *n) { return only[0] == 0 || strcmp(only, n) == 0; };
    if (want("about"))  probe<AboutPage>("AboutPage");
    if (want("log"))    probe<LogPage>("LogPage");
    if (want("desktop"))probe<DesktopPage>("DesktopPage");
    if (want("scene")) {
        printf("-- ScenePage ...\n"); fflush(stdout);
        QElapsedTimer t; t.start();
        VMPage *vm = new VMPage();
        ScenePage *sp = new ScenePage(vm);
        printf("   构造 %lldms\n", (long long)t.elapsed()); fflush(stdout);
        sp->resize(900, 700); sp->show();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 60);
        printf("   show+泵 %lldms  [完成]\n", (long long)t.elapsed()); fflush(stdout);
        delete sp; delete vm;
    }
    if (want("service"))probe<ServicePage>("ServicePage");
    if (want("ai"))     probe<AIPage>("AIPage");
    if (want("storage"))probe<StoragePage>("StoragePage");
    if (want("nas"))    probe<NasPage>("NasPage");
    if (want("ent"))    probe<EntertainmentPage>("EntertainmentPage");
    if (want("vm"))     probe<VMPage>("VMPage");
    if (want("status")) probe<StatusPage>("StatusPage");
    printf("ALL DONE\n");
    return 0;
}
