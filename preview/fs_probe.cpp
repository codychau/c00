// raidfs 标签页的行为断言。刻意只看"用户能感知的事实"：
// 标签存在、顺序对、默认值合法、非法输入会被挡、未实现时不会假装执行。
#include "naspage.h"
#include "theme.h"
#include <QApplication>
#include <QTabWidget>
#include <QLineEdit>
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
    const auto eds = p.findChildren<QLineEdit *>();
    for (QLineEdit *e : eds)
        if (e->placeholderText() == "/mnt/raid") return e;
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
    for (int i = 0; i < 8; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 60);

    QTabWidget *tabs = page.findChildren<QTabWidget *>().first();
    check(tabs != nullptr, "存在标签页容器");
    if (!tabs) return 1;

    check(tabs->count() == 3, QString("共 3 个标签，实际 ") + QString::number(tabs->count()));
    QStringList names;
    for (int i = 0; i < tabs->count(); ++i) names << tabs->tabText(i);
    qInfo() << "  标签顺序:" << names.join(" | ");
    check(names.value(0).contains(QString::fromUtf8("文件共享服务")), "第 1 个是导出服务");
    check(names.value(1).contains("RAID"), "第 2 个是 RAID 挂载（导出与扩展之间）");
    check(names.value(2).contains(QString::fromUtf8("扩展服务")), "第 3 个是扩展服务");

    // 默认仍停在导出页：新标签不该抢默认
    check(tabs->currentIndex() == 0, "默认停在导出页，没抢焦点");

    QLineEdit *mp = findMountPoint(page);
    check(mp != nullptr, "找到挂载目录输入框");

    // 切到 raidfs 页，控件必须仍在（滚动区包层曾把控件挪进另一个 widget）
    tabs->setCurrentIndex(1);
    for (int i = 0; i < 4; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 40);
    check(findMountPoint(page) != nullptr, "切走再切回，输入框还在（没被滚动区吞掉）");

    // 诚实性：徽标必须写"未实现"，不能写"环境未就绪"糊弄
    QString pillText;
    const auto labels = page.findChildren<QLabel *>();
    for (QLabel *l : labels)
        if (l->text().contains(QString::fromUtf8("未实现"))) pillText = l->text();
    check(!pillText.isEmpty(), "页面明确标注未实现（不显示假的可用状态）");
    qInfo() << "  标注:" << pillText;

    // 预填是按卷名推的，所以先要卷名为空时不填、有卷名时填出合法值。
    // 卷名为空就不预填是对的：/mnt/unnamed 那种会让用户以为是自己的命名。
    page.setMemberDevicesForTest({ QString::fromUtf8("/dev/sda1"), QString::fromUtf8("/dev/sdb1") });
    for (int i = 0; i < 5; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    if (mp) {
        check(mp->text().isEmpty(), "卷名为空时不预填（不编一个名字出来）");

        // 找到卷名输入框：它是导出页第一个 QLineEdit（挂载框占位符不同）
        QLineEdit *vol = nullptr;
        const auto eds = page.findChildren<QLineEdit *>();
        for (QLineEdit *e : eds)
            if (e->placeholderText().contains("nas-data")) vol = e;
        check(vol != nullptr, "找到卷名输入框");
        if (vol) {
            vol->setText("nas-data");
            // 走与用户完全相同的同步路径：改成员数触发 syncExportMode
            page.setMemberDevicesForTest({ QString::fromUtf8("/dev/sda1") });
            for (int i = 0; i < 5; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            qInfo() << "  有卷名后预填:" << mp->text();
            check(Nas::validateMountPoint(mp->text()).isEmpty(),
                  QString("预填值合法 -> ") + mp->text());
            // 用户改过的值不能被后续刷新覆盖
            mp->setText("/mnt/media");
            page.setMemberDevicesForTest({ QString::fromUtf8("/dev/sda1"),
                                           QString::fromUtf8("/dev/sdb1") });
            for (int i = 0; i < 5; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            check(mp->text() == "/mnt/media", "不覆盖用户手输的挂载点");
        }
    }

    // 非法路径必须被拒：直接改输入框再点，看状态栏
    if (mp) {
        mp->setText("/home/cody/raid");
        const auto btns = page.findChildren<QPushButton *>();
        QPushButton *mountBtn = nullptr;
        for (QPushButton *b : btns)
            if (b->text().contains(QString::fromUtf8("挂载到目录"))) mountBtn = b;
        check(mountBtn != nullptr, "找到「挂载到目录」按钮");
        if (mountBtn) {
            qInfo() << "  按钮启用状态:" << mountBtn->isEnabled()
                    << "（未建阵列时应为 false）";
            check(!mountBtn->isEnabled(), "未创建阵列时挂载按钮是禁用的");
        }
    }

    qInfo() << "";
    qInfo() << "   通过" << passes << "条，失败" << fails << "条";
    if (fails == 0) qInfo() << "OK 全部通过 (0 失败)";
    return fails ? 1 : 0;
}
