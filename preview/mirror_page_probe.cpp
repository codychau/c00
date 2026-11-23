// 镜像引擎的**页面行为**断言（离线，offscreen）。
//
// 模型层的判据由 mirror_probe 覆盖；这里管的是"界面上看到的东西和点下去
// 会发生的事"：
//   · 选了镜像引擎，出口只剩 SMB —— iSCSI 与"挂载目录"必须置灰，
//     否则用户会去点一条注定失败的路，而失败原因是"它给的是目录不是块设备"
//   · 两份副本的路径必须显示出来、只读，且等于 <挂载点>/<卷名>.nas-mirror
//   · 两块盘是同一块时红字必须出现（零冗余）
//   · 访问切到副本网时，共享强制只读（否则访客写的东西下一轮被同步覆盖掉）
//   · 成员数不是 2 时「① 创建阵列」必须点不动
//   · 「②」不能只在导过 iSCSI 之后才亮（那是原来纯 SMB 用法点不动的根因）
//
// 盘是灌进去的假盘（setDisksForTest）：本机所有分区都在同一块系统盘上，
// "两份在不同盘"这条正常路径在真机上根本不会出现，靠真机状态断言等于没测。
#include "naspage.h"
#include "nasboardwidget.h"
#include "theme.h"

#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QEventLoop>
#include <cstdlib>
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
static QLineEdit *pathEdit(NasPage *p)
{
    for (QLineEdit *e : p->findChildren<QLineEdit *>())
        if (e->placeholderText().contains(QString::fromUtf8("挂载点"))) return e;
    return nullptr;
}
static QCheckBox *writeBox(NasPage *p)
{
    for (QCheckBox *b : p->findChildren<QCheckBox *>())
        if (b->text().contains(QString::fromUtf8("允许写入"))) return b;
    return nullptr;
}
static QPushButton *btn(NasPage *p, const char *name)
{
    return p->findChild<QPushButton *>(name);
}

static NasBoardWidget::Disk disk(const QString &dev, const QString &mp, qint64 bytes)
{
    NasBoardWidget::Disk d;
    d.device = dev;
    d.label = dev.section('/', -1);
    d.mountpoint = mp;
    d.fstype = "ext4";
    d.bytes = bytes;
    d.freeBytes = bytes / 2;
    d.selectable = true;
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
    QLabel *primary = page.findChild<QLabel *>("mirrorPrimary");
    QLabel *secondary = page.findChild<QLabel *>("mirrorSecondary");
    QComboBox *access = page.findChild<QComboBox *>("mirrorAccess");
    QLabel *warn = page.findChild<QLabel *>("mirrorWarn");
    QPushButton *syncBtn = btn(&page, "mirrorSyncBtn");
    QRadioButton *rIscsi = radio(&page, "iSCSI");
    QRadioButton *rSmb = radio(&page, "SMB");
    QRadioButton *rMount = radio(&page, "挂载");
    QLineEdit *smbPath = pathEdit(&page);
    QCheckBox *smbWrite = writeBox(&page);
    QPushButton *create = nullptr, *exportBtn = nullptr;
    for (QPushButton *b : page.findChildren<QPushButton *>()) {
        if (b->text().contains(QString::fromUtf8("创建阵列"))) create = b;
        if (b->text().startsWith(QString::fromUtf8("②"))) exportBtn = b;
    }

    check(eng != nullptr, "引擎下拉里有三条路线（第三条是镜像）");
    check(primary && secondary && access && warn && syncBtn, "镜像那格的控件都在");
    check(rIscsi && rSmb && rMount && smbPath && smbWrite && create && exportBtn,
          "找得到单选、共享路径、可写、① 与 ②");
    if (!eng || !primary || !access || !rIscsi || !smbPath) {
        fprintf(stderr, "\n前置控件没找到，后面的断言没有意义\n");
        return 1;
    }

    // 默认引擎下镜像那格不该露脸
    check(!primary->isVisible(), "默认（mdadm）时镜像格是隐藏的");

    eng->setCurrentIndex(2);
    pump();

    check(primary->isVisible(), "切到镜像引擎后，镜像格显示出来了");
    check(rIscsi->isEnabled() == false, "iSCSI 出口置灰（镜像给的是目录，没有块设备可导）");
    check(rMount->isEnabled() == false, "「挂载目录」出口置灰（那是 RAID0 卷的出口）");
    check(rSmb->isChecked(), "出口自动落在 SMB 上（唯一说得通的一条）");
    check(smbPath->isReadOnly(), "共享路径变成只读：它由「访问哪一份」决定，不让手输打架");

    // 灌两片"不同盘"的分区，再选为成员
    page.setDisksForTest({ disk("/dev/vdb1", "/mnt/d1", 2LL << 40),
                           disk("/dev/vdc1", "/mnt/d2", 2LL << 40) });
    page.setMemberDevicesForTest({ "/dev/vdb1", "/dev/vdc1" });
    // 卷名要填：副本目录名由它推
    for (QLineEdit *e : page.findChildren<QLineEdit *>())
        if (e->placeholderText().contains("nas-data")) { e->setText("nas"); break; }
    pump();

    check(primary->text().contains("/mnt/d1/nas.nas-mirror"),
          QString("主副本路径显示正确：%1").arg(primary->text()));
    check(secondary->text().contains("/mnt/d2/nas.nas-mirror"),
          QString("副本路径显示正确：%1").arg(secondary->text()));
    check(primary->text().contains("vdb1") && secondary->text().contains("vdc1"),
          "两条路径各自带上所在分区名（两条路径长得几乎一样，不标会认错）");
    check(warn->isVisible() == false, "两块不同的盘：没有红字");
    check(create->isEnabled(), "正好 2 个成员：① 创建阵列可点");
    check(exportBtn->isEnabled(), "有成员时 ② 就是可点的（原来只有导过 iSCSI 才亮）");
    check(smbPath->text() == "/mnt/d1/nas.nas-mirror", "默认访问主副本 → 共享指向主副本");
    check(smbWrite->isEnabled() && smbWrite->isChecked(),
          "访问主副本时可写开关正常可用（默认勾选）");

    // 同盘组合：这是"看着有冗余其实没有"的那一类，必须当场红字
    page.setDisksForTest({ disk("/dev/vdb1", "/mnt/d1", 2LL << 40),
                           disk("/dev/vdb2", "/mnt/d2", 2LL << 40) });
    page.setMemberDevicesForTest({ "/dev/vdb1", "/dev/vdb2" });
    pump();
    check(warn->isVisible(), "两块盘同一块时红字出现");
    check(warn->text().contains(QString::fromUtf8("一起没")),
          "红字说的是后果（两份一起没），不是「参数不合法」");
    check(syncBtn->isEnabled() == false, "同盘组合下「同步到副本」不可点");

    // 回到两盘，测访问切换
    page.setDisksForTest({ disk("/dev/vdb1", "/mnt/d1", 2LL << 40),
                           disk("/dev/vdc1", "/mnt/d2", 2LL << 40) });
    page.setMemberDevicesForTest({ "/dev/vdb1", "/dev/vdc1" });
    pump();

    access->setCurrentIndex(1);
    pump();
    check(smbPath->text() == "/mnt/d2/nas.nas-mirror", "访问切到副本网 → 共享路径跟着换过去");
    check(smbWrite->isEnabled() == false && smbWrite->isChecked() == false,
          "访问副本网时强制只读（访客写的会被下一轮同步盖掉，那比不给写更坏）");

    access->setCurrentIndex(0);
    pump();
    check(smbPath->text() == "/mnt/d1/nas.nas-mirror", "切回主副本 → 路径与可写都回来");
    check(smbWrite->isEnabled(), "可写开关恢复可用");

    // 成员数：1 个与 3 个都不该能建
    page.setDisksForTest({ disk("/dev/vdb1", "/mnt/d1", 2LL << 40),
                           disk("/dev/vdc1", "/mnt/d2", 2LL << 40),
                           disk("/dev/vdd1", "/mnt/d3", 2LL << 40) });
    page.setMemberDevicesForTest({ "/dev/vdb1" });
    pump();
    check(create->isEnabled() == false, "只有 1 个成员：① 点不动（没有副本）");
    page.setMemberDevicesForTest({ "/dev/vdb1", "/dev/vdc1", "/dev/vdd1" });
    pump();
    check(create->isEnabled() == false, "3 个成员：① 点不动（一主一备是定义，不是参数）");

    // 换走引擎后，只读要还回去：留着只读会让用户以为 SMB 也不能填了
    eng->setCurrentIndex(0);
    pump();
    check(smbPath->isReadOnly() == false, "换回 mdadm 后共享路径恢复可编辑");
    check(primary->isVisible() == false, "换回 mdadm 后镜像格收起");

    fprintf(stderr, "\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
