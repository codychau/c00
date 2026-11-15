// 真正的风险点：systemDiskNames 把分区名解析成父盘名。
// 若它少算一个父盘，那个盘上的分区就会被判成"可选"，可以直接抹掉。
#include "nasmodel.h"
#include <cstdio>
static int fail = 0;
static void check(bool c, const char *w){ printf("  %s  %s\n", c?"✅":"❌", w); if(!c) ++fail; }

static bool hasSystemDisk(const QString &mounts, const QString &want){
    return Nas::systemDiskNames(mounts).contains(want);
}

int main(){
    printf("=== 各分区命名风格都要解析出正确的父盘 ===\n");
    check(hasSystemDisk("/dev/sda2 / ext4 rw 0 0\n", "sda"),           "sda2 -> sda");
    check(hasSystemDisk("/dev/sda1 / ext4 rw 0 0\n", "sda"),           "sda1 -> sda");
    check(hasSystemDisk("/dev/nvme0n1p2 / ext4 rw 0 0\n", "nvme0n1"),  "nvme0n1p2 -> nvme0n1");
    check(hasSystemDisk("/dev/mmcblk0p2 / ext4 rw 0 0\n", "mmcblk0"),  "mmcblk0p2 -> mmcblk0");
    check(hasSystemDisk("/dev/vda3 / ext4 rw 0 0\n", "vda"),           "vda3 -> vda");
    check(hasSystemDisk("/dev/xvda1 / ext4 rw 0 0\n", "xvda"),         "xvda1 -> xvda");

    printf("\n=== /boot、/home 也算系统盘（动了会丢用户数据）===\n");
    check(hasSystemDisk("/dev/sdb1 /home ext4 rw 0 0\n", "sdb"),       "/home 的盘被识别");
    check(hasSystemDisk("/dev/sdc1 /boot ext4 rw 0 0\n", "sdc"),       "/boot 的盘被识别");

    printf("\n=== 关键：多分区系统要**全部**识别（漏一个=那个盘可被抹）===\n");
    {
        QString m = "/dev/nvme0n1p2 / ext4 rw 0 0\n"
                    "/dev/sda1 /mnt/data ext4 rw 0 0\n"      // 非关键挂载点
                    "/dev/sdb5 /home ext4 rw 0 0\n";
        auto v = Nas::systemDiskNames(m);
        check(v.contains("nvme0n1"), "根分区盘被识别");
        check(v.contains("sdb"),     "/home 盘被识别");
        check(!v.contains("sda"),    "无关数据盘 sda 不该被算进系统盘");
    }

    printf("\n=== 非块设备挂载不能污染结果 ===\n");
    {
        QString m = "/dev/sda2 / ext4 rw 0 0\n"
                    "tmpfs /run tmpfs rw 0 0\n"
                    "overlay /var/lib/docker/overlay2/x overlay rw 0 0\n"
                    "proc /proc proc rw 0 0\n";
        auto v = Nas::systemDiskNames(m);
        check(v.contains("sda"), "真根盘仍在");
        check(v.size() == 1, "tmpfs/overlay/proc 未被误当成磁盘");
    }

    printf("\n=== 整盘直接挂载（无分区表）===\n");
    check(hasSystemDisk("/dev/sdb /data ext4 rw 0 0\n", "sdb") == false,
          "非关键挂载点不算系统盘（/data 是数据盘，应可选）");
    check(hasSystemDisk("/dev/sdb / ext4 rw 0 0\n", "sdb"), "整盘挂根也能识别");

    printf("\n%s (%d 失败)\n", fail ? "❌" : "✅ 全部通过", fail);
    return fail ? 1 : 0;
}
