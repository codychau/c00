#ifndef NASMODEL_H
#define NASMODEL_H

#include <QVector>
#include <QString>

// ─────────────────────────────────────────────────────────────
//  NAS 页面的数据模型与**安全过滤**。
//
//  这个文件里全是纯函数，不起进程、不碰设备，为的是能被 page_probe
//  之类的离线测试直接调用 —— RAID 会覆盖分区上的数据，判错一个设备
//  就是不可逆的损失，所以"哪些分区允许被选"必须可测、可审查。
//
//  过滤规则保守：拿不准的一律标为不可选，宁可让用户去别处操作。
// ─────────────────────────────────────────────────────────────
namespace Nas {

// lsblk 的一个分区/磁盘条目（只保留 RAID 决策需要的字段）
struct Block
{
    QString name;        // sda / sda1 / nvme0n1p2
    QString path;        // /dev/sda1
    QString type;        // disk | part | rom | loop ...
    QString fstype;      // ext4 / swap / crypto_LUKS / isw_raid_member ...（空=无文件系统）
    QString mountpoint;  // 空=未挂载
    QString label;
    QString uuid;
    qint64  size = 0;    // 字节，lsblk 的 SIZE 列
    QVector<Block> children;
};

enum class Risk {
    Safe,          // 可选
    WholeDisk,     // 整块磁盘：只允许选分区，选整盘会连分区表一起抹掉
    Mounted,       // 正被挂载
    HasFilesystem, // 有文件系统但未挂载（有数据）
    FileSystemRoot,// 承载系统根分区的磁盘
    Swap,          // swap
    Crypto,        // LUKS 容器
    ExistingRaid,  // 已是 md/lvm 成员，抹了会连带阵列
    TooSmall,      // 太小，做 RAID 没意义
    LoopOrRom,     // 回环设备、光驱等，不是真实磁盘
};

// 两种实现路线
enum class Engine {
    Mdadm,       // mdadm 建 /dev/mdN
    Raid0Rs,     // 自研 nasrs（RAID0 over 文件）
};

// 不可选的原因；Safe 时为 QString()
struct Verdict
{
    Risk  risk = Risk::Safe;
    QString reason;    // 给用户看的中文说明
    bool  selectable = true;
    // 需要用户二次确认（有数据但技术上可选，例如空文件系统）
    bool  needsConfirm = false;
    QString confirmText;
};

// 解析 lsblk -J 输出（失败返回空表）
QVector<Block> parseLsblk(const QByteArray &json);

// 展平成一维列表（含 disk 与 part）
QVector<Block> flatten(const QVector<Block> &tree);

// 分区名 → 父盘名（nvme0n1p2 → nvme0n1，sda2 → sda）
QString parentDiskOf(const QString &name);

// 从 /proc/mounts 找出承载 "/" 与 "/boot" 的磁盘名（"sda" 这种不含 /dev/ 的）
QVector<QString> systemDiskNames(const QString &procMounts);

// 综合判定某个条目能否用。
// engine 很关键，因为两条路线对"能不能用"的定义**不同**：
//   Mdadm    直接拿分区当成员，会抹掉分区上的文件系统，
//            所以"已挂载"必须拒绝（正在用的东西不能抹）
//   Raid0Rs  在分区**已有的文件系统里**放一个大文件，不碰分区本身，
//            所以"已挂载且可写"不但不是障碍，反而是**前提** ——
//            没挂载就没地方放文件。
// 早期版本只有 judge(b, systemDisks) 一个签名，一律拒绝已挂载分区，
// 结果本机 7 个设备里 0 个可选，自研引擎更是永远无盘可选。
Verdict judge(const Block &b, const QVector<QString> &systemDisks, Engine engine);

// ── 阵列规划 ────────────────────────────────────────────────
// 成员在阵列里的角色
enum class MemberKind {
    Partition,   // mdadm：直接用分区
    BackingFile, // 自研引擎：分区上放一个大文件
};

struct Member
{
    MemberKind kind = MemberKind::Partition;
    QString device;          // /dev/sdb1
    QString filePath;        // 自研引擎用：<mount>/<name>.nas0.img
    qint64  capacity = 0;    // 该成员贡献的字节数（自研引擎=文件大小）
};

struct Plan
{
    Engine engine = Engine::Mdadm;
    QVector<Member> members;
    int stripeChunkKiB = 512;
    QString volumeName;
};

// 阵列健康/状态
enum class ArrayHealth {
    Empty,        // 没成员
    Single,       // 只有一个成员（RAID0 至少要两个）
    Ok,           // 可用
    Uneven,       // 可用但容量不齐，多出来的一部分会被浪费
    CapacityLow,  // 有成员容量为 0 或未知
};

struct ArrayInfo
{
    ArrayHealth health = ArrayHealth::Empty;
    QString note;            // 给用户的说明（浪费多少、为什么不行）
    qint64  totalRaw = 0;    // 各成员容量之和
    qint64  usable = 0;      // RAID0 实际可用 = min(成员) × 成员数
    qint64  wasted = 0;      // totalRaw - usable
    int     members = 0;
};

// 计算阵列可用容量与告警。纯函数，不碰设备。
ArrayInfo evaluateArray(const Plan &plan);

// 自研引擎的文件大小建议：取最小成员分区的可用空间，留出安全余量。
// 留余量是因为 RAID0 必须两个文件等大，若某个分区被别的程序填满，
// 大的那个写不进去就会让阵列写成"半条带"—— 直接算成失败。
qint64 suggestBackingSize(const QVector<qint64> &partitionFreeBytes,
                          double safetyRatio = 0.95);

// IQN：遵循 ietf.2007-06 规范，反序域名 + 日期 + 序号
// 例：iqn.2026-11.com.example:servertoolbox-01
QString makeIqn(const QString &reverseDomain, const QString &dateYYMM,
                const QString &serial);

// 把字节数格式化成人类可读（1.8 TiB）
QString humanSize(qint64 bytes);

// ── SMB（单分区文件级共享）─────────────────────────────────
//
// 与 RAID 不同，SMB 导出**不覆盖任何数据**：它只是把某个已挂载的
// 分区路径通过 Samba 共享出去。所以这里的风险不是"抹掉分区"，而是
// "把不该暴露的目录暴露到局域网"。护栏因此围绕路径和认证展开。

// 共享认证模式。guest 一律默认关闭：guest 可写等于局域网内任何
// 一台机器都能匿名往盘里写，这是最常见的家庭 NAS 事故。
enum class SmbAuth {
    Password,   // 需 Samba 账号密码（默认，最安全）
    GuestRO,    // 匿名只读
    GuestRW,    // 匿名可写（危险，UI 必须红字警告）
};

struct SmbShare {
    QString shareName;     // \host\<shareName>
    QString path;          // 被共享的挂载点路径，如 /mnt/d
    QString user;          // Password 模式映射到的 Linux 用户（force user）
    SmbAuth auth = SmbAuth::Password;
    bool    browseable = true;
    bool    writable   = false;   // 是否可写（GuestRO 恒为 false）
};

// 共享名合法性：SMB 共享名不允许这些字符，且不能与内置保留名冲突。
// 返回空串表示合法，否则返回拒绝原因。
QString validateShareName(const QString &name);

// 路径护栏：判断一个待共享路径是否是危险路径（系统根、系统盘挂载点、
// 或某个系统分区的挂载点）。命中不等于禁止，但 UI 必须强确认。
// systemMounts 为系统分区挂载点列表（如 /、/boot、/home）。
bool isRiskySharePath(const QString &path, const QStringList &systemMounts);

// 生成 smb.conf 的一个 share 片段（[name] 段）。纯字符串拼接，
// 不写文件、不起进程 —— 交给上层决定落盘与 reload 的时机。
// 用参数化拼装，避免用户输入直接注入配置文件（值会转义特殊字符）。
QString makeShareSection(const SmbShare &share);

// Samba 会拒绝某些字符出现在路径配置里，这里做保守转义/校验。
// 返回空串表示可安全写入 smb.conf，否则返回拒绝原因。
QString validateSharePath(const QString &path);

} // namespace Nas

#endif // NASMODEL_H
