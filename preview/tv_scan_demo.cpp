#include "naspage.h"
#include "theme.h"
#include <QApplication>
#include <QTabWidget>
#include <QTableWidget>
#include <QPushButton>
#include <QLabel>
#include <cstdio>

// 真实扫描演示：对着本机真挂载点跑，看真实输出长什么样。
int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setStyleSheet(Theme::globalStyleSheet());
    NasPage page;
    page.resize(1208, 800);
    page.show();
    for (int i = 0; i < 8; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 60);

    // sdb1 -> /mnt/d，里面有那个 ISO
    page.setMemberDevicesForTest({ QString::fromUtf8("/dev/sdb1") });
    auto tabs = page.findChildren<QTabWidget *>();
    if (!tabs.isEmpty()) tabs.first()->setCurrentIndex(1);
    for (int i = 0; i < 4; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 30);

    QPushButton *scan = nullptr;
    for (QPushButton *b : page.findChildren<QPushButton *>())
        if (b->text().contains(QString::fromUtf8("扫描"))) scan = b;
    if (!scan) { puts("!! 找不到扫描按钮"); return 1; }
    scan->click();
    for (int i = 0; i < 40; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 80);

    QTableWidget *tb = nullptr;
    for (QTableWidget *t : page.findChildren<QTableWidget *>()) tb = t;
    if (!tb) { puts("!! 没有表格"); return 1; }
    printf("\n真实扫描结果：%d 行\n", tb->rowCount());
    for (int r = 0; r < tb->rowCount() && r < 12; ++r) {
        QStringList cells;
        for (int c = 0; c < tb->columnCount(); ++c)
            cells << (tb->item(r, c) ? tb->item(r, c)->text() : QString());
        printf("  %s | %s | %s | %s\n", qPrintable(cells[0]), qPrintable(cells[1]),
               qPrintable(cells[2]), qPrintable(cells[3]));
    }
    for (QLabel *l : page.findChildren<QLabel *>()) {
        const QString t = l->text();
        if (t.contains(QString::fromUtf8("扫描完成"))) printf("  状态: %s\n", qPrintable(t));
        if (t.contains(QString::fromUtf8("未选分区"))) printf("  扫描根: %s\n", qPrintable(t));
    }
    page.grab().save("build/tv_scanned.png");
    puts("\n已存 build/tv_scanned.png");
    return 0;
}