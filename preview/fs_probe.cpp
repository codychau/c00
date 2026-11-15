// 出口三合一后的行为断言。重点：挂载目录不再是标签，而是导出页里和
// SMB 并列的第三个出口单选。断言只盯"用户能感知的事实"。
#include "naspage.h"
#include "theme.h"
#include <QApplication>
#include <QTabWidget>
#include <QLineEdit>
#include <QRadioButton>
#include <QLabel>
#include <QPushButton>
#include <QDebug>

static int fails = 0, passes = 0;
static void check(bool ok, const QString &what)
{
    if (ok) { ++passes; qInfo() << "  OK  " << what; }
    else    { ++fails; qInfo() << "  !!" << what; }
}
static QLineEdit *findMountPoint(NasPage &p)
{
    for (QLineEdit *e : p.findChildren<QLineEdit *>())
        if (e->placeholderText() == "/mnt/raid") return e;
    return nullptr;
}
static QRadioButton *radio(NasPage &p, const QString &name)
{
    for (QRadioButton *r : p.findChildren<QRadioButton *>())
        if (r->text() == name) return r;
    return nullptr;
}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setStyleSheet(Theme::globalStyleSheet());
    NasPage page;
    page.resize(1208, 900);
    page.show();
    for (int i = 0; i < 8; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 60);

    QTabWidget *tabs = page.findChildren<QTabWidget *>().first();
    check(tabs != nullptr, "存在标签页容器");
    if (!tabs) return 1;

    // 关键变化：只剩 2 个标签，挂载目录已并入导出页
    check(tabs->count() == 2, QString("共 2 个标签，实际 ") + QString::number(tabs->count()));
    QStringList names;
    for (int i = 0; i < tabs->count(); ++i) names << tabs->tabText(i);
    qInfo() << "  标签顺序:" << names.join(" | ");
    check(names.value(0).contains(QString::fromUtf8("文件共享服务")), "第 1 个是导出页");
    check(names.value(1).contains(QString::fromUtf8("扩展服务")), "第 2 个是扩展服务");

    // 三个出口单选都在导出页里，且互斥
    QRadioButton *mi = radio(page, "iSCSI 块存储");
    QRadioButton *ms = radio(page, "SMB 文件共享");
    QRadioButton *mm = radio(page, "RAID 挂载目录");
    check(mi && ms && mm, "导出页里有三个出口单选");
    check(mi && mi->isChecked(), "默认仍选 iSCSI（不擅自改默认）");

    // 挂载单选在 0 成员时应禁用（RAID 需 ≥2）
    check(mm && !mm->isEnabled(), "0 个成员时「挂载目录」禁用");

    // 选 2 个分区：挂载应解禁、SMB 应禁用、且自动切到挂载
    page.setMemberDevicesForTest({ QString::fromUtf8("/dev/sda1"), QString::fromUtf8("/dev/sdb1") });
    for (int i = 0; i < 5; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    check(mm && mm->isEnabled(), "2 个成员时「挂载目录」解禁");
    check(ms && !ms->isEnabled(), "2 个成员时 SMB 禁用（不跨盘条带）");
    check(mm && mm->isChecked(), "升到 2 个成员自动切到挂载出口");

    // 切到挂载出口后，挂载输入框应可见（互斥显隐生效）
    QLineEdit *mp = findMountPoint(page);
    check(mp != nullptr, "找到挂载目录输入框");
    if (mp) check(mp->isVisible(), "选中挂载出口时输入框可见");

    // 掉回 1 个分区：挂载失效回落到 iSCSI，SMB 解禁
    page.setMemberDevicesForTest({ QString::fromUtf8("/dev/sda1") });
    for (int i = 0; i < 5; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    check(mm && !mm->isEnabled(), "1 个成员时「挂载目录」禁用");
    // 掉到 1 盘：SMB 变合法，按 turn-27 的约定自动切 SMB；总之不能再停在挂载出口
    check(mm && !mm->isChecked(), "挂载失效后不再停在挂载出口");
    check(ms && ms->isChecked(), "掉到 1 盘自动切到 SMB（turn-27 约定）");
    check(ms && ms->isEnabled(), "1 个成员时 SMB 解禁");

    // 诚实性：未实现徽标必须在（不显示假的可用状态）
    QString pillText;
    for (QLabel *l : page.findChildren<QLabel *>())
        if (l->text().contains(QString::fromUtf8("未实现"))) pillText = l->text();
    check(!pillText.isEmpty(), "页面标注「引擎未实现」（不假装能用）");

    // 预填：给卷名后切到挂载出口，预填值应合法
    QLineEdit *vol = nullptr;
    for (QLineEdit *e : page.findChildren<QLineEdit *>())
        if (e->placeholderText().contains("nas-data")) vol = e;
    check(vol != nullptr, "找到卷名输入框");
    if (vol && mp) {
        vol->setText("nas-data");
        page.setMemberDevicesForTest({ QString::fromUtf8("/dev/sda1"), QString::fromUtf8("/dev/sdb1") });
        for (int i = 0; i < 5; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        qInfo() << "  预填挂载点:" << mp->text();
        check(Nas::validateMountPoint(mp->text()).isEmpty(),
              QString("预填值合法 -> ") + mp->text());
        mp->setText("/mnt/media");
        page.setMemberDevicesForTest({ QString::fromUtf8("/dev/sda1") });
        page.setMemberDevicesForTest({ QString::fromUtf8("/dev/sda1"), QString::fromUtf8("/dev/sdb1") });
        for (int i = 0; i < 5; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        check(mp->text() == "/mnt/media", "不覆盖用户手输的挂载点");
    }

    // 三选一互斥：SMB 只有在 1 盘时才可用，所以先把成员降到 1，SMB 才 enabled，
    // click 才生效（disabled 的 radio click 不切状态，之前在这里踩了一次）。
    if (ms && mp) {
        page.setMemberDevicesForTest({ QString::fromUtf8("/dev/sda1") });
        for (int i = 0; i < 4; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 40);
        // 此时按 turn-27 约定会自动切到 SMB，挂载框应随之隐藏
        check(ms->isChecked(), "1 盘时已切到 SMB");
        check(!mp->isVisible(), "SMB 可见时挂载输入框隐藏");
        // 手动点回挂载不该生效（挂载在 1 盘下 disabled）—— 互斥 + 门禁都在
        mm->click();
        for (int i = 0; i < 4; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 40);
        check(!mm->isChecked(), "1 盘时点挂载无效（disabled 拦住）");
    }

    qInfo() << "";
    qInfo() << "   通过" << passes << "条，失败" << fails << "条";
    if (fails == 0) qInfo() << "OK 全部通过 (0 失败)";
    return fails ? 1 : 0;
}
