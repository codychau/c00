// nasmodel 判定逻辑的离线断言。
// 这是全项目风险最高的一段代码：判错一个设备 = 抹掉用户数据。
// 不起 GUI、不碰设备，直接喂构造的 lsblk 场景。
#include "nasmodel.h"
#include <QVector>
#include <cstdio>

static int failures = 0;
static void check(bool cond, const char *what)
{
    printf("  %s  %s\n", cond ? "✅" : "❌", what);
    if (!cond) ++failures;
}

static Nas::Block mk(const QString &name, const QString &type,
                     const QString &fstype, const QString &mnt, qint64 size)
{
    Nas::Block b;
    b.name = name; b.path = "/dev/" + name; b.type = type;
    b.fstype = fstype; b.mountpoint = mnt; b.size = size;
    return b;
}

int main()
{
    const qint64 G = 1024LL * 1024 * 1024;
    const QVector<QString> sys = { "nvme0n1" };

    printf("=== parentDiskOf ===\n");
    check(Nas::parentDiskOf("sda1") == "sda",                "sda1 -> sda");
    check(Nas::parentDiskOf("sda10") == "sda",               "sda10 -> sda");
    check(Nas::parentDiskOf("nvme0n1p2") == "nvme0n1",       "nvme0n1p2 -> nvme0n1");
    check(Nas::parentDiskOf("mmcblk0p1") == "mmcblk0",       "mmcblk0p1 -> mmcblk0");
    check(Nas::parentDiskOf("vda") == "vda",                 "vda -> vda（无尾数不动）");

    printf("\n=== 系统盘：前缀不能误伤（曾经的 bug）===\n");
    // sda10 不该被系统盘 sda1 的前缀匹配拖下水
    const QVector<QString> sys2 = { "sda" };
    // 注意别写成 `!x.selectable == false`：那等价于 `x.selectable == true`，
    // 是恒真式，探针会永远绿。这里就是要 sdb1 **可选**。
    check(Nas::judge(mk("sdb1","part","","",100*G), sys2, Nas::Engine::Mdadm).selectable,
          "无关盘 sdb1 在 mdadm 下可选");
    {
        auto v = Nas::judge(mk("sda10","part","","",100*G), sys2, Nas::Engine::Mdadm);
        check(!v.selectable, "系统盘上的 sda10 被拒（父盘名判定）");
    }

    // 这条才是前缀误判的直接反例：系统盘是 sda1（父盘 sda），
    // 那么 sda10 的父盘同样是 sda —— 它**确实**在系统盘上，被拒是对的。
    // 真正会出问题的是 sda1 在前缀上等于 sda10 的开头这种字符串巧合，
    // 所以这里用"系统盘 = nvme0n1、被检盘 = nvme0n1p2 之外的 sda1x"来构造。
    {
        const QVector<QString> sys3 = { "sda1" };   // 系统盘父名解析后是 sda
        auto v = Nas::judge(mk("sdb10", "part", "", "", 100*G), sys3, Nas::Engine::Mdadm);
        check(v.selectable, "系统盘=sda1 时，无关盘 sdb10 仍可选");
        auto w = Nas::judge(mk("nvme0n1p2", "part", "", "", 100*G), sys3, Nas::Engine::Mdadm);
        check(w.selectable, "系统盘=sda1 时，nvme0n1p2 不该被前缀误伤");
    }

    printf("\n=== mdadm：会抹分区，所以已挂载必须拒 ===\n");
    {
        auto v = Nas::judge(mk("sdb1","part","ext4","/mnt/d",100*G), sys, Nas::Engine::Mdadm);
        check(!v.selectable, "已挂载分区在 mdadm 下不可选");
    }
    {
        auto v = Nas::judge(mk("sdc1","part","ext4","",100*G), sys, Nas::Engine::Mdadm);
        check(v.selectable && v.needsConfirm, "有文件系统但未挂载：可选但需二次确认");
    }
    {
        auto v = Nas::judge(mk("sdd1","part","","",100*G), sys, Nas::Engine::Mdadm);
        check(v.selectable && !v.needsConfirm, "空分区在 mdadm 下直接可选");
    }

    printf("\n=== 自研引擎：已挂载反而是前提（曾经一律拒，导致 0 可选）===\n");
    {
        auto v = Nas::judge(mk("sdb1","part","ext4","/mnt/d",100*G), sys, Nas::Engine::Raid0Rs);
        check(v.selectable, "已挂载且有文件系统：自研引擎可选（要往里放文件）");
    }
    {
        auto v = Nas::judge(mk("sdd1","part","","",100*G), sys, Nas::Engine::Raid0Rs);
        check(!v.selectable, "空分区在自研引擎下不可选（没文件系统放不下文件）");
    }

    printf("\n=== 两类引擎都必须拒的硬红线 ===\n");
    for (auto eng : { Nas::Engine::Mdadm, Nas::Engine::Raid0Rs }) {
        check(!Nas::judge(mk("sda","disk","","",500*G), sys, eng).selectable,  "整盘不可选");
        check(!Nas::judge(mk("loop0","loop","","",1*G), sys, eng).selectable,  "loop 不可选");
        check(!Nas::judge(mk("sr0","rom","","",1*G), sys, eng).selectable,     "光驱不可选");
        check(!Nas::judge(mk("sde1","part","swap","",(qint64)8*G), sys, eng).selectable, "swap 不可选");
        check(!Nas::judge(mk("sdf1","part","crypto_LUKS","",(qint64)50*G), sys, eng).selectable, "LUKS 不可选");
        check(!Nas::judge(mk("sdg1","part","linux_raid_member","",(qint64)50*G), sys, eng).selectable, "已是 md 成员不可选");
        check(!Nas::judge(mk("sdh1","part","LVM2_member","",(qint64)50*G), sys, eng).selectable, "LVM 成员不可选");
        check(!Nas::judge(mk("sdi1","part","","",(qint64)1*G), sys, eng).selectable, "小于 2 GiB 不可选");
        check(!Nas::judge(mk("nvme0n1p3","part","","",(qint64)50*G), sys, eng).selectable, "系统盘分区不可选");
    }

    printf("\n=== RAID0 容量：最小成员 × 成员数 ===\n");
    {
        Nas::Plan p;
        p.members << Nas::Member{Nas::MemberKind::Partition, "/dev/sdb1", "", 100*G}
                  << Nas::Member{Nas::MemberKind::Partition, "/dev/sdc1", "", 500*G};
        auto a = Nas::evaluateArray(p);
        check(a.usable == 200*G, "100G+500G 的 RAID0 只有 200G 可用");
        check(a.wasted == 400*G, "400G 被浪费");
        check(a.health == Nas::ArrayHealth::Uneven, "容量不等被标出");
    }
    {
        Nas::Plan p;
        p.members << Nas::Member{Nas::MemberKind::Partition, "/dev/sdb1", "", 200*G}
                  << Nas::Member{Nas::MemberKind::Partition, "/dev/sdc1", "", 200*G};
        auto a = Nas::evaluateArray(p);
        check(a.usable == 400*G && a.health == Nas::ArrayHealth::Ok, "等容量两盘 = 2 倍");
    }
    {
        Nas::Plan p;
        p.members << Nas::Member{Nas::MemberKind::Partition, "/dev/sdb1", "", 300*G};
        auto a = Nas::evaluateArray(p);
        check(a.health == Nas::ArrayHealth::Single, "单成员被标为非 RAID");
    }

    printf("\n=== backing 文件大小：按最小可用空间并留余量 ===\n");
    {
        qint64 sz = Nas::suggestBackingSize({ 500*G, 100*G, 300*G }, 0.95);
        check(sz <= (qint64)(100.0*G*0.95) && sz > 90*G, "取最小者并打折");
        check(sz % (1024*1024) == 0, "结果对齐到 MiB（保证两文件严格等大）");
    }
    check(Nas::suggestBackingSize({}, 0.95) == 0, "空输入返回 0 而不是崩溃");

    printf("\n%s (%d 处失败)\n", failures ? "❌ 有失败" : "✅ 全部通过", failures);
    return failures ? 1 : 0;
}
