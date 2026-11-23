// nasmodel 里「软件镜像」那几条纯函数的离线探针。
//
// 全部不碰磁盘、不起进程：判据本身是纯函数，判错一次的代价是"副本目录指向
// 别人的目录"或"两份副本在同一块盘上还以为有冗余"，所以这些分支必须能被
// 无权限、无 root、无 nasrs 的环境跑绿。
//
// 编译见 preview/ 里那条 g++ 命令（与 smb_probe 同一套）。
#include "nasmodel.h"

#include <cstdarg>
#include <cstdio>
#include <QString>
#include <QStringList>
#include <QVector>

static int g_pass = 0, g_fail = 0;

// 用 stderr：release 下的 Qt 会把 qInfo 吞掉，探针输出看不见（这个坑踩过一次）
static void say(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void check(bool ok, const QString &what)
{
    if (ok) { ++g_pass; say("  ✓ %s\n", qPrintable(what)); }
    else    { ++g_fail; say("  ✗ %s\n", qPrintable(what)); }
}

static Nas::Member mem(const QString &dev, const QString &dir, qint64 cap)
{
    Nas::Member m;
    m.kind = Nas::MemberKind::MirrorDir;
    m.device = dev;
    m.filePath = dir;
    m.capacity = cap;
    return m;
}

int main()
{
    say("== mirrorDirPath ==\n");
    check(Nas::mirrorDirPath("/mnt/d1", "nas") == "/mnt/d1/nas.nas-mirror",
          "挂载点 + 卷名拼出副本目录");
    check(Nas::mirrorDirPath("/mnt/d1/", "nas") == "/mnt/d1/nas.nas-mirror",
          "挂载点带尾斜杠也拼对（不出现 //）");
    check(Nas::mirrorDirPath("", "nas").isEmpty(), "没挂载 → 空串，不猜路径");
    check(Nas::mirrorDirPath("/", "nas").isEmpty(), "挂载点是 / → 拒绝（不在系统根下建卷目录）");
    check(Nas::mirrorDirPath("/mnt", "nas").isEmpty(), "挂载点是 /mnt 本身 → 拒绝");
    check(Nas::mirrorDirPath("/mnt/d1", "").isEmpty(), "没填卷名 → 空串");
    check(Nas::mirrorDirPath("/mnt/d1", "  ").isEmpty(), "卷名全是空白 → 空串，不造 unnamed 这种怪名");
    check(Nas::mirrorDirPath("/mnt/d1", "a b/c") == "/mnt/d1/a_b_c.nas-mirror",
          "卷名里的怪字符被洗掉，斜杠也换成下划线（不跑出挂载点）");

    say("== validateMirrorDir ==\n");
    check(Nas::validateMirrorDir("/mnt/d1/nas.nas-mirror").isEmpty(), "常规路径合法");
    check(!Nas::validateMirrorDir("").isEmpty(), "空路径拒绝");
    check(!Nas::validateMirrorDir("mnt/d1/x").isEmpty(), "相对路径拒绝");
    check(!Nas::validateMirrorDir("/mnt/d1/../d2/x").isEmpty(), "含 .. 拒绝");
    check(!Nas::validateMirrorDir("/mnt//d1/x").isEmpty(), "含 // 拒绝");
    check(!Nas::validateMirrorDir("/mnt").isEmpty(), "/mnt 本身拒绝（只有一层）");
    check(!Nas::validateMirrorDir("/mnt/../../etc/x").isEmpty(), "上一层加 .. 也拒绝（要规范化）");
    check(!Nas::validateMirrorDir("/etc/samba").isEmpty(), "系统目录 etc 拒绝");
    check(!Nas::validateMirrorDir("/var/lib/x").isEmpty(), "系统目录 var 拒绝");
    check(!Nas::validateMirrorDir("/mnt/d1/a b").isEmpty(), "目录名带空格拒绝");
    check(!Nas::validateMirrorDir("/data/nas/x").isEmpty() == false,
          "非系统的一级目录 /data 允许（数据盘常挂在这儿）");

    say("== mirrorDirConflict（对真实挂载表）==\n");
    const QStringList mounts = { "/mnt/d1", "/mnt/d2", "/" };
    check(!Nas::mirrorDirConflict("/mnt/d1", mounts).isEmpty(),
          "镜像目录 == 挂载点 → 拦（删多余文件会清空整个分区）");
    check(Nas::mirrorDirConflict("/mnt/d1/nas.nas-mirror", mounts).isEmpty(),
          "在挂载点之下 → 正常，不报");
    check(!Nas::mirrorDirConflict("/mnt/d2/", mounts).isEmpty(),
          "带尾斜杠也要认出那是挂载点");

    say("== mirrorMembersWarning ==\n");
    check(!Nas::mirrorMembersWarning({}).contains("一主一备") == false,
          "0 个成员：提示要正好 2 个");
    check(Nas::mirrorMembersWarning({ "/dev/sdb1" }).contains("正好 2 个"),
          "1 个成员：提示要正好 2 个");
    check(Nas::mirrorMembersWarning({ "/dev/sdb1", "/dev/sdc1" }).isEmpty(),
          "两个不同盘上的分区：没问题");
    check(Nas::mirrorMembersWarning({ "/dev/sdb1", "/dev/sdb2" }).contains("一起没"),
          "同一块盘上两个分区：红字说清零冗余");
    check(Nas::mirrorMembersWarning({ "/dev/nvme0n1p1", "/dev/nvme0n1p2" }).contains("nvme0n1"),
          "nvme 命名也能算出父盘");
    check(Nas::mirrorMembersWarning({ "/dev/sdb1", "/dev/sdc1", "/dev/sdd1" }).contains("正好 2 个"),
          "3 个成员：同样拒绝（一主一备是定义不是参数）");

    say("== evaluateArray（镜像的容量账）==\n");
    {
        Nas::Plan p;
        p.engine = Nas::Engine::MirrorRs;
        p.volumeName = "nas";
        p.members << mem("/dev/sdb1", "/mnt/d1/nas.nas-mirror", 100LL * 1024 * 1024 * 1024)
                  << mem("/dev/sdc1", "/mnt/d2/nas.nas-mirror", 100LL * 1024 * 1024 * 1024);
        const Nas::ArrayInfo a = Nas::evaluateArray(p);
        check(a.usable == 100LL * 1024 * 1024 * 1024,
              "两盘各 100G 做镜像：可用是 100G，不是 200G");
        check(a.note.contains("副本"), "说明里点出另一份是副本");
    }
    {
        Nas::Plan p;
        p.engine = Nas::Engine::MirrorRs;
        p.volumeName = "nas";
        p.members << mem("/dev/sdb1", "/mnt/d1/nas.nas-mirror", 100LL * 1024 * 1024 * 1024)
                  << mem("/dev/sdc1", "/mnt/d2/nas.nas-mirror", 500LL * 1024 * 1024 * 1024);
        const Nas::ArrayInfo a = Nas::evaluateArray(p);
        check(a.usable == 100LL * 1024 * 1024 * 1024,
              "100G + 500G 做镜像：可用只有 100G（大的一块装不下第二份）");
    }
    {
        // 同盘的组合：容量之外更要紧的是那句红字，note 必须是它
        Nas::Plan p;
        p.engine = Nas::Engine::MirrorRs;
        p.volumeName = "nas";
        p.members << mem("/dev/sdb1", "/mnt/d1/nas.nas-mirror", 100LL * 1024 * 1024 * 1024)
                  << mem("/dev/sdb2", "/mnt/d2/nas.nas-mirror", 100LL * 1024 * 1024 * 1024);
        const Nas::ArrayInfo a = Nas::evaluateArray(p);
        check(a.note.contains("一起没"), "同盘组合：汇总条直接说这是零冗余");
    }
    {
        Nas::Plan p;   // 同一个 Plan 走 RAID0 的账：必须是 ×2，别让镜像分支漏到别人身上
        p.engine = Nas::Engine::Raid0Rs;
        p.volumeName = "nas";
        p.members << mem("/dev/sdb1", "/mnt/d1/a.img", 100LL * 1024 * 1024 * 1024)
                  << mem("/dev/sdc1", "/mnt/d2/a.img", 100LL * 1024 * 1024 * 1024);
        check(Nas::evaluateArray(p).usable == 200LL * 1024 * 1024 * 1024,
              "RAID0 的账没被镜像分支带偏（仍是 min × 2）");
    }

    say("== judge（镜像对分区的要求）==\n");
    {
        Nas::Block b;
        b.name = "sdb1"; b.path = "/dev/sdb1"; b.type = "part";
        b.fstype = "ext4"; b.mountpoint = "/mnt/d1"; b.size = 1LL << 40;
        check(Nas::judge(b, { "sda" }, Nas::Engine::MirrorRs).selectable,
              "镜像：已挂载且有文件系统 → 可选（前提，不是障碍）");
        check(Nas::judge(b, { "sda" }, Nas::Engine::Mdadm).selectable == false,
              "mdadm：同一个分区仍不可选（会抹它）—— 两条路线结论相反");

        Nas::Block e;
        e.name = "sdc1"; e.path = "/dev/sdc1"; e.type = "part";
        e.fstype = ""; e.mountpoint = ""; e.size = 1LL << 40;
        const Nas::Verdict v = Nas::judge(e, { "sda" }, Nas::Engine::MirrorRs);
        check(v.selectable == false && v.reason.contains("副本目录"),
              "镜像：空分区不可选，理由说的是「建不了副本目录」而不是 backing 文件");
        check(Nas::judge(e, { "sda" }, Nas::Engine::Mdadm).selectable,
              "mdadm：同一个空分区可选（正合适）");
    }

    say("== mirrorRoleNote ==\n");
    check(Nas::mirrorRoleNote(true).contains("副本网"), "主副本那句话提到坏了切副本网");
    check(Nas::mirrorRoleNote(false).contains("上一次同步"), "副本网那句说清读到的是旧快照");

    say("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
