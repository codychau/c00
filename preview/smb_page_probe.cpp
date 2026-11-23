// SMB 导出模式的行为断言。
//
// 为什么需要它：page_probe 只证明页面能构造、不崩，证明不了
// 「选 2 个再减到 1 个会自动切到 SMB」这条具体行为 —— 而这条恰恰是
// 用户点名要的。构造探针给人虚假的安全感。
//
// 做法：通过公开入口驱动成员数变化，再读单选按钮状态断言。
#include "naspage.h"
#include "nasboardwidget.h"
#include "theme.h"

#include <QApplication>
#include <QRadioButton>
#include <QLabel>
#include <QWidget>
#include <QEventLoop>
#include <cstdio>
#include <QTabWidget>
#include <QTableWidget>
#include <QPushButton>
#include <QScreen>
#include <QPixmap>

static int fail = 0;
static void check(bool c, const QString &w)
{
    printf("  %s  %s\n", c ? "OK " : "!! ", qPrintable(w));
    if (!c) ++fail;
}

static QRadioButton *smbRadio(NasPage *p)
{
    for (QRadioButton *r : p->findChildren<QRadioButton *>())
        if (r->text().contains("SMB")) return r;
    return nullptr;
}
static QRadioButton *iscsiRadio(NasPage *p)
{
    for (QRadioButton *r : p->findChildren<QRadioButton *>())
        if (r->text().contains("iSCSI")) return r;
    return nullptr;
}
static NasBoardWidget *boardOf(NasPage *p)
{
    const auto bs = p->findChildren<NasBoardWidget *>();
    return bs.isEmpty() ? nullptr : bs.first();
}

// 找一个「可选」的盘位设备名，用来模拟点击
static QRadioButton *mountRadio(NasPage *p)
{
    for (QRadioButton *r : p->findChildren<QRadioButton *>())
        if (r->text().contains(QString::fromUtf8("挂载"))) return r;
    return nullptr;
}

// 固定一块"挂在 /mnt/x 上的分区"。用真机布局做断言的探针不可移植，见 main 里那段。
static NasBoardWidget::Disk mkDisk(const QString &dev, const QString &mp)
{
    NasBoardWidget::Disk d;
    d.device = dev;
    d.label = dev.section('/', -1);
    d.mountpoint = mp;
    d.fstype = "ext4";
    d.bytes = 2LL << 40;
    d.freeBytes = 1LL << 40;
    d.selectable = true;
    return d;
}

static QString pickableDevice(NasPage *p, NasBoardWidget *board)
{
    // 用 tooltip 里带设备名的槽位；不可用的槽位 isBlocked，点击会被拒
    for (int i = 0; i < board->slotCount(); ++i) {
        const QString tip = board->tipForSlot(i);
        if (tip.isEmpty()) continue;
        const QString dev = tip.split("\n").first();
        if (dev.startsWith("/dev/")) return dev;
    }
    return QString();
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    // 强制 offscreen。让 Qt 自己挑平台插件会挑 xcb；本机没有 X 服务
    // （DISPLAY 空、/tmp/.X11-unix 空），xcb 插件直接 abort 并留 core dump。
    // 现象像随机崩溃，实际是调用方式问题。与其指望调用方每次记得设
    // 环境变量（忘了就得到莫名其妙的失败），不如探针自己设；
    // 但外部显式指定了别的平台就尊重它。
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    app.setStyleSheet(Theme::globalStyleSheet());

    NasPage page;
    page.resize(1000, 820);
    page.show();
    for (int i = 0; i < 10; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 40);

    NasBoardWidget *board = boardOf(&page);
    check(board != nullptr, "找到分区画板");
    if (!board) return 1;

    QRadioButton *smb = smbRadio(&page);
    QRadioButton *iqn = iscsiRadio(&page);
    check(smb && iqn, "找到 SMB / iSCSI 两个模式单选");
    if (!smb || !iqn) return 1;

    printf("\n=== 初始（0 个成员）===\n");
    check(iqn->isChecked(),  "默认停在 iSCSI（不擅自改默认）");
    check(!smb->isEnabled(), "0 个成员时 SMB 禁用");

    // 灌进固定的两块盘。原来这个探针直接用真机 lsblk 的结果，断言里写死了
    // "/dev/sdb1 挂 /mnt/d" —— 换台机器（本沙箱只有 sda1..sda6）预填断言就必然红，
    // 红了之后整条探针长期挂红，真正回归反而看不见。盘固定下来，断言才是断言。
    page.setDisksForTest({ mkDisk("/dev/sda1", "/mnt/e"), mkDisk("/dev/sdb1", "/mnt/d") });

    printf("\n=== 2 个成员 ===\n");
    page.setMemberDevicesForTest({ "/dev/sda1", "/dev/sdb1" });
    check(!smb->isEnabled(), "2 个成员：SMB 仍禁用（多盘只能 iSCSI）");
    // 这里原来断言「模式是 iSCSI」。它后来被"≥2 盘自动建议挂载目录"那条
    // 正常行为做废了，却一直没人改，于是这条探针长期是红的。真正要守的规矩是
    // 「SMB 不可选时，落在选中的必须是 ≥2 成员合法的出口（iSCSI 或挂载目录）」，
    // 而不是具体哪一个 —— 写死哪一个都会让下一次加出口时又红一次。
    check(iqn->isChecked() || mountRadio(&page)->isChecked(),
          "2 个成员：落在 iSCSI 或挂载目录之一（SMB 不可选时不许停在 SMB 上）");

    printf("\n=== 关键：2 减到 1 → 自动切 SMB ===\n");
    page.setMemberDevicesForTest({ "/dev/sda1" });
    check(smb->isEnabled(), "1 个成员：SMB 变为可选");
    check(smb->isChecked(), "自动切到了 SMB（用户点名的行为）");

    printf("\n=== 1 个成员时不该被反复强切 ===\n");
    // 手动切回 iSCSI，再触发一次「同为 1 个成员」的刷新：
    // 若实现写成「当前==1 就切 SMB」，这里会被错误地翻回 SMB。
    iqn->setChecked(true);
    page.setMemberDevicesForTest({ "/dev/sda1" });
    check(iqn->isChecked(), "同为 1 个成员的刷新不会抢用户的选择");

    printf("\n=== 回到 0 个：回到 iSCSI 默认 ===\n");
    page.setMemberDevicesForTest({});
    check(iqn->isChecked(), "清空后回到 iSCSI");
    check(!smb->isEnabled(), "清空后 SMB 再次禁用");

    printf("\n=== 0 再到 1：不自动切（只有从 >1 掉下来才切）===\n");
    page.setMemberDevicesForTest({ "/dev/sda1" });
    check(iqn->isChecked(), "0→1 不自动切 SMB");

    printf("\n=== 预填路径必须匹配成员自己的挂载点 ===\n");
    // 用真实设备名：/dev/sdb1 挂 /mnt/d，/dev/sda1 挂 /mnt/e。
    // 若预填沿用了上一次的残留，就会出现「高亮 sdb1 却共享 /mnt/e」
    // 这种把错误分区暴露到局域网的情况。
    for (QLineEdit *e : page.findChildren<QLineEdit *>()) {
        if (e->placeholderText().contains("挂载点")) e->clear();
        if (e->placeholderText().contains("nas-share")) e->clear();
    }
    page.setMemberDevicesForTest({});
    page.setMemberDevicesForTest({ "/dev/sda1", "/dev/sdb1" });
    page.setMemberDevicesForTest({ "/dev/sdb1" });
    for (int i = 0; i < 4; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
    QString pathShown, nameShown;
    for (QLineEdit *e : page.findChildren<QLineEdit *>()) {
        if (e->placeholderText().contains("挂载点")) pathShown = e->text();
        if (e->placeholderText().contains("nas-share")) nameShown = e->text();
    }
    check(pathShown == "/mnt/d",
          QString("成员 /dev/sdb1 预填其自身挂载点 -> %1（应为 /mnt/d）").arg(pathShown));
    check(!nameShown.isEmpty(), QString("共享名已预填 -> %1").arg(nameShown));
    printf("\n=== 标签页：文件共享服务 / 扩展服务 ===\n");
    {
        const auto tabs = page.findChildren<QTabWidget *>();
        check(tabs.size() == 1, QString("导出区有且只有一个标签组 -> %1").arg(tabs.size()));
        if (tabs.size() == 1) {
            QTabWidget *t = tabs.first();
            check(t->count() == 2, QString("两个标签 -> %1").arg(t->count()));
            QStringList names;
            for (int i = 0; i < t->count(); ++i) names << t->tabText(i);
            check(names.contains("文件共享服务"), "标签一：文件共享服务");
            check(names.contains("扩展服务"), "标签二：扩展服务");
            // 默认停在导出页：扩展是可选能力，不该一进来就占住主视野
            check(t->currentIndex() == 0,
                  QString("默认停在导出页 -> %1").arg(t->currentIndex()));
            // 切到扩展页后，导出用的控件仍在（只是不可见），不能被销毁
            t->setCurrentIndex(1);
            const bool iqnAlive = !page.findChildren<QLineEdit *>().isEmpty();
            check(iqnAlive, "切到扩展页后导出控件仍然存在");
            const auto tables = page.findChildren<QTableWidget *>();
            check(tables.size() == 1, QString("电视墙表格存在 -> %1").arg(tables.size()));
            if (tables.size() == 1)
                check(tables.first()->columnCount() == 4,
                      QString("四列（标题/季集/来源/文件名）-> %1").arg(tables.first()->columnCount()));
            t->setCurrentIndex(0);
        }
    }

    printf("\n=== 电视墙扫描必须只读 ===\n");
    {
        // 没选分区就点扫描：要给出明确指引，而不是扫 / 或者静默无反应
        page.setMemberDevicesForTest({});
        const auto btns = page.findChildren<QPushButton *>();
        QPushButton *scan = nullptr;
        for (QPushButton *b : btns) if (b->text().contains("扫描")) scan = b;
        check(scan != nullptr, "找到扫描按钮");
        if (scan) {
            scan->click();
            for (int i = 0; i < 4; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
            const auto lbls = page.findChildren<QLabel *>();
            bool guided = false;
            for (QLabel *l : lbls)
                if (l->text().contains("先在上方点选") || l->text().contains("没有挂载点")) guided = true;
            check(guided, "未选分区时给出明确指引（不去扫 /）");
        }
    }
    // 存一张「SMB 已激活」的截图：状态由上面 2→1 自动切来，不是硬摆的
    page.setMemberDevicesForTest({});
    page.setMemberDevicesForTest({ "/dev/sdb1", "/dev/sda1" });
    page.setMemberDevicesForTest({ "/dev/sdb1" });   // 2->1，自动切 SMB
    for (int i = 0; i < 6; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 40);
    page.resize(1208, 800);
    page.grab().save("build/nas_smb_state.png");
    printf("\n   已存 build/nas_smb_state.png（SMB 激活态）\n");
    printf("\n%s (%d 失败)\n", fail ? "!! 有失败" : "OK 全部通过", fail);
    return fail ? 1 : 0;
}
