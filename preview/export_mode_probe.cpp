// 出口切换的**可达性**：每个引擎 × 每个成员数，三条出口该不该点得动、
// 点下去停不停得住。
//
// 这里钉的是两类真出过的事：
//   1) 判据写死（「②」只在导过 iSCSI 后才亮 → 纯 SMB 用法永远点不动）；
//   2) **点不动却没处说为什么**：主题没写 :disabled，禁用的单选框画出来和
//      能点的一模一样，用户只能反复点同一格，界面一个字都不解释。
//      报上来的原话是"选了两个分区，只能到 SMB，没法切到 RAID 挂载页，
//      点了单选框没有反应"。
//
// 离线跑：盘是灌进去的假盘（真机所有分区都在同一块系统盘上）。
#include "naspage.h"
#include "nasboardwidget.h"
#include "theme.h"

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QRadioButton>
#include <QWidget>
#include <QImage>
#include <QPixmap>
#include <QEventLoop>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool c, const QString &w)
{
    if (c) { ++g_pass; fprintf(stderr, "  ✓ %s\n", qPrintable(w)); }
    else   { ++g_fail; fprintf(stderr, "  ✗ %s\n", qPrintable(w)); }
    fflush(stderr);
}
static void pump(int rounds = 6)
{
    for (int i = 0; i < rounds; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
}

static QComboBox *engineCombo(NasPage *p)
{
    for (QComboBox *c : p->findChildren<QComboBox *>())
        if (c->count() == 3 && c->itemText(2).contains(QString::fromUtf8("镜像"))) return c;
    return nullptr;
}
static QRadioButton *radio(NasPage *p, const QString &text)
{
    for (QRadioButton *r : p->findChildren<QRadioButton *>())
        if (r->text().contains(text)) return r;
    return nullptr;
}
static NasBoardWidget::Disk disk(const QString &dev, const QString &mp)
{
    NasBoardWidget::Disk d;
    d.device = dev; d.label = dev.section('/', -1); d.mountpoint = mp;
    d.fstype = "ext4"; d.bytes = 2LL << 40; d.freeBytes = 1LL << 40; d.selectable = true;
    return d;
}

int main(int argc, char **argv)
{
    setvbuf(stderr, nullptr, _IONBF, 0);
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setStyleSheet(Theme::globalStyleSheet());

    NasPage page;
    page.resize(1000, 900);
    page.show();
    pump();

    QComboBox *eng = engineCombo(&page);
    QRadioButton *rIscsi = radio(&page, "iSCSI 块存储");
    QRadioButton *rSmb   = radio(&page, "SMB 文件共享");
    QRadioButton *rMount = radio(&page, "RAID 挂载目录");
    check(eng && rIscsi && rSmb && rMount, "引擎下拉与三个出口单选都找得到");
    if (!eng || !rIscsi || !rSmb || !rMount) return 1;

    page.setDisksForTest({ disk("/dev/vdb1", "/mnt/d1"), disk("/dev/vdc1", "/mnt/d2") });
    pump();

    // ── mdadm + 2 个成员：挂载目录必须点得动，而且点下去要停得住 ──
    eng->setCurrentIndex(0);
    page.setMemberDevicesForTest({ "/dev/vdb1", "/dev/vdc1" });
    pump();
    check(rMount->isEnabled(), "mdadm+2 盘：「RAID 挂载目录」可点");
    check(rSmb->isEnabled() == false, "mdadm+2 盘：SMB 不可点（Samba 不跨盘条带）");
    rMount->click();                       // click() 等价于用户按下再松开
    pump();
    check(rMount->isChecked(), "mdadm+2 盘：点「RAID 挂载目录」真的切过去了");
    rIscsi->click();
    pump();
    check(rIscsi->isChecked(), "mdadm+2 盘：点回 iSCSI 也切得动（不是单向自动切）");

    // ── mdadm + 1 个成员：只剩 SMB 与 iSCSI ──
    page.setMemberDevicesForTest({ "/dev/vdb1" });
    pump();
    check(rSmb->isEnabled(), "mdadm+1 盘：SMB 可点");
    check(rMount->isEnabled() == false, "mdadm+1 盘：挂载目录不可点（少于 2 个成员 nasrs 自己会拒）");
    rSmb->click();
    pump();
    check(rSmb->isChecked(), "mdadm+1 盘：点 SMB 切得动");

    // ── 镜像引擎 + 2 个成员：出口只有 SMB，另外两条是**故意**点不动的 ──
    eng->setCurrentIndex(2);
    page.setMemberDevicesForTest({ "/dev/vdb1", "/dev/vdc1" });
    pump();
    check(rSmb->isChecked(), "镜像+2 盘：出口自动落在 SMB");
    check(rMount->isEnabled() == false, "镜像+2 盘：挂载目录不可点（镜像给的是目录，不是块设备）");
    check(rIscsi->isEnabled() == false, "镜像+2 盘：iSCSI 不可点（没有块设备可导）");
    rMount->click();
    pump();
    check(rSmb->isChecked() && rMount->isChecked() == false,
          "镜像+2 盘：点挂载目录不改变出口（这条是设计，不是坏掉）");

    // ── 但"点不动"必须看得出来、也必须说清为什么 —— 这是报上来的真问题 ──
    check(Theme::globalStyleSheet().contains(":disabled"),
          "主题里有 :disabled 规则（否则禁用控件画出来和能点的一样）");
    const QImage on_ = rMount->grab().toImage();
    rMount->setEnabled(true);
    const QImage off_ = rMount->grab().toImage();
    check(on_.copy(0, 0, 18, on_.height()) != off_.copy(0, 0, 18, off_.height()),
          "禁用的单选框**指示灯**画出来和能点的不同（左侧 18px 是指示圈）");
    rMount->setEnabled(false);

    check(!rMount->toolTip().isEmpty(), "点不动的那条出口带 tooltip 说明原因");
    check(!rIscsi->toolTip().isEmpty(), "iSCSI 置灰时也带 tooltip 说明原因");
    QLabel *hint = nullptr;
    for (QLabel *l : page.findChildren<QLabel *>())
        if (l->text().contains(QString::fromUtf8("镜像引擎"))) hint = l;
    check(hint && hint->text().contains(QString::fromUtf8("只有 SMB")),
          "提示语直接写明镜像的出口只有 SMB（不只靠置灰表达）");

    fprintf(stderr, "\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
