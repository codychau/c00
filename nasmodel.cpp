#include "nasmodel.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QStringList>
#include <QSet>

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
        if (engine == Engine::Raid0Rs) {
            // 自研引擎要在它已有的文件系统里放 backing 文件，挂载着是前提而非障碍
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
    //   自研引擎  用不了 —— 没文件系统就放不下 backing 文件，
    //             而本工具不会替你 mkfs（那是更大的破坏）
    if (engine == Engine::Raid0Rs) {
        v.risk = Risk::HasFilesystem;
        v.reason = "没有文件系统，自研引擎无法在其上建 backing 文件";
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

    if (info.members == 1) {
        // 单个成员就是一条直连盘，不是 RAID；允许但要说清
        info.health = ArrayHealth::Single;
        info.usable = minCap;
        info.note = "只有一个成员，等同直连单盘，没有条带加速。";
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

// ── backing 文件大小建议 ────────────────────────────────────
// 关键约束：自研引擎的 RAID0 要求所有 backing 文件**等大**，
// 否则最后一个条带写不满，实现上要么补零要么报错。
// 所以只能按最小的可用空间来定，并且再打个折 ——
// 目标分区上可能同时有别的程序在写，留余量避免文件写不下。
qint64 suggestBackingSize(const QVector<qint64> &partitionFreeBytes,
                          double safetyRatio)
{
    qint64 minFree = -1;
    for (qint64 f : partitionFreeBytes) {
        if (f <= 0) continue;
        if (minFree < 0 || f < minFree) minFree = f;
    }
    if (minFree < 0) return 0;
    // 向下取整到 MiB，避免小数尾巴造成两个文件大小不一致
    const double v = double(minFree) * safetyRatio;
    return qint64(v / (1024 * 1024)) * (1024 * 1024);
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

} // namespace Nas
