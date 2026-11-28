#include "nasmodel.h"
#include <QByteArray>
#include <pwd.h>
#include <unistd.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QStringList>
#include <QSet>
#include <QPair>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

namespace Nas {

// ── lsblk JSON 解析 ──────────────────────────────────────────
// lsblk 的 SIZE 是字符串（如 "1.8T"）还是字节数取决于版本与参数，
// 这里统一按"字节数字符串"解析；用 lsblk 时务必带 -b 强制字节，
// 否则 "1.8T" 会被 toLongLong() 截成 1，容量判断就全错了。
static qint64 toBytes(const QJsonValue &v)
{
    if (v.isDouble()) return qint64(v.toDouble());
    const QString s = v.toString();
    if (s.isEmpty()) return 0;
    bool ok = false;
    const qint64 n = s.toLongLong(&ok);
    return ok ? n : 0;
}

static Block toBlock(const QJsonObject &o)
{
    Block b;
    b.name       = o.value("name").toString();
    b.type       = o.value("type").toString();
    b.fstype     = o.value("fstype").toString();
    b.mountpoint = o.value("mountpoint").toString();
    b.label      = o.value("label").toString();
    b.uuid       = o.value("uuid").toString();
    b.size       = toBytes(o.value("size"));
    // lsblk 的 name 不带 /dev/，自己拼；mapper 类设备要特殊处理
    b.path = b.name.startsWith("/") ? b.name : ("/dev/" + b.name);

    const QJsonArray kids = o.value("children").toArray();
    for (const QJsonValue &k : kids)
        b.children.append(toBlock(k.toObject()));
    return b;
}

QVector<Block> parseLsblk(const QByteArray &json)
{
    QVector<Block> out;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return out;
    const QJsonArray devs = doc.object().value("blockdevices").toArray();
    for (const QJsonValue &d : devs)
        out.append(toBlock(d.toObject()));
    return out;
}

QVector<Block> flatten(const QVector<Block> &tree)
{
    QVector<Block> out;
    for (const Block &b : tree) {
        // 注意：先把孩子拷出来再 append 自己。若先 append 再递归，
        // push_back 可能触发 QVector 扩容搬迁，引用 b 就成了悬垂引用。
        const QVector<Block> kids = b.children;
        out.append(b);
        out += flatten(kids);
    }
    return out;
}

// ── 分区名 → 父盘名 ──────────────────────────────────────────
// nvme0n1p2 -> nvme0n1；mmcblk0p1 -> mmcblk0；sda2 -> sda；vda1 -> vda
// 规则：nvme/mmcblk 用最后一个 'p' 切；其余（sd/vd/xvd/hd）切掉尾部数字。
QString parentDiskOf(const QString &name)
{
    if (name.isEmpty()) return name;

    if (name.startsWith("nvme") || name.startsWith("mmcblk")
        || name.startsWith("loop") || name.contains("np")) {
        const int k = name.lastIndexOf('p');
        if (k > 0 && name.at(k + 1).isDigit())
            return name.left(k);
        return name;
    }

    // SCSI / virtio 风格：sdXN、vdXN、xvdXN、hdXN —— 只切尾部连续数字，
    // 字母部分（sda 的 a）必须留下，否则 sda1 会变成 "sd"，判重会误伤整盘。
    int end = name.size();
    while (end > 0 && name.at(end - 1).isDigit()) --end;
    return end > 0 ? name.left(end) : name;
}

// ── 系统盘识别 ───────────────────────────────────────────────
// 只认 /proc/mounts 里挂成 /、/boot、/boot/efi 的设备，把这些设备的
// **整块盘**也算进去：选系统盘上任何一个分区都危险。
QVector<QString> systemDiskNames(const QString &procMounts)
{
    static const QStringList critical = { "/", "/boot", "/boot/efi", "/home" };
    QSet<QString> parts;   // "sda2"
    for (const QString &line : procMounts.split('\n')) {
        const QStringList f = line.split(' ');
        if (f.size() < 2) continue;
        const QString dev = f[0], mnt = f[1];
        if (!critical.contains(mnt)) continue;
        if (!dev.startsWith("/dev/")) continue;   // tmpfs/overlay 等
        parts.insert(dev.section('/', 2));        // /dev/sda2 -> sda2
    }

    QSet<QString> disks;
    for (const QString &p : parts)
        disks.insert(parentDiskOf(p));

    QVector<QString> out;
    for (const QString &d : disks) out << d;
    return out;
}

// ── 判定 ────────────────────────────────────────────────────
static bool looksLikeRaidMember(const QString &fstype)
{
    static const QStringList sigs = {
        "linux_raid_member", "isw_raid_member", "romise_raid_member",
        "hpfs_raid_member", "ddf_raid_member", "LVM2_member",
        "crypto_LUKS", "zfs_member", "btrfs",
    };
    return sigs.contains(fstype);
}

// 最小可用容量：2 GiB。RAID0 至少两个成员，太小没意义。
static constexpr qint64 kMinBytes = 2LL * 1024 * 1024 * 1024;

// 该引擎是否要求分区**已有可写的文件系统**。
//
// mdadm 直接拿分区当成员（会抹掉上面的文件系统），所以"已挂载"是障碍、
// "空分区"正合适。两条自研路线相反：它们只在已有的文件系统里放东西
// （mergerfs 用它的挂载点当 source，镜像放一个目录），所以"已挂载"是前提、"空分区"用不了。
//
// 写成 `!= Mdadm` 而不是枚举两个自研值：将来再加第三条不碰分区的路线时，
// 枚举的写法会静默走回 mdadm 的判定 —— 那意味着界面会把"已挂载的分区"标成
// 不可选，或者更糟：把空分区标成可选，然后 nasrs 在用户点创建时才开始报错。
static bool needsExistingFileSystem(Engine e) { return e != Engine::Mdadm; }

Verdict judge(const Block &b, const QVector<QString> &systemDisks, Engine engine)
{
    Verdict v;

    // 回环/光驱不是真实磁盘
    if (b.type == "loop" || b.type == "rom") {
        v.risk = Risk::LoopOrRom;
        v.reason = "回环设备或光驱，不是真实磁盘";
        v.selectable = false;
        return v;
    }

    // 整盘：只允许选分区。整盘操作会抹掉分区表和盘上所有分区。
    if (b.type == "disk") {
        v.risk = Risk::WholeDisk;
        v.reason = "整块磁盘，请展开选择它下面的分区";
        v.selectable = false;
        return v;
    }

    // 系统盘（含 /home，动了会丢用户数据）
    // 注意判的是**父盘名**，不能对分区名直接 startsWith：那样 "sda10"
    // 会被系统盘 "sda1" 前缀匹配上，把无关分区误标成系统盘。
    const QString parent = parentDiskOf(b.name);
    if (systemDisks.contains(parent)) {
        v.risk = Risk::FileSystemRoot;
        v.reason = QString("所在磁盘 %1 承载系统或用户分区，不可动").arg(parent);
        v.selectable = false;
        return v;
    }

    if (b.type != "part") {
        v.risk = Risk::WholeDisk;
        v.reason = QString("类型为 %1，只支持分区").arg(b.type);
        v.selectable = false;
        return v;
    }

    // 已挂载：两条路线结论**相反**，这是本文件最容易被"顺手统一掉"的地方
    if (!b.mountpoint.isEmpty()) {
        if (needsExistingFileSystem(engine)) {
            // 自研引擎要在它已有的文件系统里放东西（文件或目录），挂载着是前提而非障碍
            if (b.fstype.isEmpty()) {
                v.risk = Risk::HasFilesystem;
                v.reason = "已挂载但文件系统类型未知，无法确认能否放文件";
                v.selectable = false;
                return v;
            }
            v.risk = Risk::Safe;
            return v;
        }
        v.risk = Risk::Mounted;
        v.reason = QString("已挂载在 %1；mdadm 会抹掉该分区，先卸载").arg(b.mountpoint);
        v.selectable = false;
        return v;
    }

    if (b.fstype == "swap") {
        v.risk = Risk::Swap;
        v.reason = "swap 分区";
        v.selectable = false;
        return v;
    }

    if (b.size > 0 && b.size < kMinBytes) {
        v.risk = Risk::TooSmall;
        v.reason = QString("容量 %1，小于 2 GiB").arg(humanSize(b.size));
        v.selectable = false;
        return v;
    }

    if (looksLikeRaidMember(b.fstype)) {
        v.risk = Risk::ExistingRaid;
        v.reason = QString("已是 %1 成员，抹掉会连带破坏其阵列").arg(b.fstype);
        v.selectable = false;
        return v;
    }

    if (!b.fstype.isEmpty()) {
        // 有文件系统但未挂载：技术上可选，但会毁数据 → 强制二次确认
        v.risk = Risk::HasFilesystem;
        v.reason = QString("含 %1 文件系统").arg(b.fstype);
        v.selectable = true;
        v.needsConfirm = true;
        v.confirmText = QString("%1 上有 %2 文件系统（UUID %3），"
                                "加入 RAID 会**全部销毁**其内容。").arg(b.name, b.fstype,
                                b.uuid.isEmpty() ? "无" : b.uuid);
        return v;
    }

    // 空分区（无文件系统）：
    //   mdadm    正合适 —— 直接当成员
    //   自研引擎  用不了 —— 没文件系统就放不下 backing 文件或镜像目录，
    //             而本工具不会替你 mkfs（那是更大的破坏）
    if (needsExistingFileSystem(engine)) {
        v.risk = Risk::HasFilesystem;
        v.reason = (engine == Engine::MirrorRs)
                     ? "没有文件系统，自研镜像无法在其上建副本目录"
                     : "没有文件系统，自研引擎无法在其上建 backing 文件";
        v.selectable = false;
        return v;
    }

    v.risk = Risk::Safe;
    return v;
}


// ── 阵列容量计算 ────────────────────────────────────────────
// RAID0 的可用容量是 **最小成员 × 成员数**，不是各成员之和。
// 这是 RAID0 最反直觉也最容易出事的地方：100G + 500G 只有 200G 可用，
// 多出来的 300G 永远用不上。用户挑分区时多半以为能得 600G，
// 所以这里把浪费量算出来、在 UI 上明说。
// 成员的分区路径列表。镜像那几条判定按父盘名算，得先从 Member 里把 device 掏出来。
static QVector<QString> devicesOf(const Plan &plan)
{
    QVector<QString> v;
    v.reserve(plan.members.size());
    for (const Member &m : plan.members) v << m.device;
    return v;
}

ArrayInfo evaluateArray(const Plan &plan)
{
    ArrayInfo info;
    info.members = plan.members.size();

    if (info.members == 0) {
        info.health = ArrayHealth::Empty;
        info.note = "还没有成员，拖动接线到 RAID 卡口加入阵列。";
        return info;
    }

    qint64 minCap = -1;
    for (const Member &m : plan.members) {
        if (m.capacity <= 0) {
            info.health = ArrayHealth::CapacityLow;
            info.note = "有成员容量未知，先刷新分区列表。";
            info.members = info.members;
            return info;
        }
        info.totalRaw += m.capacity;
        if (minCap < 0 || m.capacity < minCap) minCap = m.capacity;
    }

    // ── mergerfs：可用容量 = 各成员空闲之和 ──
    // 这条必须走在通用算法前面。通用算法算的是 RAID0 的 min×N，mergerfs 拿到
    // 那个数字会报出"200G+100G = 200G"这种账（min 100 × 2 = 200，而真实可写
    // 是 300）。mergerfs 没有条带、没有副本，容量就是各分区当前空闲加起来。
    // 走通用算法还会顺带报一个"浪费 %1" —— mergerfs 什么都不浪费，那条浪费
    // 说明会让用户去找根本不存在的水分。
    if (plan.engine == Engine::MergerFs) {
        info.usable = info.totalRaw;
        info.wasted = 0;

        const QString memberWarn = mergerMembersWarning(devicesOf(plan));
        if (!memberWarn.isEmpty()) {
            info.health = ArrayHealth::Single;
            info.note = memberWarn;
            return info;
        }
        info.health = ArrayHealth::Ok;
        info.note = QString("mergerfs：可用 %1（各分区空闲之和），策略 %2。"
                            "一个文件整个落在一个分区上 —— 没有条带；"
                            "坏一块盘丢的是它上面的文件，不是整卷。")
                        .arg(humanSize(info.usable), mergerPolicyLabel(plan.mergerPolicy));
        return info;
    }

    if (info.members == 1) {
        // 单个成员就是一条直连盘，不是 RAID；允许但要说清
        info.health = ArrayHealth::Single;
        info.usable = minCap;
        info.note = "只有一个成员，等同直连单盘，没有条带加速。";
        return info;
    }

    // ── 镜像：可用容量 = 最小的那个成员，不是各成员之和 ──
    // 与 RAID0 的账**正好相反**，所以这条必须走在通用算法前面：
    // RAID0 是 min×N（100G+100G 得 200G），镜像是 min×1（100G+100G 还是 100G，
    // 另一份是保险）。用通用算法报出来的数字会让用户以为镜像能多倍扩容，
    // 而真实能写进去的只有最小的那块盘的量 —— 写满才发现是"半份副本"。
    if (plan.engine == Engine::MirrorRs) {
        info.usable = minCap;
        info.wasted = info.totalRaw - minCap;   // 副本那份 + 容量差的零头

        const QString memberWarn = mirrorMembersWarning(devicesOf(plan));
        if (!memberWarn.isEmpty()) {
            // 一主一备凑不齐、或两份在同一块盘上：这里就把话说明白，
            // 不放到"点创建之后"才报错 —— 那时用户已经填完名字、按过确认了。
            info.health = ArrayHealth::Single;
            info.note = memberWarn;
            return info;
        }
        info.health = ArrayHealth::Ok;
        info.note = QString("镜像：可用 %1，另有 %2 作为副本（不计容量）。写满 %1 就真满了。")
                        .arg(humanSize(info.usable), humanSize(minCap));
        return info;
    }

    info.usable = minCap * info.members;
    info.wasted = info.totalRaw - info.usable;

    if (info.wasted > 0) {
        info.health = ArrayHealth::Uneven;
        info.note = QString("各分区容量不等，按最小的 %1 计，"
                            "多出的 %2 用不上（合计 %3，实际可用 %4）。")
                        .arg(humanSize(minCap), humanSize(info.wasted),
                             humanSize(info.totalRaw), humanSize(info.usable));
    } else {
        info.health = ArrayHealth::Ok;
        info.note = QString("%1 路条带，可用 %2。")
                        .arg(info.members).arg(humanSize(info.usable));
    }
    return info;
}

// ── 软件镜像（nasrs RAID1）──────────────────────────────────

QString mirrorDirPath(const QString &mountpoint, const QString &volumeName)
{
    if (mountpoint.isEmpty() || volumeName.isEmpty()) return QString();
    if (!mountpoint.startsWith('/')) return QString();

    // 卷名洗一遍：它会变成目录名，而目录名之后会进 nasrs 的 argv。
    // 清洗后为空就返回空串，不猜一个 "unnamed.nas-mirror" ——
    // 那会把"你还没填名字"这件事伪装成"程序建了个怪名字的目录"。
    QString n = volumeName;
    n.replace(QRegularExpression("[^A-Za-z0-9._-]"), "_");
    n = n.remove(QRegularExpression("^[._-]+"));
    if (n.isEmpty()) return QString();

    const QString base = QDir::cleanPath(mountpoint);
    // 挂载点若是 "/" 就不该在它下面建卷目录：那是系统根，
    // 在里面凭空多出一个目录，和"数据都在数据盘上"这个前提对不上。
    if (base == "/" || base == "/mnt" || base == "/media") return QString();
    return base + "/" + n + ".nas-mirror";
}

QString validateMirrorDir(const QString &path)
{
    if (path.isEmpty()) return QString::fromUtf8("镜像目录不能为空");
    if (!path.startsWith('/')) return QString::fromUtf8("镜像目录必须是绝对路径");
    const QString clean = QDir::cleanPath(path);
    if (clean != path)
        return QString::fromUtf8("镜像目录要写成规范路径（不要含 .. 或 //）：") + clean;

    const QStringList parts = clean.mid(1).split('/', Qt::SkipEmptyParts);
    // 至少两层：一层意味着它要么是两个副本的公共父目录（/mnt），
    // 要么是一个分区挂载点本身（/data）—— 后者在 --delete-extras 下会清空整个分区。
    if (parts.size() < 2)
        return QString::fromUtf8("镜像目录要在挂载点之下，例如 /mnt/d1/nas.nas-mirror");

    static const QRegularExpression ok("^[A-Za-z0-9][A-Za-z0-9._-]*$");
    for (const QString &p : parts)
        if (!ok.match(p).hasMatch())
            return QString::fromUtf8("目录名只能用字母、数字、点、下划线、短横，且以字母数字开头：") + p;

    // 系统目录一律不许。这一条不是装饰：镜像的执行命令带删除能力，
    // 而"填错一个目录"和"删掉系统里的东西"之间只差一次回车。
    static const QSet<QString> deny = {
        "bin", "boot", "dev", "etc", "lib", "lib32", "lib64",
        "proc", "root", "run", "sbin", "srv", "sys", "usr", "var"
    };
    if (deny.contains(parts.first()))
        return QString::fromUtf8("镜像目录不能在系统目录 %1 下面").arg(parts.first());
    return QString();
}

QString mirrorDirConflict(const QString &path, const QStringList &mountPoints)
{
    const QString clean = QDir::cleanPath(path);
    for (const QString &m : mountPoints) {
        const QString mm = QDir::cleanPath(m);
        if (mm.isEmpty()) continue;
        // 只判"正好等于挂载点"这一种。判"在挂载点之内"是错的：
        // 镜像目录本来就该在某个挂载点之内，那正是它的定义。
        if (clean == mm)
            return QString::fromUtf8(
                "镜像目录正好等于挂载点 %1：那意味着镜像对象是整个分区，"
                "而删除多余文件那一步会把该分区上原有的内容全部删掉。"
                "请在它下面再指一层子目录。").arg(mm);
    }
    return QString();
}

// 父盘计算只写一份：镜像与 mergerfs 的"同盘"是**同一条判据**，只是话要说两遍。
// 共用文案会逼着 mergerfs 说"两份副本"这种镜像的词 —— mergerfs 根本没有副本。
static QSet<QString> parentDisksOf(const QVector<QString> &devicePaths, bool *incomplete)
{
    QSet<QString> parents;
    *incomplete = false;
    for (const QString &p : devicePaths) {
        const QString name = p.section('/', -1);   // /dev/sdb1 → sdb1
        if (name.isEmpty()) { *incomplete = true; return parents; }
        parents.insert(parentDiskOf(name));
    }
    return parents;
}

// 判据与文案只写一份：两个 API（issue / warning）都从这里出来，且**文案在这里
// 就格式化好**。分开写会漂 —— 漂出来的结果是"界面放行了，模型层还在拒绝"，
// 或者反过来：红字说明与放行条件对不上。
// （上一版在这里留了 %1 让调用方填，两种 issue 的 %1 是不同东西：Count 填的
// 是成员数、SameDisk 填的是盘名，而"设备名不完整"那条要填的是路径 ——
// 让调用方填等于三处各填各的，必然漏。）
static QPair<MembersIssue, QString> mirrorMembersCheck(const QVector<QString> &devicePaths)
{
    if (devicePaths.size() != kMirrorMembers)
        return { MembersIssue::Count,
                 QString::fromUtf8("镜像阵列要正好 %1 个成员（一主一备），当前 %2 个。")
                     .arg(kMirrorMembers).arg(devicePaths.size()) };

    bool incomplete = false;
    const QSet<QString> parents = parentDisksOf(devicePaths, &incomplete);
    if (incomplete)
        return { MembersIssue::Count,
                 QString::fromUtf8("成员设备名不完整：%1").arg(devicePaths.first()) };
    if (parents.size() == 1)
        return { MembersIssue::SameDisk,
                 QString::fromUtf8("两个分区都在 %1 上：这块盘一坏，两份副本一起没 —— "
                                   "那等于没有镜像。要就用也行（同一块盘上的备份目录，"
                                   "防误删有用、防坏盘没用），点右边的「知道了」就放行。")
                     .arg(*parents.constBegin()) };
    return { MembersIssue::None, QString() };
}

static QPair<MembersIssue, QString> mergerMembersCheck(const QVector<QString> &devicePaths)
{
    if (devicePaths.size() < kMergerMinMembers)
        return { MembersIssue::Count,
                 QString::fromUtf8("mergerfs 要至少 %1 个分区才有意义（1 个就是直连单盘，"
                                   "没有合并这件事），当前 %2 个。")
                     .arg(kMergerMinMembers).arg(devicePaths.size()) };

    bool incomplete = false;
    const QSet<QString> parents = parentDisksOf(devicePaths, &incomplete);
    if (incomplete)
        return { MembersIssue::Count,
                 QString::fromUtf8("成员设备名不完整：%1").arg(devicePaths.first()) };
    if (parents.size() == 1)
        return { MembersIssue::SameDisk,
                 QString::fromUtf8("%1 个分区都在 %2 上：mergerfs 摊写落在同一块盘上，"
                                   "没有加速（只有一个设备在转），坏盘时这些分区上的文件"
                                   "一起没。要就用也行（同一块盘上的一个大目录），"
                                   "点右边的「知道了」就放行。")
                     .arg(devicePaths.size()).arg(*parents.constBegin()) };
    return { MembersIssue::None, QString() };
}

MembersIssue mirrorMembersIssue(const QVector<QString> &devicePaths)
{
    return mirrorMembersCheck(devicePaths).first;
}

QString mirrorMembersWarning(const QVector<QString> &devicePaths)
{
    return mirrorMembersCheck(devicePaths).second;
}

MembersIssue mergerMembersIssue(const QVector<QString> &devicePaths)
{
    return mergerMembersCheck(devicePaths).first;
}

QString mergerMembersWarning(const QVector<QString> &devicePaths)
{
    return mergerMembersCheck(devicePaths).second;
}

// ── mergerfs 参数与文案 ──────────────────────────────────────

// 只给三条，每条都**在真机上试过**（mergerfs 2.42.0，实测 `-o category.create=…`）：
//   mfs  最空闲优先（默认）
//   ff   第一块盘优先（"first found"）：新文件总落第一支，写满才轮到下一支
//   lfs  最紧优先
// 早先这里写的是 "eol"，mergerfs 直接报 `invalid value - category.create=eol` 并
// 拒绝挂载 —— 那个名字是编出来的，不是它认的策略。策略名一律以 `mergerfs -h` 与
// 实测为准，不凭印象写。
QString mergerPolicyOption(MergerPolicy p)
{
    switch (p) {
        case MergerPolicy::Mfs: return "mfs";
        case MergerPolicy::Ff:  return "ff";
        case MergerPolicy::Lfs: return "lfs";
    }
    return "mfs";
}

QString mergerPolicyLabel(MergerPolicy p)
{
    switch (p) {
        case MergerPolicy::Mfs: return QString::fromUtf8("最空闲优先（mfs）");
        case MergerPolicy::Ff:  return QString::fromUtf8("顺序填满（ff）");
        case MergerPolicy::Lfs: return QString::fromUtf8("最紧优先（lfs）");
    }
    return QString::fromUtf8("最空闲优先（mfs）");
}

// 策略的后果必须写在选它的那一刻，不能写进 tooltip：这里选错的代价是"我以为
// 数据撒在几块盘上，其实全在第一块盘上"，那要当场看见。
// 刻意不用 **加粗**：状态栏是纯文本，星号会原样显示出来。
QString mergerPolicyNote(MergerPolicy p)
{
    switch (p) {
        case MergerPolicy::Ff:
            return QString::fromUtf8("顺序填满：新文件总落在第一个分区，写满了才轮到下一个。"
                                     "账最好算，但第一块盘承担了全部写入 —— 它坏掉丢得最多。"
                                     "一个文件整个落在一个分区上，mergerfs 没有条带。");
        case MergerPolicy::Lfs:
            return QString::fromUtf8("最紧优先：新文件写到当前最紧的分区，把空的留给后来的"
                                     "大文件。少用。一个文件整个落在一个分区上，没有条带。");
        case MergerPolicy::Mfs:
            return QString::fromUtf8("最空闲优先：新文件写到当前最空的分区，各盘使用率均衡。"
                                     "一个文件整个落在一个分区上 —— mergerfs 没有条带，"
                                     "坏一块盘丢的是它上面的文件，不是整卷。");
    }
    return QString();
}

QString validateMergerSource(const QString &path)
{
    if (path.isEmpty())
        return QString::fromUtf8("source 不能为空：分区没挂载就没有 source");
    if (!path.startsWith('/'))
        return QString::fromUtf8("source 必须是绝对路径");
    if (QDir::cleanPath(path) != path)
        return QString::fromUtf8("source 路径要规范（不要有 // 或 ./）");
    // /、/mnt、/media 本身不能当 source：那等于"把整个系统/整层挂载树"
    // 变成一个 union 的一支，写进去的东西落在根盘上，而 mergerfs 会把
    // 那一整支的容量算进阵列。
    if (path == "/" || path == "/mnt" || path == "/media")
        return QString::fromUtf8("source 不能是 /、/mnt 或 /media 本身 —— 那是整层挂载树，"
                                 "不是某个分区。请在它下面指一层目录。");
    // mergerfs 用 ':' 分隔 source。挂载点里带冒号（label 里真出现过）会被读成
    // 两个 source，把同一份数据当两支用。这一条在字符串层面挡掉，不转义。
    if (path.contains(':'))
        return QString::fromUtf8("source 路径不能含冒号：mergerfs 用冒号分隔 source，"
                                 "含冒号会被解析成两个 source。");
    // 不按"至少两层"判。单层路径（/vol1、/data）是分区挂载点最常见的形状 ——
    // 本项目的部署就是宿主 /mnt/vol1 挂进容器的 /vol1，那条规则会把真正要用的
    // 那种路径挡在门外（实测 /vol1 被判"至少两层"，一路 fail 到创建按钮）。
    // 该挡的不是"层数少"，而是**系统目录**：把 /proc 并进来等于把内核视图摊进
    // 合并目录，写进去的东西还落在根盘上。
    static const QStringList kReserved = {
        QStringLiteral("/bin"),  QStringLiteral("/boot"), QStringLiteral("/dev"),
        QStringLiteral("/etc"),  QStringLiteral("/home"), QStringLiteral("/lib"),
        QStringLiteral("/lib64"), QStringLiteral("/proc"), QStringLiteral("/root"),
        QStringLiteral("/run"),  QStringLiteral("/sbin"), QStringLiteral("/srv"),
        QStringLiteral("/sys"),  QStringLiteral("/tmp"),  QStringLiteral("/usr"),
        QStringLiteral("/var"),
    };
    if (kReserved.contains(path))
        return QString::fromUtf8("%1 是系统目录，不能当 source —— 并进来等于把系统树"
                                 "摊进合并目录，写进去的东西落在根盘上。").arg(path);
    return QString();
}

QString mergerSourcesConflict(const QVector<QString> &paths)
{
    QSet<QString> seen;
    for (const QString &p : paths) {
        if (seen.contains(p))
            return QString::fromUtf8("同一个目录出现两次（%1）：mergerfs 会把同一份数据当两支 source，"
                                     "容量算两遍，删除动作也会作用到不该作用的那一层。").arg(p);
        seen.insert(p);
    }
    for (const QString &outer : paths)
        for (const QString &inner : paths)
            if (inner != outer && inner.startsWith(outer + "/"))
                return QString::fromUtf8("%1 在 %2 里面：嵌套的 source 会让同一份数据被算两次容量。")
                        .arg(inner, outer);
    return QString();
}

QStringList mergerfsArgs(const QVector<QString> &sources, const QString &mountPoint,
                         MergerPolicy p)
{
    QStringList args;
    // minfreespace 用 mergerfs 自己的默认值（4G）：mfs 类策略在一个分区接近写满时
    // 不该再把新文件塞进去，这条阈值是策略的一部分，不是可调参数。
    // allow_other：挂载后 smbd（root）与访客都读得到。少了这条，FUSE 目录默认
    // 只有挂载者能读 —— SMB 那条出口会"挂上了却共享不出东西"，是最难查的那类
    // 界面说成功、别人读不到。
    args << "-o" << QString("category.create=%1,minfreespace=4G,allow_other").arg(mergerPolicyOption(p));
    QStringList srcs;
    for (const QString &s : sources) srcs << s;
    args << srcs.join(':');
    args << mountPoint;
    return args;
}

// ── 已存在的阵列（盘位图上的彩线）──────────────────────────────
QVector<ExistingArray> parseMdstat(const QString &text)
{
    QVector<ExistingArray> out;
    const QStringList lines = text.split('\n');
    for (const QString &line : lines) {
        // 成员/状态行都是缩进的；阵列头一行以 "mdN :" 起头
        const int colon = line.indexOf(':');
        if (colon <= 2) continue;
        const QString head = line.left(colon).trimmed();
        if (!head.startsWith("md")) continue;
        bool numOk = false;
        head.mid(2).toInt(&numOk);
        // md0 / md127 是设备，md_d127 之类是容器名，这里只认设备号
        if (!numOk) continue;

        QStringList members;
        QString raid;
        const QStringList tok = line.mid(colon + 1).split(' ', Qt::SkipEmptyParts);
        for (const QString &raw : tok) {
            const int br = raw.indexOf('[');
            // 带 [n] 才是成员；[2/2] 这种是同步进度，br 位置在开头附近且左段为空
            const QString base = br > 0 ? raw.left(br) : raw;
            if (base.isEmpty() || !base.at(0).isLetterOrNumber()) continue;
            if (base == "active" || base == "inactive" || base == "auto-read-only")
                continue;
            if (base.startsWith("raid") || base == "linear" || base == "multipath"
                || base == "container") {
                raid = base;
                continue;
            }
            if (br <= 0) continue;          // 没有序号：不是成员
            if (base.contains('=')) continue;
            members << "/dev/" + base;
        }
        if (members.isEmpty()) continue;

        ExistingArray a;
        a.engine = Engine::Mdadm;
        a.name = "/dev/" + head;
        a.members = members;
        a.note = QString("mdadm %1 · %2").arg(raid.isEmpty() ? "RAID" : raid.toUpper(),
                                              a.name);
        out << a;
    }
    return out;
}

bool mergerfsCmdlineSources(const QStringList &argv, QStringList *sources,
                            QString *mountPoint)
{
    if (argv.isEmpty()) return false;
    // 用 basename 判，因为可执行名可能是 /usr/local/bin/mergerfs 或包内路径
    if (QFileInfo(argv.first()).fileName() != "mergerfs") return false;

    QStringList positional;
    for (int i = 1; i < argv.size(); ++i) {
        const QString a = argv.at(i);
        if (a == "-o" || a == "--options") { ++i; continue; }    // 选项值不是路径
        if (a.startsWith("-o")) continue;                         // -ocategory.create=…
        if (a == "--") continue;
        if (a.startsWith('-')) continue;
        positional << a;
    }
    if (positional.size() < 2) return false;

    const QStringList srcs = positional.at(0).split(':');
    if (srcs.size() < 1) return false;
    for (const QString &s : srcs)
        if (!s.startsWith('/')) return false;      // 不是挂载点 → 不是我们认的形态

    if (sources) *sources = srcs;
    if (mountPoint) *mountPoint = positional.at(1);
    return true;
}


QString mirrorRoleNote(bool primaryIsAccess)
{
    // 这句是下拉框唯一的后果说明，所以写在状态栏里而不是 tooltip：tooltip 要
    // 鼠标停上去才看得见，而这里选错一次的代价是"别人读到昨天的文件"。
    // 刻意不用 **加粗** 这类标记 —— 状态栏是纯文本，星号会原样显示出来。
    return primaryIsAccess
        ? QString::fromUtf8("访问走主副本：写入也在主副本，副本是它的完整拷贝。"
                            "主盘坏了把这里切到「副本网」，共享立刻还能读到文件。")
        : QString::fromUtf8("访问暂时走副本网：读到的是上一次同步时的内容，"
                            "这期间主副本上的新写入看不见；共享同时被强制只读 —— "
                            "访客写进来的东西会在下一轮同步被盖掉。切回主副本即可恢复。");
}


// ── IQN ────────────────────────────────────────────────────
// iSCSI Qualified Name 规范：iqn.<年份>-<月份>.<反序域名>:<自定义>
// 域名必须是申请人控制的，否则不同机器可能撞名 —— 撞名的两个 target
// 在同一个 initiator 上会互相顶掉，很难排查。
QString makeIqn(const QString &reverseDomain, const QString &dateYYMM,
                const QString &serial)
{
    QString rd = reverseDomain.trimmed();
    rd.replace(' ', '-');
    if (rd.isEmpty()) rd = "local";
    QString d = dateYYMM.trimmed();
    if (d.isEmpty()) d = "26-11";
    QString s = serial.trimmed();
    if (s.isEmpty()) s = "01";
    return QString("iqn.%1.%2:servertoolbox-%3").arg(d, rd, s);
}

QString humanSize(qint64 bytes)
{
    if (bytes <= 0) return "未知";
    static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    double v = double(bytes);
    int i = 0;
    while (v >= 1024.0 && i < 5) { v /= 1024.0; ++i; }
    return QString::number(v, 'f', v < 10 && i > 0 ? 2 : (v < 100 && i > 0 ? 1 : 0))
           + " " + units[i];
}


// ── SMB ──────────────────────────────────────────────────────

QString validateShareName(const QString &name)
{
    // SMB 共享名字符集：[A-Za-z0-9._ -]，且不能是保留名。
    // 长度上限 31 是 MS 传统限制（超过在老客户端上会出问题）。
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) return QString("共享名不能为空");
    if (trimmed.size() > 31) return QString("共享名超过 31 字符（SMB 传统上限）");

    // 反斜杠会把 shareName 变成 UNC 路径的一部分，绝对禁止
    static const QString illegal = QString("\\/*?\"<>|:");
    for (const QChar &c : trimmed)
        if (illegal.contains(c))
            return QString("共享名含非法字符 '%1'").arg(c);

    // 保留名（大小写不敏感）：这些是 IPC$ 等系统隐式共享或设备名，
    // 用它们当共享名要么被 Samba 忽略，要么和内置共享打架。
    const QString upper = trimmed.toUpper();
    static const QStringList reserved = {
        "IPC$", "ADMIN$", "print$", "HOMES", "CON", "PRN",
        "AUX", "NUL", "COM1", "COM2", "COM3", "COM4",
        "LPT1", "LPT2", "LPT3" };
    if (reserved.contains(upper))
        return QString("'%1' 是 SMB 保留名，换一个").arg(trimmed);

    // 结尾的 $ 在 SMB 里表示隐藏共享，家庭场景多半是误输入
    if (upper.endsWith("$"))
        return QString("以 $ 结尾会成为隐藏共享，通常不是本意");

    return QString();   // 合法
}

bool isRiskySharePath(const QString &path, const QStringList &systemMounts)
{
    const QString p = path.trimmed();
    if (p.isEmpty()) return true;                 // 空路径当然危险
    if (p == "/") return true;                    // 共享整个根

    // 命中任一系统挂载点：/home 会暴露所有用户家目录，/boot /boot/efi
    // 会让共享穿越到引导分区。
    //
    // 关键：'/' 必须单独处理。若把 '/' 当普通挂载走下面的前缀判断，
    // "p.startsWith('/')" 对**任何**绝对路径都成立，于是共享 /mnt/d
    // 也判危险 —— 护栏什么都拦不住，因为什么都被拦，用户就会无视它。
    // （这个 bug 正是 smb_probe 里 "/mnt/d 不判危险" 两条失败暴露的。）
    for (const QString &mRaw : systemMounts) {
        const QString m = mRaw.trimmed();
        if (m.isEmpty()) continue;
        if (m == "/") continue;                    // 根已在上面精确判过
        if (p == m) return true;
        // 被共享路径落在某系统挂载之内：p 是 m 或 m 的子路径
        if (p.startsWith(m + "/")) return true;
        // 反向：被共享路径是某系统挂载的父目录（如共享 /mnt 覆盖 /mnt/efi）
        if (m.startsWith(p + "/")) return true;
    }
    return false;
}

QString validateSharePath(const QString &path)
{
    const QString p = path.trimmed();
    if (p.isEmpty()) return QString("路径不能为空");
    if (!p.startsWith('/')) return QString("必须是绝对路径");
    // Samba 的 path= 值不支持分号，且换行会破坏 ini 结构
    if (p.contains(';')) return QString("路径含分号，Samba 会解析失败");
    if (p.contains('\n') || p.contains('\r'))
        return QString("路径含换行，会破坏 smb.conf 结构");
    return QString();
}

// 把值安全地写进 ini：转义会让 share 段错位的字符。
static QString iniEscape(const QString &v)
{
    QString out;
    out.reserve(v.size());
    for (const QChar &c : v) {
        if (c == '\n' || c == '\r') continue;   // 丢弃换行
        if (c == ';') out += "\\;";               // 分号会开启注释
        else out += c;
    }
    return out;
}

QString makeShareSection(const SmbShare &share)
{
    // 只负责生成 [share] 段文本，落盘与 reload 由上层决定。
    // force user 只在 Password 模式写：guest 模式下它和 guest ok 冲突。
    QString s;
    s += "[" + iniEscape(share.shareName) + "]\n";
    s += "   path = " + iniEscape(share.path) + "\n";
    s += "   browseable = " + QString(share.browseable ? "yes" : "no") + "\n";

    switch (share.auth) {
    case SmbAuth::Password:
        s += "   guest ok = no\n";
        s += "   writable = " + QString(share.writable ? "yes" : "no") + "\n";
        s += "   valid users = " + iniEscape(share.user) + "\n";
        s += "   force user = " + iniEscape(share.user) + "\n";
        break;
    case SmbAuth::GuestRO:
        // 匿名只读：即使 writable 传了 true 也强制只读，
        // 避免 UI 层一个 checkbox 疏忽就变成匿名可写。
        s += "   guest ok = yes\n";
        s += "   writable = no\n";
        s += "   read only = yes\n";
        s += "   force user = " + iniEscape(share.user) + "\n";
        break;
    case SmbAuth::GuestRW:
        s += "   guest ok = yes\n";
        s += "   writable = yes\n";
        s += "   read only = no\n";
        s += "   force user = " + iniEscape(share.user) + "\n";
        break;
    }
    return s;
}


QString suggestShareName(const QString &mountpoint, const QString &partitionName)
{
    // 优先用挂载点最后一段（/mnt/d → d），否则退回分区名。
    QString base = mountpoint.trimmed();
    if (!base.isEmpty()) {
        const QString seg = base.split('/', Qt::SkipEmptyParts).last();
        if (!seg.isEmpty()) base = seg;
    }
    if (base.isEmpty()) base = partitionName.trimmed();
    if (base.isEmpty()) base = QStringLiteral("share");

    // 只保留 SMB 允许的字符，其余换成 -。
    QString clean;
    clean.reserve(base.size());
    for (const QChar &c : base) {
        if (c == ' ' || c == '-') { clean += c; continue; }
        if (c.isLetterOrNumber() || c == '.' || c == '_') clean += c;
        else clean += '-';
    }
    // 去首尾空白/连字符，限长 31
    clean = clean.trimmed();
    while (clean.startsWith('-')) clean.remove(0, 1);
    while (clean.endsWith('-') || clean.endsWith('.')) clean.chop(1);
    if (clean.size() > 31) clean = clean.left(31);
    if (clean.isEmpty()) clean = QStringLiteral("share");

    // 命中保留名 / 以 $ 结尾会被 validateShareName 拒掉，这里直接规避：
    // 命中就加数字后缀，保证 UI 预填的默认值一定合法。
    static const QStringList reserved = {
        "IPC$", "ADMIN$", "print$", "HOMES", "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "LPT1", "LPT2", "LPT3" };
    if (reserved.contains(clean.toUpper()) || clean.endsWith("$"))
        clean += "2";
    return clean;
}

QString defaultSharePath(const QString &mountpoint)
{
    const QString p = mountpoint.trimmed();
    return p;   // 空串 = 未挂载，调用方据此提示
}

QString currentUserName()
{
    // 优先 SUDO_USER：以 pkexec/sudo 起来时 USER 常是 root，
    // 把 root 写成 force user 等于让局域网访客拿到 root 权限的文件。
    const QByteArray su = qgetenv("SUDO_USER");
    if (!su.isEmpty() && su != "root") return QString::fromLocal8Bit(su);
    const QByteArray u = qgetenv("USER");
    if (!u.isEmpty() && u != "root") return QString::fromLocal8Bit(u);
    const QByteArray hl = qgetenv("LOGNAME");
    if (!hl.isEmpty() && hl != "root") return QString::fromLocal8Bit(hl);

    // 环境变量常常不可靠：从桌面启动器、systemd 用户实例、或通过
    // pkexec 起来时 USER/LOGNAME 都可能缺失。回落到真实 uid 的账号名，
    // 这比环境变量更权威 —— 它反映的是进程实际的属主，而不是谁设过变量。
    if (struct passwd *pw = getpwuid(geteuid())) {
        const QString n = QString::fromLocal8Bit(pw->pw_name);
        if (!n.isEmpty() && n != "root") return n;
    }
    return QString();   // 真的拿不到就留空，UI 会要求手填
}


QStringList systemMountPoints(const QVector<Block> &blocks,
                              const QString &procMounts)
{
    QStringList out;
    // 系统盘：承载 / 与 /boot 的那些盘。刻意复用 judge() 用的同一个
    // systemDiskNames —— 两处判定口径必须一致，否则会出现
    // 「这个分区能选为 RAID 成员，却不能被 SMB 共享」这种自相矛盾。
    const QVector<QString> sys = systemDiskNames(procMounts);
    for (const Block &b : blocks) {
        if (b.mountpoint.isEmpty()) continue;
        const QString mp = b.mountpoint;
        if ((mp == "/" || mp == "/boot" || mp == "/home") && !out.contains(mp)) {
            out << mp; continue;
        }
        const QString parent = parentDiskOf(b.name);
        if (!parent.isEmpty() && sys.contains(parent) && !out.contains(mp))
            out << mp;
    }
    return out;
}
// ── 电视墙（只读影视库索引）────────────────────────────────
//
// 只读是硬约束：不重命名、不删除、不改任何媒体文件。刮削器最可怕
// 的不是读，是替用户改文件名，一旦弄错就是不可逆地打乱整个库。

qint64 minVideoBytes() { return qint64(20) * 1024 * 1024; }

static QString extOf(const QString &path)
{
    const int s = path.lastIndexOf('.');
    if (s < 0 || s == path.size() - 1) return QString();
    return path.mid(s + 1).toLower();
}

bool isVideoExtension(const QString &path)
{
    // 刻意不含 "ts"。.ts 既是 MPEG-TS 也是 TypeScript。把判定责任丢给
    // 调用方（"记得再用体积过滤"）等于埋一个必然踩到的坑：只要有一处
    // 忘了，源代码就会被当成影片列进电视墙。所以这里直接不认，
    // 需要认 MPEG-TS 的调用方必须显式走 isVideoFile()。

    // 同理，TypeScript 的类型声明 .d.ts / .d.mts / .d.cts 必须挡掉：
    // mts/cts 看起来像 MPEG-TS，实测真把 index.d.mts 列成了影片。
    // 这里不靠体积兜 —— 类型声明也可能几十 KB 以上，而真正的 .mts
    // 影片分片也会小于阈值，用体积区分这两类只会两边都错。
    const QString lower = path.toLower();
    if (lower.endsWith(".d.mts") || lower.endsWith(".d.cts")
        || lower.endsWith(".d.ts"))
        return false;
    static const QSet<QString> v = {
        "mkv", "mp4", "m4v", "avi", "mov",
        "wmv", "flv", "webm", "mpg", "mpeg",
        "m2ts", "mts", "vob", "iso", "strm",
    };
    return v.contains(extOf(path));
}

bool isSubtitleExtension(const QString &path)
{
    static const QSet<QString> s = { "srt", "ass", "ssa", "sub", "vtt", "idx" };
    return s.contains(extOf(path));
}

// 完整的「这文件像不像视频」判定：扩展名与 .ts 的体积歧义一起处理。
// 扫目录应该用这个，而不是 isVideoExtension 再自己记得过滤。
bool isVideoFile(const QString &path, qint64 sizeBytes)
{
    if (extOf(path) == "ts")
        return looksLikeMpegTs(path, sizeBytes);
    return isVideoExtension(path);
}

bool looksLikeMpegTs(const QString &path, qint64 sizeBytes)
{
    // .ts 是 TypeScript 还是 MPEG-TS，光看扩展名无法区分。
    // 实测就把 vite.config.ts 认成过视频，所以用体积兜一道：
    // 源码文件不可能上百 MB，而 MPEG-TS 分片通常至少几 MB。
    if (extOf(path) != "ts") return true;   // 非 .ts 不经这一关
    if (path.toLower().endsWith(".d.ts")) return false;   // TS 类型声明
    return sizeBytes >= minVideoBytes();
}

NameGuess guessFromFileName(const QString &fileName)
{
    NameGuess g;
    QString base = fileName;
    const int slash = base.lastIndexOf('/');
    if (slash >= 0) base = base.mid(slash + 1);
    const int dot = base.lastIndexOf('.');
    if (dot > 0) base = base.left(dot);

    // 年份。踩过两个坑：
    //  · 组号歧义。(19|20) 是捕获组、(?:19|20) 才是非捕获的。早先写
    //    captured(lastCapturedIndex()) 时取到的是 "20" 而不是 "2010"。
    //    这里只用一个捕获组，组号恒为 1。
    //  · 裸四位数不等于年份。"Blade.Runner.2049.2017" 里的 2049 合法地
    //    落在 1900-2099、前后也不是数字，lookaround 挡不住。经验上年份
    //    是最后一个独立四位数（片名在前、年份在后、再接分辨率标签），
    //    所以取最后一个而不是第一个 —— 这条是被变异测试锁住的。
    // 括号形式不需要单独分支：(?<![0-9]) / (?![0-9]) 已经覆盖了
    // "(2019)" 的边界。曾有一条 yearParen 分支，实测删掉后行为完全不变，
    // 属于只会误导读者的死代码，故移除。
    static const QRegularExpression yearRe("(?<![0-9])(19[0-9]{2}|20[0-9]{2})(?![0-9])");
    QRegularExpressionMatch ym;
    {
        QRegularExpressionMatchIterator it = yearRe.globalMatch(base);
        while (it.hasNext()) ym = it.next();   // 取最后一个，不是第一个
    }
    if (ym.hasMatch()) g.year = ym.captured(1).toInt();

    // 季集：S01E02 / s1e2 / 1x02
    static const QRegularExpression seRe("[Ss]([0-9]{1,2})[\\s._-]*[Ee]([0-9]{1,3})");
    const QRegularExpressionMatch sm = seRe.match(base);
    if (sm.hasMatch()) {
        g.season  = sm.captured(1).toInt();
        g.episode = sm.captured(2).toInt();
    } else {
        static const QRegularExpression xRe("(?<![0-9])([0-9]{1,2})[xX]([0-9]{1,3})(?![0-9])");
        const QRegularExpressionMatch xm = xRe.match(base);
        if (xm.hasMatch()) {
            g.season  = xm.captured(1).toInt();
            g.episode = xm.captured(2).toInt();
        }
    }

    // 标题 = 年份/季集标记之前的部分。取较早的那个切点，
    // 否则 "Show.2019.S01E02" 会把年份留在标题里。
    QString title = base;
    int cut = title.size();
    if (sm.hasMatch()) cut = qMin(cut, sm.capturedStart());
    if (ym.hasMatch()) cut = qMin(cut, ym.capturedStart());
    title = title.left(cut);
    title.replace(QRegularExpression("[._]+"), " ");
    title.replace(QRegularExpression("[\\s\\-\\(\\[]+$"), QString());
    g.title = title.trimmed();
    return g;
}

// 取 <tag>…</tag> 的文本。nfo 是扁平 XML、元素不嵌套同名，显式取
// 标签比引入完整 XML 解析器更好读也更好测（Qt6 已移除 QDom）。
static QString nfoTag(const QString &xml, const QString &tag)
{
    const QString open  = QString("<%1>").arg(tag);
    const QString close = QString("</%1>").arg(tag);
    const int a = xml.indexOf(open);
    if (a < 0) return QString();
    const int b = xml.indexOf(close, a + open.size());
    if (b < 0) return QString();
    QString v = xml.mid(a + open.size(), b - a - open.size()).trimmed();
    if (v.startsWith("<![CDATA[") && v.endsWith("]]>"))
        v = v.mid(9, v.size() - 12).trimmed();
    v.replace("&lt;",   "<");
    v.replace("&gt;",   ">");
    v.replace("&quot;", QString(QChar(0x22)));
    v.replace("&apos;", QString(QChar(0x27)));
    v.replace("&amp;",  "&");   // 必须最后做，否则 &amp;lt; 会被二次解错
    return v;
}

static QStringList nfoTags(const QString &xml, const QString &tag)
{
    QStringList out;
    const QString open  = QString("<%1>").arg(tag);
    const QString close = QString("</%1>").arg(tag);
    int from = 0;
    while (true) {
        const int a = xml.indexOf(open, from);
        if (a < 0) break;
        const int b = xml.indexOf(close, a + open.size());
        if (b < 0) break;
        const QString v = xml.mid(a + open.size(), b - a - open.size()).trimmed();
        if (!v.isEmpty()) out << v;
        from = b + close.size();
    }
    return out;
}

NfoInfo parseNfo(const QString &xml)
{
    NfoInfo n;
    // 空文件或压根不是 XML：直接失败，让调用方回退到原始文件名，
    // 而不是显示一个空条目假装"已刮削"。
    if (xml.trimmed().isEmpty() || !xml.contains('<')) return n;

    n.title         = nfoTag(xml, "title");
    n.originalTitle = nfoTag(xml, "originaltitle");
    n.sortTitle     = nfoTag(xml, "sorttitle");
    n.plot          = nfoTag(xml, "plot");
    n.tagline       = nfoTag(xml, "tagline");
    n.imdbId        = nfoTag(xml, "imdbid");
    n.tmdbId        = nfoTag(xml, "tmdbid");
    n.mediatype     = nfoTag(xml, "mediatype");

    // premiered 形如 2019-05-21；year 通常就是四位数。两个都试。
    static const QRegularExpression yearRe("(19|20)[0-9]{2}");
    for (const QString &cand : { nfoTag(xml, "premiered"), nfoTag(xml, "year") }) {
        const QRegularExpressionMatch m = yearRe.match(cand);
        if (m.hasMatch()) { n.year = m.captured(0).toInt(); break; }
    }

    n.season  = nfoTag(xml, "season").toInt();
    n.episode = nfoTag(xml, "episode").toInt();
    n.genres  = nfoTags(xml, "genre");
    n.studios = nfoTags(xml, "studio");
    // <actor><name>…</name></actor>：只取 name
    n.actors  = nfoTags(xml, "name");
    n.rating  = nfoTag(xml, "rating").toDouble();

    // 没有标题就算失败。只有 id 没标题的 nfo 不足以展示，
    // 装作成功会让用户看到一个空白卡片。
    n.ok = !n.title.isEmpty();
    return n;
}

bool looksLikeNfoFile(const QString &path)
{
    return extOf(path) == "nfo";
}


QString validateMountPoint(const QString &path)
{
    // 先给绝对路径与规范化把关：后面所有判断都建立在"没有 .. 与 //"上。
    // 不规范化就拦不住 /mnt/../../etc 这种，字符串前缀判断会被绕过去。
    if (path.isEmpty()) return QString::fromUtf8("挂载点不能为空");
    if (!path.startsWith("/")) return QString::fromUtf8("挂载点必须是绝对路径");
    const QString clean = QDir::cleanPath(path);
    if (clean != path)
        return QString("挂载点要写成规范路径（不要含 .. 或 //）：") + clean;

    // /mnt 本身不行：那是所有挂载的父目录，把卷挂在这儿等于把别的挂载
    // 全遮住（/mnt/d 会变成 /mnt/d/<d>，影视服务扫不到，用户只会觉得文件丢了）。
    if (clean == "/mnt")
        return QString::fromUtf8("不能直接挂到 /mnt 本身，请用 /mnt/<目录名>");

    const QStringList parts = clean.mid(1).split("/", Qt::SkipEmptyParts);
    if (parts.size() != 2 || parts.first() != "mnt")
        return QString::fromUtf8("挂载点必须是 /mnt 的直接子目录，例如 /mnt/raid");

    const QString leaf = parts.last();
    // 目录名会进 shell 命令与 systemd 单元名，字符集收到最小。
    static const QRegularExpression ok("^[A-Za-z0-9][A-Za-z0-9._-]*$");
    if (!ok.match(leaf).hasMatch())
        return QString("目录名只能用字母、数字、点、下划线、短横，且以字母数字开头：") + leaf;
    if (leaf == "." || leaf == "..")
        return QString::fromUtf8("目录名不能是 . 或 ..");
    return QString();
}

QString defaultMountPoint(const QString &volumeName)
{
    // 卷名先洗一遍再拼，避免把怪字符带进路径。清洗后为空就不猜，
    //
    // 这里刻意不再 validateMountPoint(mp) 一次：清洗留下的字符集
    // （字母数字 . _ -，且不以 . _ - 开头）是校验器允许集合的子集，
    // "/mnt/" + n 必然合法，那一步永远为真 —— 死分支。测试也证实了这点：
    // 把它删掉，40 条断言一条都不会红。留着的代价是让人以为这里有一道
    // 真实防线。子集关系由下面 mnt_probe 的「默认值自带合法性」守住。
    // 让调用方去报错 —— 返回一个 /mnt/unnamed 之类的会让用户以为是自己的命名。
    QString n = volumeName;
    n.replace(QRegularExpression("[^A-Za-z0-9._-]"), "_");
    n = n.remove(QRegularExpression("^[._-]+"));
    if (n.isEmpty()) return QString();
    return "/mnt/" + n;
}

QString mountPointConflict(const QString &path, const QStringList &systemMounts)
{
    const QString clean = QDir::cleanPath(path);
    // 根盘要单独处理，不能在循环里 continue 掉：
    //   · 若把 "/" 直接跳过，那么"目标位于已有挂载之内"这一路永远查不到根盘，
    //     而所有 /mnt/x 都在根盘之内 —— 检查等于没有做（本函数原先正是如此）；
    //   · 若不跳过，mm == "/" 时 clean.startsWith("/" + "/") 又不成立，
    //     而 startsWith("/") 会匹配一切绝对路径，等于一律报警。
    // 所以：根盘只用来判"目标目录本身是否已存在"，不参与前缀包含判断。
    const bool rootMounted = systemMounts.contains("/");
    if (rootMounted) {
        // /mnt 一定存在于根盘上，所以挂载点目录在根盘里必然能建，
        // 这一路不算冲突；真正要拦的是"挂到已有目录上遮住内容"，
        // 那由调用方用文件系统实际情况判断（见 mountPointBusy）。
    }
    for (const QString &m : systemMounts) {
        const QString mm = QDir::cleanPath(m);
        if (mm.isEmpty() || mm == "/") continue;   // 根盘已单独处理
        // 完全相同：正是要挂的地方已经被挂了
        if (mm == clean)
            return QString("该路径已挂载 ") + mm;
        // 目标在已有挂载之下：例如 /mnt/d/raid 而 /mnt/d 是分区挂载点。
        // 挂上去的文件落在分区里没问题，但一旦分区没挂，卷会写到根盘的
        // 同名目录里 —— 那种"内容跑错盘"的问题极难发现，直接拒绝。
        if (clean.startsWith(mm + "/"))
            return QString("该路径位于已挂载的 ") + mm + " 之内";
        // 目标盖住已有挂载：把 /mnt/d 挂到已经是挂载点的 /mnt 之下会遮住下层
        if (mm.startsWith(clean + "/"))
            return QString("该目录内已有挂载 ") + mm + "，挂上去会把它遮住";
    }
    return QString();
}
QString mountPointBusy(const QString &path, const QStringList &existingEntries)
{
    Q_UNUSED(path);
    // 调用方传的是"该目录下的条目"，空列表代表目录不存在或为空 —— 两种都安全。
    // 隐藏文件同样算内容：只装着 .cache 的目录被挂上一样会遮住，
    // 按"看得见的文件"来判会让用户以为没事。
    if (existingEntries.isEmpty()) return QString();
    return QString("目录已存在且有 ") + QString::number(existingEntries.size()) +
           QString(" 项内容，挂载后这些会暂时看不见");
}

QStringList osReleaseIds(const QString &osReleaseText)
{
    // 只认 ID= / ID_LIKE= 两行。os-release 的格式是 KEY=VALUE，值可带引号，
    // ID_LIKE 可含多个空格分隔的父系（如 debian）。用逐行解析而不是整串 find：
    // 整串 find("ubuntu") 会被 PRETTY_NAME="Ubuntu 26.04" 命中，也会把任意
    // 注释里提到的 ubuntu 当成是 —— 那是把安装按钮的开关交给一段自由文本。
    QStringList ids;
    for (const QString &raw : osReleaseText.split(QChar(10))) {
        const QString line = raw.trimmed();
        if (!(line.startsWith("ID=") || line.startsWith("ID_LIKE="))) continue;
        // 取第一个 = 之后的部分（KEY=VALUE）；用 indexOf 而非 section，意图更直白
        const int eq = line.indexOf(QChar(61));
        if (eq < 0) continue;
        QString val = line.mid(eq + 1).trimmed();
        // 去掉包裹的双引号
        if (val.startsWith(QChar(34)) && val.endsWith(QChar(34)) && val.size() >= 2)
            val = val.mid(1, val.size() - 2);
        // 值是空格分隔的列表（大小写不敏感：os-release 约定小写，但不依赖它）
        for (const QString &p : val.split(QChar(32), Qt::SkipEmptyParts))
            ids << p.toLower();
    }
    return ids;
}

bool aptInstallable(const QString &osReleaseText)
{
    const QStringList ids = osReleaseIds(osReleaseText);
    return ids.contains(QStringLiteral("ubuntu")) || ids.contains(QStringLiteral("debian"));
}

// ── 一键安装的清单 ──────────────────────────────────────────
//
// 每条都写的是"那家官方仓库里真的存在"的名字，各家叫法不一样的地方标在
// 注释里。刻意**不放**的两类：
//   · nasrs 引擎 —— 本项目的自研二进制，只能随程序一起发布，没有包可装；
//   · 内核 target 模块 / configfs —— 前者随内核版本走（企业系常拆在
//     kernel-modules-extra 之类的包里，猜错名字就是一条失败的 root 命令），
//     后者是挂载动作不是包。这两条留给页面用文字提示，不进这条链。
const QVector<DepSpec> &nasDepSpecs()
{
    static const QVector<DepSpec> specs = {
        // 三家同名，最没悬念的一条
        { QStringLiteral("mdadm（内核 RAID 工具）"), QStringLiteral("mdadm"),
          { QStringLiteral("mdadm") },
          { QStringLiteral("mdadm") },
          { QStringLiteral("mdadm") } },

        // LIO 配置工具。Debian/Ubuntu 的包叫 targetcli-fb（可执行文件仍是
        // targetcli）；Fedora/openEuler 起就叫 targetcli；Arch 官方仓库没有，
        // 只在 AUR —— 空列表，让界面如实说"这家装不了"，而不是点了报错。
        { QStringLiteral("targetcli（LIO/iSCSI 配置工具）"), QStringLiteral("targetcli"),
          { QStringLiteral("targetcli-fb") },
          { QStringLiteral("targetcli") },
          {} },

        // FUSE 3。装的是挂载助手 fusermount3；/dev/fuse 属内核，不归包管。
        { QStringLiteral("fuse3（挂载 FUSE 卷的助手）"), QStringLiteral("fusermount3"),
          { QStringLiteral("fuse3") },
          { QStringLiteral("fuse3") },
          { QStringLiteral("fuse3") } },

        // mergerfs（FUSE 合并目录）。这条是 RAID0 那格现在用的第三方引擎。
        // 包名只在 apt 家有：repology 上 Debian 11–14/Ubuntu 18.04–27.04 都有
        // mergerfs，而 dnf 系（Fedora/RHEL/openEuler）**一个都没有** —— 本容器
        // （openEuler 24.03）里 `dnf list --available mergerfs` 直接报
        // "No matching Packages"，这是实测，不是猜。Arch 只在 AUR，同 targetcli
        // 的处理：空列表，让界面如实说"这家装不了"，而不是摆一个必然失败的按钮。
        { QStringLiteral("mergerfs（FUSE 合并目录）"), QStringLiteral("mergerfs"),
          { QStringLiteral("mergerfs") },
          {},
          {} },

        // Samba：smbd 与 smbd.service 都在 samba 包里。Debian/Ubuntu 把
        // testparm（离线校验 smb.conf 的那个）拆进了 samba-common-bin，
        // 少了它 SMB 页会一直报"无法校验"，所以 apt 这条带两个包；
        // Fedora 与 Arch 的 testparm 就在 samba 里，一个就够。
        { QStringLiteral("samba（SMB 文件共享）"), QStringLiteral("smbd"),
          { QStringLiteral("samba"), QStringLiteral("samba-common-bin") },
          { QStringLiteral("samba") },
          { QStringLiteral("samba") } },
    };
    return specs;
}

Pkg detectPkgManager(const QString &osReleaseText, bool haveApt,
                     bool haveDnf, bool havePacman)
{
    const QStringList ids = osReleaseIds(osReleaseText);
    auto like = [&ids](const char *n) { return ids.contains(QLatin1String(n)); };

    // 归属优先于"PATH 里先找到谁"：Arch 系装 apt 兼容包、Debian 系装 dnf
    // 的人都见过，按查找顺序选家会给错的那家发命令。
    if (havePacman && (like("arch") || like("manjaro") || like("endeavouros")
                    || like("garuda") || like("cachyos")))
        return Pkg::Pacman;
    if (haveApt && (like("ubuntu") || like("debian")))
        return Pkg::Apt;
    if (haveDnf  && (like("fedora") || like("rhel") || like("centos") || like("openeuler")
                  || like("almalinux") || like("rocky")  || like("amzn")))
        return Pkg::Dnf;

    // 归属认不出来（os-release 没有 ID_LIKE，或是一个没登记过的发行版）时，
    // 退回"哪家在就用哪家"，顺序 dnf → apt-get → pacman。
    // 注意 openSUSE / Alpine 走不到这里：它们的 zypper / apk 本工具不支持，
    // 而系统里通常也没有 dnf，于是返回 None，按钮会老实说"认不出包管理器"。
    if (haveDnf)    return Pkg::Dnf;
    if (haveApt)    return Pkg::Apt;
    if (havePacman) return Pkg::Pacman;
    return Pkg::None;
}

QString pkgManagerName(Pkg pm)
{
    switch (pm) {
    case Pkg::Apt:    return QStringLiteral("apt-get");
    case Pkg::Dnf:    return QStringLiteral("dnf");
    case Pkg::Pacman: return QStringLiteral("pacman");
    case Pkg::None:   break;
    }
    return QString();
}

namespace {
// 按缺失项从表里取某一家的包名列表
const QStringList &familyPackages(const DepSpec &s, Pkg pm)
{
    switch (pm) {
    case Pkg::Apt:    return s.apt;
    case Pkg::Dnf:    return s.dnf;
    case Pkg::Pacman: return s.pacman;
    case Pkg::None:   break;
    }
    static const QStringList none;
    return none;
}
} // namespace

QStringList packagesToInstall(Pkg pm, const QStringList &missingBinaries)
{
    QStringList out;
    if (pm == Pkg::None) return out;
    for (const DepSpec &s : nasDepSpecs()) {
        if (!missingBinaries.contains(s.binary)) continue;
        for (const QString &p : familyPackages(s, pm))
            if (!out.contains(p)) out << p;   // 两个依赖撞同一个包时不重复装
    }
    return out;
}

QStringList packagesUnavailable(Pkg pm, const QStringList &missingBinaries)
{
    QStringList out;
    if (pm == Pkg::None) return out;
    for (const DepSpec &s : nasDepSpecs())
        if (missingBinaries.contains(s.binary) && familyPackages(s, pm).isEmpty())
            out << s.label;
    return out;
}

bool mergerfsSourceInstallable(Pkg pm, const QStringList &missingBinaries)
{
    // 只在"这家没有包、而用户确实缺 mergerfs"时才为真。apt 系有包（mergerfs 官方
    // 仓库里就有），走源码编译是绕远路 —— 多一份自己维护的产物，也多一次失败机会。
    if (pm != Pkg::Dnf && pm != Pkg::Pacman) return false;
    if (!missingBinaries.contains(QStringLiteral("mergerfs"))) return false;
    return packagesToInstall(pm, { QStringLiteral("mergerfs") }).isEmpty();
}

QStringList dependencyLabels(const QStringList &missingBinaries)
{
    QStringList out;
    for (const DepSpec &s : nasDepSpecs())
        if (missingBinaries.contains(s.binary)) out << s.label;
    return out;
}

bool isSafePackageName(const QString &name)
{
    // 白名单而不是转义：这条命令的终点是 root，能一眼看懂的字符集比
    // "什么都能塞但我会转义"好审得多。合法包名（含 epoch/version 里常见的
    // . + ~ : 与多架构后缀 :amd64）都在这个集合里。
    static const QRegularExpression re("^[A-Za-z0-9][A-Za-z0-9._+~:-]{0,63}$");
    return re.match(name).hasMatch();
}
} // namespace Nas
