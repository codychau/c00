// 页顶「一键安装依赖的组件」的离线探针。
//
// 这一条链的终点是 root：判定用哪家包管理器、装哪些包、拼出什么脚本，
// 全部可以离线断言，所以没有理由不等它跑绿就去点那个按钮。
//
// 构建（本机 Qt6 在容器镜像层里时，见 README 末尾的说明）：
//   moc naspage.h        -o moc_naspage.cpp        -I.
//   moc nascontroller.h  -o moc_nascontroller.cpp  -I.
//   moc nasboardwidget.h -o moc_nasboardwidget.cpp -I.
//   g++ -std=c++17 -fPIC -I. <Qt6 头> preview/install_all_probe.cpp \
//       naspage.cpp nasboardwidget.cpp nasmodel.cpp nascontroller.cpp theme.cpp \
//       moc_naspage.cpp moc_nascontroller.cpp moc_nasboardwidget.cpp \
//       -lQt6Widgets -lQt6Gui -lQt6Core -o /tmp/install_all_probe
//   QT_QPA_PLATFORM=offscreen /tmp/install_all_probe
#include "naspage.h"
#include "nascontroller.h"
#include "nasmodel.h"
#include "theme.h"

#include <QApplication>
#include <QAbstractButton>
#include <QComboBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>

#include <cstdio>

// 输出直接走 stderr，不用 qInfo：镜像里的 Qt 是 release 构建，日志规则一
// 收紧，qInfo 就会被整条吞掉 —— 探针"什么都不打印"和"一条都没跑"看起来
// 一模一样，那是最坑人的失败方式。
static void say(const QString &line)
{
    fputs(line.toUtf8().constData(), stderr);
    fputc('\n', stderr);
}

static int fails = 0, passes = 0;
static void check(bool ok, const QString &what)
{
    if (ok) { ++passes; say("  OK   " + what); }
    else    { ++fails; say("  !!   " + what); }
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setStyleSheet(Theme::globalStyleSheet());

    // ── 1) 用哪家包管理器：真值表 ────────────────────────────
    const QString debian   = "ID=ubuntu\nID_LIKE=debian\nPRETTY_NAME=\"Ubuntu 26.04\"\n";
    const QString debianNoLike = "ID=debian\n";
    const QString fedora   = "ID=fedora\n";
    const QString euler    = "ID=\"openEuler\"\nID_LIKE=\"openEuler\"\n";
    const QString arch     = "ID=arch\nPRETTY_NAME=\"Arch Linux\"\n";
    const QString manjaro  = "ID=manjaro\nID_LIKE=arch\n";
    const QString suse     = "ID=opensuse-leap\n";
    const QString weird    = "";

    check(Nas::detectPkgManager(debian, true, false, false) == Nas::Pkg::Apt,
          "ubuntu + apt-get 在 -> Apt");
    check(Nas::detectPkgManager(debianNoLike, true, false, false) == Nas::Pkg::Apt,
          "debian（无 ID_LIKE）+ apt-get -> Apt");
    check(Nas::detectPkgManager(debian, false, true, false) == Nas::Pkg::Dnf,
          "ubuntu 但只有 dnf -> 退回「有哪个用哪个」");
    check(Nas::detectPkgManager(fedora, false, true, false) == Nas::Pkg::Dnf,
          "fedora + dnf -> Dnf");
    check(Nas::detectPkgManager(euler, false, true, false) == Nas::Pkg::Dnf,
          "openEuler + dnf -> Dnf（本工具的主场之一）");
    check(Nas::detectPkgManager(arch, true, false, true) == Nas::Pkg::Pacman,
          "arch 上同时装了 apt 也仍选 pacman（归属优先）");
    check(Nas::detectPkgManager(manjaro, false, false, true) == Nas::Pkg::Pacman,
          "manjaro（ID_LIKE=arch）-> Pacman");
    check(Nas::detectPkgManager(suse, false, false, false) == Nas::Pkg::None,
          "openSUSE 没装 dnf -> None（zypper 本工具不支持，不猜）");
    check(Nas::detectPkgManager(weird, false, false, false) == Nas::Pkg::None,
          "os-release 读不到 / 三家都不在 -> None");

    check(Nas::pkgManagerName(Nas::Pkg::Apt) == "apt-get", "Apt 显示成 apt-get（真跑的那个程序）");
    check(Nas::pkgManagerName(Nas::Pkg::None).isEmpty(), "None 没有名字");

    // ── 2) 缺什么 → 装哪些包 ────────────────────────────────
    const QStringList allMissing = { "mdadm", "targetcli", "fusermount3", "smbd" };

    check(Nas::packagesToInstall(Nas::Pkg::Apt, allMissing)
              == QStringList({ "mdadm", "targetcli-fb", "fuse3", "samba", "samba-common-bin" }),
          "apt 的包名：targetcli-fb 与 samba-common-bin 都是 Debian/Ubuntu 的叫法");
    check(Nas::packagesToInstall(Nas::Pkg::Dnf, allMissing)
              == QStringList({ "mdadm", "targetcli", "fuse3", "samba" }),
          "dnf 的包名：targetcli 不加 -fb");
    check(Nas::packagesToInstall(Nas::Pkg::Pacman, allMissing)
              == QStringList({ "mdadm", "fuse3", "samba" }),
          "pacman 少一个 targetcli（官方仓库没有，绝不塞 AUR 包）");
    check(!Nas::packagesUnavailable(Nas::Pkg::Pacman, allMissing).isEmpty()
          && Nas::packagesUnavailable(Nas::Pkg::Pacman, allMissing).first().contains("targetcli"),
          "pacman 下如实报告 targetcli 装不了");
    check(Nas::packagesUnavailable(Nas::Pkg::Apt, allMissing).isEmpty(),
          "apt 下没有装不了的项");
    check(Nas::packagesToInstall(Nas::Pkg::None, allMissing).isEmpty(),
          "None 一家都不发，什么都不生成");
    check(Nas::packagesToInstall(Nas::Pkg::Dnf, {}).isEmpty(),
          "不缺东西 -> 一个包都不装");
    check(Nas::packagesToInstall(Nas::Pkg::Dnf, { "mdadm", "mdadm" }).size() == 1,
          "重复缺失项不重复装");
    check(Nas::dependencyLabels(allMissing).size() == 4,
          "人话名与缺失项一一对应");

    // ── 3) 包名白名单 ───────────────────────────────────────
    check(Nas::isSafePackageName("samba-common-bin"), "普通包名通过");
    check(Nas::isSafePackageName("libssl3"), "数字结尾通过");
    check(Nas::isSafePackageName("glibc:amd64"), "多架构后缀通过");
    check(!Nas::isSafePackageName("mdadm; rm -rf /"), "分号 + 空格被拒");
    check(!Nas::isSafePackageName("a`id`"), "反引号被拒");
    check(!Nas::isSafePackageName("$X"), "美元符被拒");
    check(!Nas::isSafePackageName(""), "空名被拒");
    check(!Nas::isSafePackageName("-rf"), "以 - 开头被拒（会被当成参数）");

    // ── 4) 生成的脚本 ───────────────────────────────────────
    NasController ctl;
    {
        NasController::Env e;
        e.pkg = Nas::Pkg::Apt;
        e.missingDeps = { "mdadm", "targetcli" };
        const auto p = ctl.planInstallDeps(e);
        check(p.valid(), "apt：计划有效");
        check(p.script.contains("set -e"), "脚本带 set -e（一条失败就不带病继续）");
        check(p.script.contains("DEBIAN_FRONTEND=noninteractive"), "apt：非交互，不会弹窗卡住");
        check(p.script.contains("apt-get update"), "apt：先刷索引（索引过期是最常见的失败）");
        check(p.script.contains("apt-get install -y mdadm targetcli-fb"), "apt：装的就是解析出来的那两个");
        check(!p.script.contains("sudo"), "脚本里没有 sudo：提权由调用方加，只写要干的事");
        check(p.commands.size() == 2 && p.rows.size() == 2, "展示用的命令行与缺失项对得上");

        e.pkg = Nas::Pkg::Dnf;
        const auto d = ctl.planInstallDeps(e);
        check(d.script.contains("dnf install -y mdadm targetcli") && !d.script.contains("update"),
              "dnf：不额外刷索引（dnf 自己会按过期策略刷）");

        e.pkg = Nas::Pkg::Pacman;
        const auto k = ctl.planInstallDeps(e);
        check(k.script.contains("pacman -S --noconfirm --needed mdadm"), "pacman：--noconfirm + --needed");
        check(!k.script.contains("-Sy"), "pacman：不做 -Sy（部分刷新数据库是 Arch 明令反对的）");

        e.pkg = Nas::Pkg::None;
        check(!ctl.planInstallDeps(e).valid(), "None：什么计划都不给");

        e.pkg = Nas::Pkg::Dnf;
        e.missingDeps = { "targetcli" };            // dnf 有 targetcli，换成 pacman 就没有
        e.pkg = Nas::Pkg::Pacman;
        check(!ctl.planInstallDeps(e).valid() && !ctl.planInstallDeps(e).blocked.isEmpty(),
              "只缺「这家装不了」的东西 -> 计划无效，但 blocked 里说清缺谁");
    }

    // ── 5) 真机探测（本机有什么就是什么，不许假装）───────────
    {
        const auto env = ctl.probe();
        say(QString("  ·   本机识别到的包管理器: %1   缺: %2")
                .arg(Nas::pkgManagerName(env.pkg).isEmpty() ? "（认不出）" : Nas::pkgManagerName(env.pkg),
                     env.missingDepLabels.join("、")));
        check(true, "probe() 跑通且没崩");
        // 每个缺失项都必须有着落：要么给出包名，要么进 blocked 说明装不了。
        // 漏在中间的项会表现为「点了装完，还缺它，而且没人提过」——最难查。
        bool everyMissingAccountedFor = true;
        for (const QString &bin : env.missingDeps) {
            const bool installable = !Nas::packagesToInstall(env.pkg, { bin }).isEmpty();
            const bool blocked = !Nas::packagesUnavailable(env.pkg, { bin }).isEmpty();
            if (!installable && !blocked) everyMissingAccountedFor = false;
        }
        check(env.pkg == Nas::Pkg::None || everyMissingAccountedFor,
              "每个缺失项要么给包名，要么在 blocked 里说明");

        // 把「本机真会跑的那几条」打出来。断言只能证明逻辑，这一行是让
        // 人亲眼看看：在自己这台机器上点那个按钮，到底会发生什么。
        if (env.hasMissingDeps()) {
            const auto real = ctl.planInstallDeps(env);
            say(QString("  ·   本机真会执行（%1 提权）：").arg(real.pmName));
            for (const QString &c : real.commands)
                say("      # " + c);
            for (const QString &b : real.blocked)
                say("      · 装不了：" + b);
        } else {
            say("  ·   本机依赖已齐备，点那个按钮不会执行任何命令");
        }
    }

    // ── 6) 页面上那个按钮：位置、文案、宽度、显隐联动 ─────────
    {
        NasPage page;
        page.resize(1208, 900);
        page.show();
        for (int i = 0; i < 8; ++i)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 60);

        // 先拿到"本机此刻该有的状态"，再拿界面上的状态对：两者必须是同一件事。
        // 写死"按钮一定可见"会把探针变成看机器脸色的东西 —— 换台装齐了的机器
        // 它就假报错，而假报错的探针比没有探针更糟。
        const auto env = ctl.probe();
        const bool wantBtn  = env.pkg != Nas::Pkg::None && env.hasMissingDeps();
        // 右上角现在只说"缺组件"，与按钮同源；configfs 之类不再混进来
        const bool wantHint = !env.missingDepLabels.isEmpty();

        QPushButton *btn = nullptr;
        for (QPushButton *b : page.findChildren<QPushButton *>())
            if (b->text() == QString::fromUtf8("点击此按钮一键安装依赖的组件")) btn = b;
        check(btn != nullptr, "找到页顶按钮，文案一字不差（控件恒在，显隐由状态定）");

        QLabel *hint = nullptr;
        for (QLabel *l : page.findChildren<QLabel *>()) {
            const QString t = l->text();
            if (t.startsWith(QString::fromUtf8("缺少："))
                || t == QString::fromUtf8("检测环境…")
                || t == QString::fromUtf8("环境就绪"))
                hint = l;
        }

        if (btn) {
            check(btn->isVisible() == wantBtn,
                  QString("按钮随本机状态显隐：该显示=%1，实际=%2（缺 %3）")
                      .arg(wantBtn).arg(btn->isVisible()).arg(env.missingDepLabels.join("、")));
            check(btn->property("role").toString() == "install", "样式角色 install（橘黄来自 Theme）");
            // 高度：用户要"矮一半"。58 是初版实测值，这里要它矮于 36。
            check(btn->height() > 0 && btn->height() <= 36,
                  QString("按钮矮下来了：%1 px（初版 58）").arg(btn->height()));
            check(btn->width() >= page.width() - 24,
                  QString("仍是通栏：按钮宽 %1，页宽 %2").arg(btn->width()).arg(page.width()));
        }

        check(hint != nullptr && hint->isVisible() == wantHint,
              QString("右上角缺失提示随本机状态显隐：该显示=%1，实际=%2，文案「%3」")
                  .arg(wantHint)
                  .arg(hint ? hint->isVisible() : false)
                  .arg(hint ? hint->text() : QStringLiteral("（没找到该标签）")));

        // ── 6b) 装齐之后：按钮和提示都该消失。喂状态进去测，不靠运气 ──
        if (btn && hint) {
            // 导出页底部那条"装包补不了的"说明。按 objectName 找 —— 按文案找
            // 会在它空着（本机什么都不挡）时找不到控件，然后报成"行为不对"，
            // 而实际是"探针没找到人"。
            QLabel *note = page.findChild<QLabel *>("blockNote");
            check(note != nullptr, "导出页有「装包补不了的」那条说明控件");

            NasController::Env ready;              // 全齐 + 认得出包管理器
            ready.mdadm = ready.targetCli = ready.configfsMounted = true;
            ready.kernelModules = ready.losetup = ready.nasrsBinary = true;
            ready.fuseDevice = ready.fuseMount = true;
            ready.smbd = ready.testparm = ready.smbService = true;
            ready.pkg = Nas::Pkg::Dnf;
            page.setEnvForTest(ready);
            QCoreApplication::processEvents();
            check(!btn->isVisible(), "依赖全齐 -> 通栏按钮消失");
            check(!hint->isVisible(), "依赖全齐 -> 右上角缺失提示一起消失");
            check(!note || !note->isVisible(), "依赖全齐 -> 导出页那条说明也不该出现");

            // 用户实际撞到的那一步：包装齐了、按钮没了，但 configfs 还没挂。
            // 这时右上角**必须**闭嘴（它说的是"缺组件"，而组件不缺了），
            // 该说话的是导出页那条 —— 它给的是能直接抄的命令，不是"缺 xx"。
            NasController::Env stillLacking = ready;
            stillLacking.configfsMounted = false;
            page.setEnvForTest(stillLacking);
            QCoreApplication::processEvents();
            check(!btn->isVisible(), "包装齐了但 configfs 没挂 -> 按钮仍消失（它无能为力）");
            check(!hint->isVisible(), "…右上角不再提 configfs（它不是缺组件）");
            check(note && note->isVisible() && note->text().contains("configfs")
                      && note->text().contains("mount -t configfs"),
                  "…改由导出页说明，并给出现成的挂载命令");

            // configfs / target 模块是 LIO 的事：切到 SMB 还挂着这条就是无关噪音。
            // 先给一个成员 —— SMB 出口只在"正好 1 个成员"时合法，0 成员时
            // syncExportMode 会把单选强行弹回 iSCSI，那测的就不是切换了。
            page.setMemberDevicesForTest({ QStringLiteral("/dev/sdb1") });
            QCoreApplication::processEvents();
            QRadioButton *smbRadio = nullptr;
            QRadioButton *iscsiRadio = nullptr;
            for (QRadioButton *rb : page.findChildren<QRadioButton *>()) {
                if (rb->text().contains("SMB"))  smbRadio = rb;
                if (rb->text().contains("iSCSI")) iscsiRadio = rb;
            }
            if (smbRadio && iscsiRadio) {
                check(smbRadio->isEnabled(), "1 个成员时 SMB 出口可选（探针前提）");
                smbRadio->setChecked(true);
                QCoreApplication::processEvents();
                check(smbRadio->isChecked() && note && !note->isVisible(),
                      "切到 SMB 出口 -> configfs 那条收起（SMB 用不着它）");
                iscsiRadio->setChecked(true);
                QCoreApplication::processEvents();
                check(note && note->isVisible(), "切回 iSCSI -> 那条又回来");
            } else {
                check(false, "找得到 SMB / iSCSI 出口的单选按钮");
            }
            page.setMemberDevicesForTest({});
            QCoreApplication::processEvents();

            NasController::Env needInstall = ready;    // 又缺东西了 -> 自己回来
            needInstall.mdadm = false;
            needInstall.targetCli = false;
            needInstall.missingDeps = { "mdadm", "targetcli" };
            needInstall.missingDepLabels = Nas::dependencyLabels(needInstall.missingDeps);
            page.setEnvForTest(needInstall);
            QCoreApplication::processEvents();
            check(btn->isVisible(), "又缺东西了 -> 按钮自己回来");
            check(hint->isVisible(), "…右上角提示也回来");
            check(hint->text().contains("mdadm") && !hint->text().contains("configfs"),
                  "…右上角只列缺的组件，不掺装包补不了的那些");

            NasController::Env noPkgMgr = needInstall; // 认不出包管理器
            noPkgMgr.pkg = Nas::Pkg::None;
            page.setEnvForTest(noPkgMgr);
            QCoreApplication::processEvents();
            check(!btn->isVisible(), "认不出包管理器 -> 不摆一个点了只会失败的按钮");
            check(hint->isVisible() && hint->text().contains("dnf"),
                  "…但提示里说明得自己装");

            page.setEnvForTest(env);   // 收尾还原成本机真实状态，别把假状态留给后面
            QCoreApplication::processEvents();
        }

        if (btn && btn->isVisible()) {
            // 「在最顶」= 在正文的直接子控件里最靠上（按钮在场时才谈得上）
            int othersTop = 1 << 20;
            for (QObject *o : page.children()) {
                auto *w = qobject_cast<QWidget *>(o);
                if (!w || w == btn || !w->isVisible()) continue;
                othersTop = qMin(othersTop, w->y());
            }
            check(btn->y() <= othersTop,
                  QString("在最顶：按钮 y=%1，其它直接子控件最小 y=%2")
                      .arg(btn->y()).arg(othersTop));
        }

        // 日志框：装之前不该占地方，也不该露出来
        QPlainTextEdit *log = nullptr;
        for (QPlainTextEdit *e : page.findChildren<QPlainTextEdit *>()) log = e;
        check(log != nullptr && !log->isVisible(), "安装日志默认隐藏（点下去才现身）");

        // 顺带确认没把老控件带坏：引擎下拉是页面上第一个下拉框，且仍可见。
        // （取第一个而不是最后一个：后面的 SMB 认证下拉在隐藏的出口框里，
        //   拿它来断言可见性必然失败。）
        const auto combos = page.findChildren<QComboBox *>();
        check(!combos.isEmpty() && combos.first()->isVisible(),
              QString("原有的引擎下拉仍在（共 %1 个下拉框）").arg(combos.size()));
    }

    say(QString("   通过 %1 条，失败 %2 条").arg(passes).arg(fails));
    if (!fails) say("OK 全部通过 (0 失败)");
    return fails ? 1 : 0;
}
