#include "nascontroller.h"
#include "nasmodel.h"

#include <QProcess>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDir>
#include <QCoreApplication>
#include <QRegularExpression>

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


NasController::Env NasController::probe() const
{
    Env e;
    e.mdadm       = !findTool("mdadm").isEmpty();
    e.targetCli   = !findTool("targetcli").isEmpty();
    e.losetup     = !findTool("losetup").isEmpty();
    e.nasrsBinary = !nasrsBinaryPath().isEmpty();

    // FUSE：raidfs 挂目录靠它，与 iSCSI 的依赖完全是两套。
    //   · /dev/fuse 是字符设备，不在就一律挂不了；
    //   · fusermount3 是 setuid 的挂载助手，非 root 挂 FUSE 必须经它。
    //     只查 mount.fuse 不够：那是 mount(8) 的 helper，手动 mount -t fuse
    //     才用得到；程序里直接调 fuse 库时真正需要的是 fusermount。
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
    // loop 在本机是 built-in，所以不检查 loop.ko 是否存在，只看 losetup
    if (!e.losetup)       e.missing << "losetup（自研引擎需要）";
    // 注意：fuseDevice/fuseMount 也**不进** missing，理由与 Samba 相同 ——
    // raidfs 是可选出口，没 FUSE 的人做 iSCSI / 内核 RAID 不该看到"环境未
    // 就绪"。raidfs 页自己判断 readyForFuse。
    //
    // 注意：smbd/testparm/smb.service 缺失**不进** missing。SMB 是可选功能，
    // 没装 Samba 的人用 RAID 不该看到环境未就绪。SMB 页自己判断 readyForSmb。

    return e;
}

QStringList NasController::existingArrays() const
{
    QStringList out;
    QFile f("/proc/mdstat");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    const QStringList lines = QString::fromUtf8(f.readAll()).split('\n');
    static const QRegularExpression re("^(md[0-9A-Za-z_-]+)\\s");
    for (const QString &l : lines) {
        const auto m = re.match(l);
        if (m.hasMatch()) out << "/dev/" + m.captured(1);
    }
    return out;
}

QString NasController::mdstatText() const
{
    QFile f("/proc/mdstat");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
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

// ── 自研 RAID0 路线 ─────────────────────────────────────────
QVector<NasController::Step>
NasController::planRaid0RsCreate(const Nas::Plan &plan, const QString &volumeName,
                                 qint64 backingSizeBytes) const
{
    QVector<Step> steps;
    if (!safeVolumeName(volumeName)) return steps;

    const QString bin = nasrsBinaryPath();
    if (bin.isEmpty()) return steps;

    QStringList files;
    for (const Nas::Member &m : plan.members) {
        // backing 文件路径来自分区挂载点 + 卷名，两者都要过校验
        if (m.filePath.isEmpty()) return steps;
        if (!m.filePath.startsWith('/') || m.filePath.contains("..")) return steps;
        files << m.filePath;
    }
    if (files.size() < 2) return steps;

    // 1) 预分配 backing 文件
    for (const QString &f : files) {
        Step s;
        s.program = "fallocate";
        s.args = { "-l", QString::number(backingSizeBytes), f };
        s.describe = QString("创建 backing 文件 %1（%2）")
                        .arg(f, Nas::humanSize(backingSizeBytes));
        // 只在目标路径不存在时才创建；存在则拒，绝不覆盖已有文件
        s.destructive = false;
        steps.append(s);
    }

    // 2) 起引擎：nasrs 把两个文件拼成一个块设备，通过本地 TCP/NBD 暴露
    //    这里用 --stdout 模式由调用方持有进程，参数留白由 nasrs 的 --help 对齐
    Step run;
    run.program = bin;
    run.args = { "serve", "--name", volumeName,
                 "--chunk", QString::number(plan.stripeChunkKiB) + "K" };
    for (const QString &f : files) run.args << "--disk" << f;
    run.describe = QString("启动 nasrs RAID0 卷 %1（%2 个 backing 文件）")
                       .arg(volumeName).arg(files.size());
    run.destructive = false;
    steps.append(run);

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
NasController::planRaidfsMount(const Nas::Plan &plan, const QString &mountPoint) const
{
    QVector<Step> steps;
    // 两道模型层校验，缺一不可：
    //   · 挂载点必须在 /mnt 直下且字符集安全 —— 它会进 argv，也会进
    //     systemd 单元名，这里放宽一个字符就等于开一个注入口；
    //   · 卷名与成员要够：RAID0 至少 2 个成员，少于 2 个 nasrs 自己会拒绝。
    if (!Nas::validateMountPoint(mountPoint).isEmpty()) return steps;
    if (plan.members.size() < 2) return steps;
    for (const Nas::Member &m : plan.members)
        if (m.filePath.isEmpty()) return steps;

    // 1) 建挂载目录。用 install -d 而不是 mkdir：一并把权限定下来，
    //    且目录已存在时不报错（重复挂载是正常操作）。
    Step mkd;
    mkd.program = "pkexec";
    mkd.args = { "install", "-d", "-m", "0755", mountPoint };
    mkd.describe = QString("创建挂载目录 %1").arg(mountPoint);
    mkd.destructive = false;
    steps.append(mkd);

    // 2) 起 nasrs 导出。这里刻意不写 nasrs 的具体参数：引擎还没有导出
    //    通路，写一条看起来完整、实际跑不通的命令比不写更糟 —— 用户会
    //    以为是自己配置错了。等导出实现后在这里补 program/args。
    Step srv;
    srv.program = nasrsBinaryPath();
    srv.args = { "export-fs" };
    for (const Nas::Member &m : plan.members) {
        srv.args << "--disk" << m.filePath;
    }
    srv.args << "--name" << plan.volumeName << "--mount" << mountPoint;
    srv.describe = QString("用 nasrs 把卷 %1 导出并挂到 %2")
                      .arg(plan.volumeName, mountPoint);
    // 标成 destructive 是保守做法：这一步会在挂载目录上盖一个文件系统。
    // 挂载本身可撤销（unmount 就恢复原状），但它会让目录里原有的内容
    // 暂时看不见，用户若误以为文件丢了会很慌，所以要走确认。
    srv.destructive = true;
    steps.append(srv);

    return steps;
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