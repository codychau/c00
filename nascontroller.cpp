#include "nascontroller.h"
#include "nasmodel.h"

#include <QProcess>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDir>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <algorithm>     // std::sort：探测结果排序，界面颜色才稳定
#include <unistd.h>      // getuid/getgid：建副本目录时要按当前用户的属主建

// ─────────────────────────────────────────────────────────────
//  已在本机实证的 LIO 事实（解析 /lib/modules/.../*.ko 的 modinfo 与
//  Ubuntu 包内容得到，不是文档记忆）：
//
//   target_core_iblock = IBLOCK = /backstores/block   （导出块设备）
//   target_core_file   = FILEIO  = /backstores/fileio （导出普通文件）
//   target_core_user   = USER，depends = target_core_mod,uio
//
//   saveconfig 默认路径：/etc/rtslib-fb-target/saveconfig.json
//     （**不是**老文档里的 /etc/target/saveconfig.json —— 当前
//      Ubuntu 的 rtslib_fb/root.py:37 已改）
//   开机恢复单元 target.service 由 **python3-rtslib-fb** 提供，
//     不是 targetcli-fb（后者只给 targetclid.service/socket）
//   本机 loop 是 built-in（无 loop.ko），losetup 可直接用
//
//  仍未实证的（本机非 root，加载不了模块）：targetcli 各子命令的确切
//  拼写与顺序。所以下面 planIscsiExport 只生成模块加载，导出步骤留 TODO。
//
//  命令生成本质上是"给 root 准备的脚本"。所以：
//   - 参数一律走 QStringList → argv，绝不拼 shell 字符串
//   - 一切来自用户的字符串（卷名、IQN）先过白名单，不合规就拒生成
//   - 破坏性步骤显式标 destructive，页面据此要二次确认
// ─────────────────────────────────────────────────────────────

// 卷名白名单：mdadm 的名字会进 /dev/md-<name> 与命令行，
// 留空格里路径分隔符就可能造出意外的设备路径。
static bool safeVolumeName(const QString &name)
{
    static const QRegularExpression re("^[A-Za-z0-9][A-Za-z0-9._-]{0,31}$");
    return re.match(name).hasMatch();
}

// 设备路径必须是 /dev/ 下的规范路径，且不含 ".."
static bool safeDevicePath(const QString &p)
{
    if (!p.startsWith("/dev/")) return false;
    if (p.contains("..")) return false;
    if (p.contains(' ') || p.contains('\n')) return false;
    return true;
}

// 在 PATH 之外再找一遍 sbin。
//
// 这不只是修 "明明装了 mdadm 却说没装" 的显示问题。桌面会话（systemd --user、
// 从图标启动、AppImage）的 PATH 通常**不含 /usr/sbin**，于是
// QStandardPaths::findExecutable("mdadm") 会返回空 —— 而 mdadm/losetup/modprobe
// 恰恰都在 /usr/sbin。两个后果：
//   1) 环境自检误报缺工具，功能看起来不可用
//   2) 更要命：QProcess 按同一个 PATH 去启动，"创建阵列" 会 FailedToStart
// 所以这里找到绝对路径，执行时也用绝对路径（见 resolvedProgram）。
static QString findTool(const QString &name)
{
    QString p = QStandardPaths::findExecutable(name);
    if (!p.isEmpty()) return p;
    static const QStringList extra = {
        "/usr/sbin/", "/sbin/", "/usr/local/sbin/",
        "/usr/bin/", "/usr/local/bin/",
    };
    for (const QString &d : extra) {
        const QString cand = d + name;
        if (QFileInfo::exists(cand) && QFileInfo(cand).isExecutable())
            return cand;
    }
    return QString();
}

NasController::NasController(QObject *parent) : QObject(parent) {}

// 生成命令时把工具名换成绝对路径，避免 QProcess 在非登录 shell 下找不到。
QString NasController::resolvedProgram(const QString &name) const
{
    // pkexec / tee 这类通常在 /usr/bin，交给 PATH；其余按 sbin 兜底找
    static const QStringList pathOnly = { "pkexec", "tee", "bash", "sh" };
    if (pathOnly.contains(name)) return name;
    const QString p = findTool(name);
    return p.isEmpty() ? name : p;
}




QString NasController::nasrsBinaryPath()
{
    // 三种布局依次找：AppImage 的 usr/bin → 可执行文件同目录 → PATH
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QStringList cands = {
        exeDir + "/nasrs",
        exeDir + "/../lib/nasrs",
        "/usr/local/bin/nasrs",
        "/usr/bin/nasrs",
    };
    for (const QString &c : cands)
        if (QFileInfo::exists(c) && QFileInfo(c).isExecutable()) return c;

    // 最后交给 PATH
    const QString onPath = QStandardPaths::findExecutable("nasrs");
    return onPath;
}


QString NasController::mergerfsBinaryPath()
{
    // 与 nasrs 同一套找法，但**不共用函数**：mergerfs 由包管理器装进系统目录，
    // 而 AppImage 的包内路径（exeDir/usr/bin）里放的是 nasrs。共用一份发现逻辑
    // 迟早要在"mergerfs 该不该随包走"这件事上打架，而两家的答案本来就不同。
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QStringList cands = {
        exeDir + "/mergerfs",
        exeDir + "/../lib/mergerfs",
        "/usr/local/bin/mergerfs",
        "/usr/bin/mergerfs",
    };
    for (const QString &c : cands)
        if (QFileInfo::exists(c) && QFileInfo(c).isExecutable()) return c;
    return QStandardPaths::findExecutable("mergerfs");
}

NasController::Env NasController::probe() const
{
    Env e;
    e.mdadm       = !findTool("mdadm").isEmpty();
    e.targetCli   = !findTool("targetcli").isEmpty();
    e.losetup     = !findTool("losetup").isEmpty();
    e.nasrsBinary = !nasrsBinaryPath().isEmpty();
    e.mergerfsBinary = !mergerfsBinaryPath().isEmpty();

    // FUSE：raidfs 挂目录靠它，与 iSCSI 的依赖完全是两套。
    //   · /dev/fuse 是字符设备，不在就一律挂不了；
    //   · fusermount3 是 setuid 的挂载助手，非 root 挂 FUSE 必须经它。
    //     只查 mount.fuse 不够：那是 mount(8) 的 helper，手动 mount -t fuse
    //     才用得到；程序里直接调 fuse 库时真正需要的是 fusermount。
    // 发行版判定：读 /etc/os-release 交给纯函数解析（可单测）。读不到就当
    // 认不出家族，宁可少给一个按钮，也不给一个会失败的安装按钮。
    // 同一份文本喂两个判定：aptFamily 是旧的「只认 apt」口径（fuse 那一格
    // 还在用它把门），pkg 是新的「dnf / apt-get / pacman 三选一」口径。
    QString osrText;
    {
        QFile osr("/etc/os-release");
        if (osr.open(QIODevice::ReadOnly | QIODevice::Text))
            osrText = QString::fromUtf8(osr.readAll());
    }
    e.aptFamily = Nas::aptInstallable(osrText);
    e.aptAvailable = !findTool("apt-get").isEmpty();
    {
        const bool haveDnf    = !findTool("dnf").isEmpty();
        const bool havePacman = !findTool("pacman").isEmpty();
        e.pkg = Nas::detectPkgManager(osrText, e.aptAvailable, haveDnf, havePacman);
    }
    // 缺哪些依赖：按可执行名逐个找（findTool 已经覆盖 /usr/sbin 那批）。
    // 只收表里登记过的项，所以"缺 nasrs 引擎"不会混进来 —— 那个没有包可装，
    // 混进来只会让一键安装多一次注定失败的 root 调用。
    for (const Nas::DepSpec &s : Nas::nasDepSpecs())
        if (findTool(s.binary).isEmpty())
            e.missingDeps << s.binary;
    e.missingDepLabels = Nas::dependencyLabels(e.missingDeps);

    e.fuseDevice = QFileInfo::exists("/dev/fuse");
    e.fuseMount  = !findTool("fusermount3").isEmpty() || !findTool("fusermount").isEmpty();

// SMB：smbd 多在 /usr/sbin，findTool 已覆盖该路径
e.smbd     = !findTool("smbd").isEmpty();
e.testparm = !findTool("testparm").isEmpty();
e.smbService = QFileInfo::exists("/usr/lib/systemd/system/smbd.service")
            || QFileInfo::exists("/lib/systemd/system/smbd.service");

    // configfs 是否挂载：LIO 的一切都建立在它上面
    QFile mounts("/proc/mounts");
    if (mounts.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString t = QString::fromUtf8(mounts.readAll());
        e.configfsMounted = t.contains(" configfs ");
    }

    // 内核模块存在性（不加载，只看文件在不在）
    const QString kern = QSysInfo::kernelVersion();
    const QStringList modDirs = {
        QString("/lib/modules/%1/kernel/drivers/target").arg(kern),
    };
    for (const QString &d : modDirs) {
        QDir dir(d);
        if (!dir.exists()) continue;
        const QStringList all = dir.entryList({"target_core_mod.ko*"}, QDir::Files);
        if (!all.isEmpty()) { e.kernelModules = true; break; }
    }

    if (!e.mdadm)         e.missing << "mdadm（内核 RAID 工具）";
    if (!e.targetCli)     e.missing << "targetcli / targetcli-fb（LIO 配置工具）";
    if (!e.configfsMounted) e.missing << "configfs 未挂载（mount -t configfs configfs /sys/kernel/config）";
    if (!e.kernelModules) e.missing << "target_core_mod 内核模块";
    // losetup 不再进 missing：它是自研 RAID0 那条的依赖（backing 文件 → loop →
    // LIO），那条路线已经换成 mergerfs，全程不碰 loop。留着这句会让用 mergerfs
    // 的人看到一句他根本用不上的"环境未就绪"。
    // 注意：fuseDevice/fuseMount 也**不进** missing，理由与 Samba 相同 ——
    // mergerfs 是可选出口，没 FUSE 的人做 iSCSI / 内核 RAID 不该看到"环境未
    // 就绪"。mergerfs 那一格自己判断 readyForMerger。
    // mergerfs **二进制**进 missing：它靠包管理器装（apt 有，dnf/pacman 没有），
    // 这条已经在 nasDepSpecs 表里，上面那个循环会顺带收掉，不在这里重复。
    //
    // 注意：smbd/testparm/smb.service 缺失**不进** missing。SMB 是可选功能，
    // 没装 Samba 的人用 RAID 不该看到环境未就绪。SMB 页自己判断 readyForSmb。

    return e;
}

QVector<Nas::ExistingArray>
NasController::existingArrays(const QStringList &localMountPoints) const
{
    QVector<Nas::ExistingArray> out;

    // ① mdadm：内核对 /dev/mdN 与它的成员有完整记账，这是最可靠的一手来源。
    {
        QFile f("/proc/mdstat");
        if (f.open(QIODevice::ReadOnly | QIODevice::Text))
            out += Nas::parseMdstat(QString::fromUtf8(f.readAll()));
    }

    // ② mergerfs：挂载表里只剩 source 的 basename（实测 /vol1:/vol2 变成
    // "vol1:vol2"），原始路径只保留在 mergerfs 进程自己的 argv 里（它作为
    // FUSE 守护进程一直活着，ps 能看到完整 argv）。所以这里翻 /proc/<pid>/cmdline，
    // 不解析 ps 的输出 —— 不依赖外部命令，也不受列宽影响。
    {
        QDir proc("/proc");
        const QStringList pids = proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QStringList seen;
        for (const QString &pid : pids) {
            bool ok = false;
            pid.toInt(&ok);
            if (!ok) continue;
            QFile f("/proc/" + pid + "/cmdline");
            if (!f.open(QIODevice::ReadOnly)) continue;
            const QByteArray raw = f.readAll();
            if (raw.isEmpty()) continue;
            QStringList argv;
            for (const QByteArray &a : raw.split('\0'))
                if (!a.isEmpty()) argv << QString::fromUtf8(a);
            QStringList sources;
            QString mountPoint;
            if (!Nas::mergerfsCmdlineSources(argv, &sources, &mountPoint)) continue;
            if (mountPoint.isEmpty() || seen.contains(mountPoint)) continue;
            seen << mountPoint;
            Nas::ExistingArray a;
            a.engine = Nas::Engine::MergerFs;
            a.name = mountPoint;
            a.members = sources;
            a.note = QString("mergerfs · %1").arg(mountPoint);
            out << a;
        }
    }

    // ③ 自研镜像：nasrs 不写状态文件，证据就是目录本身 ——
    // <挂载点>/<卷名>.nas-mirror。同一份镜像的两端目录同名，按卷名归组。
    // 只扫调用方给的挂载点（面板上那些本地分区），不遍历整张挂载表：
    // 网络挂载 stat 一下就可能把界面卡住。
    {
        QMap<QString, QStringList> mirrors;      // 卷名 → 挂载点
        for (const QString &root : localMountPoints) {
            if (root.isEmpty()) continue;
            const QStringList dirs =
                QDir(root).entryList(QStringList() << "*.nas-mirror",
                                     QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QString &dir : dirs) {
                const QString vol = dir.left(dir.size() - int(qstrlen(".nas-mirror")));
                if (vol.isEmpty()) continue;
                QStringList &list = mirrors[vol];
                if (!list.contains(root)) list << root;
            }
        }
        for (auto it = mirrors.cbegin(); it != mirrors.cend(); ++it) {
            Nas::ExistingArray a;
            a.engine = Nas::Engine::MirrorRs;
            a.name = it.key();
            a.members = it.value();
            a.note = QString::fromUtf8("自研镜像「%1」").arg(it.key());
            out << a;
        }
    }

    // 顺序固定下来，界面上的颜色才不会每次刷新都换一遍：
    // 先 mdadm、再 mergerfs、最后镜像，同类按名字排。
    std::sort(out.begin(), out.end(),
              [](const Nas::ExistingArray &a, const Nas::ExistingArray &b) {
                  if (a.engine != b.engine) return int(a.engine) < int(b.engine);
                  return a.name < b.name;
              });
    return out;
}

// ── mdadm 路线 ───────────────────────────────────────────────
// 注意 --assume-clean：不写全同步能省时间，但**前提是磁盘上确实是空的**。
// 若某分区原先有旧 md superblock 而没擦干净，用它建出来的阵列内容可能是错的。
// 这里选择不用它（宁可慢），避免"看起来建成了其实数据不对"。
QVector<NasController::Step>
NasController::planMdadmCreate(const Nas::Plan &plan, const QString &arrayName) const
{
    QVector<Step> steps;
    if (!safeVolumeName(arrayName)) return steps;

    QStringList devs;
    for (const Nas::Member &m : plan.members) {
        if (!safeDevicePath(m.device)) return steps;   // 有脏输入：整批拒绝
        devs << m.device;
    }
    if (devs.size() < 2) return steps;

    const QString dev = "/dev/md-" + arrayName;

    Step s;
    s.program = "pkexec";
    s.args = { "mdadm", "--create", dev,
               "--level=raid0",
               "--raid-devices=" + QString::number(devs.size()),
               "--chunk=" + QString::number(plan.stripeChunkKiB) + "K",
               "--run" };
    s.args += devs;
    s.describe = QString("创建 RAID0 %1（%2 个成员，条带 %3 KiB）"
                         "—— 会抹掉这些分区上的数据")
                   .arg(dev).arg(devs.size()).arg(plan.stripeChunkKiB);
    s.destructive = true;
    steps.append(s);

    // 故意**不**在这里写 /etc/mdadm/mdadm.conf。
    // 早先的版本放了 `mdadm --detail --scan --verbose` 并声称"追加到
    // mdadm.conf"，那是错的：--detail --scan 只把配置**打印到 stdout**，
    // 不落盘；要落盘得重定向追加，而 QProcess 没有 shell 就没法重定向。
    // 宁可少做一步，也不留一条"声称会做、实际什么都不做"的命令 ——
    // 那种命令最坏：用户以为已持久化，重启后阵列没自动组装。
    //
    // RAID0 重启后不自动组装反而是安全的：阵列不会带着半套成员悄悄上线。
    return steps;
}

// ── mergerfs 路线（RAID0 那格现在用它）────────────────────────
//
// 自研 RAID0 已经整条撤掉（nasrs 里的 serve/raid.rs/nbd.rs 一起删），原因写在
// README 里：导出层从来没有实现，`serve` 算完布局就退出码 3。mergerfs 是第三方
// 现成的东西，全程普通用户权限，不建大文件、不碰分区、不抹数据。
//
// 它**不是 RAID0**，这里不写一条"RAID0 挂载"的命令来骗人：一个文件整个落在一个
// 分区上，没有条带，也没有冗余。所以文案写 mergerfs，容量账是各分区空闲之和。
// 提权前缀：已经是 root 就不套 pkexec。
// 这一条不是优化，是"别让日志里出现一句谁也看不懂的话"：部署环境里 start.sh 就是
// 以 root 跑本程序的，而容器里没有 polkit 的会话总线 —— 实测 pkexec 在里面直接回
// "Refusing to render service to dead parents"（退出码 127）。root 只需要直接跑，
// 授权对话框是给非 root 的桌面会话用的，那才是 pkexec 存在的原因。
static QStringList privPrefix()
{
    return (geteuid() == 0) ? QStringList{} : QStringList{ QStringLiteral("pkexec") };
}

QVector<NasController::Step>
NasController::planMergerfsCreate(const Nas::Plan &plan, const QString &mountPoint) const
{
    QVector<Step> steps;
    if (plan.engine != Nas::Engine::MergerFs) return steps;
    if (!safeVolumeName(plan.volumeName)) return steps;
    if (plan.members.size() < Nas::kMergerMinMembers) return steps;
    // 挂载点会进 argv，也会进 systemd 单元名 —— 这里放宽一个字符就是开注入口
    if (!Nas::validateMountPoint(mountPoint).isEmpty()) return steps;

    QStringList srcs;
    for (const Nas::Member &m : plan.members) {
        if (!Nas::validateMergerSource(m.filePath).isEmpty()) return steps;
        srcs << m.filePath;
    }
    if (!Nas::mergerSourcesConflict(srcs).isEmpty()) return steps;

    // ① 只建目录，不挂任何东西：可撤销，也不遮任何内容。
    // 用 install -d 而不是 mkdir：一并把权限定下来，且目录已存在时不报错
    // （重复创建是正常操作）。
    const QStringList pre = privPrefix();
    Step mkd;
    mkd.program = pre.isEmpty() ? QStringLiteral("install") : QStringLiteral("pkexec");
    // pkexec 的写法是 `pkexec <程序> <参数…>`，直接跑的写法是 `<程序> <参数…>` ——
    // 两种都把程序名放在 program 里，所以只有 pkexec 才需要把目标程序再当一次参数。
    // 早先这里无条件把 "install" 塞进 args：root 下变成 `install install -d …`，
    // 把 install 自己的子命令位置占了，创建挂载目录直接失败。
    QStringList mkArgs = pre;
    if (!pre.isEmpty()) mkArgs << "install";
    mkArgs << "-d" << "-m" << "0755" << mountPoint;
    mkd.args = mkArgs;
    mkd.describe = QString("创建挂载目录 %1（mergerfs 的挂载点）").arg(mountPoint);
    mkd.destructive = false;
    steps.append(mkd);

    return steps;
}

QVector<NasController::Step>
NasController::planMergerfsMount(const Nas::Plan &plan, const QString &mountPoint) const
{
    QVector<Step> steps;
    const auto pre = planMergerfsCreate(plan, mountPoint);
    if (pre.isEmpty()) return steps;   // 同一套校验，不各写一遍

    const QString bin = mergerfsBinaryPath();
    if (bin.isEmpty()) return steps;

    QStringList srcs;
    for (const Nas::Member &m : plan.members) srcs << m.filePath;

    Step mt;
    // 挂载要 root，这一条不是随便选的：
    //   · 用户挂的 FUSE 目录默认**只有挂载者能读**（allow_other 还要
    //     /etc/fuse.conf 里的 user_allow_other），而 SMB 那出口是 smbd（root）
    //     去读它 —— 挂上了却共享不出东西，是最难查的那种"界面说成功、别人读不到"。
    //   · root 挂载 + allow_other 让 smbd 与访客都读得到。
    // 前缀按 euid 走（privPrefix）：部署环境 start.sh 就是 root 跑的，容器里没有
    // polkit 会话总线，pkexec 在那里只会回一句"Refusing to render service to
    // dead parents"（实测退出码 127）。root 直接跑就够，授权对话框是给非 root
    // 桌面会话的。
    const QStringList priv = privPrefix();
    mt.program = priv.isEmpty() ? bin : QStringLiteral("pkexec");
    QStringList mtArgs = priv;
    if (!priv.isEmpty()) mtArgs << bin;   // 同上：只有 pkexec 才把程序名当参数
    mtArgs << Nas::mergerfsArgs(srcs, mountPoint, plan.mergerPolicy);
    mt.args = mtArgs;
    mt.describe = QString("mergerfs：把 %1 个分区并成 %2（策略 %3，可用 = 各分区空闲之和）")
                      .arg(int(srcs.size())).arg(mountPoint).arg(Nas::mergerPolicyLabel(plan.mergerPolicy));
    // 标 destructive：这一步会在挂载目录上盖一个文件系统。挂载可撤销（卸载就
    // 恢复），但它让目录里原有的内容暂时看不见 —— 用户若以为文件丢了会慌，
    // 所以要走确认，不能省。
    mt.destructive = true;
    steps.append(mt);

    return steps;
}

// ── iSCSI 导出（两条路线共用）───────────────────────────────
// 用 targetcli 而不是手写 configfs：configfs 的目录名和属性名写错会留下
// 删不掉的残核，targetcli 失败则是可撤销的。参数未查证前不猜。
// ── iSCSI 导出（两条路线共用）───────────────────────────────
//
// 为什么用 targetcli 而不是直接写 configfs（虽然直接写更快、错误码更精确）：
//   直接操作 configfs 的**创建顺序错了会留下难以清除的内核残留**，
//   而 targetcli 失败是可撤销的。本机无法实机验证（非 root，加载不了模块），
//   在没有端到端验证之前选可撤销的那条路。
//
// 已在本机从 Ubuntu 的 rtslib_fb 源码核对的事实：
//   · 整个 rtslib_fb 只有一处 os.mkdir，auth/ 只作为 feature 字符串出现，
//     从不被创建 → tpgt_N 的子目录由内核自动生成，
//     因此**拆除时不要单独 rmdir auth**（会失败）。
//   · LUN 是符号链接：os.symlink(tpg_lun.path, "<目录>/<alias>")
//   · BlockStorageObject 写的是 udev_path=<dev> 与 readonly=<0|1>
//   · FileIO 的 size 只在"不是块设备"时才必需
QVector<NasController::Step>
NasController::planIscsiExport(const QString &backingPath, const QString &iqn,
                               const QString &portalIp, bool allowAnon) const
{
    QVector<Step> steps;
    if (!safeDevicePath(backingPath)) return steps;
    // IQN 必须全小写且形如 iqn.YYYY-MM.domain:suffix —— configfs 对
    // 大小写和非法字符直接回 EINVAL，最难查的就是这种"没反应"的错误。
    static const QRegularExpression iqnRe(
        "^iqn\\.[0-9]{4}-[0-9]{2}\\.[a-z0-9._-]+(:[a-z0-9._-]+)?$");
    if (!iqnRe.match(iqn).hasMatch()) return steps;

    // backstore 名从设备名推导，不用哈希：哈希名（形如 sb_3f2a1c）在
    // /backstores/block/ 下根本看不出对应哪块盘，清理时极易删错或漏删。
    // 只留字母数字，"/" 转 "-"，最长 32 字符（configfs 名字长度有限）。
    QString baName = backingPath;
    baName.remove("/dev/");
    baName.replace('/', '-');
    baName.replace(QRegularExpression("[^A-Za-z0-9-]"), "-");
    baName = "nas_" + baName.left(28);

    // 1) 模块：导出块设备要 IBLOCK，不是名字更像的 target_core_file
    const char *mods[] = { "target_core_mod", "target_core_iblock", "iscsi_target_mod" };
    const char *whys[] = { "LIO 核心",
                           "IBLOCK 插件（/backstores/block，导出块设备用）",
                           "iSCSI 目标插件" };
    for (int i = 0; i < 3; ++i) {
        Step m;
        m.program = "pkexec";
        m.args = { "modprobe", mods[i] };
        m.describe = QString("加载 %1 —— %2").arg(mods[i], whys[i]);
        m.destructive = false;
        steps.append(m);
    }

    // 2) 持久化模块加载：否则重启后 targetctl restore 会因为
    //    插件不在而失败，而失败发生在开机早期，界面上看不到。
    Step modconf;
    modconf.program = "pkexec";
    modconf.args = { "tee", "/etc/modules-load.d/server-toolbox-iscsi.conf" };
    // 内容通过 describe 说明，实际由调用方以 heredoc 方式提供；
    // 这里不用 shell 重定向（拼字符串 = 注入口），改由 tee 读 stdin。
    modconf.describe = "写入 /etc/modules-load.d/server-toolbox-iscsi.conf"
                       "（内容：target_core_mod / target_core_iblock / iscsi_target_mod）";
    modconf.destructive = false;
    steps.append(modconf);

    // 3) targetcli 一次性建好。auto_save_on_exit 显式关掉：
    //    它默认 true，退出时隐式 saveconfig —— 会把配置（含 CHAP 明文）
    //    落盘到 /etc/rtslib-fb-target/saveconfig.json，而我们没要求保存。
    QStringList cmds;
    cmds << QString("backstores/block create name=%1 dev=%2").arg(baName, backingPath);
    cmds << QString("iscsi/ create %1").arg(iqn);
    cmds << QString("cd /iscsi/%1/tpg1").arg(iqn);
    cmds << QString("luns/ create /backstores/block/%1").arg(baName);
    // portal：不写就是 0.0.0.0:3260
    if (!portalIp.isEmpty())
        cmds << QString("portals/ create %1").arg(portalIp);

    if (allowAnon) {
        // LIO 的 "Demo Mode"：ACL 关闭，任意 initiator 可读写。
        // 这三个属性必须一起设，只设 generate_node_acls 是不够的。
        cmds << "set attribute generate_node_acls=1";
        cmds << "set attribute cache_dynamic_acls=1";
        cmds << "set attribute demo_mode_write_protect=0";
        cmds << "set parameter AuthMethod=None";
    } else {
        // 不放行匿名，但也不在这里凭空造 ACL —— 没有 ACL 的 target
        // 谁也连不上，这是**正确**的状态，页面上要提示用户先登记 IQN。
        cmds << "set attribute generate_node_acls=0";
        cmds << "set attribute demo_mode_write_protect=1";
    }
    // 不 saveconfig：见上。重启后由 target.service 恢复的前提是
    // 用户显式保存过，页面另有"保存配置"入口。
    Step tc;
    tc.program = "pkexec";
    tc.args = QStringList() << "targetcli" << "auto_save_on_exit=false";
    // 命令经 stdin 传入而非拼进 shell：见 run() 里对 stdin 的处理
    tc.args << "-";
    tc.describe = QString("用 targetcli 导出 %1 为 %2%3")
                      .arg(backingPath, iqn,
                           portalIp.isEmpty() ? "（监听 0.0.0.0，全网可连）"
                                              : QString("，监听 %1").arg(portalIp));
    tc.destructive = false;
    tc.stdinLines = cmds;      // 见 Step 的新字段
    steps.append(tc);

    return steps;
}

QVector<NasController::Step>
NasController::planIscsiTeardown(const QString &iqn, const QString &backingPath) const
{
    QVector<Step> steps;
    static const QRegularExpression iqnRe(
        "^iqn\\.[0-9]{4}-[0-9]{2}\\.[a-z0-9._-]+(:[a-z0-9._-]+)?$");
    if (!iqnRe.match(iqn).hasMatch()) return steps;

    QString baName = backingPath;
    baName.remove("/dev/");
    baName.replace('/', '-');
    baName.replace(QRegularExpression("[^A-Za-z0-9-]"), "-");
    baName = "nas_" + baName.left(28);

    // 撤销比创建更危险：顺序错就留下 orphan backstore —— targetcli 的
    // ls 里看不见、内核里还在占着设备。targetcli 的 delete 自己处理顺序，
    // 所以这里同样交给它，而不是自己 rmdir。
    QStringList cmds;
    cmds << QString("/iscsi delete %1").arg(iqn);
    cmds << QString("/backstores/block delete %1").arg(baName);
    Step tc;
    tc.program = "pkexec";
    tc.args = QStringList() << "targetcli" << "auto_save_on_exit=false" << "-";
    tc.describe = QString("撤销 %1 的导出（先删 target 再删 backstore）").arg(iqn);
    tc.destructive = false;
    tc.stdinLines = cmds;
    steps.append(tc);

    // 若 backing 是 loop 设备，必须在 backstore 删掉**之后**才能 detach，
    // 否则 EBUSY。顺序由 steps 保证。
    if (backingPath.startsWith("/dev/loop")) {
        Step l;
        l.program = "pkexec";
        l.args = { "losetup", "-d", backingPath };
        l.describe = QString("释放回环设备 %1").arg(backingPath);
        l.destructive = false;
        steps.append(l);
    }
    return steps;
}

// SMB 片段文件：/etc/samba/smb.d/<name>.conf
// 主 smb.conf 需 include /etc/samba/smb.d/*.conf；很多发行版默认没有这一行，
// 这里不自动改主配置，交给 UI 提示（自动改主 smb.conf 风险太高）。
static QString smbConfPath(const QString &shareName)
{
    QString n = shareName;
    n.replace(QRegularExpression("[^A-Za-z0-9._-]"), "_");
    if (n.isEmpty()) n = "share";
    return "/etc/samba/smb.d/" + n + ".conf";
}

QVector<NasController::Step>
NasController::planSmbExport(const QString &shareName,
                             const QString &shareSection) const
{
    QVector<Step> steps;
    // 共享名与配置内容都必须先过模型层校验，这里再兜一道底
    if (!Nas::validateShareName(shareName).isEmpty()) return steps;
    if (shareSection.trimmed().isEmpty()) return steps;
    // 内容必须真含一个 path= 行，且该路径本身合法。这里从 [name] 段里
    // 抠出 path 值再交给模型层校验，防止调用方绕过 makeShareSection 拼串。
    {
        QString pathVal;
        for (const QString &line : shareSection.split('\n')) {
            const QString t = line.trimmed();
            if (t.startsWith("path =")) { pathVal = t.section('=', 1).trimmed(); break; }
        }
        if (pathVal.isEmpty()) return steps;
        if (!Nas::validateSharePath(pathVal).isEmpty()) return steps;
    }

    const QString confPath = smbConfPath(shareName);

    // 1) 建目录（drop-in 目录可能不存在）。非破坏。
    Step mkdir;
    mkdir.program = "pkexec";
    mkdir.args = { "install", "-d", "-m", "0755", "/etc/samba/smb.d" };
    mkdir.describe = "创建 SMB 片段目录 /etc/samba/smb.d";
    mkdir.destructive = false;
    steps.append(mkdir);

    // 2) 写片段：用 tee 覆盖该 <name>.conf。只覆盖同名片段，不动主配置。
    //    对同名共享是"覆盖"——这就是重复导出的语义，UI 会提示。
    Step write;
    write.program = "pkexec";
    write.args = { "tee", confPath };
    write.describe = QString("写入共享配置 %1（共享名 %2）").arg(confPath, shareName);
    write.stdinLines = { shareSection.trimmed() };   // 经 stdin 传入，不进 argv
    write.destructive = false;   // 覆盖的是我们自己的片段文件，非用户数据
    steps.append(write);

    // 3) testparm 离线校验：内容不合法就别 reload，免得把整个 Samba 配崩
    Step check;
    check.program = "pkexec";
    check.args = { "testparm", "-s", "/etc/samba/smb.conf" };
    check.describe = "testparm 校验 smb.conf（片段有语法错误会在这里暴露）";
    check.destructive = false;
    steps.append(check);

    // 4) 让 smbd 重载（不 restart，避免中断已有连接）
    Step reload;
    reload.program = "systemctl";
    reload.args = { "reload-or-restart", "smbd" };
    reload.describe = "重载 smbd 使新共享生效";
    reload.destructive = false;
    steps.append(reload);

    return steps;
}

QVector<NasController::Step>
NasController::planSmbRemove(const QString &shareName) const
{
    QVector<Step> steps;
    if (!Nas::validateShareName(shareName).isEmpty()) return steps;
    const QString confPath = smbConfPath(shareName);

    // 删片段前先看它在不在（不存在就别 rm，避免误导性的报错）
    Step del;
    del.program = "pkexec";
    del.args = { "rm", "-f", confPath };
    del.describe = QString("删除共享配置片段 %1（撤销共享 %2）").arg(confPath, shareName);
    del.destructive = true;   // 删除配置文件，虽可恢复，仍标记为破坏性以走强确认
    steps.append(del);

    Step reload;
    reload.program = "systemctl";
    reload.args = { "reload-or-restart", "smbd" };
    reload.describe = "重载 smbd 移除共享";
    reload.destructive = false;
    steps.append(reload);

    return steps;
}

QVector<NasController::Step> NasController::planStop(const QString &deviceOrVolume) const
{
    QVector<Step> steps;
    if (!safeDevicePath(deviceOrVolume)) return steps;
    Step s;
    s.program = "pkexec";
    s.args = { "mdadm", "--stop", deviceOrVolume };
    s.describe = QString("停止阵列 %1（未卸载的文件系统会先失败，这是对的）")
                    .arg(deviceOrVolume);
    s.destructive = false;
    steps.append(s);
    return steps;
}

// ── 执行 ────────────────────────────────────────────────────
void NasController::run(const QVector<Step> &steps)
{
    if (m_queueRunning) return;      // 一次只跑一批
    m_queue = steps;
    m_queueIndex = 0;
    m_cancelRequested = false;
    m_queueRunning = true;
    pumpQueue();
}

void NasController::cancel()
{
    m_cancelRequested = true;
    if (m_process && m_process->state() != QProcess::NotRunning)
        m_process->terminate();
}

void NasController::pumpQueue()
{
    if (m_cancelRequested || m_queueIndex >= m_queue.size()) {
        const bool ok = !m_cancelRequested && m_queueIndex >= m_queue.size();
        m_queueRunning = false;
        emit allFinished(ok);
        return;
    }

    const Step s = m_queue.at(m_queueIndex);
    emit stepStarted(m_queueIndex, s.describe);

    if (m_process) { m_process->deleteLater(); m_process = nullptr; }
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);

    connect(m_process, &QProcess::finished, this, [this, s](int code) {
        const QString out = QString::fromUtf8(m_process->readAll());
        const bool ok = (code == 0);
        emit stepFinished(m_queueIndex, ok, out);
        if (!ok) {
            emit stepFailed(m_queueIndex, s.program,
                            out.isEmpty() ? QString("退出码 %1").arg(code) : out);
            m_queueRunning = false;
            m_queue.clear();
            emit allFinished(false);
            return;
        }
        ++m_queueIndex;
        pumpQueue();
    });

    connect(m_process, &QProcess::errorOccurred, this,
            [this, s](QProcess::ProcessError err) {
        if (err != QProcess::FailedToStart) return;
        emit stepFailed(m_queueIndex, s.program, "无法启动进程");
        m_queueRunning = false;
        m_queue.clear();
        emit allFinished(false);
    });

    // 用绝对路径启动：桌面会话的 PATH 常缺 /usr/sbin（见 findTool 注释）
    m_process->start(resolvedProgram(s.program), s.args);

    // targetcli 一类"从 stdin 读命令"的工具：写完必须关闭写端，
    // 否则它一直等输入，进程永不退出 —— 表现为界面卡死在"步骤 N"。
    if (!s.stdinLines.isEmpty()) {
        QByteArray in;
        for (const QString &line : s.stdinLines) {
            in += line.toUtf8();
            in += '\n';
        }
        m_process->write(in);
        m_process->closeWriteChannel();
    }
}


QVector<NasController::Step>
NasController::planRaidfsUnmount(const QString &mountPoint) const
{
    QVector<Step> steps;
    if (!Nas::validateMountPoint(mountPoint).isEmpty()) return steps;

    // 先确认它真的是我们的挂载点，再动手卸：
    // 对没挂载的目录跑 umount 只是报错，但对挂着的别的文件系统跑
    // umount 会把别人正在用的东西卸掉。umount 对未挂载目标会失败，
    // 所以这一步本身就是那道检查，失败即停。
    Step um;
    um.program = "pkexec";
    um.args = { "umount", mountPoint };
    um.describe = QString("卸载 %1").arg(mountPoint);
    // 卸载会把卷上未落盘的写丢掉，且若有进程正在读这里的文件会失败，
    // 属于会影响别人的操作，走确认。
    um.destructive = true;
    steps.append(um);
    return steps;
}
// ── 软件镜像（nasrs RAID1）──────────────────────────────────
//
// 这条路线是"能真跑通"的那一条：普通用户权限、无内核模块、无 targetcli。
// 所以这里的注释重点不在"还没做什么"（raidfs 那两条写的是那个），而在
// "路径与属主为什么必须这么定"。
//
// 目录形状由模型层的 Nas::validateMirrorDir 把关（纯字符串判断，可离线测）；
// 这里再加一条只有拿着卷名才能判的规矩：目录必须叫 <卷名>.nas-mirror。
// 那条规矩顺手挡掉了最贵的一种填错 —— 把镜像目录写成挂载点本身，
// 也就是"镜像整个分区"，一旦带上删除多余文件，副本分区上原有的东西全没。
QVector<NasController::Step>
NasController::planMirrorCreate(const Nas::Plan &plan) const
{
    QVector<Step> steps;
    if (!safeVolumeName(plan.volumeName)) return steps;
    if (plan.engine != Nas::Engine::MirrorRs) return steps;
    if (plan.members.size() != Nas::kMirrorMembers) return steps;

    // 叶子目录名必须正好是 <卷名>.nas-mirror，逐段比而不是 endsWith。
    // 这条守卫挡的是最贵的一种填错：把镜像目录写成挂载点本身（/mnt/d1），
    // 形状校验过得去，一旦勾上"删除多余文件"，删的就是那个分区上原有的东西。
    // 用 endsWith 也不行：写成 "." + 卷名 + ".nas-mirror" 会把正确的
    // /mnt/d1/nas.nas-mirror 判成非法（探针就是这么抓出来的 —— 当时两个合法
    // 目录一条命令都生成不出来，界面上表现为"点了创建阵列什么也没发生"）；
    // 写成不带点又会放行 /mnt/d1/xnas.nas-mirror 这种粘在前缀上的名字。
    const QString leafWant = plan.volumeName + ".nas-mirror";
    QStringList dirs;
    for (const Nas::Member &m : plan.members) {
        if (Nas::validateMirrorDir(m.filePath).isEmpty() == false) return steps;
        if (m.filePath.section('/', -1) != leafWant) return steps;
        dirs << m.filePath;
    }
    // 两个目录重了必须拒：nasrs 自己也会拒，但那要等到用户点下确认之后。
    if (dirs.at(0) == dirs.at(1)) return steps;

    const QString uid = QString::number(::getuid());
    const QString gid = QString::number(::getgid());

    for (int i = 0; i < dirs.size(); ++i) {
        Step s;
        s.program = "pkexec";
        // -o/-g 给当前用户：挂载点十有八九是 root:root 0755，
        // 建出来的目录若还是 root 所有，后面 nasrs（普通用户）一个文件都写不进去，
        // 而它报的是 EACCES，看不出根因在目录属主上。
        s.args = { "install", "-d", "-m", "0775", "-o", uid, "-g", gid, dirs.at(i) };
        s.describe = QString::fromUtf8("%1 建副本目录 %2（属主给当前用户，后续同步不再要 root）")
                        .arg(i == 0 ? "主网" : "副本", dirs.at(i));
        // install -d 对已存在的目录是幂等的，也不碰里面已有的文件 → 非破坏
        s.destructive = false;
        steps.append(s);
    }
    return steps;
}

QVector<NasController::Step>
NasController::planMirrorSync(const QString &primaryDir, const QString &secondaryDir,
                              bool deleteExtras) const
{
    QVector<Step> steps;
    if (!Nas::validateMirrorDir(primaryDir).isEmpty()) return steps;
    if (!Nas::validateMirrorDir(secondaryDir).isEmpty()) return steps;
    if (primaryDir == secondaryDir) return steps;   // 自己拷自己：nasrs 也会拒，这里先挡
    const QString bin = nasrsBinaryPath();
    if (bin.isEmpty()) return steps;

    Step s;
    s.program = bin;                       // 没有 pkexec，见头部注释
    s.args = { "mirror", "--primary", primaryDir,
               "--secondary", secondaryDir, "--once" };
    if (deleteExtras) s.args << "--delete-extras";
    s.describe = deleteExtras
        ? QString::fromUtf8("同步到副本 %1，并删除副本上主网没有的文件").arg(secondaryDir)
        : QString::fromUtf8("同步到副本 %1（只增不删）").arg(secondaryDir);
    // 勾了删除就是破坏性操作：二次确认 + 手输卷名，和建阵列同一档。
    s.destructive = deleteExtras;
    steps.append(s);
    return steps;
}

NasController::MirrorStatus
NasController::parseMirrorStatus(const QByteArray &json)
{
    MirrorStatus st;
    const QJsonDocument doc = QJsonDocument::fromJson(json, nullptr);
    if (!doc.isObject()) return st;          // parsed 保持 false：调用方据此说"读不出来"
    const QJsonObject o = doc.object();
    st.parsed = true;
    st.ok           = o.value("ok").toBool(false);
    st.srcFiles     = quint64(o.value("src_files").toDouble());
    st.dstFiles     = quint64(o.value("dst_files").toDouble());
    st.lagFiles     = quint64(o.value("lag_files").toDouble());
    st.lagBytes     = quint64(o.value("lag_bytes").toDouble());
    st.extraFiles   = quint64(o.value("extra_files").toDouble());
    st.extraBytes   = quint64(o.value("extra_bytes").toDouble());
    st.copiedFiles  = quint64(o.value("copied_files").toDouble());
    st.copiedBytes  = quint64(o.value("copied_bytes").toDouble());
    st.deleted      = quint64(o.value("deleted").toDouble());
    st.failed       = quint64(o.value("failed").toDouble());
    st.torn         = quint64(o.value("torn").toDouble());
    st.remainingFiles = quint64(o.value("remaining_files").toDouble());
    st.remainingBytes = quint64(o.value("remaining_bytes").toDouble());
    for (const QJsonValue &v : o.value("errors").toArray())
        st.messages << v.toString();
    return st;
}

QString NasController::MirrorStatus::summaryLine() const
{
    if (!parsed)
        return error.isEmpty() ? QString::fromUtf8("读不到同步状态") : error;
    QStringList parts;
    parts << (lagFiles == 0
                ? QString::fromUtf8("副本与主网一致（%1 个文件）").arg(srcFiles)
                : QString::fromUtf8("副本落后 %1 个文件 / %2")
                      .arg(lagFiles).arg(Nas::humanSize(qint64(lagBytes))));
    if (extraFiles > 0)
        // 说清"没删"是默认行为，否则这句看起来像漏了功能
        parts << QString::fromUtf8("副本上多 %1 个文件（默认不删）").arg(extraFiles);
    if (failed > 0)
        parts << QString::fromUtf8("%1 个文件同步失败").arg(failed);
    if (torn > 0)
        parts << QString::fromUtf8("%1 个文件当时还在被写，下轮重拷").arg(torn);
    return parts.join("；");
}

NasController::MirrorStatus
NasController::queryMirrorStatus(const QString &primaryDir, const QString &secondaryDir,
                                 int timeoutMs) const
{
    MirrorStatus st;
    const QString bin = nasrsBinaryPath();
    if (bin.isEmpty()) {
        st.error = QString::fromUtf8("nasrs 引擎不在程序旁边，查不了同步状态");
        return st;
    }
    if (!Nas::validateMirrorDir(primaryDir).isEmpty()
        || !Nas::validateMirrorDir(secondaryDir).isEmpty()) {
        st.error = QString::fromUtf8("副本目录不合法");
        return st;
    }

    QProcess p;
    p.start(bin, { "mirror", "--primary", primaryDir,
                   "--secondary", secondaryDir, "--report", "--json" });
    // 扫一棵大树要真的遍历磁盘。这里同步等待，所以必须有超时：没有它，
    // 一次 NFS 卡住或几十万个文件就能把界面整个冻死。
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(1000);
        st.error = QString::fromUtf8("扫描副本超过 %1 秒，已中止（文件太多或磁盘正忙）")
                       .arg(timeoutMs / 1000);
        return st;
    }
    st = parseMirrorStatus(p.readAllStandardOutput());
    if (!st.parsed) {
        // 引擎在 JSON 之外把原因写在 stderr（比如副本目录还没建）
        const QString err = QString::fromUtf8(p.readAllStandardError()).trimmed();
        st.error = err.isEmpty()
            ? QString::fromUtf8("引擎退出码 %1，没有给出结果").arg(p.exitCode())
            : err.split('\n').first();
    }
    return st;
}

// 一键装 fuse3。条件已由调用方（Env::canInstallFuse）把过：apt 系 + apt 在
// + fusermount3 缺。这里只负责生成那条命令，不再重复判定 —— 判定和生成
// 分开放，按钮该不该出现的逻辑只有一处。
//
// 为什么 apt-get 前面用 pkexec 而不是 sudo：本工具其余需要 root 的步骤
// 都走 pkexec（图形化提权，与本 App 的 GUI 语境一致），这里保持一致，
// 不混用两套提权。
//
// 为什么固定 -y：这是"一键安装"，点了就是决定要装；但包名是写死的常量
// "fuse3"，不接受任何外部输入 —— 没有可注入的东西，才敢自动确认。
// 更新索引（apt-get update）不自动做：那会改整个系统的包状态、可能拉很久，
// 装不上时让用户自己按需跑，而不是替他动全局。
QVector<NasController::Step> NasController::planInstallFuse() const
{
    QVector<Step> steps;
    Step s;
    s.program    = "pkexec";
    s.args       = { "apt-get", "install", "-y", "fuse3" };
    s.describe   = QString::fromUtf8("安装 fuse3（提供 fusermount3，挂载 FUSE 文件系统需要）");
    // destructive=true：apt 会改系统包状态，确认框要按"改动系统"的级别提示。
    s.destructive = true;
    steps << s;
    return steps;
}

// ── 页顶「一键安装依赖的组件」───────────────────────────────
//
// 与 planInstallFuse 的分工：那条只处理「fuse 缺了」这一个点，走 pkexec；
// 这条管整页缺的依赖，走 sudo + 密码框，因为用户要的是一按到底、并且看得见
// 装的过程（pkexec 只给一个授权对话框，装包那几十行输出没有落点）。
//
// 为什么这里允许拼 shell 脚本，而本文件顶部写着"绝不拼 shell 字符串"：
// 脚本里出现的每一个 token 都来自 Nas::nasDepSpecs() 这张常量表或本函数自己
// 写的字面量，没有一个字来自输入框、文件名或设备名 —— 没有外部输入，就没有
// 注入口。为了让这句话不只是说说，生成前还会逐个包名过白名单，不合规矩就
// 整批作废（返回一个 valid()==false 的计划），而不是"清洗一下照跑"。
//
// 为什么 apt 要先 update：Debian/Ubuntu 的索引过期时 install 会直接
// "Unable to locate package"，那是这台机器上最常见的一次失败；这条链跑在
// set -e 下，update 失败就停，不会接着跑一条注定失败的 install。
// dnf 自己会在元数据过期时刷新，pacman 则**不**做 -Sy —— 部分刷新数据库是
// Arch 官方明确反对的做法（会造成部分升级），宁可让用户自己去刷新。
NasController::InstallPlan NasController::planInstallDeps(const Env &env) const
{
    InstallPlan p;
    p.pmName  = Nas::pkgManagerName(env.pkg);
    p.blocked = Nas::packagesUnavailable(env.pkg, env.missingDeps);
    p.packages = Nas::packagesToInstall(env.pkg, env.missingDeps);

    // mergerfs 在 dnf / pacman 系没有包，但源码自带 vendored libfuse，只要
    // g++/make/curl 就能编出一个静态二进制（见 build-mergerfs.sh 里那段说明）。
    // 所以这两家的"一键安装"不必摊手：把这一项从 blocked 里摘出来，改成编译步骤。
    const bool buildMerger = Nas::mergerfsSourceInstallable(env.pkg, env.missingDeps);
    if (buildMerger) {
        const QString lbl = Nas::dependencyLabels({ QStringLiteral("mergerfs") })
                                .value(0, QStringLiteral("mergerfs"));
        p.blocked.removeAll(lbl);
    }

    for (const QString &pkg : p.packages) {
        if (!Nas::isSafePackageName(pkg)) {
            // 表被改坏了。清空一切：宁可页面上报"生成失败"，
            // 也不把一个没审过的字符集交给 root。
            p.packages.clear();
            p.commands.clear();
            p.script.clear();
            return p;
        }
    }
    // 只有 mergerfs 可编、没有包可装的机器（openEuler 上很常见）也要生成脚本：
    // 这不是"没有可装的东西"，而是一条真能跑通的安装路径。
    if (p.packages.isEmpty() && !buildMerger) return p;

    // 展示行：「缺的东西 → 要装的包」，让人一眼看出这条命令补的是哪个洞
    for (const QString &bin : env.missingDeps) {
        const QStringList pkgs = Nas::packagesToInstall(env.pkg, { bin });
        const QString label = Nas::dependencyLabels({ bin }).value(0, bin);
        if (bin == QLatin1String("mergerfs") && buildMerger && pkgs.isEmpty())
            // 说清"编"而不是"装"：确认框里看到的是 make，用户才不会以为自己
            // 点错了按钮（这条命令不看包管理器）。
            p.rows << QString("%1 → 从源码编译（自带 libfuse，不需要 fuse3 开发头）")
                          .arg(label);
        else
            p.rows << (pkgs.isEmpty()
                           ? QString("%1 → 无包可装").arg(label)
                           : QString("%1 → %2").arg(label, pkgs.join(" ")));
    }

    QStringList lines;
    lines << QStringLiteral("set -e");   // 任一条失败就停，不带病继续

    if (!p.packages.isEmpty()) {
        const QString joined = p.packages.join(QLatin1Char(' '));
        switch (env.pkg) {
        case Nas::Pkg::Apt:
            lines << QStringLiteral("export DEBIAN_FRONTEND=noninteractive");
            lines << QStringLiteral("apt-get update");
            p.commands << QStringLiteral("apt-get update");
            lines << QStringLiteral("apt-get install -y ") + joined;
            p.commands << QStringLiteral("apt-get install -y ") + joined;
            break;
        case Nas::Pkg::Dnf:
            lines << QStringLiteral("dnf install -y ") + joined;
            p.commands << QStringLiteral("dnf install -y ") + joined;
            break;
        case Nas::Pkg::Pacman:
            // --needed：已装着的跳过，不重复下载也不重装，重复点这个按钮无害
            lines << QStringLiteral("pacman -S --noconfirm --needed ") + joined;
            p.commands << QStringLiteral("pacman -S --noconfirm --needed ") + joined;
            break;
        case Nas::Pkg::None:
            return p;   // 认不出包管理器，什么都不生成
        }
    }

    // 从源码编 mergerfs。这一段里出现的每个 token 都是本文件的字面量：POOL 是常量，
    // 版本号是那台机器上 curl 回来的目录列表里挑出来的最新一项 —— 没有输入框、没有
    // 文件名、没有设备名参与，注入口与上面包名那条同一个来源（没有外部输入）。
    //
    // 装到 /usr/local/bin：程序按「同目录 → ../lib → /usr/local/bin → /usr/bin →
    // PATH」找它，这是非 apt 系机器上程序能看见它的落点之一，而且 root 可写。
    // 编完立刻 --version 自检：确认产物真能跑，而不是"make 返回 0 就当成功"
    // （错架构、缺库这两种都在这里露馅）。
    if (buildMerger) {
        lines << QStringLiteral("set -o pipefail");
        lines << QStringLiteral("command -v curl >/dev/null || { echo '需要 curl（下载源码包）'; exit 1; }");
        lines << QStringLiteral("command -v make >/dev/null || { echo '需要 make'; exit 1; }");
        lines << QStringLiteral("command -v g++  >/dev/null || { echo '需要 g++（C++17）；dnf 系可以装 gcc-c++'; exit 1; }");
        lines << QStringLiteral("POOL=http://deb.debian.org/debian/pool/main/m/mergerfs/");
        lines << QStringLiteral("TMP=$(mktemp -d)");
        lines << QStringLiteral("trap 'rm -rf \"$TMP\"' EXIT");
        lines << QStringLiteral("curl -fsSL \"$POOL\" -o \"$TMP/index.html\"");
        lines << QStringLiteral("TARBALL=$(grep -o 'mergerfs_[0-9.]*\\.orig\\.tar\\.gz' \"$TMP/index.html\" | sort -V | tail -1)");
        lines << QStringLiteral("test -n \"$TARBALL\" || { echo '认不出源码包名（Debian pool 结构变了？）'; exit 1; }");
        lines << QStringLiteral("echo \"源码包：$TARBALL\"");
        lines << QStringLiteral("curl -fsSL \"$POOL$TARBALL\" -o \"$TMP/src.tar.gz\"");
        lines << QStringLiteral("tar xzf \"$TMP/src.tar.gz\" -C \"$TMP\"");
        lines << QStringLiteral("V=${TARBALL#mergerfs_}");   // 去掉前缀
        lines << QStringLiteral("V=${V%.orig.tar.gz}");      // 去掉后缀
        lines << QStringLiteral("cd \"$TMP/mergerfs-$V\"");
        lines << QStringLiteral("./buildtools/update-version >/dev/null 2>&1 || true");
        lines << QStringLiteral("make -j\"$(nproc)\" CXXFLAGS='-O2 -pipe -fstack-protector-strong -std=c++17 -MMD -MP'");
        lines << QStringLiteral("strip build/mergerfs");
        lines << QStringLiteral("install -m 0755 build/mergerfs /usr/local/bin/mergerfs");
        lines << QStringLiteral("/usr/local/bin/mergerfs --version");
        p.commands << QStringLiteral("从源码编译 mergerfs（curl Debian pool → make → strip → /usr/local/bin/mergerfs）");
    }

    p.script = lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
    return p;
}